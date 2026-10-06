// netutils.cpp —— 局域网扫描工具的底层网络工具实现（ICMP / TCP / ARP / 本机信息 / 反向 DNS）
//
// 平台：Linux（Debian / Red Hat / Arch / SUSE 系均可）+ Qt 5.15
// 说明：本文件不再依赖任何 Win32 API，全部改用 POSIX 接口与内核 /proc 文件：
//       - 网卡枚举        getifaddrs()
//       - ICMP 探测       调用系统 ping 命令（无需 root，发行版自带 setuid/cap_net_raw）
//       - ARP 表          /proc/net/arp
//       - IPv6 邻居表     ip -6 neigh show
//       - 默认网关        /proc/net/route
//       - 裸 socket       netcompat.h 中的 POSIX 封装

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstring>

#include <QAbstractSocket>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkInterface>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include "netcompat.h"
#include "netutils.h"

namespace
{

/// 定位系统可执行文件：先走 PATH，再回退常见绝对路径
/// （iproute2 装在 /usr/sbin 时，桌面会话的 PATH 里往往没有 /usr/sbin）
QString findSystemCommand(const QString &name)
{
    const QString found = QStandardPaths::findExecutable(name);
    if (!found.isEmpty())
        return found;

    static const QStringList directories = {
        QStringLiteral("/usr/bin"),  QStringLiteral("/bin"),
        QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
        QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/local/sbin"),
    };
    for (const QString &directory : directories)
    {
        const QString candidate = directory + QLatin1Char('/') + name;
        const QFileInfo info(candidate);
        if (info.exists() && info.isExecutable())
            return candidate;
    }
    return QString();
}

/// 让子进程输出稳定的英文文本，避免本地化文案（中文「时间=」等）破坏解析
void applyCommandEnvironment(QProcess &process)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
}

/// 严格校验 IPv4 字符串（4 段十进制）并取主机字节序数值
bool parseIpv4(const QString &text, quint32 *value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})$"));

    const QString trimmed = text.trimmed();
    if (!pattern.match(trimmed).hasMatch())
        return false;

    const QHostAddress address(trimmed);
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;

    if (value)
        *value = static_cast<quint32>(address.toIPv4Address());
    return true;
}

/// 把 IPv4 字符串转成网络字节序数值（sockaddr_in 需要）
bool ipv4ToNetworkOrder(const QString &ip, quint32 *networkOrder)
{
    quint32 hostOrder = 0;
    if (!parseIpv4(ip, &hostOrder))
        return false;
    if (networkOrder)
        *networkOrder = htonl(hostOrder);
    return true;
}

