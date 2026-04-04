#!/usr/bin/env python3
"""Export KiCad PCB to Specctra DSN format for FreeRouting.

Usage:
    python3 export_dsn.py <board.kicad_pcb> [output.dsn]

Works around pcbnew.ExportSpecctraDSN() returning False in headless mode.
"""

import pcbnew
import sys
import os
import math
from collections import defaultdict

def to_mil(nm):
    return nm / 25400.0

def escape_id(s):
    if not s or any(c in s for c in ' ()"\t\n-'):
        return f'"{s}"'
    return s

def generate_dsn(board_path, dsn_path, exclude_nets=None):
    """Generate DSN. exclude_nets: list of net names handled by copper zones."""
    if exclude_nets is None:
        exclude_nets = set()
    else:
        exclude_nets = set(exclude_nets)
    board = pcbnew.LoadBoard(board_path)

    bbox = board.GetBoardEdgesBoundingBox()
    bx1 = to_mil(bbox.GetX())
    by1 = to_mil(bbox.GetY())
    bx2 = to_mil(bbox.GetRight())
    by2 = to_mil(bbox.GetBottom())

    L = []
    L.append('(pcb generated.dsn')
    L.append('  (parser')
    L.append('    (string_quote ")')
    L.append('    (space_in_quoted_tokens on)')
    L.append('    (host_cad "KiCad")')
    L.append('    (host_version "7.0.11")')
    L.append('  )')
    L.append('  (resolution mil 10)')
    L.append('  (unit mil)')

    # Structure
    L.append('  (structure')
    L.append('    (layer F.Cu (type signal) (property (index 0)))')
    L.append('    (layer B.Cu (type signal) (property (index 1)))')
    L.append('    (boundary')
    L.append(f'      (path pcb 0 {bx1:.1f} {by1:.1f} {bx2:.1f} {by1:.1f} {bx2:.1f} {by2:.1f} {bx1:.1f} {by2:.1f} {bx1:.1f} {by1:.1f})')
    L.append('    )')
    L.append('    (via "Via[0-1]_600:300_um")')
    L.append('    (rule (width 5.9) (clearance 5.9))')
    L.append('  )')

    # Collect footprints, skip those with no nets (mounting holes)
    all_fps = list(board.GetFootprints())
    routable_fps = []
    skip_refs = set()
    for fp in all_fps:
        has_net = False
        for pad in fp.Pads():
            if pad.GetNetname():
                has_net = True
                break
        if has_net:
            routable_fps.append(fp)
        else:
            skip_refs.add(fp.GetReference())

    # Group placements by image (footprint type)
    image_placements = defaultdict(list)
    for fp in routable_fps:
        fp_id = f'{fp.GetFPID().GetLibItemName()}'
        ref = fp.GetReference()
        pos = fp.GetPosition()
        x = to_mil(pos.x)
        y = to_mil(pos.y)
        angle = fp.GetOrientationDegrees()
        side = 'front' if fp.GetLayer() == pcbnew.F_Cu else 'back'
        image_placements[fp_id].append((ref, x, y, side, angle))

    # Placement
    L.append('  (placement')
    for fp_id, placements in image_placements.items():
        L.append(f'    (component {escape_id(fp_id)}')
        for ref, x, y, side, angle in placements:
            L.append(f'      (place {escape_id(ref)} {x:.1f} {y:.1f} {side} {angle:.0f})')
        L.append('    )')
    L.append('  )')

    # Library: images and padstacks
    padstacks = {}

    def get_ps_name(pad):
        size = pad.GetSize()
        is_th = pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH
        w = to_mil(size.x)
        h = to_mil(size.y)
        if is_th:
            return f'Round[A]Pad_{w:.0f}_um'
        else:
            shape = pad.GetShape()
            if shape == pcbnew.PAD_SHAPE_CIRCLE:
                return f'Round[T]Pad_{w:.0f}_um'
            else:
                return f'Rect[T]Pad_{w:.0f}x{h:.0f}_um'

    L.append('  (library')

    # Images
    fp_images_done = set()
    for fp in routable_fps:
        fp_id = f'{fp.GetFPID().GetLibItemName()}'
        if fp_id in fp_images_done:
            continue
        fp_images_done.add(fp_id)

        L.append(f'    (image {escape_id(fp_id)}')
        for pad in fp.Pads():
            pin_num = pad.GetNumber()
            if not pin_num:
                continue

            # Pad offset relative to footprint center (unrotated)
            fp_pos = fp.GetPosition()
            pad_pos = pad.GetPosition()
            dx = to_mil(pad_pos.x - fp_pos.x)
            dy = to_mil(pad_pos.y - fp_pos.y)

            angle_rad = math.radians(fp.GetOrientationDegrees())
            if abs(angle_rad) > 0.001:
                cos_a = math.cos(-angle_rad)
                sin_a = math.sin(-angle_rad)
                rdx = dx * cos_a - dy * sin_a
                rdy = dx * sin_a + dy * cos_a
                dx, dy = rdx, rdy

            ps_name = get_ps_name(pad)
            if ps_name not in padstacks:
                padstacks[ps_name] = pad

            L.append(f'      (pin {escape_id(ps_name)} {escape_id(pin_num)} {dx:.1f} {dy:.1f})')
        L.append('    )')

    # Padstacks
    for ps_name, pad in padstacks.items():
        size = pad.GetSize()
        is_th = pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH
        w = to_mil(size.x)
        h = to_mil(size.y)

        L.append(f'    (padstack {escape_id(ps_name)}')
        if is_th:
            L.append(f'      (shape (circle F.Cu {w:.1f}))')
            L.append(f'      (shape (circle B.Cu {w:.1f}))')
        else:
            shape = pad.GetShape()
            if shape == pcbnew.PAD_SHAPE_CIRCLE:
                L.append(f'      (shape (circle F.Cu {w:.1f}))')
            else:
                L.append(f'      (shape (rect F.Cu {-w/2:.1f} {-h/2:.1f} {w/2:.1f} {h/2:.1f}))')
        L.append('      (attach off)')
        L.append('    )')

    # Via padstack
    L.append('    (padstack "Via[0-1]_600:300_um"')
    L.append('      (shape (circle F.Cu 23.6))')
    L.append('      (shape (circle B.Cu 23.6))')
    L.append('      (attach off)')
    L.append('    )')
    L.append('  )')

    # Network
    L.append('  (network')
    net_map = defaultdict(list)
    for fp in routable_fps:
        ref = fp.GetReference()
        for pad in fp.Pads():
            net_name = pad.GetNetname()
            pin_num = pad.GetNumber()
            if net_name and pin_num:
                net_map[net_name].append(f'{escape_id(ref)}-{escape_id(pin_num)}')

    routable_nets = []
    for net_name, pins in net_map.items():
        if len(pins) < 2:
            continue
        if net_name in exclude_nets:
            continue  # Handled by copper zones
        routable_nets.append((net_name, pins))

    for net_name, pins in routable_nets:
        L.append(f'    (net {escape_id(net_name)}')
        L.append(f'      (pins {" ".join(pins)})')
        L.append('    )')

    # Net class
    L.append('    (class default')
    for net_name, pins in routable_nets:
        L.append(f'      {escape_id(net_name)}')
    L.append('      (circuit (use_via "Via[0-1]_600:300_um"))')
    L.append('      (rule (width 5.9) (clearance 5.9))')
    L.append('    )')
    L.append('  )')

    L.append('  (wiring)')
    L.append(')')

    with open(dsn_path, 'w') as f:
        f.write('\n'.join(L))

    print(f'DSN exported: {dsn_path}')
    print(f'  Routable footprints: {len(routable_fps)} (skipped: {len(skip_refs)})')
    print(f'  Nets routed: {len(routable_nets)} (excluded: {len(exclude_nets)})')

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <board.kicad_pcb> [output.dsn]')
        sys.exit(1)

    # Parse args: script.py <board> [output.dsn] [--exclude NET1 NET2 ...]
    exclude = []
    args = sys.argv[1:]
    if '--exclude' in args:
        idx = args.index('--exclude')
        exclude = args[idx+1:]
        args = args[:idx]

    board_path = args[0]
    dsn_path = args[1] if len(args) > 1 else board_path.replace('.kicad_pcb', '.dsn')
    generate_dsn(board_path, dsn_path, exclude_nets=exclude)
