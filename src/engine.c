#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>

#include "auth_hook.h"
#include "config.h"
#include "conn_map.h"
#include "logger.h"
#include "protocol.h"
#include "scope_map.h"

/**
 * ===========================================================
 * SIGNAL FLAGS
 * ===========================================================
 */

static volatile sig_atomic_t engine_running = 1;
static volatile sig_atomic_t reopen_log = 0;

/**
 * ===========================================================
 * ENGINE STATE
 * Private to this file.
 * ===========================================================
 */
static int epfd = -1;
static int server_fd = -1;
static int health_fd = -1;
static struct engine_config *cfg = NULL;
static auth_hook_fn current_hook = NULL;
static time_t start_time = 0;

static struct scope_map scope_map;
static struct conn_map conn_map;

// Memory pool - pre-allocated client_info structs.
#define POOL_MAX 10000
static struct client_info pool[POOL_MAX];
static struct client_info *free_list[POOL_MAX];
static int free_count = 0;

/**
 * ===========================================================
 * FORWARD DECLARATIONS
 * All internal functions are static. Declaring them here
 * to be able to call each other in any order below.
 * ===========================================================
 */
static int set_nonblocking(int fd);
static void handle_new_connections(void);
static void setup_client_socket(int fd);
static struct client_info *pool_alloc(void);
static void handle_health_check(void);

 /**
  * ===========================================================
  * SIGNAL HANDLERS
  * Must be minimal - set a flag and return
  * ===========================================================
  */

static void handle_shutdown(int sig)
{
    (void)sig;
    engine_running = 0;
}

static void handle_sighup(int sig)
{
    (void)sig;
    reopen_log = 1;
}

/**
 * ===========================================================
 * PUBLIC API
 * ===========================================================
 */

void engine_set_auth_hook(auth_hook_fn hook)
{
    if (!hook) {
        log_error("engine_set_auth_hook: NULL hook ignored");
        return;
    }
    current_hook = hook;
    log_info("Auth hook registered");
}

void engine_stop(void)
{
    engine_running = 0;
}

int engine_init (struct engine_config *config)
{
    cfg = config;
    start_time = time(NULL);
    // Default to dev hook until engine_set_auth_hook() is called
    current_hook = default_auth_hook;

    /**
     * SIGPIPE: ignore it.
     * When send() is called on a closed socket, the OS sends SIGPIPE.
     * Default behavior is to kill the process immediately.
     * SIG_IGN makes send() return -1 with errno=EPIPE instead, which our
     * error handling already handles correctly.
     */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_shutdown);
    signal(SIGINT, handle_shutdown);
    signal(SIGHUP, handle_sighup);
    log_info("Signal handlers registered");

    
    // Raise soft file descriptor to the hard limit maximum
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
            int e = errno;
            log_error("setrlimit failed: %s - fd limit may be low",
                strerror(e));
        }
        // Read back actual result, OS may cap it
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    log_info("File descriptor limit: %lu", (unsigned long)rl.rlim_cur);

    /**
     * libcurl Init.
     * Must be called before any fork() because curl_global_init is not
     * safe to call after forking.
     */
    auth_http_curl_init();

    // Data structures
    scope_map_init(&scope_map);
    conn_map_init(&conn_map);
    billing_log_init(config->billing_log_path);

    // Memory pool
    int pool_size = config->max_clients < POOL_MAX ? config->max_clients :
        POOL_MAX;
    for (int i = 0; i < pool_size; i++) {
        memset(&pool[i], 0, sizeof(struct client_info));
        free_list[i] = &pool[i];
    }
    free_count = pool_size;
    log_info("Memory pool: %d client slots pre-allocated", pool_size);

    // Server socket
    server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    set_nonblocking(server_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)config->port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        log_error("bind port %d: %s", config->port, strerror(errno));
        return -1;
    }
    if (listen(server_fd, config->backlog) == -1) {
        log_error("listen: %s", strerror(errno));
        return -1;
    }

    // Health check socket
    health_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (health_fd != -1) {
        setsockopt(health_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(health_fd);
        addr.sin_port = htons((uint16_t)config->health_port);
        if (bind(health_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 ||
            listen(health_fd, 16) == -1)
        {
            log_error("health socket setup failed: %s", strerror(errno));
            health_fd = -1;
        }
    }

    // Epoll instance
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        log_error("epoll_create1: %s", strerror(errno));
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    if (health_fd != -1) {
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = health_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, health_fd, &ev);
    }

    log_info("Engine listening on port %d", config->port);
    log_info("Health check on port %d", config->health_port);
    return 0;
}