/// 填充 IPv4 的 sockaddr_in
bool buildSockAddr(const QString &ip, sockaddr_in *addr)
{
    quint32 networkOrder = 0;
    if (!ipv4ToNetworkOrder(ip, &networkOrder))
        return false;

    std::memset(addr, 0, sizeof(sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = networkOrder;
    return true;
}

/// 把网卡物理地址格式化为 AA-BB-CC-DD-EE-FF（大写），非法长度返回空
QString formatMacAddress(const QString &raw)
{
    QString hex = raw;
    hex.remove(QLatin1Char(':'));
    hex.remove(QLatin1Char('-'));
    hex.remove(QLatin1Char('.'));
    hex = hex.toUpper();
    if (hex.size() != 12)
        return QString();

    QStringList pairs;
    pairs.reserve(6);
    for (int i = 0; i < 12; i += 2)
        pairs.append(hex.mid(i, 2));
    return pairs.join(QLatin1Char('-'));
}

/// 执行外部命令并返回标准输出（命令输出已强制 LC_ALL=C，按 Latin-1 解码即可）；超时或失败返回空
QString runSystemCommand(const QString &program, const QStringList &arguments, int timeoutMs)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    applyCommandEnvironment(process);
    process.start(program, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(500))
        return QString();

    const int budget = qMax(1, timeoutMs);
    QByteArray output;
    QElapsedTimer timer;
    timer.start();

    while (process.state() != QProcess::NotRunning && timer.elapsed() < budget)
    {
        const int remaining = static_cast<int>(budget - timer.elapsed());
        if (!process.waitForReadyRead(qMax(1, remaining)))
            break; // 超时或出错
        output += process.readAllStandardOutput();
    }

    if (process.state() != QProcess::NotRunning)
    {
        // 超时视为失败
        process.kill();
        process.waitForFinished(200);
        return QString();
    }

    output += process.readAll();
    return QString::fromLatin1(output);
}

/// 内核表输出的行分隔（/proc/net/arp、/proc/net/route、ip neigh 通用）
QStringList splitArpOutput(const QString &output)
{
    return output.split(QRegularExpression(QStringLiteral("[\\r\\n]")), Qt::SkipEmptyParts);
}

/// 内核表输出的列分隔（空格 / Tab）
QStringList splitArpFields(const QString &line)
{
    return line.split(QRegularExpression(QStringLiteral("[ \\t]+")), Qt::SkipEmptyParts);
}

/// 专用 DNS 解析线程。
/// QHostInfo 依赖事件循环，而扫描工作线程里没有事件循环，因此在独立线程里跑查找，
/// 调用方通过条件变量阻塞等待结果。每次调用持有独立的 CallState，
/// 因此可以有任意多个线程并发解析（超时后调用方放弃，回调仍会安全地填充自己的状态对象）。
class DnsResolver
{
public:
    static DnsResolver &instance()
    {
        // 堆上分配且永不析构，避免进程退出时静态对象析构顺序带来的线程清理问题
        static DnsResolver *resolver = new DnsResolver;
        return *resolver;
    }

    QString resolve(const QString &ip, int timeoutMs)
    {
        QObject *context = ensureThread();

        QSharedPointer<CallState> state(new CallState);

        QMetaObject::invokeMethod(
            context,
            [context, ip, state]() {
                QHostInfo::lookupHost(ip, context, [ip, state](const QHostInfo &info) {
                    QString result;
                    bool ok = false;
                    if (info.error() == QHostInfo::NoError)
                    {
                        const QString hostName = info.hostName();
                        // Qt 会把 IP 字面量反向解析为它自身；无 PTR 记录时视作解析失败。
                        if (!hostName.isEmpty() && hostName.compare(ip, Qt::CaseInsensitive) != 0)
                        {
                            result = hostName;
                            ok = true;
                        }
                    }

                    QMutexLocker resultLocker(&state->mutex);
                    state->result = result;
                    state->ok = ok;
                    state->done = true;
                    state->cond.wakeAll();
                });
            },
            Qt::QueuedConnection);

        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&state->mutex);
        while (!state->done)
        {
            const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
            if (remaining <= 0)
                break;
            state->cond.wait(&state->mutex, static_cast<unsigned long>(remaining));
        }

        return state->ok ? state->result : QString();
    }

private:
    struct CallState
    {
        QMutex mutex;
        QWaitCondition cond;
        bool done = false;
        bool ok = false;
        QString result;
    };

    DnsResolver() = default;
    ~DnsResolver() = default;
    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;

    QObject *ensureThread()
    {
        QMutexLocker locker(&m_mutex);
        if (!m_thread)
        {
            m_thread = new QThread;
            m_thread->setObjectName(QStringLiteral("DnsResolver"));
            m_context = new QObject; // 无父对象，随后移入解析线程
            m_context->moveToThread(m_thread);
            m_thread->start();
        }
        return m_context;
    }

    QThread *m_thread = nullptr;
    QObject *m_context = nullptr;
    QMutex m_mutex;
};

} // namespace

