#!/usr/bin/env bash
# scripts/run_integration_against_production.sh
#
# Brings up the production container, waits for health, runs the existing
# test_integration suite against it, tears down regardless of pass/fail
# so no orphaned containers are left behind.

set -uo pipefail # not -e: we need to capture the test exit code, not abort on it

COMPOSE_FILE="docker-compose.integration.yaml"

cleanup() {
    echo "Tearing down..."
    docker-compose -f "$COMPOSE_FILE" down -v
}
trap cleanup EXIT

echo "=== Building and starting production engine container ==="
docker-compose -f "$COMPOSE_FILE" up -d --build

echo ""
echo "=== Waiting for health check ==="
HEALTHY="false"
for i in $(seq 1 15); do
    ENGINE_CID=$(docker-compose -f "$COMPOSE_FILE" ps -q engine)
    STATUS=$(docker inspect --format='{{.State.Health.Status}}' "$ENGINE_CID" 2>/dev/null || echo "")
    if [ "$STATUS" = "healthy" ]; then
        HEALTHY="true"
        echo "Engine is healthy."
        break
    fi
    sleep 1
done

if ["$HEALTHY" != "true"]; then
    echo "FAIL: engine never became healthy."
    docker-compose -f "$COMPOSE_FILE" logs engine
    exit 1
fi

echo ""
echo "=== Running integration test suite against production container ==="
docker run --rm \
    --network container:"$ENGINE_CID" \
    -v "$(pwd)":/app -w /app \
    ubuntu:24.04 \
    ./tests/run_integration
TEST_EXIT=$?

if [ "$TEST_EXIT" -ne 0 ]; then
    echo "Integration tests FAILED against production image. Container logs:"
    docker-compose -f "$COMPOSE_FILE" logs engine
fi

exit "$TEST_EXIT"