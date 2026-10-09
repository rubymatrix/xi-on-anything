/* BSD sockets for the browser build (net_web.c): browsers can't open TCP or UDP sockets, so every
 * socket is a channel on one WebSocket to the local server (tools/webserve.py, /net), which opens
 * the real one. ws2.c and lsb_login.c include this after the system headers; the macros send their
 * socket calls here. Socket numbers start at WN_BASE, below FD_SETSIZE, so fd_set holds them. */
#pragma once

#if defined(__EMSCRIPTEN__)
#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#define WN_BASE 600
#define WN_MAX 256

int wn_socket(int domain, int type, int protocol);
int wn_close(int s);
int wn_bind(int s, const struct sockaddr* a, socklen_t len);
int wn_listen(int s, int backlog);
int wn_accept(int s, struct sockaddr* a, socklen_t* len);
int wn_connect(int s, const struct sockaddr* a, socklen_t len);
ssize_t wn_send(int s, const void* buf, size_t n, int flags);
ssize_t wn_recv(int s, void* buf, size_t n, int flags);
ssize_t wn_sendto(int s, const void* buf, size_t n, int flags, const struct sockaddr* a, socklen_t len);
ssize_t wn_recvfrom(int s, void* buf, size_t n, int flags, struct sockaddr* a, socklen_t* len);
int wn_shutdown(int s, int how);
int wn_select(int nfds, fd_set* rd, fd_set* wr, fd_set* ex, struct timeval* tv);
int wn_fcntl(int s, int cmd, ...);
int wn_ioctl(int s, int req, ...);
int wn_setsockopt(int s, int level, int name, const void* v, socklen_t len);
int wn_getsockopt(int s, int level, int name, void* v, socklen_t* len);
int wn_getpeername(int s, struct sockaddr* a, socklen_t* len);
int wn_getsockname(int s, struct sockaddr* a, socklen_t* len);

#if !defined(WN_IMPL) /* net_web.c itself calls nothing by these names */
#define socket wn_socket
#define bind wn_bind
#define listen wn_listen
#define accept wn_accept
#define connect wn_connect
#define send wn_send
#define recv wn_recv
#define sendto wn_sendto
#define recvfrom wn_recvfrom
#define shutdown wn_shutdown
#define select wn_select
#define fcntl wn_fcntl
#define ioctl wn_ioctl
#define setsockopt wn_setsockopt
#define getsockopt wn_getsockopt
#define getpeername wn_getpeername
#define getsockname wn_getsockname
#endif
#endif
