# Flux Core (C State Bus)

A high-performance, application-agnostic binary packet routing engine
written in C. Flux Core handles WebSocket-free raw TCP transport,
scope-based publish/subscribe routing, and connection lifecycle
management — nothing else. It is intentionally "dumb": no application
logic, no data persistence, no opinions about what your packets mean.

## The Philosophy: "Dumb" for Speed

Flux Core does exactly one job — move framed binary packets between
connected clients, grouped by scope, as fast as the kernel allows. It
does not know what a "task," "message," or "document" is. That
separation is deliberate: a transport layer that doesn't understand
your application's semantics can be radically simpler, faster, and
more reliable than one that does. Application logic belongs in the
layer above Flux Core, not inside it.

## Features

- Non-blocking, edge-triggered epoll event loop
- Binary framed protocol with zero-copy parsing on the hot path
- Scope-based pub/sub routing (join/leave/broadcast)
- Multi-process horizontal scaling via `SO_REUSEPORT`
- Pluggable authentication: JWT (HMAC-SHA256), HTTP callback, or
  development mode
- Per-IP connection rate limiting (4-way associative, collision-hardened)
- TTL-based packet expiry
- Async logging (lock-free ring buffer, zero hot-path disk I/O) with a
  separate synchronous billing log for revenue-critical events
- Graceful shutdown with client notification
- Per-worker health check endpoint

## Wire Protocol

### Packet Header (21 bytes, network byte order / big-endian)

This layout is `__attribute__((packed))` — there is no padding between
fields. SDK implementers in non-C languages must serialize and
deserialize using these exact byte offsets, not their language's
native struct alignment rules.

| Offset | Size | Field         | Type       | Notes                                    |
|--------|------|---------------|------------|-------------------------------------------|
| 0      | 1    | `type`        | `uint8_t`  | See Packet Types below                    |
| 1      | 4    | `payload_len` | `uint32_t` | Length in bytes of the payload following  |
| 5      | 4    | `scope_id`    | `uint32_t` | Routing scope (room/channel) identifier   |
| 9      | 4    | `sender_id`   | `uint32_t` | Server-stamped; clients must not set this |
| 13     | 8    | `expires_at`  | `uint64_t` | Unix timestamp; `0` = never expires       |

Total header size: **21 bytes**. This is enforced at build time by a
unit test (`test_documented_byte_offsets_match_struct` in
`tests/test_protocol.c`) that asserts `sizeof`/`offsetof` against this
table — if the struct ever changes without this table being updated,
the test suite fails.

The payload (`payload_len` bytes) immediately follows the header, with
no padding or delimiter.

### Packet Types

| Value | Name                   | Direction       | Meaning                                          |
|-------|------------------------|-----------------|---------------------------------------------------|
| 1     | `TYPE_SYS_IDENTIFY`    | client → server | Authenticate this connection with a token         |
| 2     | `TYPE_SYS_JOIN`        | client → server | Subscribe to a scope                              |
| 3     | `TYPE_SYS_PING`        | client → server | Keepalive; server replies with `TYPE_SYS_PING`    |
| 4     | `TYPE_SYS_ACK`         | server → client | Generic acknowledgement                           |
| 5     | `TYPE_SYS_ERROR`       | server → client | Generic error response                            |
| 6     | `TYPE_SYS_LEAVE`       | client → server | Unsubscribe from a scope                          |
| 7     | `TYPE_SYS_SHUTDOWN`    | server → client | Server is shutting down; connection will close    |
| 10    | `TYPE_APP_REALTIME`    | either          | Application data, realtime priority               |
| 11    | `TYPE_APP_STANDARD`    | either          | Application data, standard priority               |
| 12    | `TYPE_APP_BACKGROUND`  | either          | Application data, background priority             |

Priority levels (`REALTIME`/`STANDARD`/`BACKGROUND`) are currently
routing-equivalent — Flux Core does not yet apply different scheduling
per priority level. The distinction exists in the protocol for
forward compatibility.

### Status Codes

Returned in a `response_payload` (a single `uint32_t status_code`,
4 bytes, packed) following a `TYPE_SYS_ACK` or `TYPE_SYS_ERROR` header.

