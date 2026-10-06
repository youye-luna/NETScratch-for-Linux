#pragma once

// netcompat.h —— Linux 下的 socket / poll 兼容层
//
// 原项目基于 WinSock（SOCKET / WSAPoll / ioctlsocket / closesocket …）。
// 移植到 Linux 后统一改用本头文件里的小封装，语义与 WinSock 一一对应，
// 使 netutils / netdiscovery / cameradetector 三处裸 socket 代码写法保持一致。

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace NetCompat
{
/// 套接字句柄（Linux 下就是文件描述符）
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;

/// pollfd 结构与就绪事件（对应 WinSock 的 WSAPOLLFD / POLLRDNORM / POLLWRNORM）
using pollfd_t = struct pollfd;
constexpr short kPollIn = POLLIN;
constexpr short kPollOut = POLLOUT;

/// 常见错误码（对应 WSAEWOULDBLOCK / WSAECONNREFUSED）
constexpr int kErrInProgress = EINPROGRESS;
constexpr int kErrWouldBlock = EWOULDBLOCK;
constexpr int kErrConnRefused = ECONNREFUSED;

/// 进程内初始化：WinSock 需要 WSAStartup，Linux 无需任何操作
inline void ensureStartup() {}

inline void closeSocket(socket_t sock)
{
    if (sock != kInvalidSocket)
        ::close(sock);
}

/// 置为非阻塞，失败返回 false
inline bool setNonBlocking(socket_t sock)
{
    const int flags = ::fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// 最近一次 socket 调用错误码
inline int lastError()
{
    return errno;
}

/// 一次 poll；返回就绪的 fd 数
inline int pollSockets(pollfd_t *fds, int count, int timeoutMs)
{
    return ::poll(fds, static_cast<nfds_t>(count), timeoutMs);
}
} // namespace NetCompat
