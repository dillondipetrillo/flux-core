#ifndef AUTH_HOOK
#define AUTH_HOOK

#include <stddef.h>
#include <stdint.h>

/**
 * auth_hook.h - Pluggable authentication for the C State Bus
 * 
 * Three modes, selected automatically based on environment variables:
 * 
 * Mode 1 - JWT validation (ENGINE_AUTH_JWT_SECRET is set)
 *  The engine validates the token locally using HMAC-SHA256.
 *  No network call. Microsecond validation.
 *  Token format: standard JWT with "sub" (user ID) and "exp" claims.
 * 
 * Mode 2 - HTTP callback (ENGINE_AUTH_HTTP_URL is set)
 *  The engine POSTs {"token": "<base64>"} to your existing server.
 *  Your server responds {"valid": true/false, "user_id": N}.
 *  Write one endpoint endpoint in whatever language you already use.
 * 
 * Mode 3 - Development mode (neither set)
 *  Accepts all tokens. Logs WARNING on every call.
 *  Never use in production.
 * 
 * Priority: if both JWT and HTTP are configured, JWT takes priority because
 * it requires no network round trip and is significantly faster.
 * 
 * -- HTTP Callback Protocol -----------------------------------------------
 * 
 * Request (engine -> your server):
 *  POST <ENGINE_AUTH_HTTP_URL>
 *  Content-Type: application/json
 *  {"token": "<base64url-encoded token bytes>"}
 * 
 * Response (your server -> engine):
 *  HTTP 200 OK
 *  Content-Type: application/json
 *  {"valid": true, "user_id": 42}      <- on success
 *  {"valid": false, "user_id": 0}      <- on rejection
 * 
 * Any non-200 status is treated as rejection.
 * Timeout is configurable via ENGINE_AUTH_HTTP_TIMEOUT_MS (default 500).
 * 
 * -- JWT Token Format ----------------------------------------------------
 * 
 * Standard JWT signed with HMAC-SHA256 (algorithm "HS256").
 * Required claims:
 *  "sub" - user ID as a positive integer string, e.g. "42"
 *  "exp" - expiry as Unix timestamp
 * 
 * Example payload: {"sub": "42", "exp": 1718000000}
 */

struct engine_config; // Forward declaration to avoid circular dependency

struct auth_request {
    const char *token;  // raw token bytes from client
    size_t token_len;   // length of token in bytes
    int conn_fd;        // file descriptor of connecting client
};

struct auth_result {
    int valid;          // 1= authenticated, 0 = rejected
    uint32_t user_id;   // opaque user id, engine stores but doesn't read
};

// Function pointer type, engine calls this, never knowing the implementation
typedef struct auth_result (*auth_hook_fn)(struct auth_request request);

/**
 * Development hook.
 * Default implementation - accepts any token, assigns user_id = conn_fd
 * Use this during development and testing only
 */
struct auth_result default_auth_hook(struct auth_request request);

// JWT validation - validates HMAC-SHA256 signed JWT locally
struct auth_result jwt_auth_hook(struct auth_request request);

// HTTP callback - POSTs to ENGINE_AUTH_HTTP_URL on users server
struct auth_result http_auth_hook(struct auth_request request);

/**
 * Examine configuration and return the correct hook. Called once in main.c.
 * Logs which mode is active.
 */
auth_hook_fn select_auth_hook(const struct engine_config *config);

#endif