| Value | Name                          | Returned when...                                                   |
|-------|-------------------------------|------------------------------------------------------------------------|
| 100   | `STATUS_OK`                   | The operation succeeded                                            |
| 401   | `STATUS_ERR_UNIDENTIFIED`     | A non-`IDENTIFY` packet arrived before authentication               |
| 402   | `STATUS_ERR_ALREADY_ID`       | A second `TYPE_SYS_IDENTIFY` arrived on an already-authenticated connection |
| 403   | `STATUS_ERR_AUTH_FAILED`      | The configured auth hook rejected the provided token                |
| 404   | `STATUS_ERR_NOT_IN_ROOM`      | `TYPE_SYS_LEAVE` was sent for a scope the connection hadn't joined  |
| 405   | `STATUS_ERR_ALREADY_IN_ROOM`  | `TYPE_SYS_JOIN` was sent for a scope already joined                 |
| 406   | `STATUS_ERR_SCOPES_FULL`      | The connection has already joined `MAX_SCOPES` (16) scopes          |
| 407   | `STATUS_ERR_ROOM_FULL`        | The target scope has reached its subscriber capacity                |
| 410   | `STATUS_ERR_EXPIRED`          | The packet's `expires_at` was in the past when routed                |
| 413   | `STATUS_ERR_PAYLOAD_SIZE`     | `payload_len` exceeded `MAX_PAYLOAD` (1024 bytes)                    |

### Protocol Constants

| Constant         | Value  | Meaning                                             |
|------------------|--------|------------------------------------------------------|
| `MAX_BUFF_SIZE`   | 8192   | Per-client receive buffer size, in bytes            |
| `MAX_SEND_BUFF`   | 65536  | Per-client outbound send queue size, in bytes       |
| `MAX_PAYLOAD`     | 1024   | Maximum payload size accepted for a single packet   |
| `MAX_SCOPES`      | 16     | Maximum scopes a single connection may join at once |
| `MAX_CLIENTS`     | 10000  | Default connection pool size per worker             |

### Protocol Stability

The packet header layout, status codes, and packet types documented
above are locked for the v1 protocol. Any future breaking change
(field reordering, size changes, semantic changes to existing status
codes) will be introduced as a new protocol version with an explicit
version negotiation mechanism — not as a silent change to v1. Adding
new packet types or status codes with new numeric values is
non-breaking and may happen without a version bump.

## Logging

Every log line follows this format:
[YYYY-MM-DD HH:MM:SS] LEVEL pid=<worker_pid> conn_id=<uint64> fd=<int> user_id=<uint32> <message>

- `pid` identifies which worker process emitted the line — necessary
  because multiple workers run concurrently, each with its own
  independent file descriptor table.
- `conn_id` is a monotonically increasing identifier assigned once per
  connection, at accept time, and never reused for the lifetime of the
  worker process. Unlike `fd`, which the kernel recycles as connections
  close and new ones open, `conn_id` uniquely identifies a single
  connection's entire timeline in the logs.
- `fd` is the raw kernel file descriptor, useful for correlating with
  OS-level tools (`strace`, `lsof`, `tcpdump`).
- `user_id` is `0` until the connection successfully authenticates.
  A `user_id` of `0` never refers to a real authenticated user.

Log lines without an associated connection (startup, shutdown,
listener-level errors) omit `conn_id`/`fd`/`user_id` entirely.

## Deployment Sizing Guide

Each worker process is a fully independent OS process (not a thread),
using `SO_REUSEPORT` for kernel-level load balancing across workers.
Workers do not share connection state or memory.

**Idle memory per worker:** ~2MB. Receive and send buffers
(`MAX_BUFF_SIZE` + `MAX_SEND_BUFF` = 72KB combined) are allocated only
for active connections, not pre-allocated for the full connection pool.

**Memory under load, per worker:**
idle_baseline + (active_connections × 72KB)

A worker handling 1,000 simultaneous active connections needs roughly
72MB beyond the idle baseline.

**Choosing `ENGINE_WORKER_COUNT`:**

- `0` — auto-detect CPU core count. Verify available RAM covers
  `cores × ENGINE_MAX_CLIENTS × ~75KB` before using this in production;
  the server logs this exact calculation at startup as a reminder.
- `1` — single worker. Required during development, and required for
  any deployment where clients in the same scope must be able to reach
  each other, since scope routing is currently worker-local (see
  constraint below).
- `N` — a specific worker count for manual tuning.

**Known constraint — cross-worker routing:** two clients only receive
each other's messages if they are connected to the *same* worker
process. `SO_REUSEPORT` distributes new connections across workers at
the kernel level with no guarantee that two specific clients land on
the same one. If your deployment requires clients in a shared scope to
reliably communicate with each other, run `ENGINE_WORKER_COUNT=1`
until a cross-worker routing mechanism ships (planned for a future
phase). This is a deliberate
current design tradeoff for per-worker performance and simplicity, not
an oversight — but it must be accounted for when planning a deployment
topology.

