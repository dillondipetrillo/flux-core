#include <arpa/inet.h>
#include <endian.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "protocol.h"
#include "test_runner.h"

/**
 * test_integration.c - End-to-end integration tests for the C State Bus.
 * 
 * Requires a running server:
 *      ENGINE_WORKER_COUNT=1 ./server &
 * 
 * These tests connect real TCP clients, send real binary packets, and verify
 * real responses. They test the entire system together - epoll, auth hook,
 * packet parsing, routing, TTL, and disconnect.
 * 
 * Run with:
 *      make integration
 *      ./tests/run_integration
 */

 #define SERVER_HOST "127.0.0.1"
 #define SERVER_PORT 8080
 #define TEST_TOKEN "test-integration-token"

 /**
  * connect_client - establish a TCP connection to the server.
  * Returns the socket fd, or -1 on failure.
  */
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
    return fd;
}

/**
 * send_packet - build and send a complete packet.
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
    hdr.sender_id = htonl(0); // server stamps this
    hdr.expires_at = htobe64(expires_at);

    if (send(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) return 0;
    if (payload_len > 0) {
        if (send(fd, payload, payload_len, 0) != (ssize_t)payload_len)
            return 0;
    }
    return 1;
}

/**
 * recv_response - receive a header + response_payload (ACK or ERROR).
 * Returns the status code, or -1 on failure.
 */
static int recv_response(int fd)
{
    struct packet_header hdr;
    ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n != sizeof(hdr)) return -1;

    uint32_t payload_len = ntohl(hdr.payload_len);
    if (payload_len == 0) return (int)hdr.type;

    struct response_payload rp;
    n = recv(fd, &rp, sizeof(rp), MSG_WAITALL);
    if (n != sizeof(rp)) return -1;

    return (int)ntohl(rp.status_code);
}

/**
 * recv_app_packet - receive a full application data packet.
 * Fills buf with the payload. Returns payload length, or -1.
 */
static int recv_app_packet(int fd, char *buf, size_t buf_size,
    uint32_t *out_scope, uint32_t *out_sender)
{
    struct packet_header hdr;
    ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n != sizeof(hdr)) return -1;

    uint32_t payload_len = ntohl(hdr.payload_len);
    if (out_scope) *out_scope = ntohl(hdr.scope_id);
    if (out_sender) *out_sender = ntohl(hdr.sender_id);

    if (payload_len == 0) return 0;
    if (payload_len >= buf_size) return -1;

    n = recv(fd, buf, payload_len, MSG_WAITALL);
    if (n != (ssize_t)payload_len) return -1;

    buf[payload_len] = '\0';
    return (int)payload_len;
}

/**
 * authenticate - send IDENTITY and verify ACK.
 * Returns 1 on success.
 */
static int authenticate(int fd)
{
    send_packet(fd, TYPE_SYS_IDENTIFY, 0, TEST_TOKEN,
        (uint32_t)strlen(TEST_TOKEN), 0);
    int code = recv_response(fd);
    return code == STATUS_OK;
}

/**
 * ===========================================================
 * TEST CASES
 * ===========================================================
 */

static void test_connect_and_authenticate(void)
{
    printf("\n-- connect and authenticate --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect to server");

    send_packet(fd, TYPE_SYS_IDENTIFY, 0, TEST_TOKEN,
        (uint32_t)strlen(TEST_TOKEN), 0);
    int code = recv_response(fd);
    ASSERT(code == STATUS_OK, "IDENTIFY returns STATUS_OK");
    close(fd);
}

static void test_double_identify_rejected(void)
{
    printf("\n-- double IDENTIFY rejected --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect");
    ASSERT(authenticate(fd), "first IDENTIFY succeeds");

    // Second IDENTIFY must fail
    send_packet(fd, TYPE_SYS_IDENTIFY, 0, TEST_TOKEN,
        (uint32_t)strlen(TEST_TOKEN), 0);
    int code = recv_response(fd);
    ASSERT(code == STATUS_ERR_ALREADY_ID,
        "second IDENTIFY returns STATUS_ERR_ALREADY_ID");
    close(fd);
}

