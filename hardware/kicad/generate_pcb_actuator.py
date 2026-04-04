#!/usr/bin/env python3
"""Generate PCB layout for Actuator Board v3 (80mm x 50mm)
Pico 2W 4-channel relay driver board.
External relay modules connected via jumper headers.

Components:
  J1, J2: Pico headers (1x20 pin socket, 2.54mm pitch)
  J3: Relay module connector (1x06 pin header: 5V, GND, IN1-IN4)
  J7: 5V external power (pin header 2-pin)
  U2-U5: PC817 optocouplers (DIP-4)
  Q1-Q4: 2N7002 N-MOSFET (SOT-23)
  LED1-LED4: Red status LEDs (0805)
  R1-R4: 100R optocoupler output (0805)
  R5-R8: 10k MOSFET pull-down (0805)
  R9-R12: 330R LED current limiting (0805)
  R13-R16: 330R optocoupler input (0805)
  C1: 100uF bulk capacitor (electrolytic 6.3mm)
  C2: 100nF decoupling capacitor (0805)

Board: 80mm x 50mm, 2-layer, JLCPCB standard rules
Layout: Pico bottom, driver circuits middle, relay header top
External relay module: Amazon generic 4ch relay (VCC/GND/IN1-IN4)
"""

import pcbnew
import os

# --- Constants ---
FP_PATH = '/usr/share/kicad/footprints'
BOARD_W = 80.0  # mm
BOARD_H = 50.0  # mm
MM = pcbnew.FromMM

PICO_ROW_SPACING = 17.78
PICO_PIN_PITCH = 2.54

# --- Helper functions ---
def pos(x_mm, y_mm):
    return pcbnew.VECTOR2I(MM(x_mm), MM(y_mm))

def load_fp(lib, name, ref, value, x, y, angle=0):
    fp = pcbnew.FootprintLoad(os.path.join(FP_PATH, lib), name)
    fp.SetReference(ref)
    fp.SetValue(value)
    fp.SetPosition(pos(x, y))
    if angle:
        fp.SetOrientationDegrees(angle)
    return fp

def get_pad(fp, pad_num):
    for pad in fp.Pads():
        if pad.GetNumber() == str(pad_num):
            return pad
    return None

# --- Create board ---
board = pcbnew.CreateEmptyBoard()

# --- Board outline (Edge.Cuts) ---
outline = pcbnew.PCB_SHAPE(board)
outline.SetShape(pcbnew.SHAPE_T_RECT)
outline.SetStart(pos(0, 0))
outline.SetEnd(pos(BOARD_W, BOARD_H))
outline.SetLayer(pcbnew.Edge_Cuts)
outline.SetWidth(MM(0.1))
board.Add(outline)

# --- Mounting holes (M3, 4 corners) ---
for mx, my in [(3.5, 3.5), (3.5, BOARD_H-3.5), (BOARD_W-3.5, 3.5), (BOARD_W-3.5, BOARD_H-3.5)]:
    mh = pcbnew.FootprintLoad(os.path.join(FP_PATH, 'MountingHole.pretty'), 'MountingHole_3.2mm_M3')
    mh.SetReference('H')
    mh.SetPosition(pos(mx, my))
    board.Add(mh)

# --- Design rules (JLCPCB standard) ---
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(2)
ds.m_TrackMinWidth = MM(0.15)
ds.m_ViasMinSize = MM(0.6)
ds.m_ViasMinDrill = MM(0.3)
ds.m_MinClearance = MM(0.15)

# --- Layout ---
# Bottom area: Pico headers (rotated 90° so pins run along X/long axis)
# Top half: 4 relay channels in a row
# Middle: driver circuitry (opto -> MOSFET -> relay)

pico_cx = BOARD_W / 2  # 40mm (center, pin 10/11 area)
pico_cy = 40  # near bottom
# Rotated 90°: rows are separated in Y direction (17.78mm apart)
j1_y = pico_cy - PICO_ROW_SPACING / 2  # ~31.11mm (top row)
j2_y = pico_cy + PICO_ROW_SPACING / 2  # ~48.89mm (bottom row)

