#include "socket_util.hpp"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <unistd.h>
#endif

namespace platform {

void init_networking() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        initialized = true;
    }
#endif
}

void set_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

int poll_socket(socket_t sock, short events, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd;
    pfd.fd = sock;
    pfd.events = events;
    pfd.revents = 0;
    int ret = WSAPoll(&pfd, 1, timeout_ms);
    return (ret > 0) ? pfd.revents : 0;
#else
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = events;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, timeout_ms);
    return (ret > 0) ? pfd.revents : 0;
#endif
}

void close_socket(socket_t sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

} // namespace platform
