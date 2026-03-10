#!/bin/bash
#
# Warren's Village — build, start, and teardown.
#
# Usage:
#   ./village.sh          # teardown old, build, start village + human REPL
#   ./village.sh stop     # just teardown
#   ./village.sh start    # start without rebuild (must be built already)
#   ./village.sh build    # just build village + human CLI
#   ./village.sh all      # stop, build all targets (including tests)
#

set -e

DB="/tmp/warren_village.db"
PID_FILE="/tmp/warren_village.pid"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

STORE_EP="tcp://127.0.0.1:5560"
SMITH_EP="tcp://127.0.0.1:5590"
COBBLER_EP="tcp://127.0.0.1:5591"
MESSENGER_EP="tcp://127.0.0.1:5592"

# Agent endpoints
GEE_REP="tcp://127.0.0.1:5570"
INCH_REP="tcp://127.0.0.1:5571"
LOOM_REP="tcp://127.0.0.1:5572"
CLAUDE_REP="tcp://127.0.0.1:5573"
GEE_PUB="tcp://127.0.0.1:5580"
INCH_PUB="tcp://127.0.0.1:5581"
LOOM_PUB="tcp://127.0.0.1:5582"
CLAUDE_PUB="tcp://127.0.0.1:5583"

stop_village() {
    echo "stopping village..."
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID"
            # Wait for cleanup
            for i in $(seq 1 10); do
                kill -0 "$PID" 2>/dev/null || break
                sleep 0.2
            done
            # Force if still alive
            kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
        fi
        rm -f "$PID_FILE"
    fi
    # Clean up any orphan processes on our ports
    for port in 5560 5570 5571 5572 5573 5580 5581 5582 5583 5590 5591 5592; do
        lsof -ti :$port 2>/dev/null | xargs kill 2>/dev/null || true
    done
    rm -f "$DB" "${DB}-wal" "${DB}-shm"
    echo "village stopped."
}

build_village() {
    if ! command -v cmake &>/dev/null; then
        echo "ERROR: cmake not found. Install with: brew install cmake (macOS) or sudo apt install cmake (Debian/Ubuntu)"
        exit 1
    fi
    echo "building..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Debug
    cmake --build . --target warren_village --target strata_human_cli -j4
    echo "build complete."
}

start_village() {
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "village already running (pid $(cat "$PID_FILE"))"
        return 1
    fi

    echo "starting village..."
    cd "$PROJECT_DIR"
    "$BUILD_DIR/warren_village" &
    sleep 3

    if [ ! -f "$PID_FILE" ]; then
        echo "ERROR: village failed to start"
        return 1
    fi

    echo "village running (pid $(cat "$PID_FILE"))"
}

run_human() {
    echo ""
    echo "=== Warren entering the village ==="
    echo "  store: $STORE_EP"
    echo "  events from: gee ($GEE_PUB), inch ($INCH_PUB), loom ($LOOM_PUB)"
    echo ""
    echo "  Talk to agents:  talk gee hello!"
    echo "  Talk to claude:  talk claude hello!"
    echo ""

    "$BUILD_DIR/strata-human" \
        --endpoint "$STORE_EP" \
        --entity warren \
        --sub "$GEE_PUB" \
        --topic "town-hall/" \
        --agent "gee=$GEE_REP" \
        --agent "inch=$INCH_REP" \
        --agent "loom=$LOOM_REP" \
        --agent "claude=$CLAUDE_REP" \
        --agent "code-smith=$SMITH_EP" \
        --agent "cobbler=$COBBLER_EP" \
        --agent "messenger=$MESSENGER_EP"
}

# --- Main ---

case "${1:-}" in
    stop)
        stop_village
        ;;
    build)
        build_village
        ;;
    all)
        stop_village
        if ! command -v cmake &>/dev/null; then
            echo "ERROR: cmake not found. Install with: brew install cmake (macOS) or sudo apt install cmake (Debian/Ubuntu)"
            exit 1
        fi
        echo "building all targets..."
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Debug
        cmake --build . -j4
        echo "build complete."
        ;;
    start)
        start_village
        ;;
    human)
        run_human
        ;;
    *)
        # Full cycle: stop, build, start, human
        stop_village
        build_village
        start_village
        run_human
        # When human exits, stop the village
        stop_village
        ;;
esac
