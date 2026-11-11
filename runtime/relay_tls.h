/* TLS for the relay's https connection (relay.c), through OpenSSL: used where the
 * system has no https of its own (Linux; Windows uses WinHTTP). Built with
 * DQIX_TLS (build.mk sets it when OpenSSL is found); without it these
 * functions still exist, and tls_connect says why it can't. */
#pragma once
#include <stddef.h>

typedef struct TlsConn TlsConn;

/* TLS over the connected socket fd, checking the certificate and that it is
 * for host (a name or an IP address). NULL: not possible, and why says so. */
TlsConn* tls_connect(int fd, const char* host, char* why, size_t wn);
/* 1 to n bytes, waiting as long as it takes; 0 or less: the connection ended.
 * Safe to call from one thread while another uses tls_write. */
int tls_read(TlsConn* t, void* buf, size_t n);
/* all n bytes, or 0 if the connection broke (or the peer stopped reading for 15 s) */
int tls_write(TlsConn* t, const void* buf, size_t n);
/* the socket stays the caller's: close it first or after, as you like */
void tls_free(TlsConn* t);
