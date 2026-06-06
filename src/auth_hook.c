#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth_hook.h"
#include "config.h"
#include "logger.h"

// OpenSSL for HMAC-SHA256
#include <openssl/hmac.h>
#include <openssl/sha.h>

// libcurl for HTTP callback
#include <curl/curl.h>

/**
 * ===========================================================
 * SECTION 1: DEVELOPMENT HOOK
 * ===========================================================
 */

 struct auth_result default_auth_hook(struct auth_request request)
 {
    log_error("!!! DEFAULT AUTH HOOK - ACCEPTS ALL TOKENS !!!");
    log_error("!!! SET ENGINE_AUTH_JWT_SECRET or ENGINE_AUTH_HTTP_URL !!!");
    log_error("!!! DEVELOPMENT MODE ONLY - DO NOT USE IN PRODUCTION !!!");

    struct auth_result result;
    result.valid = 1;
    result.user_id = (uint32_t)request.conn_fd;
    return result;
 }

 /**
  * ===========================================================
  * SECTION 2: CRYPTOGRAPHIC UTILITIES
  * 
  * We use OpenSSL's HMAC implementation rather than rolling our own.
  * We write our own base64url decode and JSON extraction because those
  * are straight forward string operations with no cryptographic subtlety.
  * ===========================================================
  */

/**
 * Compute HMAC-SHA256 using OpenSSL.
 * Output must be 32 bytes (SHA-256 produces a 32-byte digest).
 * 
 * HMAC_Result is a thin wrapper - OpenSSL handles all the complexity:
 * key padding, ipad/opad construction, and two-pass SHA-256.
 */
static int compute_hmac_sha256(const uint8_t *key, size_t klen,
    const uint8_t *msg, size_t mlen, uint8_t *output)
{
    unsigned int outlen = 0;
    unsigned char *result = HMAC(EVP_sha256(), key, (int)klen, msg, (int)mlen, 
        output, &outlen);
    return (result != NULL && outlen == 32) ? 1 : 0;
}

/**
 * Constantr-time byte comparison - prevents timing attacks.
 * 
 * Regular strcmp() returns early when it finds a mismatch, leaking
 * information about where in the string the secret value differs. By always
 * comparing all bytes regardless of early mismatch, this function takes the
 * same time for any two inputs of equal length.
 * 
 * Returns 1 if equal, 0 if not.
 */
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

/**
 * Base64url decode.
 * 
 * Returns number of decoded bytes, or -1 on invalid input.
 */
static int b64url_decode(const char *src, size_t slen, uint8_t *dest,
    size_t dsize)
{
    size_t out = 0;
    uint32_t buf = 0;
    int bits = 0;

    for (size_t i = 0; i < slen; i++) {
        char c = src[i];
        int v;

        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break; // optional padding
        else return -1;

        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= dsize) return -1;
            dest[out++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return (int)out;
}

/**
 * Extract a long integer value from a JSON key.
 * 
 * Handles: {"sub": "42"} and {"sub": 42} (quoted and unquoted).
 * Returns -1 if the key is not found.
 */
static long json_get_long(const char *json, const char *key)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    // skip whitespace, colon, and optional quote
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    if (!*p) return -1;
    return strtol(p, NULL, 10);
}

/**
 * ===========================================================
 * SECTION 3: JWT VALIDATION HOOK
 * 
 * Validates a JWT with algorithm HS256 (HMAC-SHA256).
 * 
 * Use OpenSSL for computing HMAC-SHA256, everything else is straightforward
 * string parsing work.
 * ===========================================================
 */

struct auth_result jwt_auth_hook(struct auth_request request)
{
    struct auth_result fail = {0, 0};

    const char *secret = getenv("ENGINE_AUTH_JWT_SECRET");
    if (!secret || !*secret) {
        log_error("jwt_auth_hook: ENGINE_AUTH_JWT_SECRET not set");
        return fail;
    }

    if (request.token_len == 0 || request.token_len > 8192)
        return fail;

    // Use a mutable null-terminated copy
    char token_cpy[8193];
    memcpy(&token_cpy, request.token, request.token_len);
    token_cpy[request.token_len] = '\0';

