#pragma once

#include <functional>

#include <QString>
#include <QStringList>
#include <QVector>

struct ScanCancelToken;

/// nmap 主机发现的结果
struct NmapHost
{
    QString ipAddress;
    QString macAddress; ///< 仅在以 root 运行（nmap 可发 ARP）时 nmap 才会返回 MAC
    QString hostName;
    qint64 rttMs = -1;
};

/// 官方 nmap 子进程封装：只用于主机发现（nmap -sn）
namespace NmapRunner
{
/// 定位 nmap 可执行文件：优先程序目录下的 nmap/nmap，其次 PATH；找不到返回空
QString findNmapExecutable();

/// 用 nmap -sn 对 ipList 做主机发现，只返回在线主机。
/// adapterName 为 Linux 网络接口名（eth0 / wlan0 …），非空且以 root 运行时用 -e 指定出口网卡；
/// 传空串（或非 root 运行）表示交给 nmap 按系统路由自动选择。
/// onProgress 收到 nmap 报告的 0-100 进度；token 用于取消。
/// 失败时返回空列表并写入 errorMessage（调用方据此区分「扫描失败」与「没有在线主机」）。
QVector<NmapHost> scanHosts(const QStringList &ipList, const QString &adapterName,
                            const std::function<void(int)> &onProgress,
                            const ScanCancelToken *token,
                            QString *errorMessage);
} // namespace NmapRunner
