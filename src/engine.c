#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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
static uint64_t next_conn_id = 1; // 0 is reserved as "connection not assigned"

static struct scope_map scope_map;
static struct conn_map conn_map;

// Memory pool - pre-allocated client_info structs.
#define POOL_MAX 10000
static struct client_info *pool = NULL;
static struct client_info **free_list = NULL;
static int free_count = 0;

/**
 * Rate limiter - 4-way associative hash map
 * 
 * RATE_MAP_SIZE buckets, each holding 4 slots.
 * An IP is hashed to a bucket. All 4 slots in the bucket are searched for a
 * matching IP. If not found, an empty slot is used, or the slot with the
 * oldest window_start is evicted.
 */
#define RATE_MAP_SIZE 8192
#define RATE_MAP_WAYS 4

struct rate_slot {
    uint32_t ip;
    int count;
    time_t window_start;
    int in_use;
};

struct rate_bucket {
    struct rate_slot slots[RATE_MAP_WAYS];
};

static struct rate_bucket rate_map[RATE_MAP_SIZE];

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
static void disconnect_client(int fd);
static void pool_free(struct client_info *client);
static void handle_client_readable(int fd);
static void process_recv_buffer(struct client_info *client, int fd);
static void dispatch_packet(struct client_info *client, int fd, uint8_t type,
    uint32_t scope_id, uint32_t sender_id, uint64_t expires_at, char *payload,
    uint32_t payload_len);
static int engine_send_vector(int fd, const struct packet_header *hdr,
    const char *payload, uint32_t payload_len);
static void engine_send_response(int fd, enum packet_type type,
    enum status_code code);
static int engine_send(int fd, const char *data, size_t len);
static int engine_queue_send(struct client_info *client, const char *data, 
    size_t len);
static int client_in_scope(const struct client_info *client,
    uint32_t scope_id);
static int client_leave_scope(struct client_info *client, uint32_t scope_id);
static int route_to_scope_map(int sender_fd, uint32_t scope_id,
    uint32_t sender_id, uint8_t type, uint64_t expires_at, const char *payload,
    uint32_t payload_len);
static void handle_client_writable(int fd);
static void perform_graceful_shutdown(void);
static int rate_limit_check(struct sockaddr_in *addr);
static void sleep_interruptible(int seconds);
static void shutdown_callback(int fd, struct client_info *client,
    void *userdata);
static void cleanup_callback(int fd, struct client_info *client,
    void *userdata);

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

