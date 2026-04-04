#!/usr/bin/env python3
"""Optimize SMD component placement for Grove Shield v3.

Moves R1-R7, C1-C3, D1, Q1 within the header gap zone,
tries multiple rotations, runs FreeRouting each time,
and picks the placement with fewest unrouted nets.

Usage:
    python3 optimize_placement.py [--iterations N]
"""

import pcbnew
import os
import sys
import subprocess
import random
import re
import shutil
import math

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BOARD_FILE = os.path.join(SCRIPT_DIR, 'grove_shield_v3.kicad_pcb')
DSN_FILE = os.path.join(SCRIPT_DIR, 'grove_shield_v3.dsn')
SES_FILE = os.path.join(SCRIPT_DIR, 'grove_shield_v3.ses')
BEST_FILE = os.path.join(SCRIPT_DIR, 'grove_shield_v3_best.kicad_pcb')
FP_PATH = '/usr/share/kicad/footprints'

BOARD_W = 56.0
BOARD_H = 56.0

# Components that can be moved (SMD passives)
MOVABLE_REFS = ['R1','R2','R3','R4','R5','R6','R7','C1','C2','C3','D1','Q1']

# Placement zone (between header rows, SMD-only zone)
# Headers at y≈19.11 (J1) and y≈36.89 (J2), header span ~50.8mm along X
# header_x center = 28, half span = 25.4 → x range ~2.6 to ~53.4
# Zone inside headers: y from ~21 to ~35 (clear of header pads)
# X from ~4 to ~52 (inside header pin span with margin)
ZONE_X_MIN = 4.0
ZONE_X_MAX = 52.0
ZONE_Y_MIN = 21.0
ZONE_Y_MAX = 35.0

ROTATIONS = [0, 90, 180, 270]

EXCLUDE_NETS = ['Net-(J2-Pin_18)', 'Net-(J2-Pin_16)']


def add_zones_and_save(board_path):
    """Add copper zones to a board file."""
    board = pcbnew.LoadBoard(board_path)
    MM = pcbnew.FromMM
    margin = 0.3
    for net_name, layer in [('Net-(J2-Pin_18)', pcbnew.B_Cu), ('Net-(J2-Pin_16)', pcbnew.F_Cu)]:
        net = board.FindNet(net_name)
        if not net:
            continue
        zone = pcbnew.ZONE(board)
        zone.SetNet(net)
        zone.SetLayer(layer)
        zone.SetIsRuleArea(False)
        zone.SetDoNotAllowTracks(False)
        zone.SetDoNotAllowVias(False)
        zone.SetDoNotAllowPads(False)
        zone.SetDoNotAllowCopperPour(False)
        ol = zone.Outline()
        ol.NewOutline()
        for x, y in [(margin,margin),(BOARD_W-margin,margin),
                      (BOARD_W-margin,BOARD_H-margin),(margin,BOARD_H-margin)]:
            ol.Append(MM(x), MM(y))
        zone.SetMinThickness(MM(0.2))
        zone.SetThermalReliefGap(MM(0.3))
        zone.SetThermalReliefSpokeWidth(MM(0.3))
        board.Add(zone)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    pcbnew.SaveBoard(board_path, board)


def run_autoroute():
    """Run DSN export + FreeRouting, return unrouted count."""
    # Export DSN
    exclude_args = []
    for n in EXCLUDE_NETS:
        exclude_args.append(n)
    cmd_dsn = ['python3', os.path.join(SCRIPT_DIR, 'export_dsn.py'),
               BOARD_FILE, DSN_FILE, '--exclude'] + exclude_args
    subprocess.run(cmd_dsn, capture_output=True, timeout=30)

    # Run FreeRouting (reduced passes for speed)
    cmd_fr = ['java', '-Djava.awt.headless=true',
              '-jar', os.path.join(SCRIPT_DIR, 'freerouting.jar'),
              '-de', DSN_FILE, '-do', SES_FILE, '-mp', '30']
    result = subprocess.run(cmd_fr, capture_output=True, text=True, timeout=120)

    # Parse unrouted count from output
    output = result.stdout + result.stderr
    unrouted = 999
    for line in output.split('\n'):
        m = re.search(r'(\d+) unrouted', line)
        if m:
            unrouted = min(unrouted, int(m.group(1)))
    return unrouted