namespace NetUtils
{

qint64 ipToLong(const QString &ip)
{
    quint32 value = 0;
    if (!parseIpv4(ip, &value))
        return -1;
    return static_cast<qint64>(value);
}

QString longToIp(qint64 value)
{
    const quint32 v = static_cast<quint32>(value);
    return QStringLiteral("%1.%2.%3.%4")
        .arg((v >> 24) & 0xFF)
        .arg((v >> 16) & 0xFF)
        .arg((v >> 8) & 0xFF)
        .arg(v & 0xFF);
}

bool isPrivateIp(const QString &ip)
{
    quint32 value = 0;
    if (!parseIpv4(ip, &value))
        return false;

    const quint32 a = (value >> 24) & 0xFF;
    const quint32 b = (value >> 16) & 0xFF;

    if (a == 10) // 10.0.0.0/8
        return true;
    if (a == 172 && b >= 16 && b <= 31) // 172.16.0.0/12
        return true;
    if (a == 192 && b == 168) // 192.168.0.0/16
        return true;
    return false;
}

bool icmpPing(const QString &ip, int timeoutMs, qint64 *roundTripMs)
{
    if (roundTripMs)
        *roundTripMs = -1;

    quint32 address = 0;
    if (!parseIpv4(ip, &address))
        return false;

    // 调用系统 ping（iputils / busybox 均可）。桌面环境下 ping 通常带 setuid 或 cap_net_raw，
    // 无需 root 即可使用 ICMP 数据报套接字。
    const QString ping = findSystemCommand(QStringLiteral("ping"));
    if (ping.isEmpty())
        return false;

    const int seconds = qMax(1, (timeoutMs + 999) / 1000);
    const QStringList arguments = QStringList()
        << QStringLiteral("-n") << QStringLiteral("-c") << QStringLiteral("1")
        << QStringLiteral("-W") << QString::number(seconds) << ip;
    const QString output = runSystemCommand(ping, arguments, timeoutMs + 1500);
    if (output.isEmpty())
        return false;

    // 例：64 bytes from 192.168.1.1: icmp_seq=1 ttl=64 time=1.23 ms
    static const QRegularExpression timePattern(
        QStringLiteral("time[=<]\\s*([0-9.]+)\\s*ms"));
    const QRegularExpressionMatch match = timePattern.match(output);
    if (!match.hasMatch())
        return false;

    if (roundTripMs)
        *roundTripMs = qMax<qint64>(0, static_cast<qint64>(qRound(match.captured(1).toDouble())));
    return true;
}

QByteArray tcpExchange(const QString &ip, int port, const QByteArray &request, int budgetMs)
{
    QByteArray response;

    sockaddr_in target;
    if (!buildSockAddr(ip, &target))
        return response;

    NetCompat::ensureStartup();

    const NetCompat::socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == NetCompat::kInvalidSocket)
        return response;

    if (!NetCompat::setNonBlocking(sock))
    {
        NetCompat::closeSocket(sock);
        return response;
    }

    target.sin_port = htons(static_cast<quint16>(port));

    bool connected = ::connect(sock, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) == 0;
    if (!connected)
    {
        if (NetCompat::lastError() != NetCompat::kErrInProgress)
        {
            NetCompat::closeSocket(sock);
            return response;
        }

        NetCompat::pollfd_t pollFd;
        pollFd.fd = sock;
        pollFd.events = NetCompat::kPollOut;
        pollFd.revents = 0;
        if (NetCompat::pollSockets(&pollFd, 1, budgetMs) <= 0)
        {
            NetCompat::closeSocket(sock);
            return response;
        }

        int socketError = 0;
        socklen_t socketErrorSize = sizeof(socketError);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) != 0
            || socketError != 0)
        {
            NetCompat::closeSocket(sock);
            return response;
        }
    }

    // 请求为空时只读服务端主动送出的内容（个别设备连上就推 banner）
    if (!request.isEmpty()
        && ::send(sock, request.constData(), static_cast<size_t>(request.size()), MSG_NOSIGNAL) <= 0)
    {
        NetCompat::closeSocket(sock);
        return response;
    }

    QElapsedTimer timer;
    timer.start();
    char buffer[2048];
    while (response.size() < 8192)
    {
        const int remaining = budgetMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0)
            break;

        NetCompat::pollfd_t pollFd;
        pollFd.fd = sock;
        pollFd.events = NetCompat::kPollIn;
        pollFd.revents = 0;
        if (NetCompat::pollSockets(&pollFd, 1, remaining) <= 0)
            break;

        const int received = ::recv(sock, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0)
            break;
        response.append(buffer, received);
    }

    NetCompat::closeSocket(sock);
    return response;
}