int engine_prefork_init(struct engine_config *config)
{
    cfg = config;

    /**
     * curl_global_init must be called before any fork().
     * After fork, each child inherits the initialized state.
     * Calling it after fork causes undefined behavior because curl may
     * initialize global state that uses shared memory that cannot be safely
     * duplicated across processes.
     */
    auth_http_curl_init();

    log_info("Pre-fork initialization complete");
    return 0;
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
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    sa.sa_handler = handle_shutdown;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = handle_sighup;
    sigaction(SIGHUP, &sa, NULL);
    log_info("Signal handlers registered (worker pid=%d)", (int)getpid());

    
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
    log_info("Worker pid=%d fd limit: %lu",
        (int)getpid(), (unsigned long)rl.rlim_cur);

    // Data structures
    scope_map_init(&scope_map);
    conn_map_init(&conn_map);
    billing_log_init(config->billing_log_path);

    // Memory pool
    int pool_size = config->max_clients < POOL_MAX ? config->max_clients :
        POOL_MAX;
    if (pool_size <= 0) pool_size = 100;

    pool = malloc((size_t)pool_size * sizeof(struct client_info));
    free_list = malloc((size_t)pool_size * sizeof(struct client_info));

    if (!pool || !free_list) {
        log_error("engine_init: failed to allocate pool metadata for %d slots",
            pool_size);
        free(pool);
        free(free_list);
        return -1;
    }

    for (int i = 0; i < pool_size; i++) {
        memset(&pool[i], 0, sizeof(struct client_info));
        /**
         * After memset, recv_buf and send_buf are NULL
         * (zero bytes = NULL pointer). This is correct - buffers are not
         * allocated until a client connects. pool_free checks for non-NULL
         * pointers so NULL here is the correct initial state indicating
         * "no buffers allocated yet".
         */
        pool[i].recv_buf = NULL;
        pool[i].send_buf = NULL;
        free_list[i] = &pool[i];
    }
    free_count = pool_size;

    /**
     * Memory usage at startup:
     *      Pool metadata: pool_size x ~200 bytes = ~2MB for 10,000 slots
     *      Buffer memory: 0 bytes (allocated per-connection on accept)
     * 
     * Memory usage at max load:
     *      Pool metadata: ~2MB (unchanged)
     *      Buffer memory: active_connections x (MAX_BUFF_SIZE + MAX_SEND_BUF)
     *          = active_connections x ~74KB
     */
    log_info("Memory pool: %d client slots (metadata only, buffers allocated "
        "per-connection)", pool_size);

    // Epoll instance
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        log_error("epoll_create1: %s", strerror(errno));
        return -1;
    }

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

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        log_error("epoll_ctl ADD server_fd failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }
    log_info("Engine listening on port %d", config->port);

    // Health check socket
    health_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (health_fd != -1) {
        int hopt = 1;
        setsockopt(health_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(hopt));
        setsockopt(health_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(hopt));
        set_nonblocking(health_fd);

        struct sockaddr_in haddr;
        memset(&haddr, 0, sizeof(haddr));
        haddr.sin_family = AF_INET;
        haddr.sin_port = htons((uint16_t)config->health_port);
        haddr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(health_fd, (struct sockaddr *)&haddr, sizeof(haddr)) == -1 ||
            listen(health_fd, 16) == -1)
        {
            log_error("Health socket failed to bind port %d: %s",
                config->health_port, strerror(errno));
            close(health_fd);
            health_fd = -1;
            log_error("Health check DISABLED - "
                "load balancer checks will fail");
        } else {
            struct epoll_event hev;
            hev.events = EPOLLIN | EPOLLET;
            hev.data.fd = health_fd;
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, health_fd, &hev) == -1) {
                log_error("epoll_ctl ADD health_fd failed: %s",
                    strerror(errno));
                close(health_fd);
                health_fd = -1;
            } else {
                log_info("Health check ENABLED on port %d (worker pid=%d)",
                    config->health_port, (int)getpid());
            }
        }
    }

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
            int fd = events[i].data.fd;

            if (fd == server_fd)
                handle_new_connections();
            else if (fd == health_fd)
                handle_health_check();
            else if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                disconnect_client(fd);
            else if (events[i].events & EPOLLIN)
                handle_client_readable(fd);
            else if (events[i].events & EPOLLOUT)
                handle_client_writable(fd);
        }
    }

    perform_graceful_shutdown();
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
    
    struct client_info *client = free_list[--free_count];

    /**
     * Allocate network buffers for this specific client connection.
     * Buffers are allocated here - not at startup - so idle pool slots
     * consume zero buffer memory. Only active connections use buffer RAM.
     * 
     * recv_buf: accumulates incoming TCP data between epoll events.
     *      Must hold at least one maximum-size packet (header + payload).
     * 
     * send_buf: queues outgoing data for slow clients (backpressue buffer).
     *      Sized to hold several maximum-size packets before we disconnect.
     */
    client->recv_buf = malloc(MAX_BUFF_SIZE);
    client->send_buf = malloc(MAX_SEND_BUFF);

    if (!client->recv_buf || !client->send_buf) {
        log_error("OOM: cannot allocate network buffers (active connections "
            "may be exhausting available RAM)");
        free(client->recv_buf);
        free(client->send_buf);
        client->recv_buf = NULL;
        client->send_buf = NULL;
        free_list[free_count++] = client; // return slot to pool
        return NULL;
    }

    /**
     * Initialize fields explicitly rather than memset-ing the whole struct.
     * We cannot memset because recv_buf and send_buf are now pointers -
     * memset would overwrite them with zeros, losing the just-allocated
     * addresses.
     */
    client->socket_fd = -1;
    client->conn_id = next_conn_id++; // assign once, never changes
    client->user_id = 0;
    client->scope_count = 0;
    client->is_authenticated = 0;
    client->recv_len = 0;
    client->send_len = 0;
    client->send_offset = 0;
    client->bytes_sent = 0;
    client->bytes_recv = 0;
    memset(client->session_token, 0, sizeof(client->session_token));
    memset(client->scopes, 0, sizeof(client->scopes));

    return client;
}

static void pool_free(struct client_info *client)
{
    if (!client) return;
    
    /**
     * Buffers must be freed by disconnect_client BEFORE calling pool_free.
     * If recv_buf or send_buf are non_NULL here, it is a bug in the caller.
     * Log it loudly so it is caught.
     */
    if (client->recv_buf != NULL) {
        log_error("pool_free: recv_buf not freed before returning slot "
            "(memory leak) - fd=%d", client->socket_fd);
        free(client->recv_buf);
        client->recv_buf = NULL;
    }
    if (client->send_buf != NULL) {
        log_error("pool_free: send_buf not freed before returning slot "
            "(memory leak) - fd=%d", client->socket_fd);
        free(client->send_buf);
        client->send_buf = NULL;
    }

    free_list[free_count++] = client;
}

/**
 * engine_send - send data to a client, handling partial sends.
 * 
 * With non-blocking sockets, send() may not accept all bytes at once if the
 * kernel's send buffer is full. When that happens we queue the unsent data in
 * client->send_buf and register EPOLLOUT so handle_client_writeable drains the
 * queue when space is available.
 * 
 * The engine never blocks waiting for a slow client, and never drops data
 * silently.
 */
