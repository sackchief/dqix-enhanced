/* TLS for the relay (relay_tls.h). */
#include <stdio.h>
#include "relay_tls.h"

#ifdef DQIX_TLS
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

struct TlsConn {
    SSL* ssl;
    int fd;
    pthread_mutex_t mx;     /* an SSL object isn't safe for a read and a write at the same time */
};

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

static SSL_CTX* tls_ctx(char* why, size_t wn) {
    static SSL_CTX* ctx;
    if (ctx) return ctx;
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { snprintf(why, wn, "TLS isn't available (OpenSSL)"); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) { snprintf(why, wn, "this system's certificates can't be found"); SSL_CTX_free(ctx); ctx = NULL; return NULL; }
    return ctx;
}

void tls_free(TlsConn* t) {
    if (!t) return;
    if (t->ssl) SSL_free(t->ssl);
    pthread_mutex_destroy(&t->mx);
    free(t);
}

TlsConn* tls_connect(int fd, const char* host, char* why, size_t wn) {
    SSL_CTX* ctx = tls_ctx(why, wn);
    if (!ctx) return NULL;
    TlsConn* t = calloc(1, sizeof *t);
    t->fd = fd; pthread_mutex_init(&t->mx, NULL);
    t->ssl = SSL_new(ctx);
    if (!t->ssl) { snprintf(why, wn, "TLS couldn't start"); tls_free(t); return NULL; }
    unsigned char ip[16];
    int is_ip = inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1;
    if (is_ip) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host);
    else { SSL_set_tlsext_host_name(t->ssl, host); SSL_set1_host(t->ssl, host); }
    SSL_set_fd(t->ssl, fd);
    struct timeval tv = { 10, 0 };                               /* the handshake: blocking, but not forever */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (SSL_connect(t->ssl) != 1) {
        long v = SSL_get_verify_result(t->ssl);
        if (v != X509_V_OK) snprintf(why, wn, "the relay's certificate isn't trusted (%s)", X509_verify_cert_error_string(v));
        else snprintf(why, wn, "the secure connection to the relay failed (TLS)");
        tls_free(t);
        return NULL;
    }
    memset(&tv, 0, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);      /* from here the calls wait with poll, so a read never holds up a write */
    return t;
}

static int tls_io(TlsConn* t, void* buf, size_t n, int write) {
    double t0 = now_ms();
    for (;;) {
        pthread_mutex_lock(&t->mx);
        int k = write ? SSL_write(t->ssl, buf, (int)n) : SSL_read(t->ssl, buf, (int)n);
        int e = k > 0 ? 0 : SSL_get_error(t->ssl, k);
        pthread_mutex_unlock(&t->mx);
        if (k > 0) return k;
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return 0;
        if (write && now_ms() - t0 > 15000) return 0;
        struct pollfd p = { t->fd, (short)(e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0 };
        poll(&p, 1, 100);                                        /* reading waits as long as it takes (a lobby can be quiet) */
    }
}

int tls_read(TlsConn* t, void* buf, size_t n) { return tls_io(t, buf, n, 0); }

int tls_write(TlsConn* t, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n) {
        int k = tls_io(t, (void*)p, n > 16384 ? 16384 : n, 1);
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

#else   /* built without OpenSSL */

TlsConn* tls_connect(int fd, const char* host, char* why, size_t wn) {
    (void)fd; (void)host;
    snprintf(why, wn, "the relay needs TLS, which this build doesn't have (it was built without OpenSSL)");
    return NULL;
}
int tls_read(TlsConn* t, void* buf, size_t n) { (void)t; (void)buf; (void)n; return 0; }
int tls_write(TlsConn* t, const void* buf, size_t n) { (void)t; (void)buf; (void)n; return 0; }
void tls_free(TlsConn* t) { (void)t; }
#endif
