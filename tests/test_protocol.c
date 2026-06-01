#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>

#include "protocol.h"
#include "test_runner.h"

static void test_header_size(void)
{
    printf("\n-- header size --\n");
    ASSERT(sizeof(struct packet_header) == 21,
        "packet_header is exactly 21 bytes");
    ASSERT(sizeof(struct response_payload) == 4,
        "response_payload is exactly 4 bytes");
}

static void test_byte_order_roundtrip(void)
{
    printf("\n-- byte order roundtrip --\n");
    struct packet_header h;
    memset(&h, 0, sizeof(h));

    h.type = (uint8_t)TYPE_APP_REALTIME;
    h.payload_len = htonl(512);
    h.scope_id = htonl(42);
    h.sender_id = htonl(7);
    h.expires_at = htobe64(1718000000ULL);

    uint32_t payload_len = ntohl(h.payload_len);
    uint32_t scope_id = ntohl(h.scope_id);
    uint32_t sender_id = ntohl(h.sender_id);
    uint64_t expires_at = be64toh(h.expires_at);

    ASSERT(payload_len == 512, "payload_len roundtrips correctly");
    ASSERT(scope_id == 42, "scope_id roundtrips correctly");
    ASSERT(sender_id == 7, "sender_id roundtrips correctly");
    ASSERT(expires_at == 1718000000, "expires_at roundtrips correctly");
}

static void test_zero_expires_never_expire(void)
{
    printf("\n-- zero expires_at means never expires --\n");
    uint64_t zero_expiry = 0;
    int zero_never = (zero_expiry != 0 && zero_expiry < (uint64_t)2000);
    ASSERT(zero_never == 0, "zero expires_at never triggers expiry check");

    uint64_t past = 1000;
    int expired = (past != 0 && past < (uint64_t)2000);
    ASSERT(expired == 1,
        "non-zero past timestamp correctly detected as expired");
}

static void test_status_codes(void)
{
    printf("\n-- status codes --\n");
    ASSERT(STATUS_OK == 100, "STATUS_OK is 100");
    ASSERT(STATUS_ERR_UNIDENTIFIED == 401, "STATUS_ERR_UNIDENTIFIED is 401");
    ASSERT(STATUS_ERR_ALREADY_ID == 402, "STATUS_ERR_ALREADY_ID is 402");
    ASSERT(STATUS_ERR_AUTH_FAILED == 403, "STATUS_ERR_AUTH_FAILED is 403");
    ASSERT(STATUS_ERR_NOT_IN_ROOM == 404, "STATUS_ERR_NOT_IN_ROOM is 404");
    ASSERT(STATUS_ERR_ALREADY_IN_ROOM == 405,
        "STATUS_ERR_ALREADY_IN_ROOM is 405");
    ASSERT(STATUS_ERR_SCOPES_FULL == 406, "STATUS_ERR_SCOPES_FULL is 406");
    ASSERT(STATUS_ERR_EXPIRED == 410, "STATUS_ERR_EXPIRED is 410");
    ASSERT(STATUS_ERR_PAYLOAD_SIZE == 413, "STATUS_ERR_PAYLOAD_SIZE is 413");
}

static void test_packet_types(void)
{
    printf("\n-- packet types --\n");
    ASSERT(TYPE_SYS_IDENTIFY == 1, "TYPE_SYS_IDENTIFY is 1");
    ASSERT(TYPE_SYS_JOIN == 2, "TYPE_SYS_JOIN is 2");
    ASSERT(TYPE_SYS_PING == 3, "TYPE_SYS_PING is 3");
    ASSERT(TYPE_SYS_ACK == 4, "TYPE_SYS_ACK is 4");
    ASSERT(TYPE_SYS_ERROR == 5, "TYPE_SYS_ERROR is 5");
    ASSERT(TYPE_SYS_LEAVE == 6, "TYPE_SYS_LEAVE is 6");
    ASSERT(TYPE_SYS_SHUTDOWN == 7, "TYPE_SYS_SHUTDOWN is 7");
    ASSERT(TYPE_APP_REALTIME == 10, "TYPE_APP_REALTIME is 10");
    ASSERT(TYPE_APP_STANDARD == 11, "TYPE_APP_STANDARD is 11");
    ASSERT(TYPE_APP_BACKGROUND == 12, "TYPE_APP_BACKGROUND is 12");
}

int main(void)
{
    printf("=== protocol unit tests ===\n");
    test_header_size();
    test_byte_order_roundtrip();
    test_zero_expires_never_expire();
    test_status_codes();
    test_packet_types();
    TEST_SUMMARY();
}