QHash<QString, QString> readArpTable(int timeoutMs)
{
    Q_UNUSED(timeoutMs);

    QHash<QString, QString> table;

    QFile file(QStringLiteral("/proc/net/arp"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return table;
    const QString output = QString::fromLatin1(file.readAll());
    file.close();

    const QStringList lines = splitArpOutput(output);
    for (const QString &line : lines)
    {
        const QStringList fields = splitArpFields(line);
        // 表头：IP address HW type Flags HW address Mask Device
        if (fields.size() < 4)
            continue;

        quint32 addressValue = 0;
        if (!parseIpv4(fields.at(0), &addressValue))
            continue;

        // Flags 0x2 = ATF_COM，仅保留已完成解析的有效表项
        bool ok = false;
        const uint flags = fields.at(2).toUInt(&ok, 0);
        if (!ok || (flags & 0x2) == 0)
            continue;

        const QString mac = formatMacAddress(fields.at(3));
        if (mac.isEmpty() || mac == QStringLiteral("00-00-00-00-00-00")
            || mac == QStringLiteral("FF-FF-FF-FF-FF-FF"))
            continue;

        table.insert(longToIp(static_cast<qint64>(addressValue)), mac);
    }

    return table;
}

QString queryArpEntry(const QString &ip, int timeoutMs)
{
    if (ip.isEmpty())
        return QString();
    return readArpTable(timeoutMs).value(ip);
}

QString formatMac(const QString &raw)
{
    return formatMacAddress(raw);
}

QVector<LocalInterface> localInterfaces()
{
    QVector<LocalInterface> interfaces;

    struct ifaddrs *head = nullptr;
    if (getifaddrs(&head) != 0 || head == nullptr)
        return interfaces;

    for (struct ifaddrs *ifa = head; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if ((ifa->ifa_flags & IFF_UP) == 0)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        const QString name = QString::fromLatin1(ifa->ifa_name);
        if (name.isEmpty())
            continue;

        const sockaddr_in *addr = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
        const QHostAddress address(ntohl(addr->sin_addr.s_addr));
        if (address.isNull() || address.isLoopback())
            continue;

        // 同一接口可能报告多个 IPv4（别名 / 多地址），只取第一个
        bool exists = false;
        for (const LocalInterface &entry : interfaces)
        {
            if (entry.adapterName == name)
            {
                exists = true;
                break;
            }
        }
        if (exists)
            continue;

        LocalInterface entry;
        entry.name = name;
        entry.index = static_cast<int>(if_nametoindex(ifa->ifa_name));
        entry.ipv4 = address.toString();
        entry.adapterName = name;
        interfaces.append(entry);
    }

    freeifaddrs(head);
    return interfaces;
}

void primeIpv6Neighbors(int interfaceIndex)
{
    if (interfaceIndex <= 0)
        return;

    char nameBuffer[IF_NAMESIZE] = {0};
    if (if_indextoname(static_cast<unsigned int>(interfaceIndex), nameBuffer) == nullptr)
        return;

    const QString ping = findSystemCommand(QStringLiteral("ping"));
    if (ping.isEmpty())
        return;

    const QString target = QStringLiteral("ff02::1%") + QString::fromLatin1(nameBuffer);
    const QStringList arguments = QStringList()
        << QStringLiteral("-6") << QStringLiteral("-n") << QStringLiteral("-c") << QStringLiteral("1")
        << QStringLiteral("-W") << QStringLiteral("1") << target;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    applyCommandEnvironment(process);
    process.start(ping, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(500))
        return;

    if (process.state() != QProcess::NotRunning)
    {
        if (!process.waitForFinished(1500))
        {
            process.kill();
            process.waitForFinished(200);
        }
    }
}

QHash<QString, QString> readIpv6Neighbors(int timeoutMs)
{
    QHash<QString, QString> table;

    const QString ipCommand = findSystemCommand(QStringLiteral("ip"));
    if (ipCommand.isEmpty())
        return table;

    const QStringList arguments = QStringList()
        << QStringLiteral("-6") << QStringLiteral("neigh") << QStringLiteral("show");
    const QString output = runSystemCommand(ipCommand, arguments, timeoutMs);
    if (output.isEmpty())
        return table;

    const QStringList lines = splitArpOutput(output);
    for (const QString &line : lines)
    {
        // 例：fe80::1 dev eth0 lladdr aa:bb:cc:dd:ee:ff router STALE
        const QStringList fields = splitArpFields(line);
        if (fields.size() < 3)
            continue;

        // 只保留链路本地地址（自动排除 ff02:: 组播与全局地址）
        const QString address = fields.at(0);
        if (!address.startsWith(QStringLiteral("fe80"), Qt::CaseInsensitive))
            continue;

        const int lladdrIndex = fields.indexOf(QStringLiteral("lladdr"));
        if (lladdrIndex < 0 || lladdrIndex + 1 >= fields.size())
            continue;

        const QString mac = formatMacAddress(fields.at(lladdrIndex + 1));
        if (mac.isEmpty() || mac == QStringLiteral("00-00-00-00-00-00"))
            continue;

        if (!table.contains(mac))
            table.insert(mac, address);
    }

    return table;
}

QString localMacAddress()
{
    QString fallback;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces)
    {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;

        const QString mac = formatMacAddress(iface.hardwareAddress());
        if (mac.isEmpty())
            continue;

        bool hasIpv4 = false;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries)
        {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            {
                hasIpv4 = true;
                break;
            }
        }

        if (hasIpv4)
            return mac; // 优先返回持有 IPv4 地址的网卡
        if (fallback.isEmpty())
            fallback = mac;
    }

    return fallback;
}

