#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/**
 * engine_config - All runtime configuration for the C State Bus.
 * 
 * Every field is loaded from an environment variable at startup.
 * If the environment variable is not set, a production-appropriate
 * default is used. No recompilation is needed to change any value.
 * 
 * See config_load() for startup logging of all values.
 */

struct engine_config {
    /* Network */
    int port;                   // which port to bind to
    int health_port;            // health check port (default 8081)
    int backlog;                // listen() backlog queue size

    /* Capacity */
    int max_clients;            // maximum simultaneous connections
    int max_events;             // epoll_wait max events per call
    int worker_count;           /* ENGINE_WORKER_COUNT, default 0 = auto-detect
                                 * CPU count. Set explicitly to limit workers
                                 * on memory-constrained machines. Each worker
                                 * uses ~2MB idle + ~74KB per active
                                 * connection. */

    /* Rate limiting */
    int conn_rate_limit;        // max new connections per IP per second

    /* Socket tuning */
    int send_buf_size;          // per-socket send buffer bytes
    int recv_buf_size;          // per-socket receive buffer bytes
    int tcp_keepalive_idle;     // seconds before keepalive probes start
    int tcp_keepalive_intvl;    // seconds between keepalive probes
    int tcp_keepalive_cnt;      // probes before declaring dead

    /* Logging */
    char log_path[256];         // path to log file, default logs/server.log
    char billing_log_path[256]; // default logs/billing.log

    /**
     * Auth configuration - exactly one of these should be set in production.
     * Priority order when multiple are set:
     *  1. ENGINE_AUTH_JWT_SECRET (highest priority - no network call needed)
     *  2. ENGINE_AUTH_HTTP_URL (second priority - HTTP callback)
     *  3. Neither set (development mode - accepts all, logs warning)
     */
    char auth_jwt_secret[512];
    char auth_http_url[512];
    char auth_http_timeout_ms[16];  // default 500

    /* Observer metrics */
    char metrics_socket[256];   // default "" = disabled
};

/**
 * Load configuration from environment variables.
 * Missing variables use production-appropriate defaults.
 */
struct engine_config config_load(void);

/**
 * Log all configuration values at INFO level.
 * Redacts auth secrets - shows only whether they are set.
 */
void config_log(const struct engine_config *config);

#endif