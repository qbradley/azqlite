#!/bin/bash
# --------------------------------------------------------------------------
# sqlite-objs cross-platform Docker test suite
#
# Run from repo root:
#   ./rust/docker-test/test.sh
#
# Or from any directory:
#   /path/to/repo/rust/docker-test/test.sh
# --------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUST_DIR="$REPO_ROOT/rust"

PASS=0
FAIL=0
SKIP=0

echo "=== sqlite-objs Docker Test Suite ==="
echo "  Repo root : $REPO_ROOT"
echo "  Rust dir  : $RUST_DIR"
echo ""

run_test() {
    local name="$1"
    local dockerfile="$2"
    local image_tag="sqlite-objs-test-${name}"

    echo "--- $name ---"

    echo "  Building image from $dockerfile ..."
    if ! docker build -f "$SCRIPT_DIR/$dockerfile" -t "$image_tag" "$SCRIPT_DIR" 2>&1; then
        echo "  FAIL — image build failed"
        FAIL=$((FAIL + 1))
        return
    fi

    echo "  Running container (rust/ mounted read-only at /rust-crates/) ..."
    if docker run --rm -v "$RUST_DIR:/rust-crates:ro" "$image_tag" 2>&1; then
        echo "  PASS — $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL — $name"
        FAIL=$((FAIL + 1))
    fi
    echo ""
}

# ---- Ubuntu 24.04 ----
run_test "ubuntu24" "Dockerfile.ubuntu24"

# ---- Azure Linux 3 ----
if [ -f "$SCRIPT_DIR/Dockerfile.azurelinux3" ]; then
    run_test "azurelinux3" "Dockerfile.azurelinux3"
else
    echo "--- Azure Linux 3 ---"
    echo "  SKIP — Dockerfile.azurelinux3 not found"
    SKIP=$((SKIP + 1))
    echo ""
fi

# ---- macOS ----
echo "--- macOS ---"
echo "  SKIP — macOS containers are not feasible in Docker."
echo "         Apple's license prohibits running macOS in non-Apple hardware."
echo "         Use native CI runners (GitHub Actions macos-* images) instead."
SKIP=$((SKIP + 1))
echo ""

# ---- Summary ----
echo "=== Results ==="
echo "  Passed  : $PASS"
echo "  Failed  : $FAIL"
echo "  Skipped : $SKIP"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "FAIL — some platforms did not pass"
    exit 1
fi

echo ""
echo "OK — all tested platforms passed"
exit 0