static int engine_send(int fd, const char *data, size_t len)
{
    struct client_info *client = conn_map_get(&conn_map, fd);
    if (!client) return -1;

    // If data is already queued, append to maintain ordering
    if (client->send_len > 0)
        return engine_queue_send(client, data, len);

    ssize_t n = send(fd, data, len, 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return engine_queue_send(client, data, len);
        disconnect_client(fd);
        return -1;
    }

    client->bytes_sent += (uint64_t)n;

    if ((size_t)n < len)
        return engine_queue_send(client, data + n, len - (size_t)n);

    return 0;
}

static int engine_queue_send(struct client_info *client, const char *data,
    size_t len)
{
    /**
     * Compact first: if we've already drained some bytes from the front
     * (send_offset > 0), shift the remaining unsent bytes back to the
     * start of the buffer before appending more. Without this, send_offset
     * only ever grows until the queue fully empties, so a receiver that
     * stays continuously backlogged (the exact burst-mode pattern) sees
     * available space shrink to zero even though most of the buffer is
     * actually free. That falsely trips the overflow check below and
     * disconnects a healthy, just-slow client.
     */
    if (client->send_offset > 0) {
        if (client->send_len > 0) {
            memmove(client->send_buf, client->send_buf + client->send_offset,
                client->send_len);
        }
        client->send_offset = 0;
    }

    if (client->send_len + len > MAX_SEND_BUFF) {
        log_error("Send buffer overflow fd=%d, disconnecting",
            client->socket_fd);
        disconnect_client(client->socket_fd);
        return -1;
    }

    memcpy(client->send_buf + client->send_len, data, len);
    client->send_len += len;

    // Register for EPOLLOUT to drain the queue when writeable
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = client->socket_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, client->socket_fd, &ev);
    return 0;
}

/**
 * engine_send_vector - send a header and payload as one atomic operation.
 * 
 * Uses writev() to push both buffers to the kernel in a single syscall
 * instead of two separate send() calls. Falls back to the existing
 * engine_queue_send() backpressure path on partial writes or EAGAIN,
 * preserving strict byte ordering with anything already queued.
 */
static int engine_send_vector(int fd, const struct packet_header *hdr,
    const char *payload, uint32_t payload_len)
{
    struct client_info *client = conn_map_get(&conn_map, fd);
    if (!client) return -1;

    if (client->send_len > 0) {
        if (engine_queue_send(client, (const char *)hdr,
            sizeof(struct packet_header)) == -1) return -1;
        if (payload_len > 0)
            return engine_queue_send(client, payload, payload_len);
        return 0;
    }

    struct iovec iov[2];
    iov[0].iov_base = (void *)hdr;
    iov[0].iov_len = sizeof(struct packet_header);
    int iovcnt = 1;

    if (payload_len > 0) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = payload_len;
        iovcnt = 2;
    }

    size_t total_expected = sizeof(struct packet_header) + payload_len;
    ssize_t n = writev(fd, iov, iovcnt);

    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (engine_queue_send(client, (const char *)hdr,
                sizeof(struct packet_header)) == -1) return -1;
            if (payload_len > 0)
                return engine_queue_send(client, payload, payload_len);
            return 0;
        }
        disconnect_client(fd);
        return -1;
    }

    client->bytes_sent += (uint64_t)n;
    size_t written = (size_t)n;

    if (written == total_expected) return 0; // fully sent, common case

    // Partial write - queue the remainder, preserving order
    if (written < sizeof(struct packet_header)) {
        size_t hdr_left = sizeof(struct packet_header) - written;
        if (engine_queue_send(client, (const char *)hdr + written,
            hdr_left) == -1) return -1;
        if (payload_len > 0)
            return engine_queue_send(client, payload, payload_len);
    } else {
        size_t payload_written = written - sizeof(struct packet_header);
        size_t payload_left = payload_len - payload_written;
        if (payload_left > 0)
            return engine_queue_send(client, payload + payload_written,
                payload_left);
    }
    return 0;
}

/**
 * engine_send_response - send ACK or ERROR with a status code.
 * Builds the packet_header + response_payload and sends both.
 */
static void engine_send_response(int fd, enum packet_type type,
    enum status_code code)
{
    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = (uint8_t)type;
    hdr.payload_len = htonl(sizeof(struct response_payload));
    hdr.scope_id = htonl(0);
    hdr.sender_id = htonl(0);
    hdr.expires_at = htobe64(0);

    struct response_payload rp;
    rp.status_code = htonl((uint32_t)code);

    engine_send_vector(fd, &hdr, (const char *)&rp, sizeof(rp));
}

static int client_in_scope(const struct client_info *client,
    uint32_t scope_id)
{
    for (int i = 0; i < client->scope_count; i++)
        if (client->scopes[i] == scope_id) return 1;
    return 0;
}

