#!/usr/bin/env python3
"""Generate PCB layout for Grove Shield v3 (56mm x 56mm)
W5500-EVB-Pico-PoE stacking compatible sensor node shield.
Based on Seeed Grove Shield for Pi Pico form factor (56x56mm).
Shield mounts BELOW W5500 (PoE module occupies top).

Components:
  J1, J2: Pico headers (1x20 pin socket, 2.54mm pitch)
  J3, J4: Grove I2C connectors (JST PH 4-pin)
  J5-J7: ADC connectors (JST PH 3-pin)
  J8: 1-Wire connector (JST PH 3-pin)
  J9: Fan connector (screw terminal 2-pin, 5.08mm pitch)
  R1-R5: 4.7k pull-up resistors (0805)
  R6: 100R gate series resistor (0805)
  R7: 10k pull-down resistor (0805)
  C1-C3: 100nF decoupling capacitors (0805)
  D1: 1N4148 flyback diode (SOD-123)
  Q1: 2N7002 N-MOSFET (SOT-23)

Layout:
  Top edge (J1 outside): Grove I2C x2, 1-Wire - facing board edge
  Bottom edge (J2 outside): ADC x3 - facing board edge
  Right edge: Fan screw terminal - facing board edge
  Between headers: SMD passives only (no tall components)

Board: 56mm x 56mm, 2-layer, JLCPCB standard rules
Pico pin spacing: 17.78mm (7 x 2.54mm) between rows
"""

import pcbnew
import os

# --- Constants ---
FP_PATH = '/usr/share/kicad/footprints'
BOARD_W = 56.0  # mm
BOARD_H = 56.0  # mm
MM = pcbnew.FromMM

# Pico header spacing: 17.78mm between rows (standard)
PICO_ROW_SPACING = 17.78
PICO_PIN_PITCH = 2.54
PICO_PINS = 20

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

# No mounting holes - held by stacking pin headers

# --- Design rules (JLCPCB standard) ---
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(2)
ds.m_TrackMinWidth = MM(0.15)
ds.m_ViasMinSize = MM(0.6)
ds.m_ViasMinDrill = MM(0.3)
ds.m_MinClearance = MM(0.15)

# --- Layout coordinates ---
# Headers 90° rotated: pins run along X axis
# PinSocket_1x20 footprint: pin 1 at origin, pin 20 at -(19*2.54)mm along Y
# After 90° rotation: pin 1 at origin, pin 20 at +48.26mm along X
# To center the header span on the board: pin 1 x = (56 - 48.26) / 2 = 3.87mm
HEADER_SPAN = (PICO_PINS - 1) * PICO_PIN_PITCH  # 48.26mm
header_x = (BOARD_W - HEADER_SPAN) / 2  # ~3.87mm (pin 1 position)
center_y = BOARD_H / 2
j1_y = center_y - PICO_ROW_SPACING / 2   # ~19.11 (top header row)
j2_y = center_y + PICO_ROW_SPACING / 2   # ~36.89 (bottom header row)

# Connector zones:
# Top zone (above J1): y = 0 to ~17mm  → Grove I2C, 1-Wire
# Bottom zone (below J2): y = ~39mm to 56mm → ADC connectors
# Between headers: y = ~21mm to ~35mm → SMD passives only
# Right edge: Fan terminal

# Connector Y positions
top_conn_y = 7       # connectors near top edge, facing up
bottom_conn_y = 49   # connectors near bottom edge, facing down

# --- Place footprints ---
footprints = {}

# === Pico headers (90° rotated, pins along X axis) ===
footprints['J1'] = load_fp('Connector_PinSocket_2.54mm.pretty',
                           'PinSocket_1x20_P2.54mm_Vertical',
                           'J1', 'Pico_Left', header_x, j1_y, 90)

