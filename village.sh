#!/bin/bash
#
# Warren's Village — build, start, and teardown.
#
# Usage:
#   ./village.sh          # teardown old, build, start village + human REPL
#   ./village.sh stop     # just teardown
#   ./village.sh start    # start without rebuild (must be built already)
#   ./village.sh build    # just build
#

set -e

DB="/tmp/warren_village.db"
PID_FILE="/tmp/warren_village.pid"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

STORE_EP="tcp://127.0.0.1:5560"
SMITH_EP="tcp://127.0.0.1:5590"

# Agent endpoints
GEE_REP="tcp://127.0.0.1:5570"
INCH_REP="tcp://127.0.0.1:5571"
LOOM_REP="tcp://127.0.0.1:5572"
GEE_PUB="tcp://127.0.0.1:5580"
INCH_PUB="tcp://127.0.0.1:5581"
LOOM_PUB="tcp://127.0.0.1:5582"

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
    for port in 5560 5570 5571 5572 5580 5581 5582 5590; do
        lsof -ti :$port 2>/dev/null | xargs kill 2>/dev/null || true
    done
    rm -f "$DB" "${DB}-wal" "${DB}-shm"
    echo "village stopped."
}

build_village() {
    echo "building..."
    cd "$BUILD_DIR"
    cmake "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
    cmake --build . --target warren_village --target strata_human_cli -j4 2>&1 | tail -5
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
    echo ""

    "$BUILD_DIR/strata-human" \
        --endpoint "$STORE_EP" \
        --entity warren \
        --sub "$GEE_PUB" \
        --topic "town-hall/" \
        --agent "gee=$GEE_REP" \
        --agent "inch=$INCH_REP" \
        --agent "loom=$LOOM_REP" \
        --agent "code-smith=$SMITH_EP"
}

# --- Main ---

case "${1:-}" in
    stop)
        stop_village
        ;;
    build)
        build_village
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