static int client_leave_scope(struct client_info *client, uint32_t scope_id)
{
    for (int i = 0; i < client->scope_count; i++) {
        if (client->scopes[i] == scope_id) {
            // Swap with last element, decrement count, O(1) removal
            client->scopes[i] = client->scopes[--client->scope_count];
            return 1;
        }
    }
    return 0;
}

/**
 * disconnect_client - clean up a client connection completely.
 * 
 * Order matters:
 * 1. Log before removing from maps (so we can log user_id)
 * 2. Remove from scope_map so no messages are routed to this fd
 * 3. Remove from conn_map so no code tries to look up this fd
 * 4. Remove from epoll before closing (Linux removes automatically on close
 *      but explicit removal is cleaner)
 * 5. Close the socket
 * 6. Return the client_info to the pool
 */
static void disconnect_client(int fd)
{
    struct client_info *client = conn_map_get(&conn_map, fd);
    if (!client) {
        close(fd);
        return;
    }

    if (client->is_authenticated)
        log_info("conn_id=%lu fd=%d user_id=%u Disconnected bytes_sent=%lu "
            "bytes_recv=%lu", (unsigned long)client->conn_id, fd,
            client->user_id, (unsigned long)client->bytes_sent,
            (unsigned long)client->bytes_recv);
    else
        log_info("conn_id=%lu fd=%d user_id=0 Disconnected unauthenticated",
            (unsigned long)client->conn_id, fd);

    billing_log_disconnect(fd, client->user_id, client->bytes_sent,
        client->bytes_recv);

    // Clean up client from all joined scopes
    for (int i = 0; i < client->scope_count; i++)
        scope_map_remove(&scope_map, client->scopes[i], fd);

    conn_map_remove(&conn_map, fd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    /**
     * Free network buffers before returning the slot to the pool. Order
     * matters: buffers freed here, then pool_free returns the metadata slot.
     * pool_free checks for non-NULL pointers and logs an error if buffers
     * were not freed - this catches bugs early.
     */
    if (client->recv_buf != NULL) {
        free(client->recv_buf);
        client->recv_buf = NULL;
    }
    if (client->send_buf != NULL) {
        free(client->send_buf);
        client->send_buf = NULL;
    }
    
    pool_free(client);
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

    // TCP keepalive - detect dead connections
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

        // Rate limiting - reject connections over the per-IP limit
        if (!rate_limit_check(&client_addr)) {
            close(client_fd);
            continue;
        }

        setup_client_socket(client_fd);

        struct client_info *client = pool_alloc();
        if (!client) {
            log_error("Memory pool exhausted (max=%d), rejecting fd=%d. "
                "Increase ENGINE_MAX_CLIENTS to allow more connections.",
                cfg->max_clients, client_fd);
            close(client_fd);
            continue;
        }
        client->socket_fd = client_fd;

        conn_map_add(&conn_map, client_fd, client);

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = client_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        log_info("conn_id=%lu fd=%d user_id=0 Connected ip=%s",
            (unsigned long)client->conn_id, client_fd, ip_str);
        billing_log_connect(client_fd, &client_addr);
    }
}

/**
 * handle_client_readable - read ALL available data from a client.
 * 
 * Edge-triggered: must read in a loop until EAGAIN.
 * If we stop reading early, the data stays in the kernel buffer and epoll
 * will NOT notify us again until NEW data arrives.
 * The client would hang.
 * 
 * All data is appended to client->recv_buf. Complete packets are extracted
 * and dispatched by process_recv_buffer().
 */
static void handle_client_readable(int fd)
{
    struct client_info *client = conn_map_get(&conn_map, fd);
    if (!client) return;

    while (1) {
        size_t space = MAX_BUFF_SIZE - client->recv_len;
        if (space == 0) {
            log_error("Recv buffer full fd=%d, disconnecting", fd);
            disconnect_client(fd);
            return;
        }

        ssize_t n = recv(fd, client->recv_buf + client->recv_len, space, 0);

        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            int e = errno;
            log_error("recv fd=%d: %s", fd, strerror(e));
            disconnect_client(fd);
            return;
        }
        if (n == 0) {
            disconnect_client(fd);
            return;
        }

        client->recv_len += (size_t)n;
        client->bytes_recv += (uint64_t)n;
        
        /**
         * Process immediately, not after the loop. This reclaims recv_buf space
         * as packets are parsed, so the buffer never has to absorb an entire
         * burst before a single byte is consumed. Without this, MAX_BUFF_SIZE
         * gets exhausted mid-burst even though complete packets are sitting in
         * the buffer ready to be parsed.
         * 
         * process_recv_buffer() may call disconnect_client() internally
         * (oversized payload). client_info is heap-owned and the pool slot is
         * recycled inside disconnect_client(), so we must stop touching 'client'
         * immediately after this call returns if that happened. We re-fetch
         * nothing here because we simply return, the fd is already gone from
         * conn_map by the time control comes back to us.
         */
        process_recv_buffer(client, fd);
        if (conn_map_get(&conn_map, fd) == NULL) return; //disconnected mid-parse
    }
}