    // Find the two dots that separate JWT parts
    char *dot1 = strchr(token_cpy, '.');
    if (!dot1) {
        log_error("jwt: missing first dot fd=%d", request.conn_fd);
        return fail;
    }
    char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        log_error("jwt: missing second dot fd=%d", request.conn_fd);
        return fail;
    }

    /**
     * The signing input is everything before the second dot:
     * "<header_b64url>.<payload_b64url>"
     * This is the exact byte sequence that was signed when the JWT was
     * issued. We need to compute HMAC over this exactly.
     */
    size_t signing_len = (size_t)(dot2 - token_cpy);
    uint8_t computed[32];
    if (!compute_hmac_sha256((const uint8_t *)secret, strlen(secret),
        (const uint8_t *)token_cpy, signing_len, computed))
    {
        log_error("jwt: HMAC computation failed fd=%d", request.conn_fd);
        return fail;
    }

    // Decode the provided signature (part after the second dot)
    char *sig_b64 = dot2 + 1;
    uint8_t provided[64];
    int sig_len = b64url_decode(sig_b64, strlen(sig_b64), provided,
        sizeof(provided));

    if (sig_len != 32) {
        log_error("jwt: invalid signature length %d fd=%d", sig_len,
            request.conn_fd);
        return fail;
    }

    // Compare computed vs provided - constant-time
    if (!ct_equal(computed, provided, 32)) {
        log_error("jwt: signature verification failed fd=%d", request.conn_fd);
        return fail;
    }

    // Decode the payload (between the two dots)
    *dot2 = '\0';
    char *payload_b64 = dot1 + 1;
    uint8_t payload_bytes[4096];
    int plen = b64url_decode(payload_b64, strlen(payload_b64), payload_bytes,
        sizeof(payload_bytes) - 1);
    if (plen <= 0) {
        log_error("jwt: payload decode failed fd=%d", request.conn_fd);
        return fail;
    }
    payload_bytes[plen] = '\0';
    char *payload = (char *)payload_bytes;

    // Check expiry - "exp" claim is Unix timestamp
    long exp = json_get_long(payload, "exp");
    if (exp > 0 && (time_t)exp < time(NULL)) {
        log_error("jwt: token expired exp=%ld now=%ld fd=%d", exp,
            (long)time(NULL), request.conn_fd);
        return fail;
    }

    // Extract user_id from "sub" claim
    long sub = json_get_long(payload, "sub");
    if (sub <= 0) {
        log_error("jwt: missing or invalid 'sub' claim fd=%d",
            request.conn_fd);
            return fail;
    }

    struct auth_result result = {1, (uint32_t)sub};
    return result;
}

/**
 * ===========================================================
 * SECTION 4: HTTP CALLBACK HOOK using libcurl
 * 
 * The engine POSTs {"token": "<base64-encoded token>"} to the URL.
 * The server responds {"valid": true/false, "user_id": N}.
 * libcurl handles connection poolling, timeouts, and errors.
 * 
 * Thread safety: curl_easy_init() creates a new handle per call.
 * This is safe across forked processes and concurrent connections.
 * We use curl_global_init() once at startup for efficiency.
 * ===========================================================
 */

 /**
  * Response accumulator for libcurl write callback.
  * libcurl calls our write function each time it receives data.
  * We accumulate it into this buffer.
  */
 struct curl_response {
    char buf[4096];
    size_t len;
 };

 /**
  * lincurl write callback.
  * Called by libcurl each time it receives response data.
  * We append it to our response buffer.
  * 
  * Returns the number of bytes handled. If we return less than size*nmemb,
  * libcurl treats it as an error and aborts the transfer.
  */
static size_t curl_write_cb(void *data, size_t size, size_t nmemb,
    void *userdata)
{
    struct curl_response *resp = (struct curl_response *)userdata;
    size_t total = size * nmemb;

    if (resp->len + total >= sizeof(resp->buf) - 1)
        total = sizeof(resp->buf) - 1 - resp->len;

    memcpy(resp->buf + resp->len, data, total);
    resp->len += total;
    resp->buf[resp->len] = '\0';
    return size * nmemb; // always return original to avoid curl error
}

