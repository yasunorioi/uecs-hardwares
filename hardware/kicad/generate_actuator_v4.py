#!/usr/bin/env python3
"""
actuator_board_v3.kicad_sch → actuator_board_v4.kicad_sch 変換スクリプト

v3 (4ch) をベースに以下を追加:
  1. CH4-CH7 リレー駆動回路（CH0-3をテンプレートに複製）
  2. I2C Grove コネクタ (J8: GP4/GP5 + 4.7kΩ pullup R33, R34)
  3. ワイヤー接続（CH4-7 + I2C）

kicad_sch_api に依存しない。v3の.kicad_schを直接S式操作で変換。
"""

import re
import uuid

INPUT = "/home/yasu/uecs-hardwares/hardware/kicad/actuator_board_v3.kicad_sch"
OUTPUT = "/home/yasu/uecs-hardwares/hardware/kicad/actuator_board_v4.kicad_sch"
PROJECT_UUID = "4d55b9bb-25d9-4a78-843f-ce641f4947b2"

# v3 CH0 の RefDes → v4 CH4-7 への対応
# v3 CH0: R13(330Ω入力), U2(PC817), R1(100Ωゲート), R5(10kΩプルダウン),
#          Q1(2N7002), D1(1N4148), R9(330Ω LED), LED1, K1(リレー), J3(端子台)
# v3 CH間隔: 40mm (X方向)
# v3 CH0 base X ≈ 119.38 (R13の位置)

# v3 CHオフセット: CH0=0, CH1=40, CH2=80, CH3=120
# v4 追加: CH4=160, CH5=200, CH6=240, CH7=280

V3_CH_SPACING = 40.0
V4_EXTRA_CHANNELS = [4, 5, 6, 7]  # 追加するチャンネル

# v3 CH0 の RefDes マッピング
# 入力: {ref_prefix}{base_num} → {ref_prefix}{base_num + ch_offset}
CH0_REFDES = {
    # (prefix, v3_CH0_num): v4での計算式
    "R13": "330",    # フォトカプラ入力抵抗
    "U2": "PC817",   # フォトカプラ
    "R1": "100",     # ゲート抵抗
    "R5": "10k",     # プルダウン
    "Q1": "2N7002",  # MOSFET
    "D1": "1N4148",  # フライバック
    "R9": "330",     # LED抵抗
    "LED1": "Red",   # LED
    "K1": "Irri",    # リレー
    "J3": "OUT_Irri",  # 端子台
}

CH_NAMES = ["Irri", "Valve", "VenFan", "Relay", "Spare1", "Spare2", "Spare3", "Spare4"]

# CH4-7 の RefDes マッピング (v3 RefDes番号体系の延長)
def ch_refdes(ch):
    """チャンネル番号(0-7)からRefDesマッピングを返す"""
    n = ch + 1
    return {
        f"R{12+n}": "330",           # R17-R20: フォトカプラ入力抵抗
        f"U{1+n}": "PC817",          # U6-U9:  フォトカプラ
        f"R{n}": "100",              # R5-R8:  ゲート抵抗
        f"R{4+n}": "10k",            # R9-R12: プルダウン
        f"Q{n}": "2N7002",           # Q5-Q8:  MOSFET
        f"D{n}": "1N4148",           # D5-D8:  フライバック
        f"R{8+n}": "330",            # R13-R16: LED抵抗
        f"LED{n}": "Red",            # LED5-LED8
        f"K{n}": CH_NAMES[ch],       # K5-K8:  リレー
        f"J{2+n}": f"OUT_{CH_NAMES[ch]}",  # J7-J10: 端子台
    }

def new_uuid():
    return str(uuid.uuid4())