## Authentication

Configured via environment variables, selected in priority order:

1. **JWT** (`ENGINE_AUTH_JWT_SECRET` set) — validates tokens as
   HMAC-SHA256-signed JWTs against the given secret.
2. **HTTP callback** (`ENGINE_AUTH_HTTP_URL` set, JWT secret unset) —
   forwards the token to your own auth service at the given URL via
   libcurl; your service returns valid/invalid and a `user_id`.
3. **Development mode** (neither set) — accepts every token
   unconditionally. Logs a loud repeated warning on every connection.
   **Never use in production.**

If both `ENGINE_AUTH_JWT_SECRET` and `ENGINE_AUTH_HTTP_URL` are set,
JWT takes priority.

## Configuration Reference

All configuration is via environment variables (see `include/config.h`
and `src/config.c` for the authoritative source).

| Variable                        | Default              | Meaning                                    |
|----------------------------------|-----------------------|----------------------------------------------|
| `ENGINE_PORT`                    | `8080`                | Main protocol listener port                |
| `ENGINE_HEALTH_PORT`             | `8081`                | Health check listener port                 |
| `ENGINE_BACKLOG`                 | `128`                 | TCP listen backlog                         |
| `ENGINE_MAX_CLIENTS`             | `10000`               | Connection pool size per worker            |
| `ENGINE_MAX_EVENTS`              | `1024`                | epoll max events per `epoll_wait` call     |
| `ENGINE_WORKER_COUNT`            | `0` (auto-detect)     | See Deployment Sizing Guide above          |
| `ENGINE_CONN_RATE_LIMIT`         | `10`                  | Max new connections per second per IP      |
| `ENGINE_SEND_BUF_SIZE`           | `262144`              | Kernel socket send buffer (`SO_SNDBUF`)    |
| `ENGINE_RECV_BUF_SIZE`           | `262144`              | Kernel socket recv buffer (`SO_RCVBUF`)    |
| `ENGINE_TCP_KEEPALIVE_IDLE`      | `60`                  | Seconds idle before first keepalive probe  |
| `ENGINE_TCP_KEEPALIVE_INTVL`     | `10`                  | Seconds between keepalive probes           |
| `ENGINE_TCP_KEEPALIVE_CNT`       | `3`                   | Failed probes before connection is dropped |
| `ENGINE_LOG_PATH`                | `logs/server.log`     | Main log file path                         |
| `ENGINE_BILLING_LOG_PATH`        | `logs/billing.log`    | Billing/usage event log path (JSON lines)  |
| `ENGINE_AUTH_JWT_SECRET`         | *(unset)*             | Enables JWT auth mode                      |
| `ENGINE_AUTH_HTTP_URL`           | *(unset)*              | Enables HTTP callback auth mode            |
| `ENGINE_AUTH_HTTP_TIMEOUT_MS`    | `500`                 | Timeout for HTTP auth callback requests    |
| `ENGINE_METRICS_SOCKET`          | *(unset)*             | Reserved for the future Observer sidecar   |

## Building and Running

### Development

```bash
docker compose -f docker-compose.dev.yaml up --build
```

### Production Image

```bash
docker build -f Dockerfile -t flux-core:latest .
docker run -d \
  -p 8080:8080 -p 8081:8081 \
  -e ENGINE_WORKER_COUNT=1 \
  -e ENGINE_AUTH_JWT_SECRET=your-secret-here \
  flux-core:latest
```

### From Source

```bash
make server_opt   # optimized production binary, -O2, no debug symbols
./server_opt
```

## Testing

```bash
make test                                          # all unit tests
make integration                                    # integration tests (requires a running dev server)
./scripts/verify_production_image.sh                 # production image smoke test
./scripts/run_integration_against_production.sh       # integration tests against the production container
./scripts/run_shutdown_test.sh                        # graceful shutdown lifecycle test
```

See the project's testing strategy documentation for the full,
ordered verification procedure covering every phase built to date.

## Health Checks

The engine exposes a plain-text health endpoint on `ENGINE_HEALTH_PORT`
(default `8081`). It accepts any TCP connection and responds with:
STATUS OK
WORKER_PID <pid>
CONNECTIONS <count>
POOL_FREE <count>
POOL_MAX <count>
UPTIME <seconds>

This is not an HTTP endpoint — it does not speak HTTP status codes or
headers. Use a raw TCP check (e.g. `nc -z host port`), not an HTTP
health checker expecting a `200 OK`.