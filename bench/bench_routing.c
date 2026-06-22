#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

/**
 * bench_routing.c - C State Bus Routing Benchmark
 * 
 * Measures two things:
 * 
 * Synchronous mode: send one packet, wait for receipt, measure round-trip.
 * This is the latency measurement. Conservative throughput but accurate.
 * 
 * Burst mode: send packets in bursts without waiting, count received.
 * This is the throughput measurement. Exercises the engine's ability to
 * pipeline many packets simultaneously.
 * 
 * Requires a running optimized server:
 *      ENGINE_WORKER_COUNT=1 ./server_opt &
 * 
 * Build:
 *      make bench
 * 
 * Run:
 *      ./bench/bench_routing
 * 
 * Memory note: this benchmark allocates nothing on the heap during the
 * measurement loop. All buffers are stack-allocated or statically sized.
 * This ensures benchmark overhead does not contaminate results.
 */

#define SERVER_HOST "127.0.0.1"
#define BENCH_SCOPE 9999
#define SYNC_COUNT 10000 // packets for synchronous latency test
#define BURST_COUNT 50000 // packets for burst throughput test
#define BURST_SIZE 500 // packets per burst window
#define PAYLOAD_SIZE 64 // bytes, realistic small message size
#define RECV_TIMEOUT_S 5 // seconds before recv gives up

/**
 * Read port from environment - matches how the engine is configured.
 * Allows benchmark to run against non-default ports in CI/CD pipelines.
 */
static int server_port = 8080;

/**
 * ===========================================================
 * TIMING
 * ===========================================================
 */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * ===========================================================
 * PROTOCOL HELPERS
 * ===========================================================
 */

/**
 * send_packet - build and send one complete packet.
 * Returns 1 on success, 0 on failure.
 */
