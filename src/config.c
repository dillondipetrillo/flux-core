#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "logger.h"

/**
 * Helper: read an integer from an environment variable.
 * Returns default_val if the variable is not set or is not a valid integer.
 */
static int env_int(const char *name, const int default_val)
{
    const char *val = getenv(name);
    if (!val || *val == '\0') return default_val;
    return atoi(val);
}

/**
 * Helper: read a string from an environment variable into a fixed buffer.
 * Writes an empty string if the variable is not set.
 */
static void env_str(const char *name, char *dest, const size_t dest_size,
    const char *default_val)
{
    const char *val = getenv(name);
    if (!val || *val == '\0')
        strncpy(dest, default_val, dest_size - 1);
    else
        strncpy(dest, val, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

struct engine_config config_load(void)
{
    struct engine_config config;
    memset(&config, 0, sizeof(config));

    /* Network */
    config.port = env_int("ENGINE_PORT", 8080);
    config.health_port = env_int("ENGINE_HEALTH_PORT", 8081);
    config.backlog = env_int("ENGINE_BACKLOG", 128);

    /* Capacity */
    config.max_clients = env_int("ENGINE_MAX_CLIENTS", 10000);
    config.max_events = env_int("ENGINE_MAX_EVENTS", 1024);
    config.worker_count = env_int("ENGINE_WORKER_COUNT", 0);

    /* Rate limiting */
    config.conn_rate_limit = env_int("ENGINE_CONN_RATE_LIMIT", 10);

    /* Socket tuning */
    config.send_buf_size = env_int("ENGINE_SEND_BUF_SIZE", 262144); // 256KB
    config.recv_buf_size = env_int("ENGINE_RECV_BUF_SIZE", 262144); // 256KB
    config.tcp_keepalive_idle = env_int("ENGINE_TCP_KEEPALIVE_IDLE", 60);
    config.tcp_keepalive_intvl = env_int("ENGINE_TCP_KEEPALIVE_INTVL", 10);
    config.tcp_keepalive_cnt = env_int("ENGINE_TCP_KEEPALIVE_CNT", 3);

    /* Logging */
    env_str("ENGINE_LOG_PATH", config.log_path, sizeof(config.log_path),
        "logs/server.log");
    env_str("ENGINE_BILLING_LOG_PATH", config.billing_log_path,
        sizeof(config.billing_log_path), "logs/billing.log");

    /* Auth */
    env_str("ENGINE_AUTH_JWT_SECRET", config.auth_jwt_secret,
        sizeof(config.auth_jwt_secret), "");
    env_str("ENGINE_AUTH_HTTP_URL", config.auth_http_url,
        sizeof(config.auth_http_url), "");
    env_str("ENGINE_AUTH_HTTP_TIMEOUT_MS", config.auth_http_timeout_ms,
        sizeof(config.auth_http_timeout_ms), "500");

    /* Observer */
    env_str("ENGINE_METRICS_SOCKET", config.metrics_socket,
        sizeof(config.metrics_socket), "");

    return config;
}

void config_log(const struct engine_config *config)
{
    log_info("=== Engine Configuration ===");
    log_info("Network:      port=%d health=%d backlog=%d", config->port,
        config-> health_port, config->backlog);
    log_info("Capacity:     max_clients=%d max_events=%d worker_count=%d%s",
        config->max_clients, config->max_events, config->worker_count,
        config->worker_count == 0 ? " (default - auto-detect CPU count)" :
        "");
    log_info("Rate:         conn_rate_limit=%d/sec/ip",
        config->conn_rate_limit);
    log_info("Buffers:      send=%d recv=%d", config->send_buf_size,
        config->recv_buf_size);
    log_info("Keepalive:    idle=%d intvl=%d cnt=%d",
            config->tcp_keepalive_idle, config->tcp_keepalive_intvl,
            config->tcp_keepalive_cnt);
    log_info("Logging:      log=%s billing=%s", config->log_path,
        config->billing_log_path);

    /* Auth - show what mode is active, never log the secret value */
    if (config->auth_jwt_secret[0] != '\0' &&
        config->auth_http_url[0] != '\0')
    {
        log_info("Auth:         JWT (priority) + HTTP configured - using JWT");
    } else if (config->auth_jwt_secret[0] != '\0') {
        log_info("Auth:         JWT validation (ENGINE_AUTH_JWT_SECRET set)");
    } else if (config->auth_http_url[0] != '\0') {
        log_info("Auth:         HTTP callback (%s)", config-> auth_http_url);
    } else {
        log_error("Auth:        DEVELOPMENT MODE - accepts all tokens - "
            "DO NOT USE IN PRODUCTION");
    }

    if (config->metrics_socket[0] != '\0')
        log_info("Observer:     metrics socket at %s", config->metrics_socket);
    else
        log_info("Observer:     disabled");

    log_info("=== End Configuration ===");
}