footprints['J2'] = load_fp('Connector_PinSocket_2.54mm.pretty',
                           'PinSocket_1x20_P2.54mm_Vertical',
                           'J2', 'Pico_Right', header_x, j2_y, 90)

# === Top edge connectors (above J1, facing top edge) ===
# Pin X positions after 90° rotation: pin N at header_x + (N-1)*2.54
# J3 connects to J1 pins 6,7 (I2C0 SDA/SCL) + VCC/GND → place J3 at x=14
# J4 connects to J1 pins 9,10 (I2C1 SDA/SCL) + VCC/GND → place J4 at x=25
# J8 connects to J1 pin 4 → x ≈ 12mm → place J8 at x=40 (spread out)
footprints['J3'] = load_fp('Connector_JST.pretty',
                           'JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical',
                           'J3', 'Grove_I2C0', 14, top_conn_y)

footprints['J4'] = load_fp('Connector_JST.pretty',
                           'JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical',
                           'J4', 'Grove_I2C1', 27, top_conn_y)

footprints['J8'] = load_fp('Connector_JST.pretty',
                           'JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical',
                           'J8', '1-Wire', 42, top_conn_y)

# === Bottom edge connectors (below J2, facing bottom edge) ===
# J5 connects to J2 pins 3,11 → x ≈ 9,29 → place at x=10
# J6 connects to J2 pins 8,12 → x ≈ 22,32 → place at x=22
# J7 connects to J2 pins 13,14 → x ≈ 35,37 → place at x=36
footprints['J5'] = load_fp('Connector_JST.pretty',
                           'JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical',
                           'J5', 'ADC0', 6.13, bottom_conn_y)

footprints['J6'] = load_fp('Connector_JST.pretty',
                           'JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical',
                           'J6', 'ADC1', 20.13, bottom_conn_y)

footprints['J7'] = load_fp('Connector_JST.pretty',
                           'JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical',
                           'J7', 'ADC2', 34.13, bottom_conn_y)

# === Fan screw terminal (2-pin, 5.08mm pitch) - next to J7 on bottom edge ===
footprints['J9'] = load_fp('TerminalBlock.pretty',
                           'TerminalBlock_bornier-2_P5.08mm',
                           'J9', 'Fan', 46.13, bottom_conn_y)

# === SMD passives (between headers, y ≈ 21 to 35) ===
mid_y = center_y  # 28mm
# Pin X positions: pin N at header_x + (N-1)*2.54 ≈ 3.87 + (N-1)*2.54