/**
 * engine_run - the main event loop.
 * 
 * Blocks until engine_running becomes 0 (set by signal handler).
 * The 1000ms timeout ensures we check engine_running at least once per
 * second even if no events arrive.
 * 
 * Event processing:
 *      - server_fd ready: new connection(s) pending
 *      - health_fd ready: health check probe
 *      - EPOLLERR/HUP/RDHUP: client disconnected or error
 *      - EPOLLIN: client sent data
 *      - EPOLLOUT: kernel send buffer has space, drain queue
 */
void engine_run(void)
{
    struct epoll_event events[1024];
    log_info("Event loop started");

    while (engine_running) {
        int n = epoll_wait(epfd, events, 1024, 1000);
        if (n == -1) {
            if (errno == EINTR) continue; // signal interrupted, recheck
            int e = errno;
            log_error("epoll_wait: %s", strerror(e));
            break;
        }

        // Handle log rotation request from SIGHUP
        if (reopen_log) {
            logger_reopen();
            reopen_log = 0;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.id;

            if (fd == server_fd)
                handle_new_connections();
            else if (fd == health_fd)
                handle_health_check();
        }
    }
}

/**
 * ===========================================================
 * HELPER FUNCTIONS
 * ===========================================================
 */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct client_info *pool_alloc(void)
{
    if (free_count == 0) return NULL;
    struct client_info *c = free_list[--free_count];
    memset(c, 0, sizeof(*c));
    return c;
}

/**
 * setup_client_socket - configure every accepted client socket.
 * 
 * TCP_NODELAY: disable small packet batching to reduce latency.
 * 
 * SO_KEEPALIVE: detect dead connections. Without this, a client that silently
 * loses network connectivity keeps its slot occupied forever.
 * 
 * Buffer sizing: increase kernel socket buffers for high-throughput.
 */
static void setup_client_socket(int fd)
{
    set_nonblocking(fd);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &cfg->tcp_keepalive_idle,
        sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &cfg->tcp_keepalive_intvl,
        sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cfg->tcp_keepalive_cnt,
        sizeof(int));

    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg->recv_buf_size, sizeof(int));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cfg->send_buf_size, sizeof(int));
}

/**
 * handle_new_connections - accept all pending connections.
 * 
 * Edge-triggered epoll: one EPOLLIN may represent multiple pending
 * connections. Must loop until accept() returns EAGAIN, otherwise
 * connections queue up and never get processed.
 */
static void handle_new_connections(void)
{
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
            &addr_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            int e = errno;
            log_error("accept: %s", strerror(e));
            break;
        }

        // TODO: add rate limiting

        setup_client_socket(client_fd);

        struct client_info *client = pool_alloc();
        if (!client) {
            log_error("Pool exhausted, rejecting fd=%d", client_fd);
            close(client_fd);
            continue;
        }
        client->socket_fd = client_fd;

        conn_map_add(&conn_map, client_fd, client);

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = client_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

        log_info("Connected fd=%d ip=%s", client_fd,
            inet_ntoa(client_addr.sin_addr));
        billing_log_connect(client_fd, &client_addr);
    }
}

/**
 * handle_health_check - respond to load balancer health probes.
 * 
 * AWS load balancers, Kubernetes, and Docker healthcheck all connect to a
 * designated port periodically. If no response, the instance is marked
 * unhealthy and traffic is rerouted.
 * 
 * We accept, send a one-line response, and close immediately.
 * No auth, no epoll registration - just a quick reply.
 */
static void handle_health_check(void)
{
    int fd = accept(health_fd, NULL, NULL);
    if (fd == -1) return;

    char response[256];
    int n = snprintf(response, sizeof(response),
        "STATUS OK\r\n"
        "CONNECTIONS %d\r\n"
        "UPTIME %lu\r\n",
        conn_map_count(&conn_map),
        (unsigned long)(time(NULL) - start_time));

    send(fd, response, (size_t)n, 0);
    close(fd);
}