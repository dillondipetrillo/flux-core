#include <arpa/inet.h>
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "protocol.h"
#include "test_runner.h"

/**
 * test_shutdown_integration.c - Standalone graceful shutdown test.
 * 
 * This is deliberately NOT part of the test_integration.c. It requires an
 * external signal to be sent to the server container at a specific
 * moment mid-test - test_integration.c's other tests are independent and
 * self-contained, and forcing timing-dependent signal orchestration into
 * that file would make every other test in it fragile to change.
 * 
 * This binary does NOT send the SIGTERM itself. It:
 *      1. Connects and authenticates a client
 *      2. Joins a scope
 *      3. Writes a sentinel file (/tmp/shutdown_test_ready) to signal the
 *         orchestrating shell script that it is now blocked in recv(),
 *         waiting for a TYPE_SYS_SHUTDOWN packet
 *      4. Blocks in recv() until either a packet arrives or the socket times
 *         out
 * 
 * The orchestrating script (scripts/run_shutdown_test.sh) is responsible
 * for watching for that sentinel file, then sending SIGTERM to the container
 * at the right moment.
 * 
 * Run via scripts/run_shutdown_test.sh - do not run this binary directly
 * without that orchestration, since nothing will ever send the SIGTERM and
 * the test will simply time out and fail.
 */

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 8080
#define TEST_TOKEN "shutdown-test-token"
#define RECV_TIMEOUT_S 15   // generous - gives the orchestrating script
                            // time to detect the sentinel and send SIGTERM

static const char *get_sentinel_path(void)
{
    const char *dir = getenv("SENTINEL_DIR");
    static char path[512];
    if (dir && *dir) {
        snprintf(path, sizeof(path), "%s/shutdown_test_ready", dir);
    } else {
        snprintf(path, sizeof(path), "/tmp/shutdown_test_ready");
    }
    return path;
}

static void set_recv_timeout(int fd)
{
    struct timeval tv = {RECV_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int connect_client(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(fd);
        return -1;
    }
    
    set_recv_timeout(fd);
    return fd;
}

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

    if (send(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) return 0;
    if (payload_len > 0) {
        if (send(fd, payload, payload_len, 0) != (ssize_t)payload_len)
            return 0;
    }
    return 1;
}

static int recv_response(int fd)
{
    struct packet_header hdr;
    ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n != (ssize_t)sizeof(hdr)) return -1;

    uint32_t payload_len = ntohl(hdr.payload_len);
    if (payload_len == 0) return (int)hdr.type;
    if (payload_len > sizeof(struct response_payload)) return -1;

    struct response_payload rp;
    n = recv(fd, &rp, sizeof(rp), MSG_WAITALL);
    if (n != (ssize_t)sizeof(rp)) return -1;

    return (int)ntohl(rp.status_code);
}

static int authenticate(int fd)
{
    send_packet(fd, TYPE_SYS_IDENTIFY, 0, TEST_TOKEN,
        (uint32_t)strlen(TEST_TOKEN), 0);
    int code = recv_response(fd);
    return code == STATUS_OK;
}

int main(void)
{
    printf("=== Graceful shutdown integration test ===\n");
    printf("Requires orchestration - run via "
        "scripts/run_shutdown_test.sh, not directly.\n\n");

    int fd = connect_client();
    ASSERT(fd != -1, "can connect to server");
    if (fd == -1) { TEST_SUMMARY(); }

    ASSERT(authenticate(fd), "authenticated");

    send_packet(fd, TYPE_SYS_JOIN, 800, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_OK, "joined scope 800");

    /**
     * Write the sentinel file. The orchestrating shell script polls for
     * this files existence and sends SIGTERM to the container the moment
     * it appears - guaranteeing the signal is sent only after this process
     * is actually blocked in recv() below, not before.
     */
    FILE *sentinel = fopen(get_sentinel_path(), "w");
    if (sentinel) {
        fprintf(sentinel, "ready\n");
        fclose(sentinel);
    } else {
        printf("WARNING: could not write sentinel file %s\n",
            get_sentinel_path());
    }

    printf("Waiting up to %d seconds for TYPE_SYS_SHUTDOWN...\n",
        RECV_TIMEOUT_S);

    struct packet_header hdr;
    ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);

    ASSERT(n == (ssize_t)sizeof(hdr),
        "received a packet before disconnect (did not time out)");
    ASSERT(n == (ssize_t)sizeof(hdr) && hdr.type == TYPE_SYS_SHUTDOWN,
        "received TYPE_SYS_SHUTDOWN notification, not a slient/abrupt "
        "disconnect");

    close(fd);
    remove(get_sentinel_path());

    TEST_SUMMARY();
}