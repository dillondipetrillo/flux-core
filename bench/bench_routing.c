#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

/**
 * bench_routing.c - Routing throughput and latency benchmark.
 * 
 * Requires a running server:
 *      ENGINE_WORKER_COUNT=1 ./server &
 * 
 * Run:
 *      make bench
 *      ./bench/bench_routing
 * 
 * Reports:
 *      - Packets sent
 *      - Packets received
 *      - Elapsed time
 *      - Throughput (packets/second)
 *      - Average round-trip latency (microseconds)
 */

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 8080
#define BENCH_TOKEN "bench-token"
#define BENCH_SCOPE 999
#define PACKET_COUNT 50000
#define PAYLOAD_SIZE 64 // bytes per payload - realistic message size

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int connect_and_auth(const char *token)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect - is the server running?");
        close(fd);
        return -1;
    }

    // IDENTIFY
    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = TYPE_SYS_IDENTIFY;
    hdr.payload_len = htonl((uint32_t)strlen(token));
    send(fd, &hdr, sizeof(hdr), 0);
    send(fd, token, strlen(token), 0);

    // Read ACK
    struct packet_header ack;
    struct response_payload rp;
    recv(fd, &ack, sizeof(ack), MSG_WAITALL);
    recv(fd, &rp, sizeof(rp), MSG_WAITALL);
    if (ntohl(rp.status_code) != STATUS_OK) {
        fprintf(stderr, "Auth failed: %u\n", ntohl(rp.status_code));
        close(fd);
        return -1;
    }

    // JOIN scope
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = TYPE_SYS_JOIN;
    hdr.scope_id = htonl(BENCH_SCOPE);
    send(fd, &hdr, sizeof(hdr), 0);
    recv(fd, &ack, sizeof(ack), MSG_WAITALL);
    recv(fd, &rp, sizeof(rp), MSG_WAITALL);

    return fd;
}

int main(void)
{
    printf("=== C State Bus Routing Benchmark ===\n");
    printf("Packets: %d | Payload: %d bytes | Scope: %d\n\n", PACKET_COUNT,
        PAYLOAD_SIZE, BENCH_SCOPE);

    int sender = connect_and_auth("bench-sender");
    int receiver = connect_and_auth("bench-receiver");

    if (sender == -1 || receiver == -1) {
        fprintf(stderr, "Could not connect. Start server first:\n");
        fprintf(stderr, " ENGINE_WORKER_COUNT=1 ./server &\n");
        return 1;
    }

    // Build the payload once - fixed content, realistic size
    char payload[PAYLOAD_SIZE];
    memset(payload, 'A', sizeof(payload));

    // Build the packet once - same packet sent N times
    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = TYPE_APP_REALTIME;
    hdr.payload_len = htonl(PAYLOAD_SIZE);
    hdr.scope_id = htonl(BENCH_SCOPE);
    hdr.sender_id = htonl(0);
    hdr.expires_at = htobe64(0); // never expires

    // Receive buffer
    char recv_buf[sizeof(struct packet_header) + PAYLOAD_SIZE];

    printf("Warming up (100 packets)...\n");
    for (int i = 0; i < 100; i++) {
        send(sender, &hdr, sizeof(hdr), 0);
        send(sender, payload, PAYLOAD_SIZE, 0);
        recv(receiver, recv_buf, sizeof(recv_buf), MSG_WAITALL);
    }

    printf("Benchmarking %d packets...\n", PACKET_COUNT);

    uint64_t t_start = now_ns();
    uint64_t latency_ns = 0;
    int received = 0;

    for (int i = 0; i < PACKET_COUNT; i++) {
        uint64_t t_send = now_ns();

        send(sender, &hdr, sizeof(hdr), 0);
        send(sender, payload, PAYLOAD_SIZE, 0);

        ssize_t n = recv(receiver, recv_buf,
            sizeof(struct packet_header) + PAYLOAD_SIZE, MSG_WAITALL);

        uint64_t t_recv = now_ns();

        if (n > 0) {
            received++;
            latency_ns += (t_recv - t_send);
        }
    }

    uint64_t t_end = now_ns();
    double elapsed = (double)(t_end - t_start) / 1e9;
    double pps = received / elapsed;
    double avg_lat = received > 0 ? (double)latency_ns / received / 1000.0 :
        0.0;

    printf("\n=== Results ===\n");
    printf("Sent:           %d packets\n", PACKET_COUNT);
    printf("Received:       %d packets\n", received);
    printf("Lost:           %d packets\n", PACKET_COUNT - received);
    printf("Elapsed:        %.3f seconds\n", elapsed);
    printf("Throughput:     %.0f packets/seconds\n", pps);
    printf("Avg latency:    %.1f microseconds (round-trip)\n", avg_lat);
    printf("\n");

    if (pps >= 100000) {
        printf("EXCELLENT - exceeds 100k pps target\n");
    } else if (pps >= 50000) {
        printf("GOOD - exceeds 50k pps target\n");
    } else {
        printf("BELOW TARGET - investigate bottlenecks\n");
    }

    close(sender);
    close(receiver);
    return 0;
}