static void test_unauthenticated_packet_rejected(void)
{
    printf("\n-- unauthenticated packet rejected --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect");

    // Send JOIN without authenticating first
    send_packet(fd, TYPE_SYS_JOIN, 42, NULL, 0, 0);
    int code = recv_response(fd);
    ASSERT(code == STATUS_ERR_UNIDENTIFIED,
        "JOIN without auth returns STATUS_ERR_UNIDENTIFIED");
    close(fd);
}

static void test_join_and_leave(void)
{
    printf("\n-- join and leave scope --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect");
    ASSERT(authenticate(fd), "authenticated");

    send_packet(fd, TYPE_SYS_JOIN, 100, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_OK, "JOIN scope 100 succeeds");

    send_packet(fd, TYPE_SYS_LEAVE, 100, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_OK, "LEAVE scope 100 succeeds");

    // LEAVE again - not in room
    send_packet(fd, TYPE_SYS_LEAVE, 100, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_ERR_NOT_IN_ROOM,
        "second LEAVE returns STATUS_ERR_NOT_IN_ROOM");
    close(fd);
}

static void test_double_join_rejected(void)
{
    printf("\n-- double join rejected --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect");
    ASSERT(authenticate(fd), "authenticated");

    send_packet(fd, TYPE_SYS_JOIN, 200, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_OK, "first JOIN succeeds");

    send_packet(fd, TYPE_SYS_JOIN, 200, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_ERR_ALREADY_IN_ROOM,
        "second JOIN returns STATUS_ERR_ALREADY_IN_ROOM");
    close(fd);
}

static void test_message_routing(void)
{
    printf("\n-- message routing between two clients --\n");
    int sender = connect_client();
    int receiver = connect_client();
    ASSERT(sender != -1, "sender can connect");
    ASSERT(receiver != -1, "receiver can connect");
    ASSERT(authenticate(sender), "sender authenticated");
    ASSERT(authenticate(receiver), "receiver authenticated");

    send_packet(sender, TYPE_SYS_JOIN, 300, NULL, 0, 0);
    ASSERT(recv_response(sender) == STATUS_OK, "sender joined scope 300");
    send_packet(receiver, TYPE_SYS_JOIN, 300, NULL, 0, 0);
    ASSERT(recv_response(receiver) == STATUS_OK, "receiver joined scope 300");

    const char *msg = "hello from integration test";
    time_t exp = time(NULL) + 300;
    send_packet(sender, TYPE_APP_REALTIME, 300, msg, (uint32_t)strlen(msg),
        (uint64_t)exp);

    char buf[1024];
    uint32_t scope = 0;
    uint32_t sender_id = 0;
    int len = recv_app_packet(receiver, buf, sizeof(buf), &scope, &sender_id);

    ASSERT(len > 0, "receiver got a packet");
    ASSERT(scope == 300, "packet arrived in correct scope");
    ASSERT(strcmp(buf, msg) == 0, "payload matches what was sent");

    close(sender);
    close(receiver);
}

static void test_sender_does_not_receive_own_message(void)
{
    printf("\n-- sender does not receive own message --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect");
    ASSERT(authenticate(fd), "authenticated");

    send_packet(fd, TYPE_SYS_JOIN, 400, NULL, 0, 0);
    ASSERT(recv_response(fd) == STATUS_OK, "joined scope 400");

    const char *msg = "self-send test";
    send_packet(fd, TYPE_APP_REALTIME, 400, msg, (uint32_t)strlen(msg),
        (uint64_t)(time(NULL) + 60));

    /**
     * Set a short receive timeout so we do not block forever waiting for a
     * message that should NOT arrive.
     */
    struct timeval tv = {1, 0}; // 1 second timeout
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[1024];
    int n = recv(fd, buf, sizeof(buf), 0);
    ASSERT(n <= 0, "sender did not receive its own message");

    close(fd);
}

