#!/bin/bash

set -eu

IMAGE="omcproxy-test"

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

echo "Building test image..."

docker build \
    -f "$PROJECT_DIR/Dockerfile.test" \
    -t "$IMAGE" \
    "$PROJECT_DIR"

echo "Running integration test..."

docker run \
    --rm \
    --cap-add=NET_ADMIN \
    --sysctl net.ipv4.conf.all.rp_filter=0 \
    --sysctl net.ipv4.conf.default.rp_filter=0 \
    --sysctl net.ipv4.conf.all.accept_local=1 \
    --sysctl net.ipv4.conf.default.accept_local=1 \
    "$IMAGE"