# Pull-up resistors: near their connected header pins
# I2C0 pull-ups (R1→SDA0 pin6, R2→SCL0 pin7) → x ≈ 16.6, 19.1
footprints['R1'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R1', '4.7k', 17, mid_y - 3)
footprints['R2'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R2', '4.7k', 19, mid_y + 3)

# I2C1 pull-ups (R3→SDA1 pin9, R4→SCL1 pin10) → x ≈ 24.2, 26.7
footprints['R3'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R3', '4.7k', 24, mid_y - 3)
footprints['R4'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R4', '4.7k', 27, mid_y + 3)

# 1-Wire pull-up (R5→pin4) → x ≈ 11.5
footprints['R5'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R5', '4.7k', 12, mid_y)

# MOSFET driver: R6→pin9(J2), R7(pulldown), Q1, D1
# J2 pin9 → x ≈ 24.2, J2 pin19 → x ≈ 49.6 (D1/drain)
# Fan terminal at right edge → group near right side
footprints['R6'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R6', '100', 42, mid_y - 3)
footprints['R7'] = load_fp('Resistor_SMD.pretty', 'R_0805_2012Metric',
                           'R7', '10k', 42, mid_y + 3)
footprints['Q1'] = load_fp('Package_TO_SOT_SMD.pretty', 'SOT-23',
                           'Q1', '2N7002', 48, mid_y)
footprints['D1'] = load_fp('Diode_SMD.pretty', 'D_SOD-123',
                           'D1', '1N4148', 48, mid_y + 5)

# Decoupling capacitors - spread across center
footprints['C1'] = load_fp('Capacitor_SMD.pretty', 'C_0805_2012Metric',
                           'C1', '100n', 32, mid_y - 3)
footprints['C2'] = load_fp('Capacitor_SMD.pretty', 'C_0805_2012Metric',
                           'C2', '100n', 32, mid_y + 3)
footprints['C3'] = load_fp('Capacitor_SMD.pretty', 'C_0805_2012Metric',
                           'C3', '100n', 38, mid_y)

# Add all footprints to board
for ref, fp in footprints.items():
    board.Add(fp)

# --- Create nets and assign to pads ---
net_defs = {
    'Net-(D1-A)': [('D1','1'), ('D1','2'), ('J2','19'), ('J9','1'), ('Q1','2')],
    # Grove I2C pinout: Pin1=SCL(yellow), Pin2=SDA(white), Pin3=VCC(red), Pin4=GND(black)
    'Net-(J1-Pin_4)': [('J1','4'), ('J8','2'), ('R5','1')],
    'Net-(J1-Pin_6)': [('J1','6'), ('J3','2'), ('R1','1')],   # GP4(SDA) → J3 Pin2(SDA)
    'Net-(J1-Pin_7)': [('J1','7'), ('J3','1'), ('R2','1')],   # GP5(SCL) → J3 Pin1(SCL)
    'Net-(J1-Pin_9)': [('J1','9'), ('J4','2'), ('R3','1')],   # GP6(SDA) → J4 Pin2(SDA)
    'Net-(J1-Pin_10)': [('J1','10'), ('J4','1'), ('R4','1')], # GP7(SCL) → J4 Pin1(SCL)
    # ADC connectors: Pin1=GND, Pin2=VCC, Pin3=Signal
    # J2 Pin3/8/13 are Pico GND pins → merge into main GND net (J2-Pin_18)
    'Net-(J2-Pin_9)': [('J2','9'), ('R6','2')],
    'Net-(J2-Pin_11)': [('J2','11'), ('J5','3')],   # GP26(ADC0) → J5 Pin3
    'Net-(J2-Pin_12)': [('J2','12'), ('J6','3')],   # GP27(ADC1) → J6 Pin3
    'Net-(J2-Pin_14)': [('J2','14'), ('J7','3')],   # GP28(ADC2) → J7 Pin3
    'Net-(J2-Pin_15)': [('C3','1'), ('J2','15')],
    'Net-(J2-Pin_16)': [('C1','1'), ('C2','1'), ('J2','16'), ('J3','3'), ('J4','3'),
                        ('J5','2'), ('J6','2'), ('J7','2'), ('J8','3'),
                        ('R1','2'), ('R2','2'), ('R3','2'), ('R4','2'), ('R5','2')],
    'Net-(J2-Pin_18)': [('C1','2'), ('C2','2'), ('C3','2'), ('J2','18'), ('J3','4'), ('J4','4'),
                        ('J5','1'), ('J6','1'), ('J7','1'),
                        ('J8','1'), ('J9','2'), ('Q1','3'), ('R7','2')],
    'Net-(Q1-G)': [('Q1','1'), ('R6','1'), ('R7','1')],
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
                           'grove_shield_v3.kicad_pcb')
pcbnew.SaveBoard(output_path, board)
print(f'PCB saved: {output_path}')
print(f'Board size: {BOARD_W}mm x {BOARD_H}mm')
print(f'Components placed: {len(footprints)}')
print(f'Nets created: {net_code - 1}')
print()
print('Next steps:')
print('  1. Open in pcbnew and verify placement')
print('  2. Run: bash autoroute.sh grove_shield_v3.kicad_pcb --exclude "Net-(J2-Pin_18)" "Net-(J2-Pin_16)"')
print('  3. File -> Import -> Specctra Session')
