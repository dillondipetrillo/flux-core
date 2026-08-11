#!/usr/bin/env bash
# scripts/run_shutdown_test.sh
#
# Orchestrates test_shutdown_integration: starts the production
# container, runs the standalone shutdown test binary (a Linux binary,
# run inside a throwaway container since the host may be macOS) in the
# background, watches for its sentinel file, sends SIGTERM to the
# engine container at exactly the right moment, checks the test's exit
# code, cleans up regardless of outcome.

set -uo pipefail

SENTINEL_DIR="$(pwd)/tmp_shutdown_sentinel"
SENTINEL_PATH="$SENTINEL_DIR/shutdown_test_ready"
COMPOSE_FILE="docker-compose.integration.yaml"
MAX_WAIT_FOR_SENTINEL=20

cleanup() {
    echo "Tearing down container..."
    docker compose -f "$COMPOSE_FILE" down -v
    rm -rf "$SENTINEL_DIR"
}
trap cleanup EXIT

echo "=== Building test_shutdown_integration binary (Linux, via disposable container) ==="
# tests/run_shutdown_integration is a Linux ELF binary and must be
# compiled with a Linux toolchain - the Mac host has none. Compile it
# inside a throwaway container using the same Ubuntu base as the dev
# image, mounting the project directory so the compiled binary lands
# back on disk at tests/run_shutdown_integration, exactly where the
# rest of this script expects to find it.
docker run --rm \
    -v "$(pwd)":/app -w /app \
    ubuntu:24.04 \
    bash -c "apt-get update -qq && apt-get install -y -qq gcc make libssl-dev libcurl4-openssl-dev >/dev/null && make tests/run_shutdown_integration"

if [ ! -f "tests/run_shutdown_integration" ]; then
    echo "FAIL: tests/run_shutdown_integration was not built"
    exit 1
fi
chmod +x tests/run_shutdown_integration
echo "Build complete."

rm -rf "$SENTINEL_DIR"
mkdir -p "$SENTINEL_DIR"

echo ""
echo "=== Building and starting production engine container ==="
docker compose -f "$COMPOSE_FILE" up -d --build

echo ""
echo "=== Waiting for container health ==="
ENGINE_CID=""
HEALTHY="false"
for i in $(seq 1 15); do
    ENGINE_CID=$(docker compose -f "$COMPOSE_FILE" ps -q engine)
    if [ -n "$ENGINE_CID" ]; then
        STATUS=$(docker inspect --format='{{.State.Health.Status}}' "$ENGINE_CID" 2>/dev/null || echo "")
        if [ "$STATUS" = "healthy" ]; then
            HEALTHY="true"
            break
        fi
    fi
    sleep 1
done

if [ "$HEALTHY" != "true" ]; then
    echo "FAIL: container never became healthy"
    docker compose -f "$COMPOSE_FILE" logs engine
    exit 1
fi
echo "Container is healthy."

echo ""
echo "=== Starting standalone shutdown test binary (in a throwaway container) ==="
docker run --rm \
    --network container:"$ENGINE_CID" \
    -v "$(pwd)":/app -w /app \
    -e SENTINEL_DIR=/app/tmp_shutdown_sentinel \
    ubuntu:24.04 \
    ./tests/run_shutdown_integration &
TEST_PID=$!

echo "Waiting for sentinel file (test is blocked in recv())..."
WAITED=0
while [ ! -f "$SENTINEL_PATH" ]; do
    sleep 0.5
    WAITED=$((WAITED + 1))
    if [ "$WAITED" -ge $((MAX_WAIT_FOR_SENTINEL * 2)) ]; then
        echo "FAIL: sentinel file never appeared within ${MAX_WAIT_FOR_SENTINEL}s"
        kill "$TEST_PID" 2>/dev/null
        exit 1
    fi
done

echo "Sentinel detected. Sending SIGTERM to container..."
docker compose -f "$COMPOSE_FILE" kill -s SIGTERM engine

echo "Waiting for test binary to finish..."
wait "$TEST_PID"
TEST_EXIT=$?

if [ "$TEST_EXIT" -ne 0 ]; then
    echo "Shutdown test FAILED. Container logs:"
    docker compose -f "$COMPOSE_FILE" logs engine
fi

exit "$TEST_EXIT"