/**
 * process_recv_buffer - extract and dispatch complete packets.
 * 
 * The receive buffer may contain:
 *      - Zero complete packets (partial header arrived, wait for more)
 *      - One complete packet
 *      - Multiple complete packets
 * 
 * Loop until no complete packets remain in the buffer. After extracting each
 * packet, shift the buffer left with memmove.
 */
static void process_recv_buffer(struct client_info *client, int fd)
{
    size_t offset = 0;
    const size_t header_sz = sizeof(struct packet_header);

    while (client->recv_len - offset >= header_sz) {
        // Zero-copy: cast directly over the buffer, no per-packet memcpy
        struct packet_header *hdr =
            (struct packet_header *)(client->recv_buf + offset);

        // Convert from network byte order to host byte order
        uint32_t payload_len = ntohl(hdr->payload_len);
        uint32_t scope_id = ntohl(hdr->scope_id);
        uint64_t expires_at = be64toh(hdr->expires_at);
        uint8_t type = hdr->type;

        if (payload_len > MAX_PAYLOAD) {
            log_error("Oversized payload %u fd=%d, disconnecting", payload_len,
                fd);
            disconnect_client(fd);
            return;
        }

        size_t total = header_sz + payload_len;
        if (client->recv_len - offset < total) break; // partial frame, wait

        // Complete packet available
        char *payload = client->recv_buf + offset + header_sz;

        // Server stamps sender_id, client cannot spoof identity
        uint32_t sender_id = (uint32_t)fd;

        dispatch_packet(client, fd, type, scope_id, sender_id, expires_at,
            payload, payload_len);

        offset += total;
    }

    // Single memmove per call, not per packet
    if (offset > 0) {
        size_t remaining = client->recv_len - offset;
        if (remaining > 0)
            memmove(client->recv_buf, client->recv_buf + offset, remaining);
        client->recv_len = remaining;
    }
}

/**
 * dispatch_packet - route a complete packet to the correct handler.
 * 
 * This is the core routing logic of the engine.
 * The engine knows packet types (JOIN, IDENTIFY, etc.) but never knows what
 * the payload bytes mean - that is the application's job.
 * 
 * The security gate: all non-IDENTIFY packets require authentication.
 * Unauthenticated packets receive an error response but the connection is kept
 * open - the client can still auth.
 */
static void dispatch_packet(struct client_info *client, int fd, uint8_t type,
    uint32_t scope_id, uint32_t sender_id, uint64_t expires_at, char *payload,
    uint32_t payload_len)
{
    if (type != TYPE_SYS_IDENTIFY && !client->is_authenticated) {
        log_error("Unauthenticated packet type=%u fd=%d", type, fd);
        engine_send_response(fd, TYPE_SYS_ERROR, STATUS_ERR_UNIDENTIFIED);
        return; // keep connection open, let them authenticate
    }

    switch ((enum packet_type)type) {
        case TYPE_SYS_IDENTIFY: {
            if (client->is_authenticated) {
                engine_send_response(fd, TYPE_SYS_ERROR,
                    STATUS_ERR_ALREADY_ID);
                break;
            }

            struct auth_request req;
            req.token = payload;
            req.token_len = payload_len;
            req.conn_fd = fd;

            struct auth_result result = current_hook(req);

            if (!result.valid) {
                log_error("Auth rejected fd=%d", fd);
                engine_send_response(fd, TYPE_SYS_ERROR,
                    STATUS_ERR_AUTH_FAILED);
                disconnect_client(fd);
                break;
            }

            size_t tlen = payload_len < 255 ? payload_len : 255;
            memcpy(client->session_token, payload, tlen);
            client->session_token[tlen] = '\0';
            client->is_authenticated = 1;
            client->user_id = result.user_id;

            log_info("conn_id=%lu fd=%d user_id=%u Authenticated",
                (unsigned long)client->conn_id, fd, result.user_id);
            engine_send_response(fd, TYPE_SYS_ACK, STATUS_OK);
            billing_log_auth(fd, result.user_id);
            break;
        }

        case TYPE_SYS_JOIN: {
            if (client_in_scope(client, scope_id)) {
                engine_send_response(fd, TYPE_SYS_ERROR,
                    STATUS_ERR_ALREADY_IN_ROOM);
                break;
            }
            if (client->scope_count >= MAX_SCOPES) {
                engine_send_response(fd, TYPE_SYS_ERROR,
                    STATUS_ERR_SCOPES_FULL);
                break;
            }
            client->scopes[client->scope_count++] = scope_id;
            scope_map_add(&scope_map, scope_id, fd);
            log_info("conn_id=%lu fd=%d user_id=%u joined scope=%u",
                (unsigned long)client->conn_id, fd, client->user_id, scope_id);
            engine_send_response(fd, TYPE_SYS_ACK, STATUS_OK);
            break;
        }

        case TYPE_SYS_LEAVE: {
            if (!client_leave_scope(client, scope_id)) {
                engine_send_response(fd, TYPE_SYS_ERROR,
                    STATUS_ERR_NOT_IN_ROOM);
                break;
            }
            scope_map_remove(&scope_map, scope_id, fd);
            log_info("conn_id=%lu fd=%d user_id=%u left scope=%u",
                (unsigned long)client->conn_id, fd, client->user_id, scope_id);
            engine_send_response(fd, TYPE_SYS_ACK, STATUS_OK);
            break;
        }

        case TYPE_SYS_PING: {
            struct packet_header pong;
            memset(&pong, 0, sizeof(pong));
            pong.type = (uint8_t)TYPE_SYS_PING;
            pong.payload_len = htonl(0);
            pong.scope_id = htonl(0);
            pong.sender_id = htonl(0);
            pong.expires_at = htobe64(0);
            engine_send(fd, (const char *)&pong, sizeof(pong));
            break;
        }

        default: {
            // Application data - check TTL then route
            if (expires_at != 0 && expires_at < (uint64_t)time(NULL)) {
                log_info("conn_id=%lu fd=%d user_id=%u TTL expired scope=%u",
                    (unsigned long)client->conn_id, fd, client->user_id,
                    scope_id);
                engine_send_response(fd, TYPE_SYS_ERROR, STATUS_ERR_EXPIRED);
                break;
            }
            route_to_scope_map(fd, scope_id, sender_id, type, expires_at,
                payload, payload_len);
            break;
        }
    }
    (void)sender_id; // used in route_to_scope_map, surpress warning
}

