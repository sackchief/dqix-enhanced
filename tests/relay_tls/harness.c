/* tests/relay_tls: join through the relay (relay.c, as the game does), send
 * "hello" to the local port it gives, expect "pong:hello" back through TLS. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "relay.h"
int main(void) {
    char why[300] = ""; int port = 0;
    if (!relay_join_start("ABCD2345", &port, why, sizeof why)) { printf("JOIN FAILED: %s\n", why); return 2; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) { printf("local connect failed\n"); return 3; }
    send(s, "hello", 5, 0);
    char buf[64] = ""; int got = 0;
    while (got < 10) { int k = (int)recv(s, buf + got, sizeof buf - 1 - (size_t)got, 0); if (k <= 0) break; got += k; }
    printf("JOINED; reply through TLS: %.*s\n", got, buf);
    return strcmp(buf, "pong:hello") ? 4 : 0;
}
