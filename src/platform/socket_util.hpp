#pragma once

// Cross-platform socket utilities.

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define TCCP_INVALID_SOCKET INVALID_SOCKET
   // WSAPoll uses the same constants as POSIX poll
#else
#  include <poll.h>
   using socket_t = int;
#  define TCCP_INVALID_SOCKET (-1)
#endif

namespace platform {

// Initialize networking (WSAStartup on Windows, no-op on Unix).
void init_networking();

// Set a socket to non-blocking mode.
void set_nonblocking(socket_t sock);

// Poll events on a single socket. Returns revents, or 0 on timeout.
// events: POLLIN, POLLOUT, etc.
int poll_socket(socket_t sock, short events, int timeout_ms);

// Close a socket.
void close_socket(socket_t sock);

} // namespace platform