/**
 * route_to_scope_map - send a packet to all subscribers of a scope.
 * 
 * Uses scope_map_get() for O(k) lookup where k = subscribers in scope.
 * The header is converted to network byte order ONCE before the loop, not
 * once per recipient.
 */
static int route_to_scope_map(int sender_fd, uint32_t scope_id,
    uint32_t sender_id, uint8_t type, uint64_t expires_at, const char *payload,
    uint32_t payload_len)
{
    int count = 0;
    int *fds = scope_map_get(&scope_map, scope_id, &count);
    if (!fds || count == 0) return 0;

    // Build outgoing header once
    struct packet_header out;
    out.type = type;
    out.payload_len = htonl(payload_len);
    out.scope_id = htonl(scope_id);
    out.sender_id = htonl(sender_id);
    out.expires_at = htobe64(expires_at);

    int routed = 0;
    for (int i = 0; i < count; i++) {
        if (fds[i] == sender_fd) continue; // never echo to sender
        engine_send_vector(fds[i], &out, payload, payload_len);
        routed++;
    }
    return routed;
}

/**
 * handle_client_writable - drain the outbound queue.
 * 
 * Called when EPOLLOUT fires - the kernel send buffer has space.
 * We send as much of client->send_buf as possible.
 * If we drain it completely, remove EPOLLOUT monitoring.
 * If the buffer fills again (EAGAIN), leave EPOLLOUT registered.
 */
static void handle_client_writable(int fd)
{
    struct client_info *client = conn_map_get(&conn_map, fd);
    if (!client || client->send_len == 0) {
        // Nothing queued - stop watching for writability
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
        return;
    }

    while (client->send_len > 0) {
        ssize_t n = send(fd, client->send_buf + client->send_offset,
            client->send_len, 0);

        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            disconnect_client(fd);
            return;
        }

        client->bytes_sent += (uint64_t)n;
        client->send_offset += (size_t)n;
        client->send_len -= (size_t)n;
    }

    // Queue drained - reset and stop watching writability
    client->send_offset = 0;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/**
 * handle_health_check - respond to load balancer and monitoring probes.
 * 
 * Accepts a connection on health_fd, sends a status response, closes.
 * Does not register with epoll - health checks are short-lived and handled
 * synchronously. One accept per event, no looping needed because health_fd
 * is edge-triggered and health probes are infrequent.
 * 
 * Response format is line-delimited key-value pairs ending with CRLF.
 * Load balancers check for HTTP 200 or a specific string like "STATUS OK".
 * 
 * WORKER_PID: identifies which worker responded - essential for debugging
 *      worker-specific issues in multi-process mode.
 * CONNECTIONS: active connections on THIS work (not aggregate).
 * POOL_FREE: available client slots on THIS worker.
 * POOL_MAX: total client slots configured for this worker.
 * UPTIME: seconds since this worker's engine_init completed.
 */