QString defaultGatewayIp()
{
    QFile file(QStringLiteral("/proc/net/route"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QString output = QString::fromLatin1(file.readAll());
    file.close();

    const QStringList lines = splitArpOutput(output);
    for (const QString &line : lines)
    {
        // 例：eth0  00000000  0102A8C0  0003  0  0  100  00000000  0  0  0
        const QStringList fields = splitArpFields(line);
        if (fields.size() < 3)
            continue;

        if (fields.at(1) != QStringLiteral("00000000")) // Destination 全 0 即默认路由
            continue;

        bool ok = false;
        const quint32 raw = fields.at(2).toUInt(&ok, 16);
        if (!ok || raw == 0)
            continue;

        // 内核以十六进制小端存放网关：raw 的最低字节是 IP 第一段
        const QString gateway = QStringLiteral("%1.%2.%3.%4")
            .arg(raw & 0xFF)
            .arg((raw >> 8) & 0xFF)
            .arg((raw >> 16) & 0xFF)
            .arg((raw >> 24) & 0xFF);
        if (gateway != QStringLiteral("0.0.0.0"))
            return gateway;
    }

    return QString();
}

QString localIpv4Address()
{
    // 向公网地址做一次 UDP connect（不实际发包），由内核挑选默认出口并回填本机地址
    const NetCompat::socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != NetCompat::kInvalidSocket)
    {
        sockaddr_in target;
        std::memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_port = htons(53);

        QString result;
        if (::inet_pton(AF_INET, "8.8.8.8", &target.sin_addr) == 1
            && ::connect(sock, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) == 0)
        {
            sockaddr_in local;
            socklen_t localLength = sizeof(local);
            std::memset(&local, 0, sizeof(local));
            if (getsockname(sock, reinterpret_cast<sockaddr *>(&local), &localLength) == 0)
            {
                const QHostAddress address(ntohl(local.sin_addr.s_addr));
                if (!address.isNull() && !address.isLoopback()
                    && address.protocol() == QAbstractSocket::IPv4Protocol)
                {
                    result = address.toString();
                }
            }
        }
        NetCompat::closeSocket(sock);
        if (!result.isEmpty())
            return result;
    }

    // 回退：遍历本机地址
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : addresses)
    {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            return address.toString();
    }
    return QString();
}

QString resolveHostName(const QString &ip, int timeoutMs)
{
    if (ip.isEmpty())
        return QString();
    return DnsResolver::instance().resolve(ip, timeoutMs > 0 ? timeoutMs : 1);
}

} // namespace NetUtils