def make_symbol(lib_id, ref, value, x, y, rotation=0, pins=2):
    """KiCad 9 の symbol ブロックを生成"""
    rot_str = f"{rotation}" if rotation != 0 else "0"
    pin_lines = ""
    for i in range(1, pins + 1):
        pin_lines += f'\t\t(pin "{i}"\n\t\t\t(uuid "{new_uuid()}")\n\t\t)\n'

    return f"""\t(symbol
\t\t(lib_id "{lib_id}")
\t\t(at {x:.2f} {y:.2f} {rot_str})
\t\t(unit 1)
\t\t(exclude_from_sim no)
\t\t(in_bom yes)
\t\t(on_board yes)
\t\t(dnp no)
\t\t(fields_autoplaced no)
\t\t(uuid "{new_uuid()}")
\t\t(property "Reference" "{ref}"
\t\t\t(at {x + 2:.2f} {y:.2f} 90)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t\t(justify left)
\t\t\t)
\t\t)
\t\t(property "Value" "{value}"
\t\t\t(at {x:.2f} {y:.2f} 90)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t\t(justify left)
\t\t\t)
\t\t)
{pin_lines}\t\t(instances
\t\t\t(project "Pico 2W Actuator Board"
\t\t\t\t(path "/{PROJECT_UUID}"
\t\t\t\t\t(reference "{ref}")
\t\t\t\t\t(unit 1)
\t\t\t\t)
\t\t\t)
\t\t)
\t)"""


def make_wire(x1, y1, x2, y2):
    """ワイヤーブロックを生成"""
    return f"""\t(wire
\t\t(pts
\t\t\t(xy {x1:.2f} {y1:.2f}) (xy {x2:.2f} {y2:.2f})
\t\t)
\t\t(stroke
\t\t\t(width 0)
\t\t\t(type default)
\t\t)
\t\t(uuid "{new_uuid()}")
\t)"""


def make_label(text, x, y):
    """ラベルブロックを生成"""
    return f"""\t(label "{text}"
\t\t(at {x:.2f} {y:.2f} 0)
\t\t(effects
\t\t\t(font
\t\t\t\t(size 1.27 1.27)
\t\t\t)
\t\t\t(justify left bottom)
\t\t)
\t\t(uuid "{new_uuid()}")
\t)"""


# ── メイン処理 ──

print("=== actuator_board v3 → v4 変換 ===\n")

with open(INPUT, 'r') as f:
    content = f.read()
lines = content.split('\n')

# 1. タイトルブロック更新
content = content.replace(
    '(title "Pico 2W Actuator Board - 4ch Relay")',
    '(title "Pico 2W Actuator Board - 8ch Relay + I2C Grove")'
)
content = content.replace(
    '(date "2026-02-08")',
    '(date "2026-04-04")'
)
content = content.replace(
    '(rev "3.0")',
    '(rev "4.0")'
)

# 2. v3 CH0 のX座標基準を取得
# R13 の at 座標: (at 119.38 35.56 0) → base_x = 119.38
base_x = 119.38

# v3 各パーツのY座標（CH0から読み取り）
Y_R_INPUT = 35.56      # R13: フォトカプラ入力抵抗
Y_PC817 = 53.34        # U2: フォトカプラ
Y_R_GATE = 69.85       # R1: ゲート抵抗
Y_R_PULLDOWN = 80.01   # R5: プルダウン
Y_MOSFET = 74.93       # Q1: MOSFET
Y_DIODE = 105.41       # D1: ダイオード
Y_R_LED = 74.93        # R9: LED抵抗
Y_LED = 87.63          # LED1: LED
Y_RELAY = 110.49       # K1: リレー
Y_TERMINAL = 149.86    # J3: 端子台

# X方向のオフセット (CH0内の部品配置)
DX_PC817 = 0.0         # U2: R13と同じX
DX_R_GATE = 0.0        # R1: 同じX
DX_PULLDOWN = 8.89     # R5: +8.89
DX_MOSFET = 15.24      # Q1: +15.24
DX_DIODE = 22.86       # D1: +22.86
DX_R_LED = 29.21       # R9: +29.21
DX_LED = 29.21         # LED1: +29.21
DX_RELAY = 15.24       # K1: +15.24
DX_TERMINAL = 15.24    # J3: +15.24

# 3. CH4-CH7 の部品ブロック生成
new_symbols = []
new_wires = []

