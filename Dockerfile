# ===========================================================
# Stage 1: Builder
# Compiles the engine using the real Makefile target 'server_opt'.
# This entire stage is discarded after build. Nothing here -
# compiler, headers, source files - ships in the final image.
# ===========================================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libssl-dev \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY include/ include/
COPY src/ src/
COPY Makefile Makefile

RUN make clean && make server_opt

# ===========================================================
# Stage 2: Runtime
# Minimal image. Only the compiled binary, its runtime shared
# library dependencies, and netcat for the health check.
# ===========================================================
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libcurl4 \
    netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

# Non-root user
RUN groupadd -r flux && useradd -r -g flux flux

WORKDIR /app
COPY --from=builder /build/server_opt /app/server

# logs/ is where ENGINE_LOG_PATH and ENGINE_BILLING_LOG_PATH default to
# (logs/server.log, logs/billing.log - see config.c). Must exist and be
# writable by the non-root 'flux' user before the process starts.
RUN mkdir -p /app/logs && chown -R flux:flux /app

USER flux

EXPOSE 8080 8081

ENV ENGINE_PORT=8080 \
    ENGINE_HEALTH_PORT=8081 \
    ENGINE_WORKER_COUNT=1

# nc -z: opens a TCP connection to the health port and closes it
# immediately. Matches the health handler's actual behavior (it replies
# to any accept())
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD nc -z localhost 8081 || exit 1

ENTRYPOINT ["/app/server"]