# Driver circuit y positions (middle area between relay header and Pico)
driver_spacing = 16  # 16mm between channel centers
driver_x_start = (BOARD_W - (3 * driver_spacing)) / 2  # centered
opto_y = 12
mosfet_y = 18
resistor_y = 15

footprints = {}

# === Pico headers (pin sockets, rotated 90° so pins run along X axis) ===
footprints['J1'] = load_fp('Connector_PinSocket_2.54mm.pretty',
                           'PinSocket_1x20_P2.54mm_Vertical',
                           'J1', 'Pico_Left', pico_cx, j1_y, 90)

footprints['J2'] = load_fp('Connector_PinSocket_2.54mm.pretty',
                           'PinSocket_1x20_P2.54mm_Vertical',
                           'J2', 'Pico_Right', pico_cx, j2_y, 90)

# === Relay module header (1x06: 5V, GND, IN1, IN2, IN3, IN4) ===
# Connects to external Amazon generic 4ch relay module
footprints['J3'] = load_fp('Connector_PinHeader_2.54mm.pretty',
                           'PinHeader_1x06_P2.54mm_Vertical',
                           'J3', 'RELAY_MOD', BOARD_W / 2, 4, 90)

# === 4 Driver channels (opto-isolated MOSFET drivers) ===
for ch in range(4):
    rx = driver_x_start + ch * driver_spacing
    n = ch + 1

    # Optocoupler (DIP-4)
    footprints[f'U{n+1}'] = load_fp('Package_DIP.pretty', 'DIP-4_W7.62mm',
                                     f'U{n+1}', 'PC817',
                                     rx - 5, opto_y)

    # MOSFET (SOT-23)
    footprints[f'Q{n}'] = load_fp('Package_TO_SOT_SMD.pretty', 'SOT-23',
                                   f'Q{n}', '2N7002',
                                   rx + 3, mosfet_y)

    # LED (0805, status indicator)
    footprints[f'LED{n}'] = load_fp('LED_SMD.pretty', 'LED_0805_2012Metric',
                                     f'LED{n}', 'Red',
                                     rx, mosfet_y + 5, 90)

    # R (100R, opto output series)
    footprints[f'R{n}'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                                   f'R{n}', '100',
                                   rx - 2, resistor_y, 90)

    # R (10k, MOSFET pull-down)
    footprints[f'R{n+4}'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                                     f'R{n+4}', '10k',
                                     rx + 5, mosfet_y + 2, 90)

    # R (330R, LED current limiting)
    footprints[f'R{n+8}'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                                     f'R{n+8}', '330',
                                     rx + 2, mosfet_y + 5, 90)

    # R (330R, opto input)
    footprints[f'R{n+12}'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                                      f'R{n+12}', '330',
                                      rx - 5, opto_y - 3, 90)

# === Power ===
# 5V external power (screw terminal, right edge)
footprints['J7'] = load_fp('Connector_PinHeader_2.54mm.pretty',
                           'PinHeader_1x02_P2.54mm_Vertical',
                           'J7', '5V_EXT', BOARD_W - 8, 25)

# Bulk capacitor (electrolytic, near power input)
footprints['C1'] = load_fp('Capacitor_THT.pretty',
                           'CP_Radial_D6.3mm_P2.50mm',
                           'C1', '100u', BOARD_W - 15, 25)

# Decoupling cap (0805, near Pico, between header rows)
footprints['C2'] = load_fp('Capacitor_SMD.pretty', 'C_0805_2012Metric',
                           'C2', '100n', pico_cx, pico_cy)

# Add all footprints to board
for ref, fp in footprints.items():
    board.Add(fp)