def randomize_placement(board_path, seed_positions=None):
    """Move SMD components to random positions within the zone."""
    board = pcbnew.LoadBoard(board_path)
    MM = pcbnew.FromMM
    positions = {}

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in MOVABLE_REFS:
            if seed_positions and ref in seed_positions:
                x, y, angle = seed_positions[ref]
            else:
                x = random.uniform(ZONE_X_MIN, ZONE_X_MAX)
                y = random.uniform(ZONE_Y_MIN, ZONE_Y_MAX)
                angle = random.choice(ROTATIONS)
            fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
            fp.SetOrientationDegrees(angle)
            positions[ref] = (x, y, angle)

    pcbnew.SaveBoard(board_path, board)
    return positions


def perturb_placement(board_path, current_positions, temperature=2.0):
    """Slightly perturb current positions (simulated annealing style)."""
    board = pcbnew.LoadBoard(board_path)
    MM = pcbnew.FromMM
    positions = {}

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in MOVABLE_REFS and ref in current_positions:
            ox, oy, oa = current_positions[ref]
            # Perturb position
            dx = random.gauss(0, temperature)
            dy = random.gauss(0, temperature * 0.5)  # less room in Y
            x = max(ZONE_X_MIN, min(ZONE_X_MAX, ox + dx))
            y = max(ZONE_Y_MIN, min(ZONE_Y_MAX, oy + dy))
            # Maybe rotate
            if random.random() < 0.2:
                angle = random.choice(ROTATIONS)
            else:
                angle = oa
            fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
            fp.SetOrientationDegrees(angle)
            positions[ref] = (x, y, angle)

    pcbnew.SaveBoard(board_path, board)
    return positions


def main():
    iterations = 20
    if '--iterations' in sys.argv:
        idx = sys.argv.index('--iterations')
        iterations = int(sys.argv[idx + 1])

    print(f"=== Placement Optimizer ({iterations} iterations) ===")
    print(f"Board: {BOARD_W}x{BOARD_H}mm")
    print(f"Movable: {', '.join(MOVABLE_REFS)}")
    print(f"Zone: x=[{ZONE_X_MIN},{ZONE_X_MAX}] y=[{ZONE_Y_MIN},{ZONE_Y_MAX}]")
    print()

    # Generate base board
    print("[0] Generating base board...")
    subprocess.run(['python3', os.path.join(SCRIPT_DIR, 'generate_pcb_grove.py')],
                   capture_output=True, timeout=30)
    add_zones_and_save(BOARD_FILE)

    best_score = 999
    best_positions = None

    # Phase 1: Random search (first half)
    random_iters = iterations // 2
    print(f"\n--- Phase 1: Random search ({random_iters} iterations) ---")
    for i in range(random_iters):
        positions = randomize_placement(BOARD_FILE)
        add_zones_and_save(BOARD_FILE)
        score = run_autoroute()
        marker = ""
        if score < best_score:
            best_score = score
            best_positions = positions.copy()
            shutil.copy2(BOARD_FILE, BEST_FILE)
            marker = " *** NEW BEST ***"
        print(f"  [{i+1}/{random_iters}] unrouted={score} (best={best_score}){marker}")

        if best_score == 0:
            break

    if best_score == 0:
        print("\nPerfect routing found!")
    else:
        # Phase 2: Local search around best (second half)
        refine_iters = iterations - random_iters
        print(f"\n--- Phase 2: Local refinement ({refine_iters} iterations) ---")
        temperature = 3.0
        for i in range(refine_iters):
            # Restore best board
            subprocess.run(['python3', os.path.join(SCRIPT_DIR, 'generate_pcb_grove.py')],
                           capture_output=True, timeout=30)
            positions = perturb_placement(BOARD_FILE, best_positions, temperature)
            add_zones_and_save(BOARD_FILE)
            score = run_autoroute()
            marker = ""
            if score < best_score:
                best_score = score
                best_positions = positions.copy()
                shutil.copy2(BOARD_FILE, BEST_FILE)
                marker = " *** NEW BEST ***"
            temperature *= 0.9  # cool down
            print(f"  [{i+1}/{refine_iters}] unrouted={score} (best={best_score}) temp={temperature:.1f}{marker}")

            if best_score == 0:
                break

    # Restore best
    if best_positions and os.path.exists(BEST_FILE):
        shutil.copy2(BEST_FILE, BOARD_FILE)
        print(f"\n=== Best result: {best_score} unrouted ===")
        print(f"Saved to: {BOARD_FILE}")
        print("\nBest positions:")
        for ref in sorted(best_positions.keys()):
            x, y, a = best_positions[ref]
            print(f"  {ref}: ({x:.1f}, {y:.1f}) @ {a}°")
    else:
        print("\nNo improvement found.")


if __name__ == '__main__':
    main()
