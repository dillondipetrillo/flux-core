#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "test_runner.h"

/**
 * test_ttl.c - Unit tests for packet TTL expiry logic.
 * 
 * The TTL check in dispatch_packet is:
 *      if (expires_at != 0 && expires_at < (uint64_t)time(NULL))
 *          -> packet is expired, send STATUS_ERR_EXPIRED
 * 
 * Tests all boundary conditions and realistic use cases.
 */

// Replicate the exact check from dispatch_packet
static int is_expired(uint64_t expires_at, time_t now)
{
    return (expires_at != 0 && expires_at < (uint64_t)now);
}

static void test_zero_never_expires(void)
{
    printf("\n-- expires_at=0 means never expires --\n");
    /**
     * expires_at=0 is the sentinel for "no expiry."
     * The check explicitly excludes 0 with the != 0 condition.
     * This must hold true regardless of what time(NULL) returns.
     */
    time_t now = time(NULL);
    ASSERT(is_expired(0, now) == 0,
        "expires_at=0 is never expired (even in the past)");
    ASSERT(is_expired(0, now + 999999) == 0,
        "expires_at=0 is never expired (far future now)");
    ASSERT(is_expired(0, 1) == 0,
        "expires_at=0 is never expired (now=1, earliest valid time)");
}

static void test_past_timestamp_is_expired(void)
{
    printf("\n-- past timestamp is expired --\n");
    time_t now = time(NULL);
    ASSERT(is_expired((uint64_t)(now - 1), now) == 1,
        "expires_at one second ago is expired");
    ASSERT(is_expired((uint64_t)(now - 300), now) == 1,
        "expires_at 5 minutes ago is expired");
    ASSERT(is_expired(1, now) == 1,
        "expires_at=1 (Unix epoch 1970) is expired");
}

static void test_future_timestamp_is_not_expired(void)
{
    printf("\n-- future timestamp is not expired --\n");
    time_t now = time(NULL);
    ASSERT(is_expired((uint64_t)(now + 1), now) == 0,
        "expires_at one second in future is not expired");
    ASSERT(is_expired((uint64_t)(now + 300), now) == 0,
        "expires_at 5 minutes in future is not expired");
    ASSERT(is_expired((uint64_t)(now + 86400), now) == 0,
        "expires_at 24 hours in future is not expired");
}

static void test_exact_boundary(void)
{
    printf("\n-- exact boundary: expires_at == now --\n");
    /**
     * When expires_at equals now exactly, the check is:
     *      expires_at < now -> false (equal, not less than)
     * So a packet that expires exactly NOW is not yet expired.
     * This is intentional - the packet was valid when sent and
     * we give it the benefit of the doubt at the exact boundary.
     */
    time_t now = time(NULL);
    ASSERT(is_expired((uint64_t)now, now) == 0,
        "expires_at == now is not yet expired (boundary is exclusive)");
}

static void test_typing_indicator_ttl(void)
{
    printf("\n-- realistic TTL: 3-second typing indicator --\n");
    /**
     * Typing indicators in a messaging system will use a 3-second TTL.
     * A packet sent 4 seconds ago with a 3-second TTL must be dropped.
     * A packet sent 2 seconds ago with a 3-second TTL must be routed.
     */
    time_t now = time(NULL);
    time_t sent_4s = now - 4;
    time_t sent_2s = now - 2;
    uint64_t ttl_3s = 3;

    uint64_t expires_old = (uint64_t)(sent_4s + ttl_3s);
    uint64_t expires_new = (uint64_t)(sent_2s + ttl_3s);

    ASSERT(is_expired(expires_old, now) == 1,
        "typing indicator sent 4s ago with 3s TTL is expired");
    ASSERT(is_expired(expires_new, now) == 0,
        "typing indicator sent 2s ago with 3s TTL is not expired");
}

static void test_message_ttl(void)
{
    printf("\n-- realistic TTL: 300-second message TTL --\n");
    /**
     * Messages use expires_in=300 seconds (5 minutes) by default.
     * This is what the SDK sends for TYPE_APP_REALTIME.
     */
    time_t now = time(NULL);
    ASSERT(is_expired((uint64_t)(now + 300), now) == 0,
        "message with 300s TTL from now is not expired");
    ASSERT(is_expired((uint64_t)(now - 1), now) == 1,
        "message that expired 1 second ago is dropped");
}

static void test_max_uint64_not_expired(void)
{
    printf("\n-- UINT64_MAX is not expired --\n");
    /**
     * A timestamp of UINT64_MAX is billions of years in the future.
     * Verify the comparison handles large values without overflow.
     */
    time_t now = time(NULL);
    ASSERT(is_expired(UINT64_MAX, now) == 0,
        "UINT64_MAX expires_at is not expired");
}

int main(void)
{
    printf("=== TTL expiry unit tests ===\n");
    test_zero_never_expires();
    test_past_timestamp_is_expired();
    test_future_timestamp_is_not_expired();
    test_exact_boundary();
    test_typing_indicator_ttl();
    test_message_ttl();
    test_max_uint64_not_expired();
    TEST_SUMMARY();
}