# --- Create nets and assign to pads ---
net_defs = {
    # 5V bus (opto collectors, power input, relay module VCC)
    # J3 pin 1 = 5V to relay module
    'Net-(5V)': [('C1','1'), ('J2','19'), ('J7','1'), ('J3','1'),
                 ('U2','4'), ('U3','4'), ('U4','4'), ('U5','4')],
    # GND bus
    # J3 pin 2 = GND to relay module
    'Net-(GND)': [('C1','2'), ('C2','2'), ('J1','3'), ('J7','2'), ('J3','2'),
                  ('LED1','2'), ('LED2','2'), ('LED3','2'), ('LED4','2'),
                  ('Q1','3'), ('Q2','3'), ('Q3','3'), ('Q4','3'),
                  ('R5','2'), ('R6','2'), ('R7','2'), ('R8','2'),
                  ('U2','2'), ('U3','2'), ('U4','2'), ('U5','2')],
    # Decoupling
    'Net-(J2-Pin_16)': [('C2','1'), ('J2','16')],
    # GPIO inputs to optocouplers
    'Net-(J1-Pin_14)': [('J1','14'), ('R13','1')],
    'Net-(J1-Pin_15)': [('J1','15'), ('R14','1')],
    'Net-(J1-Pin_16)': [('J1','16'), ('R15','1')],
    'Net-(J1-Pin_17)': [('J1','17'), ('R16','1')],
    # Opto input connections
    'Net-(R13-Pad2)': [('R13','2'), ('U2','1')],
    'Net-(R14-Pad2)': [('R14','2'), ('U3','1')],
    'Net-(R15-Pad2)': [('R15','2'), ('U4','1')],
    'Net-(R16-Pad2)': [('R16','2'), ('U5','1')],
    # Opto output -> gate resistor
    'Net-(R1-Pad1)': [('R1','1'), ('U2','3')],
    'Net-(R2-Pad1)': [('R2','1'), ('U3','3')],
    'Net-(R3-Pad1)': [('R3','1'), ('U4','3')],
    'Net-(R4-Pad1)': [('R4','1'), ('U5','3')],
    # MOSFET gate (opto output + pull-down)
    'Net-(Q1-G)': [('Q1','1'), ('R1','2'), ('R5','1')],
    'Net-(Q2-G)': [('Q2','1'), ('R2','2'), ('R6','1')],
    'Net-(Q3-G)': [('Q3','1'), ('R3','2'), ('R7','1')],
    'Net-(Q4-G)': [('Q4','1'), ('R4','2'), ('R8','1')],
    # MOSFET drain -> relay module IN + LED indicator
    # J3 pins 3-6 = IN1-IN4 to relay module (active low typically)
    'Net-(Q1-D)': [('Q1','2'), ('J3','3'), ('R9','1')],
    'Net-(Q2-D)': [('Q2','2'), ('J3','4'), ('R10','1')],
    'Net-(Q3-D)': [('Q3','2'), ('J3','5'), ('R11','1')],
    'Net-(Q4-D)': [('Q4','2'), ('J3','6'), ('R12','1')],
    # LED connections
    'Net-(LED1-K)': [('LED1','1'), ('R9','2')],
    'Net-(LED2-K)': [('LED2','1'), ('R10','2')],
    'Net-(LED3-K)': [('LED3','1'), ('R11','2')],
    'Net-(LED4-K)': [('LED4','1'), ('R12','2')],
}

net_code = 1
for net_name, connections in net_defs.items():
    net = pcbnew.NETINFO_ITEM(board, net_name, net_code)
    board.Add(net)
    for ref, pin in connections:
        if ref in footprints:
            pad = get_pad(footprints[ref], pin)
            if pad:
                pad.SetNet(net)
    net_code += 1

# --- Save board ---
output_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'actuator_board_v3.kicad_pcb')
pcbnew.SaveBoard(output_path, board)
print(f'PCB saved: {output_path}')
print(f'Board size: {BOARD_W}mm x {BOARD_H}mm')
print(f'Components placed: {len(footprints)}')
print(f'Nets created: {net_code - 1}')
print()
print('Next steps:')
print('  1. Open in pcbnew and verify placement')
print('  2. File -> Export -> Specctra DSN for FreeRouting')
print('  3. Run FreeRouting for auto-routing')
print('  4. File -> Import -> Specctra Session to import routes')