static void handle_health_check(void)
{
    while (1) {
        int fd = accept(health_fd, NULL, NULL);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
    
        char response[512];
        int n = snprintf(response, sizeof(response),
            "STATUS OK\r\n"
            "WORKER_PID %d\r\n"
            "CONNECTIONS %d\r\n"
            "POOL_FREE %d\r\n"
            "POOL_MAX %d\r\n"
            "UPTIME %lu\r\n",
            (int)getpid(),
            conn_map_count(&conn_map),
            free_count,
            cfg->max_clients,
            (unsigned long)(time(NULL) - start_time));
    
        send(fd, response, (size_t)n, MSG_DONTWAIT);
        close(fd);
    }
}

/**
 * shutdown_ctx - context passed to the graceful shutdown callback.
 * Tracks how many clients were notified successfully vs attempted.
 */
struct shutdown_ctx {
    struct packet_header *hdr;
    int attempted;
    int notified;
};

/**
 * shutdown_callback - called once per active connection during shutdown.
 * 
 * Sends TYPE_SYS_SHUTDOWN to authenticated clients using MSG_DONTWAIT.
 * MSG_DONTWAIT: if the kernel send buffer is full, skip this client rather
 * than blocking. During shutdown we cannot afford to stall on one slow client,
 * the whole process needs to exit cleanly.
 * 
 * Unauthenticated clients are skipped - they have not completed the handshake
 * and cannot meaningfully interpret a shutdown packet. They will receive a
 * broken pipe when their fd is closed.
 * 
 * Note: do NOT call conn_map_remove or disconnect_client from this callback.
 * conn_map_foreach must notify the map during iteration. Cleanup happens in
 * the second pass in perform_graceful_shutdown.
 */
static void shutdown_callback(int fd, struct client_info *client,
    void *userdata)
{
    struct shutdown_ctx *ctx = (struct shutdown_ctx *)userdata;

    if (!client) return;

    ctx->attempted++;

    if (!client->is_authenticated) return;

    ssize_t sent = send(fd, ctx->hdr, sizeof(struct packet_header),
        MSG_DONTWAIT);

    if (sent == (ssize_t)sizeof(struct packet_header))
        ctx->notified++;
    else {
        /**
         * Send failed, client's buffer is full or socket is broken. Client
         * will get a broken pipe when we close the fd. This is acceptable,
         * we made a best-effort notification.
         */
        log_info("conn_id=%lu fd=%d user_id=%u Shutdown notification "
            "not delivered (send returned %zd)",
            (unsigned long)client->conn_id, fd, client->user_id, sent);
    }
}

// cleanup_ctx - context for the force-close pass
struct cleanup_ctx {
    int closed;
};

static void cleanup_callback(int fd, struct client_info *client,
    void *userdata)
{
    struct cleanup_ctx *ctx = (struct cleanup_ctx *)userdata;

    /**
     * Call disconnect_client which handles the complete cleanup sequence:
     *      1. Remove from conn_map and scope_map
     *      2. Remove from epoll
     *      3. Close the fd
     *      4. Free recv_buf and send_buf
     *      5. Return slot to pool
     * 
     * We cannot call conn_map_foreach AND modify the map during iteration -
     * but disconnect_client calls conn_map_remove which modifies the map.
     * 
     * This is safe here because conn_map_foreach has already captured the fd
     * from the bucket before calling this callback. The removal of that
     * specific entry does not affect iteration of subsequent buckets because
     * conn_map_foreach iterates by index, not by following pointers.
     */
    (void)client;
    disconnect_client(fd);
    ctx->closed++;
}

