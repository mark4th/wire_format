// demo.c  - wire_format DNS query demo
// -----------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "wire_format.h"
#include "dns.h"

// -----------------------------------------------------------------------

#define BUF_SIZE   512
#define DNS_SERVER "8.8.8.8"
#define DNS_PORT   53

// -----------------------------------------------------------------------

int main(int argc, char *argv[])
{
    const char *hostname = (argc > 1) ? argv[1] : "example.com";
    uint8_t     query[BUF_SIZE];
    uint8_t     response[BUF_SIZE];

    srand((unsigned)time(NULL));
    uint16_t txid = (uint16_t)rand();

    size_t qlen = dns_build_query(hostname, DNS_QTYPE_A, txid, query, BUF_SIZE);

    printf("querying %s for A records (txid=0x%04x, %zu bytes)\n",
           hostname, txid, qlen);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = inet_addr(DNS_SERVER),
    };

    if (sendto(fd, query, qlen, 0,
               (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        perror("sendto");
        close(fd);
        return 1;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t rlen = recv(fd, response, BUF_SIZE, 0);
    close(fd);

    if (rlen < 0)
    {
        fprintf(stderr, "no response (timeout or error)\n");
        return 1;
    }

    dns_print_response(response, (size_t)rlen);

    return 0;
}

// =======================================================================