static int send_packet(int fd, uint8_t type, uint32_t scope_id,
    const char *payload, uint32_t payload_len, uint64_t expires_at)
{
    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = type;
    hdr.payload_len = htonl(payload_len);
    hdr.scope_id = htonl(scope_id);
    hdr.sender_id = htonl(0);
    hdr.expires_at = htobe64(expires_at);

    if (send(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) return 0;
    if (payload_len > 0) {
        if (send(fd, payload, payload_len, 0) != (ssize_t)payload_len)
            return 0;
    }
    return 1;
}

/**
 * recv_exact - receive exactly n bytes, looping until complete.
 * Returns 1 on success, 0 on connection closed or error.
 * 
 * TCP may deliver data in fragments. MSG_WAITALL handles this in theory but
 * can fail if interrupted. This explicit loop is correct in all cases.
 */
static int recv_exact(int fd, void *buf, size_t n)
{
    size_t received = 0;
    uint8_t *ptr = (uint8_t *)buf;
    while (received < n) {
        ssize_t r = recv(fd, ptr + received, n - received, 0);
        if (r <= 0) return 0;
        received += (size_t)r;
    }
    return 1;
}

/**
 * drain_response - read and discard one ACK or ERROR response.
 * 
 * The server sends TYPE_SYS_ACK or TYPE_SYS_ERROR after every system packet
 * (IDENTIFY, JOIN, LEAVE). These must be consumed before the socket buffer
 * can be used for routed messages. Failing to drain them corrupts all
 * subsequent reads.
 * 
 * Returns the status code, or -1 on failure.
 */
static int drain_response(int fd)
{
    struct packet_header hdr;
    if (!recv_exact(fd, &hdr, sizeof(hdr))) return -1;

    uint32_t plen = ntohl(hdr.payload_len);
    if (plen == 0) return (int)hdr.type;

    // Read the payload, should be response_payload (4 bytes)
    uint8_t payload[256];
    if (plen > sizeof(payload)) return -1;
    if (!recv_exact(fd, payload, plen)) return -1;

    if (plen >= 4) {
        uint32_t status;
        memcpy(&status, payload, 4);
        return (int)ntohl(status);
    }
    return (int)hdr.type;
}

/**
 * recv_routed_packet - receive one application data packet.
 * 
 * Reads the header first, then exactly payload_len bytes. This is the correct
 * way to receive framed binary protocol packets over TCP. Never assume the
 * entire packet arrives in one recv().
 * 
 * Returns payload length received, or -1 on failure/timeout.
 */
static int recv_routed_packet(int fd, char *buf, size_t buf_size)
{
    struct packet_header hdr;
    if (!recv_exact(fd, &hdr, sizeof(hdr))) return -1;

    uint32_t plen = ntohl(hdr.payload_len);
    if (plen == 0) return 0;
    if (plen > buf_size) return -1;
    if (!recv_exact(fd, buf, plen)) return -1;
    return (int)plen;
}

/**
 * ===========================================================
 * CONNECTION SETUP
 * ===========================================================
 */

/**
 * connect_and_setup - connect, authenticate, join scope.
 * 
 * After each system packet, drain_response() consumes the server's ACK. This
 * leaves the socket buffer clean for routed messages only.
 * 
 * Returns the connected fd, or -1 on any failure.
 */
static int connect_and_setup(const char *token)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    // Set receive timeout, prevents hangs if server misbehaves
    struct timeval tv = {RECV_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Disable Nagle algo, send packets immediately without 200ms batching
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect - is ./server_opt running?");
        close(fd);
        return -1;
    }

    // IDENTIFY
    if (!send_packet(fd, TYPE_SYS_IDENTIFY, 0, token, (uint32_t)strlen(token),
        0))
    {
        fprintf(stderr, "send IDENTIFY failed\n");
        close(fd);
        return -1;
    }
    int code = drain_response(fd);
    if (code != STATUS_OK) {
        fprintf(stderr, "IDENTIFY rejected: status %d\n", code);
        close(fd);
        return -1;
    }

    // JOIN scope
    if (!send_packet(fd, TYPE_SYS_JOIN, BENCH_SCOPE, NULL, 0, 0)) {
        fprintf(stderr, "send JOIN failed\n");
        close(fd);
        return -1;
    }
    code = drain_response(fd);
    if (code != STATUS_OK) {
        fprintf(stderr, "JOIN rejected: status %d\n", code);
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * ===========================================================
 * BENCHMARK MODES
 * ===========================================================
 */

/**
 * bench_synchronous - latency measurement.
 * 
 * Sends one packet, waits for it to arrive at the receiver, records the
 * round-trip time, then sends the next. This measures true end-to-end
 * latency through the engine's routing path.
 * 
 * Why synchronous for latency: if you send without waiting, you are measuring
 * throughput, not latency. The two are different metrics. Latency is what a
 * single message experiences. Throughput is how many messages the system
 * handles concurrently.
 * 
 * Memory: zero heap allocation in the measurement loop. All buffers are
 * stack-allocated and reused for every packet.
 */
static void bench_synchronous(int sender, int receiver)
{
    printf("\n-- Synchronous Mode (latency measurement) --\n");
    printf("Packets: %d | Payload: %d bytes\n", SYNC_COUNT, PAYLOAD_SIZE);
    printf("Each packet is confirmed before the next is sent.\n\n");

    // Build payload once, reuse for every packet
    char payload[PAYLOAD_SIZE];
    memset(payload, 'A', sizeof(payload));

    // Build header template once
    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = TYPE_APP_REALTIME;
    hdr.payload_len = htonl(PAYLOAD_SIZE);
    hdr.scope_id = htonl(BENCH_SCOPE);
    hdr.sender_id = htonl(0);
    hdr.expires_at = htobe64(0); // never expires

    // Receive buffer, reused for every packet
    char recv_buf[PAYLOAD_SIZE + 64];

    // Warmup, 200 packets, not measured
    printf("Warming up (1000 packets)...\n");
    for (int i = 0; i < 1000; i++) {
        send(sender, &hdr, sizeof(hdr), 0);
        send(sender, payload, PAYLOAD_SIZE, 0);
        recv_routed_packet(receiver, recv_buf, sizeof(recv_buf));
    }

    printf("Measuring...\n");

    uint64_t t_start = now_ns();
    uint64_t latency_ns = 0;
    int received = 0;
    int errors = 0;

    for (int i = 0; i < SYNC_COUNT; i++) {
        uint64_t t_send = now_ns();

        if (send(sender, &hdr, sizeof(hdr), 0) <= 0 ||
            send(sender, payload, PAYLOAD_SIZE, 0) <= 0)
        {
            errors++;
            continue;
        }

        int n = recv_routed_packet(receiver, recv_buf, sizeof(recv_buf));
        if (n > 0) {
            uint64_t t_recv = now_ns();
            received++;
            latency_ns += t_recv - t_send;
        } else {
            errors++;
        }
    }

    uint64_t t_end = now_ns();
    double elapsed = (double)(t_end - t_start) / 1e9;
    double pps = received > 0 ? received / elapsed : 0;
    double avg_us = received > 0 ? (double)latency_ns / received / 1000.0 :
        0.0;
    double p_loss = (double)(SYNC_COUNT - received) / SYNC_COUNT * 100.0;
    
    printf("\nSynchronous Results:\n");
    printf("    Sent:           %d packets\n", SYNC_COUNT);
    printf("    Received:       %d packets\n", received);
    printf("    Errors:         %d\n", errors);
    printf("    Loss:           %.2f%%\n", p_loss);
    printf("    Elapsed:        %.3f seconds\n", elapsed);
    printf("    Throughput:     %.0f packets/second\n", pps);
    printf("    Avg latency:    %.1f microseconds (round-trip)\n", avg_us);

    if (avg_us < 100.0)
        printf("    Rating:         EXCELLENT (< 100us)\n");
    else if (avg_us < 500.0)
        printf("    Rating:         GOOD (< 500us)\n");
    else {
        printf("    Rating:         INVESTIGATE "
            "(> 500us in Docker is expected)\n");
    }
}

/**
 * bench_burst - throughput measurement.
 * 
 * Sends packets in batches of BURST_SIZE without waiting for each one to be
 * received. The receiver counts packets separately. This exercises the
 * engine's pipeline, many packets in flight simultaneously through the event
 * loop.
 * 
 * Why burst for throughput: synchronous send-then-wait artificially limits
 * throughput to 1 / latency packets per second. Real clients send without
 * waiting. Burst mode reflects actual production load.
 * 
 * Memory: zero heap allocation in the measurement loop.
 */
static void bench_burst(int sender, int receiver)
{
    printf("\n-- Burst Mode (throughput measurement) --\n");
    printf("Packets: %d | Burst size: %d | Payload: %d bytes\n", BURST_COUNT,
        BURST_SIZE, PAYLOAD_SIZE);
    printf("Sender and receiver operate independently.\n\n");

    char payload[PAYLOAD_SIZE];
    memset(payload, 'B', sizeof(payload));

    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = TYPE_APP_REALTIME;
    hdr.payload_len = htonl(PAYLOAD_SIZE);
    hdr.scope_id = htonl(BENCH_SCOPE);
    hdr. sender_id = htonl(0);
    hdr.expires_at = htobe64(0);
    
    char recv_buf[PAYLOAD_SIZE + 64];

    /**
     * Set a short receive timeout for the drain loops.
     * After sending all packets, we wait up to RECV_TIMEOUT_S seconds
     * for the last packets to arrive. After that we stop counting.
     */
    struct timeval tv_short = {0, 50000};  // 50ms, fast drain
    struct timeval tv_long  = {RECV_TIMEOUT_S, 0};

    printf("Warming up (1000 packets)...\n");
    for (int i = 0; i < 1000; i++) {
        send(sender, &hdr, sizeof(hdr), 0);
        send(sender, payload, PAYLOAD_SIZE, 0);
        recv_routed_packet(receiver, recv_buf, sizeof(recv_buf));
    }

    // Switch receiver to short timeout for burst drain
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &tv_short, sizeof(tv_short));

    printf("Measuring...\n");

    uint64_t t_start = now_ns();
    int sent = 0;
    int recvd = 0;

    while (sent < BURST_COUNT) {
        // Send one burst
        int burst_this = BURST_SIZE;
        if (sent + burst_this > BURST_COUNT)
            burst_this = BURST_COUNT - sent;

        for (int i = 0; i < burst_this; i++) {
            if(send(sender, &hdr, sizeof(hdr), 0) > 0 &&
                send(sender, payload, PAYLOAD_SIZE, 0) > 0)
            {
                sent++;
            }
        }

        // Drain whatever arrived so far
        while (recv_routed_packet(receiver, recv_buf, sizeof(recv_buf)) > 0)
            recvd++;
    }

    /**
     * Final drain - wait for in-flight packets to arrive.
     * Switch to longer timeout for the final sweep.
     */
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &tv_long, sizeof(tv_long));
    while (recv_routed_packet(receiver, recv_buf, sizeof(recv_buf)) > 0)
        recvd++;

    // Restore original timeout
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO,
        &(struct timeval){RECV_TIMEOUT_S, 0}, sizeof(struct timeval));

    uint64_t t_end = now_ns();
    double elapsed = (double)(t_end - t_start) / 1e9;
    double pps = recvd > 0 ? recvd / elapsed: 0;
    double p_loss = (double)(sent - recvd) / sent * 100.0;

    printf("\nBurst Results:\n");
    printf("    Sent:       %d packets\n", sent);
    printf("    Received:   %d packets\n", recvd);
    printf("    Lost:       %d (%.2f%%)\n", sent - recvd, p_loss);
    printf("    Elapsed:    %.3f seconds\n", elapsed);
    printf("    Throughput: %.0f packets/seconds\n", pps);

    if (pps >= 200000)
        printf("    Rating:     EXCELLENT (>200k pps)\n");
    else if (pps >= 50000)
        printf("    Rating:     GOOD (>50k pps)\n");
    else
        printf("    Rating:     INVESTIGATE\n");
}