static void test_expired_packet_dropped(void)
{
    printf("\n-- expired packet is dropped --\n");
    int sender = connect_client();
    int receiver = connect_client();
    ASSERT(sender != -1, "sender can connect");
    ASSERT(receiver != -1, "receiver can connect");
    ASSERT(authenticate(sender), "sender authenticated");
    ASSERT(authenticate(receiver), "receiver authenticated");

    send_packet(sender, TYPE_SYS_JOIN, 500, NULL, 0, 0);
    ASSERT(recv_response(sender) == STATUS_OK, "sender joined 500");
    send_packet(receiver, TYPE_SYS_JOIN, 500, NULL, 0, 0);
    ASSERT(recv_response(receiver) == STATUS_OK, "receiver joined 500");

    // Send with expires_at 1 second in the past
    uint64_t past = (uint64_t)(time(NULL) - 1);
    send_packet(sender, TYPE_APP_REALTIME, 500, "expired", 7, past);

    // Sender should receive STATUS_ERR_EXPIRED
    int code = recv_response(sender);
    ASSERT(code == STATUS_ERR_EXPIRED,
        "expired packet returns STATUS_ERR_EXPIRED to sender");

    // Receiver should NOT receive the packet
    struct timeval tv = {1, 0};
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[1024];
    int n = recv(receiver, buf, sizeof(buf), 0);
    ASSERT(n <= 0, "receiver did not get expired packet");

    close(sender);
    close(receiver);
}

static void test_ping(void)
{
    printf("\n-- ping response --\n");
    int fd = connect_client();
    ASSERT(fd != -1, "can connect");
    ASSERT(authenticate(fd), "authenticated");

    send_packet(fd, TYPE_SYS_PING, 0, NULL, 0, 0);

    struct packet_header hdr;
    ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    ASSERT(n == sizeof(hdr), "received ping response header");
    ASSERT(hdr.type == TYPE_SYS_PING, "response type is TYPE_SYS_PING");

    close(fd);
}

static void test_leave_stops_routing(void)
{
    printf("\n-- leave stops message routing --\n");
    int sender = connect_client();
    int receiver = connect_client();
    ASSERT(sender != -1, "sender connected");
    ASSERT(receiver != -1, "receiver connected");
    ASSERT(authenticate(sender), "sender auth");
    ASSERT(authenticate(receiver), "receiver auth");

    send_packet(sender, TYPE_SYS_JOIN, 600, NULL, 0, 0);
    ASSERT(recv_response(sender) == STATUS_OK, "sender joined 600");
    send_packet(receiver, TYPE_SYS_JOIN, 600, NULL, 0, 0);
    ASSERT(recv_response(receiver) == STATUS_OK, "receiver joined 600");

    // Receiver leaves
    send_packet(receiver, TYPE_SYS_LEAVE, 600, NULL, 0, 0);
    ASSERT(recv_response(receiver) == STATUS_OK, "receiver left 600");

    // Sender sends a message - receiver must not get it
    send_packet(sender, TYPE_APP_REALTIME, 600, "after leave", 11,
        (uint64_t)(time(NULL) + 60));

    struct timeval tv = {1, 0};
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[1024];
    int n = recv(receiver, buf, sizeof(buf), 0);
    ASSERT(n <= 0, "receiver did not get message after leaving");

    close(sender);
    close(receiver);
}

int main(void)
{
    printf("=== Integration tests (requires running server) ===\n");
    printf("Start with: ENGINE_WORKER_COUNT=1 ./server &\n\n");

    test_connect_and_authenticate();
    test_double_identify_rejected();
    test_unauthenticated_packet_rejected();
    test_join_and_leave();
    test_double_join_rejected();
    test_message_routing();
    test_sender_does_not_receive_own_message();
    test_expired_packet_dropped();
    test_ping();
    test_leave_stops_routing();

    TEST_SUMMARY();
}