/* Blocking TCP for the POL client: Winsock on Windows, BSD sockets elsewhere. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef intptr_t PolSocket;
#define POL_NO_SOCKET ((PolSocket)-1)

int pol_net_init(void);
/* Connects with a timeout; POL_NO_SOCKET on failure. */
PolSocket pol_tcp_connect(const char* host, uint16_t port, int timeout_ms);
int pol_send_all(PolSocket s, const void* data, size_t n);
/* Exactly n bytes within timeout_ms; 0 on timeout, close or error. */
int pol_recv_exact(PolSocket s, void* data, size_t n, int timeout_ms);
/* Whatever is available within timeout_ms: bytes read, 0 on timeout, -1 on close or error. */
int pol_recv_some(PolSocket s, void* data, size_t n, int timeout_ms);
void pol_close(PolSocket s);
