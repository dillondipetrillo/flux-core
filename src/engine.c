#include <arpa/inet.h>
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

static struct scope_map scope_map;
static struct conn_map conn_map;

// Memory pool - pre-allocated client_info structs.
#define POOL_MAX 10000
static struct client_info pool[POOL_MAX];
static struct client_info *free_list[POOL_MAX];
static int free_count = 0;

// Rate limit - epoll connections per IP    
#define RATE_MAP_SIZE 256
struct rate_entry {
    uint32_t ip;
    int count;
    time_t window_start;
};
static struct rate_entry rate_map[RATE_MAP_SIZE];

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
    struct client_info *c = free_list[--free_count];
    memset(c, 0, sizeof(*c));
    return c;
}

static void pool_free(struct client_info *client)
{
    if (!client) return;
    memset(client, 0, sizeof(*client));
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
    // Check if data fits in the send buffer
    if (client->send_offset + client->send_len + len > MAX_SEND_BUFF) {
        log_error("Send buffer overflow fd=%d, disconnecting",
            client->socket_fd);
        disconnect_client(client->socket_fd);
        return -1;
    }

    memcpy(client->send_buf + client->send_offset + client->send_len, data,
        len);
    client->send_len += len;

    // Register for EPOLLOUT to drain the queue when writeable
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = client->socket_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, client->socket_fd, &ev);
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

    engine_send(fd, (const char *)&hdr, sizeof(hdr));
    engine_send(fd, (const char *)&rp, sizeof(rp));
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
        log_info("Disconnected fd=%d user_id=%u bytes_sent=%lu bytes_recv=%lu",
            fd, client->user_id, (unsigned long)client->bytes_sent,
            (unsigned long)client->bytes_recv);
    else
        log_info("Disconnected unauthenticated fd=%d", fd);

    billing_log_disconnect(fd, client->user_id, client->bytes_sent,
        client->bytes_recv);

    // Clean up client from all joined scopes
    for (int i = 0; i < client->scope_count; i++)
        scope_map_remove(&scope_map, client->scopes[i], fd);

    conn_map_remove(&conn_map, fd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
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
    }

    process_recv_buffer(client, fd);
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
    while (client->recv_len >= sizeof(struct packet_header)) {
        // Peek, don't consume yet
        struct packet_header hdr;
        memcpy(&hdr, client->recv_buf, sizeof(hdr));

        // Convert from network byte order to host byte order
        uint32_t payload_len = ntohl(hdr.payload_len);
        uint32_t scope_id = ntohl(hdr.scope_id);
        uint64_t expires_at = be64toh(hdr.expires_at);
        uint8_t type = hdr.type;

        if (payload_len > MAX_PAYLOAD) {
            log_error("Oversized payload %u fd=%d, disconnecting", payload_len,
                fd);
            disconnect_client(fd);
            return;
        }

        size_t total = sizeof(struct packet_header) + payload_len;
        if (client->recv_len < total) break; // Wait for more data

        // Complete packet available
        char *payload = client->recv_buf + sizeof(struct packet_header);

        // Server stamps sender_id, client cannot spoof identity
        uint32_t sender_id = (uint32_t)fd;

        dispatch_packet(client, fd, type, scope_id, sender_id, expires_at,
            payload, payload_len);

        // Remove processed packet from buffer
        memmove(client->recv_buf, client->recv_buf + total,
            client->recv_len - total);
        client-> recv_len -= total;
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
            client->client_id = (uint32_t)fd;

            log_info("Authenticated fd=%d user_id=%d", fd, result.user_id);
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
            log_info("fd=%d joined scope=%u", fd, scope_id);
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
            log_info("fd=%d left scope=%u", fd, scope_id);
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
                log_info("TTL expired fd=%d scope=%u", fd, scope_id);
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
        engine_send(fds[i], (const char *)&out, sizeof(out));
        if (payload_len > 0)
            engine_send(fds[i], payload, payload_len);
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
        client->send_len += (size_t)n;
    }

    // Queue drained - reset and stop watching writability
    client->send_offset = 0;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
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

static void perform_graceful_shutdown(void)
{
    log_info("Graceful shutdown initiated");

    auth_http_curl_cleanup();

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

    log_info("Graceful shutdown complete");
}

/**
 * rate_limit_checl - reject connections from IPs exceeding the limit.
 * 
 * Returns 1 if the connection should be allowed.
 * Returns 0 if the connection should be rejected.
 */
static int rate_limit_check(struct sockaddr_in *addr)
{
    uint32_t ip = addr->sin_addr.s_addr;
    int slot = (int)(ip % RATE_MAP_SIZE);
    time_t now = time(NULL);

    // If the slot holds a different IP, reset it
    if (rate_map[slot].ip != ip) {
        rate_map[slot].ip = ip;
        rate_map[slot].count = 0;
        rate_map[slot].window_start = now;
    }

    // If the time window has passed, reset the counter
    if (now > rate_map[slot].window_start) {
        rate_map[slot].count = 0;
        rate_map[slot].window_start = now;
    }

    rate_map[slot].count++;

    if (rate_map[slot].count > cfg->conn_rate_limit) {
        log_error("Rate limit exceeded ip=%s count=%d limit=%d",
            inet_ntoa(addr->sin_addr), rate_map[slot].count,
            cfg->conn_rate_limit);
        return 0;
    }
    return 1;
}