static void perform_graceful_shutdown(void)
{
    
    log_info("Graceful shutdown initiated (worker pid=%d)", (int)getpid());

    /**
     * Build the TYPE_SYS_SHUTDOWN packet once.
     * All fields in network byte order.
     * Zero payload_len, this packet carries no payload.
     */
    struct packet_header shutdown_hdr;
    memset(&shutdown_hdr, 0, sizeof(shutdown_hdr));
    shutdown_hdr.type = (uint8_t)TYPE_SYS_SHUTDOWN;
    shutdown_hdr.payload_len = htonl(0);
    shutdown_hdr.scope_id = htonl(0);
    shutdown_hdr.sender_id = htonl(0);
    shutdown_hdr.expires_at = htobe64(0);

    /**
     * Pass 1: send TYPE_SYS_SHUTDOWN to all authenticated clients.
     * 
     * Use conn_map_foreach, no direct bucket access.
     * conn_map's internals stay private to conn_map.c.
     */
    struct shutdown_ctx sctx;
    sctx.hdr = &shutdown_hdr;
    sctx.attempted = 0;
    sctx.notified = 0;

    conn_map_foreach(&conn_map, shutdown_callback, &sctx);

    log_info("Shutdown notification: sent to %d/%d client(s) "
        "(%d unauthenticated skipped)", sctx.notified, sctx.attempted,
        sctx.attempted - sctx.notified);

    /**
     * Wait for clients to receive the shutdown notification.
     * 
     * This window is for NOTIFICATION DELIVERY, not data draining. Clients
     * who receive TYPE_SYS_SHUTDOWN will start reconnecting. Clients who do
     * not receive it (full buffer, broken socket) will get a broken pipe
     * when we close their fd below - that is acceptable.
     * 
     * Use sleep_interruptible to guarantee the full window even if signals
     * arrive (e.g., a second SIGTERM from an impatient orchestrator).
     */
    if (sctx.notified > 0) {
        log_info("Waiting 2 seconds for shutdown notifications to deliver...");
        sleep_interruptible(2);
    }

    /**
     * Pass 2: force-close remaining client connections.
     * 
     * Calls epoll_ctl(EPOLL_CTL_DEL) before close() for each fd. This is
     * explicit and correct even though Linux auto-removes closed fds from
     * epoll.
     * 
     * Does NOT use disconnect_client() because that modifies conn_map during
     * iteration. Here we are shutting down entirely, we do not need to
     * maintain conn_map state after this loop.
     */
    struct cleanup_ctx cctx;
    cctx.closed = 0;

    conn_map_foreach(&conn_map, cleanup_callback, &cctx);

    log_info("Force-closed %d client connection(s)", cctx.closed);

    // Close engine sockets and epoll
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    if (health_fd != -1) {
        close(health_fd);
        health_fd = -1;
    }
    if (epfd != -1) {
        close(epfd);
        epfd = -1;
    }

    // Free pool metadata arrays
    free(pool);
    pool = NULL;
    free(free_list);
    free_list = NULL;
    free_count = 0;

    // Clean up libcurl global state
    auth_http_curl_cleanup();

    log_info("Graceful shutdown complete (worker pid=%d)", (int)getpid());
}

/**
 * rate_limit_check - 4-way associative per-IP rate limiter.
 * 
 * Returns 1 if the connection should be allowed.
 * Returns 0 if the connection should be rejected (over limit).
 * 
 * Algorithm:
 *      1. Hash IP to a bucket (bitwise AND, O(1))
 *      2. Search 4 slots for matching IP
 *      3. If found: check/reset window, increment count, apply limit
 *      4. If not found: find empty slot or evict oldest, init
 */
static int rate_limit_check(struct sockaddr_in *addr)
{
    uint32_t ip = addr->sin_addr.s_addr;
    int bucket = (int)(ip & (RATE_MAP_SIZE - 1));
    time_t now = time(NULL);

    struct rate_bucket *b = &rate_map[bucket];

    // Search for an existing slot for this IP
    for (int i = 0; i < RATE_MAP_WAYS; i++) {
        if (!b->slots[i].in_use) continue;
        if (b->slots[i].ip != ip) continue;

        // Found IP's slot
        if (now > b->slots[i].window_start) {
            // Time window expired, reset counter for new window
            b->slots[i].count = 0;
            b->slots[i].window_start = now;
        }

        b->slots[i].count++;

        if (b->slots[i].count > cfg->conn_rate_limit) {
            log_error("Rate limit exceeded ip=%s count=%d limit=%d",
                inet_ntoa(addr->sin_addr), b->slots[i].count,
                cfg->conn_rate_limit);
            return 0;
        }
        return 1;
    }

    // IP has no slot yet, find empty slot or one with oldest window_start
    int target = -1;
    time_t oldest_window = (time_t)LONG_MAX;

    for (int i = 0; i < RATE_MAP_WAYS; i++) {
        if (!b->slots[i].in_use) {
            target = i;
            break; // Choose empty slot over eviction
        }
        if (b->slots[i].window_start < oldest_window) {
            oldest_window = b->slots[i].window_start;
            target = i;
        }
    }

    // Init slot for this IP
    b->slots[target].ip = ip;
    b->slots[target].count = 1;
    b->slots[target].window_start = now;
    b->slots[target].in_use = 1;

    return 1;
}

/**
 * sleep_interruptible - sleep for the specified seconds, resuming
 * automatically if interrupted by a signal.
 * 
 * Standard sleep() returns early when a signal arrives and does not resume.
 * In production, signals may arrive during the shutdown window (e.g., a
 * second SIGTERM from an orchestrator that is impatient). This function
 * guarantees the full sleep duration regardless of signal interruptions.
 * 
 * Uses nanosleep() which provides the remaining time on EINTR, allowing the
 * loop to continue from where it was interrupted.
 */
static void sleep_interruptible(int seconds)
{
    struct timespec remaining;
    remaining.tv_sec = seconds;
    remaining.tv_nsec = 0;

    while (remaining.tv_sec > 0 || remaining.tv_nsec > 0) {
        struct timespec interrupted;
        if (nanosleep(&remaining, &interrupted) == 0)
            break; // completed full sleep without interruption
        if (errno == EINTR) {
            // signal interrupted - continue with remaining time
            remaining = interrupted;
            continue;
        }
        break; // other error - stop sleeping
    }
}