#!/bin/bash
# JLCPCB Gerber Export Script
# Usage: ./export_gerber.sh <board.kicad_pcb> [output_dir]
#
# Generates all files needed for JLCPCB order:
#   - Gerber files (F.Cu, B.Cu, F.Mask, B.Mask, F.SilkS, B.SilkS, Edge.Cuts)
#   - Excellon drill files
#   - Component placement (CPL) file
#
# Output is zipped for direct upload to jlcpcb.com

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <board.kicad_pcb> [output_dir]"
    echo "Example: $0 grove_shield_v3.kicad_pcb gerber_grove"
    exit 1
fi

BOARD="$1"
BOARD_NAME=$(basename "$BOARD" .kicad_pcb)
OUTPUT_DIR="${2:-gerber_${BOARD_NAME}}"

# Validate input
if [ ! -f "$BOARD" ]; then
    echo "Error: Board file not found: $BOARD"
    exit 1
fi

echo "=== JLCPCB Gerber Export ==="
echo "Board: $BOARD"
echo "Output: $OUTPUT_DIR/"
echo ""

# Clean output directory
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# --- Step 1: Gerber files ---
echo "[1/4] Exporting Gerber files..."
kicad-cli pcb export gerbers "$BOARD" \
    --output "$OUTPUT_DIR/" \
    --layers "F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,Edge.Cuts,F.Paste,B.Paste" \
    --precision 6 \
    --no-x2 \
    --subtract-soldermask \
    --exclude-value
echo "  Done."

# --- Step 2: Drill files ---
echo "[2/4] Exporting drill files..."
kicad-cli pcb export drill "$BOARD" \
    --output "$OUTPUT_DIR/" \
    --format excellon \
    --excellon-units mm \
    --excellon-zeros-format decimal \
    --excellon-oval-format alternate \
    --excellon-separate-th \
    --generate-map \
    --map-format gerberx2
echo "  Done."

# --- Step 3: Component placement (CPL) ---
echo "[3/4] Exporting component placement..."
kicad-cli pcb export pos "$BOARD" \
    --output "$OUTPUT_DIR/${BOARD_NAME}-all-pos.csv" \
    --format csv \
    --units mm \
    --side both
echo "  Done."

# --- Step 4: ZIP for JLCPCB upload ---
echo "[4/4] Creating ZIP archive..."
ZIP_PATH="$(dirname "$OUTPUT_DIR")/${BOARD_NAME}_jlcpcb.zip"
(cd "$OUTPUT_DIR" && zip -q "$ZIP_PATH" *)
echo "  Done: $ZIP_PATH"

echo ""
echo "=== Export Complete ==="
echo "Files in $OUTPUT_DIR/:"
ls -la "$OUTPUT_DIR/"
echo ""
echo "ZIP archive: $ZIP_PATH ($(du -h "$ZIP_PATH" | cut -f1))"
echo ""
echo "Next: Upload $ZIP_PATH to https://cart.jlcpcb.com/quote"
