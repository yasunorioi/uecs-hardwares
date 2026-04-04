#!/bin/bash
# Automated PCB routing pipeline
# Usage: ./autoroute.sh <board.kicad_pcb> [--exclude NET1 NET2 ...]
#
# Steps:
#   1. Export DSN from KiCad PCB
#   2. Run FreeRouting autorouter (headless CLI)
#   3. Results saved as .ses file
#
# After running, import .ses in pcbnew:
#   File -> Import -> Specctra Session

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <board.kicad_pcb> [--exclude NET1 NET2 ...]"
    echo ""
    echo "Example:"
    echo "  $0 grove_shield_v3.kicad_pcb --exclude 'Net-(J2-Pin_18)' 'Net-(J2-Pin_16)'"
    exit 1
fi

BOARD="$1"
BOARD_NAME=$(basename "$BOARD" .kicad_pcb)
BOARD_DIR="$(cd "$(dirname "$BOARD")" && pwd)"
DSN="${BOARD_DIR}/${BOARD_NAME}.dsn"
SES="${BOARD_DIR}/${BOARD_NAME}.ses"

shift
EXCLUDE_ARGS="$@"

echo "=== PCB Auto-Route Pipeline ==="
echo "Board: $BOARD"
echo ""

# Step 1: Export DSN
echo "[1/2] Exporting Specctra DSN..."
python3 "${SCRIPT_DIR}/export_dsn.py" "$BOARD" "$DSN" $EXCLUDE_ARGS
echo ""

# Step 2: FreeRouting
echo "[2/2] Running FreeRouting autorouter..."
java -Djava.awt.headless=true \
    -jar "${SCRIPT_DIR}/freerouting.jar" \
    -de "$DSN" \
    -do "$SES" \
    -mp 100 2>&1 | grep -E 'INFO|ERROR|WARN' | tail -20

echo ""
echo "=== Done ==="
echo "Session file: $SES"
echo ""
echo "Next: Open $BOARD in pcbnew, then:"
echo "  File -> Import -> Specctra Session -> select $SES"
echo "  Inspect -> Design Rules Checker"