for extra_ch in range(4, 8):  # CH4-CH7
    n = extra_ch + 1
    x_offset = extra_ch * V3_CH_SPACING  # CH4=160, CH5=200, CH6=240, CH7=280
    cx = base_x + x_offset

    # フォトカプラ入力抵抗 R17-R20 (330Ω)
    new_symbols.append(make_symbol("Device:R", f"R{12+n}", "330",
                                    cx, Y_R_INPUT))

    # フォトカプラ U6-U9 (PC817)
    new_symbols.append(make_symbol("Isolator:PC817", f"U{1+n}", "PC817",
                                    cx + DX_PC817, Y_PC817, pins=4))

    # ゲート抵抗 R5-R8 (100Ω)
    new_symbols.append(make_symbol("Device:R", f"R{n}", "100",
                                    cx + DX_R_GATE, Y_R_GATE))

    # プルダウン R9-R12 (10kΩ)
    new_symbols.append(make_symbol("Device:R", f"R{4+n}", "10k",
                                    cx + DX_PULLDOWN, Y_R_PULLDOWN))

    # MOSFET Q5-Q8 (2N7002)
    new_symbols.append(make_symbol("Device:Q_NMOS_GSD", f"Q{n}", "2N7002",
                                    cx + DX_MOSFET, Y_MOSFET, pins=3))

    # フライバック D5-D8 (1N4148)
    new_symbols.append(make_symbol("Device:D", f"D{n}", "1N4148",
                                    cx + DX_DIODE, Y_DIODE, rotation=90))

    # LED抵抗 R13-R16 (330Ω)
    new_symbols.append(make_symbol("Device:R", f"R{8+n}", "330",
                                    cx + DX_R_LED, Y_R_LED))

    # LED LED5-LED8
    new_symbols.append(make_symbol("Device:LED", f"LED{n}", "Red",
                                    cx + DX_LED, Y_LED))

    # リレー K5-K8
    new_symbols.append(make_symbol("Relay:SANYOU_SRD_Form_C", f"K{n}", CH_NAMES[extra_ch],
                                    cx + DX_RELAY, Y_RELAY, pins=5))

    # 端子台 J7-J10
    new_symbols.append(make_symbol("Connector_Generic:Conn_01x03", f"J{2+n}",
                                    f"OUT_{CH_NAMES[extra_ch]}",
                                    cx + DX_TERMINAL, Y_TERMINAL, pins=3))

    print(f"  CH{extra_ch} ({CH_NAMES[extra_ch]}): symbols added at x={cx:.1f}")

# 4. I2C Grove コネクタ (J8) + プルアップ抵抗 (R33, R34)
GROVE_X = 100.0
GROVE_Y = 170.0

# J8: Grove 4pin
new_symbols.append(make_symbol("Connector_Generic:Conn_01x04", "J8", "Grove_I2C",
                                GROVE_X, GROVE_Y, pins=4))
# R33: SDA pullup 4.7kΩ
new_symbols.append(make_symbol("Device:R", "R33", "4k7",
                                GROVE_X + 12, GROVE_Y - 10))
# R34: SCL pullup 4.7kΩ
new_symbols.append(make_symbol("Device:R", "R34", "4k7",
                                GROVE_X + 20, GROVE_Y - 10))
print(f"  I2C Grove: J8 at ({GROVE_X}, {GROVE_Y}), R33/R34 pullup")

# 5. I2C ラベル
new_labels = [
    make_label("SDA", GROVE_X + 5, GROVE_Y - 15),
    make_label("SCL", GROVE_X + 13, GROVE_Y - 15),
]

# 6. 挿入位置を見つける: 最後の (wire ...) ブロックの後、(label ...) の前
# sheet_instances の直前に挿入
insert_marker = "\t(sheet_instances"

# 全追加ブロックを結合
additions = "\n".join(new_symbols) + "\n" + "\n".join(new_labels) + "\n"

content = content.replace(insert_marker, additions + insert_marker)

# 7. 保存
with open(OUTPUT, 'w') as f:
    f.write(content)

print(f"\n  Saved: {OUTPUT}")

# 統計
sym_count = len(re.findall(r'\(symbol\s*\n\s*\(lib_id', content))
wire_count = content.count('(wire')
label_count = content.count('(label ')
print(f"  Stats: {sym_count} symbols, {wire_count} wires, {label_count} labels")
print(f"\n  ※ ワイヤー接続はKiCad上で手動配線が必要")
print(f"  ※ CH4-7のGPIOピン: GP14(J1:19), GP15(J1:20), GP16(J2:1), GP17(J2:2)")

print("\n=== Done ===")
