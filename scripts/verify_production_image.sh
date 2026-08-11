#!/usr/bin/env bash
# scripts/verify_production_image.sh
#
# Smoke test for the production Docker image. Run after every Dockerfile
# change and before any release. Fails loudly (non-zero exit) on any check
# failure so it's CI-safe.

set -euo pipefail

IMAGE_TAG="flux-core:verify"

echo "=== Building production image ==="
docker build -f Dockerfile -t "$IMAGE_TAG" .

echo ""
echo "=== Checking image size ==="
SIZE_MB=$(docker image inspect "$IMAGE_TAG" --format='{{.Size}}' | awk '{print int($1/1024/1024)}')
echo "Image size: ${SIZE_MB}MB"
if [ "$SIZE_MB" -gt 150 ]; then
    echo "WARNING: Image exceeds 150MB - investigate what's bloating it"
fi

echo ""
echo "=== Confirming no build toolchain leaked into the final image ==="
for tool in gcc cc make ld; do
    if docker run --rm --entrypoint sh "$IMAGE_TAG" -c "command -v $tool" 2>/dev/null; then
        echo "FAIL: $tool is present in production image - multi-stage build leaked the builder stage"
        exit 1
    fi
done
echo "No build toolchain found. Good."

echo ""
echo "=== Confirming no source code leaked into the final image ==="
if docker run --rm --entrypoint find "$IMAGE_TAG" / -name "*.c" 2>/dev/null | grep -q .; then
    echo "FAIL: .c source files found in production image"
    exit 1
fi
echo "No source files found. Good."

echo ""
echo "=== Confirming the binary is not statically linked unexpectedly ==="
docker run --rm --entrypoint ldd "$IMAGE_TAG" /app/server || {
    echo "FAIL: ldd failed - binary may be malformed"
    exit 1
}

echo ""
echo "=== Starting container and verifying health check ==="
docker rm -f flux-verify 2>/dev/null || true
docker run -d --name flux-verify -p 8080:8080 -p 8081:8081 "$IMAGE_TAG"
sleep 6 # allow start-period to elapse

STATUS=$(docker inspect --format='{{.State.Health.Status}}' flux-verify)
echo "Container health status: $STATUS"
docker logs flux-verify
docker rm -f flux-verify

if [ "$STATUS" != "healthy" ]; then
    echo "FAIL: container did not report healthy"
    exit 1
fi

echo ""
echo "=== All production image checks passed ==="