/**
 * ===========================================================
 * MEMORY EFFICIENCY CHECK
 * ===========================================================
 */

/**
 * check_memory - read /proc/self/status to report RSS before and after.
 * 
 * Must verify it does not leak memory during the measurement loop. RSS
 * growing significantly between start and end indicates allocations on the
 * hot path that should not exist.
 */
static long read_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;

    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, " %ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

int main(void)
{
    printf("=== C State Bus Routing Benchmark ===\n");
    printf("Payload: %d bytes | Scope: %d\n\n", PAYLOAD_SIZE, BENCH_SCOPE);
    printf("Start server with: ENGINE_WORKER_COUNT=1 ./server_opt &\n\n");

    const char *port_env = getenv("ENGINE_PORT");
    if (port_env && *port_env) server_port = atoi(port_env);
    printf("Target: %s:%d\n", SERVER_HOST, server_port);

    long rss_start = read_rss_kb();

    int sender = connect_and_setup("bench-sender");
    int receiver = connect_and_setup("bench-receiver");

    if (sender == -1 || receiver == -1) {
        fprintf(stderr,
            "\nFATAL: Cannot connect.\n"
            "1. Is the server running? ENGINE_WORKER_COUNT=1 ./server_opt &\n"
            "2. Wait 1 second after starting before running the benchmark.\n"
            "3. Verify port %d is not in use by another process.\n",
            server_port);
        if (sender != -1) close(sender);
        if (receiver != -1) close(receiver);
        return 1;
    }
    
    printf("Connected: sender fd=%d receiver fd=%d\n", sender, receiver);
    printf("Both clients authenticated and joined scope %d\n\n", BENCH_SCOPE);

    bench_synchronous(sender, receiver);
    bench_burst(sender, receiver);

    long rss_end = read_rss_kb();

    printf("\n-- Memory Check --\n");
    if (rss_start > 0 && rss_end > 0) {
        printf("    RSS at start:   %ld KB\n", rss_start);
        printf("    RSS at end:     %ld KB\n", rss_end);
        printf("    Delta:          %ld KB\n", rss_end - rss_start);
        if (rss_end - rss_start < 512)
            printf("    Memory:         PASS (no significant growth)\n");
        else {
            printf("    Memory:         INVESTIGATE "
                "(RSS grew > 512KB during benchmark)\n");
        }
    } else {
        printf("    Memory check unavailable (non-Linux platform)\n");
    }

    close(sender);
    close(receiver);

    printf("\n=== Benchmark Complete ===\n");
    printf("Record results in bench/RESULTS.md\n");
    return 0;
}