/**
 * Standard base64 encode for the JSON body.
 * Standatd base64 is safe inside a JSON string value (not url-safe).
 */
static int b64_encode(const uint8_t *src, size_t slen, char *dest,
    size_t dsize)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16)
            | (i+1 < slen ? (uint32_t)src[i+1] << 8 : 0)
            | (i+2 < slen ? (uint32_t)src[i+2] : 0);
        if (out + 4 >= dsize) return -1;
        dest[out++] = t[(v >> 18) & 63];
        dest[out++] = t[(v >> 12) & 63];
        dest[out++] = (i+1 < slen) ? t[(v >>  6) & 63] : '=';
        dest[out++] = (i+2 < slen) ? t[v & 63] : '=';
    }
    if (out >= dsize) return -1;
    dest[out] = '\0';
    return (int)out;
}

/**
 * Initialize libcurl global state.
 * Must be called once before any HTTP auth requests.
 * curl_global_init is not thread-safe - must be called before forking.
 */
void auth_http_curl_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    log_info("libcurl initialized for HTTP auth callbacks");
}

/**
 * Clean up libcurl global state.
 * Called from the graceful shutdown sequence.
 */
void auth_http_curl_cleanup(void)
{
    curl_global_cleanup();
}

struct auth_result http_auth_hook(struct auth_request request)
{
    struct auth_result fail = {0, 0};

    const char *url = getenv("ENGINE_AUTH_HTTP_URL");
    if (!url || !*url) {
        log_error("http_auth_hook: ENGINE_AUTH_HTTP_URL not set");
        return fail;
    }

    // Base64-encode the raw token bytes for JSON transport
    char token_b64[4096];
    if (b64_encode((const uint8_t *)request.token, request.token_len,
        token_b64, sizeof(token_b64)) < 0)
    {
        log_error("http_auth: token too large to encode");
        return fail;
    }

    // Build JSON request body
    char body[4200];
    int blen = snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token_b64);
    if (blen < 0 || (size_t)blen >= sizeof(body)) return fail;

    // Set up libcurl for this request
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_error("http_auth: curl_easy_init failed");
        return fail;
    }

    struct curl_response resp;
    resp.len = 0;
    resp.buf[0] = '\0';

    // Configure the request
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    long timeout_ms = 500; // default
    const char *t = getenv("ENGINE_AUTH_HTTP_TIMEOUT_MS");
    if (t && *t) timeout_ms = strtol(t, NULL, 10);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)blen);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    // Disable signal-based timeout - required for multi-thread use
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        log_error("http_auth: curl request to %s failed: %s", url,
            curl_easy_strerror(rc));
        return fail;
    }

    // Parse {"valid": true/false, "user_id": N}
    int valid = (strstr(resp.buf, "\"valid\":true") != NULL ||
        strstr(resp.buf, "\"valid\": true") != NULL);
    long uid = json_get_long(resp.buf, "user_id");

    struct auth_result result;
    result.valid = valid;
    result.user_id = (valid && uid > 0) ? (uint32_t)uid : 0;
    return result;
}

/**
 * ===========================================================
 * SECTION 5: HOOK SELECTOR
 * ===========================================================
 */

 auth_hook_fn select_auth_hook(const struct engine_config *config)
 {
    int has_jwt = config->auth_jwt_secret[0] != '\0';
    int has_http = config->auth_http_url[0] != '\0';

    if (has_jwt && has_http) {
        log_info("Auth: both JWT and HTTP configured - "
            "JWT takes priority (no network round trip needed)");
    }

    if (has_jwt) {
        log_info("Auth: JWT validation mode (HMAC-SHA256 via OpenSSL)");
        return jwt_auth_hook;
    }
    if (has_http) {
        log_info("Auth: HTTP callback mode (libcurl) - %s",
            config->auth_http_url);
        return http_auth_hook;
    }

    log_error("Auth: DEVELOPMENT MODE - set ENGINE_AUTH_JWT_SECRET or "
        "ENGINE_AUTH_HTTP_URL");
    return default_auth_hook;
 }