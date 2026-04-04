#!/usr/bin/env python3
"""Import Specctra SES routes into KiCad PCB.

Parses the SES file and adds PCB_TRACK segments to the board.
Works headlessly without pcbnew GUI.

Usage:
    python3 import_ses.py <board.kicad_pcb> <session.ses>
"""

import pcbnew
import sys


def tokenize(text):
    """Tokenize S-expression text."""
    tokens = []
    i = 0
    while i < len(text):
        c = text[i]
        if c in ' \t\n\r':
            i += 1
        elif c == '(':
            tokens.append('(')
            i += 1
        elif c == ')':
            tokens.append(')')
            i += 1
        elif c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 1
            tokens.append(text[i + 1:j])
            i = j + 1
        else:
            j = i
            while j < len(text) and text[j] not in ' \t\n\r()':
                j += 1
            tokens.append(text[i:j])
            i = j
    return tokens


def parse_sexp(tokens, pos=0):
    """Parse S-expression from token list. Returns (node, next_pos)."""
    if tokens[pos] == '(':
        pos += 1
        node = []
        while tokens[pos] != ')':
            child, pos = parse_sexp(tokens, pos)
            node.append(child)
        return node, pos + 1
    else:
        return tokens[pos], pos + 1


def find_nodes(tree, tag):
    """Find all child nodes with given tag."""
    results = []
    if isinstance(tree, list) and len(tree) > 0:
        if tree[0] == tag:
            results.append(tree)
        for child in tree[1:]:
            results.extend(find_nodes(child, tag))
    return results


def find_node(tree, tag):
    """Find first child node with given tag."""
    if isinstance(tree, list):
        for child in tree:
            if isinstance(child, list) and len(child) > 0 and child[0] == tag:
                return child
    return None


def import_ses(board_path, ses_path):
    """Import SES routes into KiCad PCB."""
    with open(ses_path) as f:
        content = f.read()

    tokens = tokenize(content)
    tree, _ = parse_sexp(tokens, 0)

    # Get resolution
    routes_node = find_node(tree, 'routes')
    if not routes_node:
        print("No routes section found")
        return

    res_node = find_node(routes_node, 'resolution')
    resolution = int(res_node[2]) if res_node else 10

    # Get network_out
    net_out = find_node(routes_node, 'network_out')
    if not net_out:
        print("No network_out found")
        return

    # Load board
    board = pcbnew.LoadBoard(board_path)

    layer_map = {
        'F.Cu': pcbnew.F_Cu,
        'B.Cu': pcbnew.B_Cu,
    }

    def to_nm(val):
        """Convert SES coordinate to nanometers."""
        return int(int(val) * 25400 / resolution)

    tracks_added = 0
    vias_added = 0

    # Process each net
    for child in net_out[1:]:
        if not isinstance(child, list) or child[0] != 'net':
            continue

        net_name = child[1]
        net = board.FindNet(net_name)
        if not net:
            print(f"  WARNING: net '{net_name}' not found, skipping")
            continue

        for element in child[2:]:
            if not isinstance(element, list):
                continue

            if element[0] == 'wire':
                # Parse wire path
                path_node = find_node(element, 'path')
                if not path_node:
                    continue

                layer_name = path_node[1]
                width = int(path_node[2])
                layer = layer_map.get(layer_name)
                if layer is None:
                    print(f"  WARNING: unknown layer '{layer_name}'")
                    continue

                # Remaining elements are coordinate pairs
                coords = [int(x) for x in path_node[3:] if isinstance(x, str)]
                points = [(coords[i], coords[i + 1]) for i in range(0, len(coords), 2)]

                width_nm = to_nm(width)
                for i in range(len(points) - 1):
                    track = pcbnew.PCB_TRACK(board)
                    track.SetStart(pcbnew.VECTOR2I(to_nm(points[i][0]), to_nm(points[i][1])))
                    track.SetEnd(pcbnew.VECTOR2I(to_nm(points[i + 1][0]), to_nm(points[i + 1][1])))
                    track.SetWidth(width_nm)
                    track.SetLayer(layer)
                    track.SetNet(net)
                    board.Add(track)
                    tracks_added += 1

            elif element[0] == 'via':
                vx = int(element[2])
                vy = int(element[3])
                via = pcbnew.PCB_VIA(board)
                via.SetPosition(pcbnew.VECTOR2I(to_nm(vx), to_nm(vy)))
                via.SetNet(net)
                via.SetViaType(pcbnew.VIATYPE_THROUGH)
                via.SetWidth(pcbnew.FromMM(0.6))
                via.SetDrill(pcbnew.FromMM(0.3))
                board.Add(via)
                vias_added += 1

    pcbnew.SaveBoard(board_path, board)
    print(f"Import complete: {tracks_added} tracks, {vias_added} vias added")
    return tracks_added, vias_added


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <board.kicad_pcb> <session.ses>")
        sys.exit(1)
    import_ses(sys.argv[1], sys.argv[2])
