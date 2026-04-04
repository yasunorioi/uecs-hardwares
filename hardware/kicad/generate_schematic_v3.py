#!/usr/bin/env python3
"""
Grove Shield for Pi Pico 代替基板 - 回路図自動生成スクリプト v3
Version: 3.0 - レイアウト最適化版
Created: 2026-02-08

論理ブロック配置:
  上部: Picoピンヘッダ (J1左, J2右)
  中段左: I2C0 (J3, R1, R2, C1)
  中段右: I2C1 (J4, R3, R4, C2)
  下段左: ADC (J5, J6, J7, C3)
  下段中: 1-Wire (J8, R5)
  下段右: Fan PWM (Q1, R6, R7, D1, J9)
"""

import sys
import os
from pathlib import Path

sys.path.insert(0, "/home/yasu/unipi-agri-ha/.venv/lib/python3.12/site-packages")
import kicad_sch_api as ksa

os.environ["KICAD_SYMBOL_DIR"] = "/home/yasu/unipi-agri-ha/hardware/kicad/kicad-symbols"

output_dir = Path("/home/yasu/unipi-agri-ha/hardware/kicad")

print("=== Grove Shield v3 - Layout Optimized ===\n")

try:
    sch = ksa.create_schematic("Grove Shield for Pi Pico Alternative")
    sch.set_title_block(
        title="Grove Shield for Pi Pico Alternative",
        date="2026-02-08",
        rev="3.0",
        company="AgriHA Project"
    )

    # =========================================================================
    # レイアウト定数 (mm)
    # A4 = 297 x 210mm, KiCad uses top-left origin, +Y down
    # =========================================================================

    # === セクションA: Picoピンヘッダ (上部) ===
    # J1(左20pin) と J2(右20pin) を横に並べる
    # 20pin コネクタは pin1が上、pin20が下 (各pin 2.54mm pitch → 約48mm高さ)
    PICO_Y = 40.0          # ヘッダ上端
    J1_X = 55.0            # 左ヘッダ
    J2_X = 195.0           # 右ヘッダ (間隔を広めに)

    # === セクションB: I2C0 (中段左) ===
    I2C0_X = 40.0
    I2C0_Y = 115.0

    # === セクションC: I2C1 (中段右) ===
    I2C1_X = 140.0
    I2C1_Y = 115.0

    # === セクションD: ADC (下段左) ===
    ADC_X = 35.0
    ADC_Y = 170.0
    ADC_SPACING = 30.0     # ADCコネクタ間隔

    # === セクションE: 1-Wire (下段中) ===
    WIRE1_X = 140.0
    WIRE1_Y = 170.0

    # === セクションF: Fan PWM (下段右) ===
    FAN_X = 210.0
    FAN_Y = 115.0

    # =========================================================================
    # 1. コンポーネント配置
    # =========================================================================
    print("[1] Adding components with optimized layout...")

    # --- Section A: Pico Headers ---
    j1 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x20",
        reference="J1", value="Pico_Left",
        position=(J1_X, PICO_Y), rotation=0.0
    )
    j2 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x20",
        reference="J2", value="Pico_Right",
        position=(J2_X, PICO_Y), rotation=0.0
    )
    print("  A: Pico Headers (J1, J2)")

    # --- Section B: I2C0 ---
    # Grove I2C0 コネクタ (4pin)
    j3 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x04",
        reference="J3", value="Grove_I2C0",
        position=(I2C0_X, I2C0_Y), rotation=0.0
    )
    # プルアップ抵抗: SDAとSCLの上に横向きに配置
    r1 = sch.components.add(
        lib_id="Device:R", reference="R1", value="4.7k",
        position=(I2C0_X + 18, I2C0_Y - 8), rotation=0.0
    )
    r2 = sch.components.add(
        lib_id="Device:R", reference="R2", value="4.7k",
        position=(I2C0_X + 25, I2C0_Y - 8), rotation=0.0
    )
    # デカップリングコンデンサ
    c1 = sch.components.add(
        lib_id="Device:C", reference="C1", value="100n",
        position=(I2C0_X + 10, I2C0_Y + 15), rotation=0.0
    )
    print("  B: I2C0 (J3, R1, R2, C1)")

    # --- Section C: I2C1 ---
    j4 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x04",
        reference="J4", value="Grove_I2C1",
        position=(I2C1_X, I2C1_Y), rotation=0.0
    )
    r3 = sch.components.add(
        lib_id="Device:R", reference="R3", value="4.7k",
        position=(I2C1_X + 18, I2C1_Y - 8), rotation=0.0
    )
    r4 = sch.components.add(
        lib_id="Device:R", reference="R4", value="4.7k",
        position=(I2C1_X + 25, I2C1_Y - 8), rotation=0.0
    )
    c2 = sch.components.add(
        lib_id="Device:C", reference="C2", value="100n",
        position=(I2C1_X + 10, I2C1_Y + 15), rotation=0.0
    )
    print("  C: I2C1 (J4, R3, R4, C2)")

    # --- Section D: ADC ---
    j5 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J5", value="ADC0",
        position=(ADC_X, ADC_Y), rotation=0.0
    )
    j6 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J6", value="ADC1",
        position=(ADC_X + ADC_SPACING, ADC_Y), rotation=0.0
    )
    j7 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J7", value="ADC2",
        position=(ADC_X + ADC_SPACING * 2, ADC_Y), rotation=0.0
    )
    # ADC_VREF デカップリング
    c3 = sch.components.add(
        lib_id="Device:C", reference="C3", value="100n",
        position=(ADC_X + ADC_SPACING * 2 + 15, ADC_Y + 5), rotation=0.0
    )
    print("  D: ADC (J5, J6, J7, C3)")

    # --- Section E: 1-Wire ---
    j8 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J8", value="1-Wire",
        position=(WIRE1_X, WIRE1_Y), rotation=0.0
    )
    r5 = sch.components.add(
        lib_id="Device:R", reference="R5", value="4.7k",
        position=(WIRE1_X + 15, WIRE1_Y - 5), rotation=0.0
    )
    print("  E: 1-Wire (J8, R5)")

    # --- Section F: Fan PWM ---
    # MOSFET回路: GP22 → R6(ゲート抵抗) → Q1(Gate)
    #             Q1(Drain) → D1 → J9(Fan)
    #             R7(プルダウン) → GND
    r6 = sch.components.add(
        lib_id="Device:R", reference="R6", value="100",
        position=(FAN_X, FAN_Y), rotation=90.0
    )
    q1 = sch.components.add(
        lib_id="Device:Q_NMOS_GSD", reference="Q1", value="2N7002",
        position=(FAN_X + 15, FAN_Y + 8), rotation=0.0
    )
    r7 = sch.components.add(
        lib_id="Device:R", reference="R7", value="10k",
        position=(FAN_X + 8, FAN_Y + 18), rotation=0.0
    )
    d1 = sch.components.add(
        lib_id="Device:D", reference="D1", value="1N4148",
        position=(FAN_X + 25, FAN_Y + 3), rotation=90.0
    )
    j9 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x02",
        reference="J9", value="Fan",
        position=(FAN_X + 35, FAN_Y), rotation=0.0
    )
    print("  F: Fan PWM (Q1, R6, R7, D1, J9)")

    # =========================================================================
    # 2. ネットラベル配置 (電源レール)
    # =========================================================================
    print("\n[2] Adding net labels...")

    # 3V3とGNDのラベルを各ブロックの適切な場所に配置
    sch.add_label(text="3V3", position=(J2_X + 15, PICO_Y + 38))      # J2 pin16近く
    sch.add_label(text="GND", position=(J1_X - 10, PICO_Y + 5))       # J1 pin3近く
    sch.add_label(text="VSYS", position=(J2_X + 15, PICO_Y + 46))     # J2 pin19近く
    print("  Added power labels")

    # =========================================================================
    # 3. ワイヤー接続 (Manhattan routing)
    # =========================================================================
    print("\n[3] Adding wire connections...")

    wire_count = 0

    def add_wire(ref1, pin1, ref2, pin2, label=""):
        """ワイヤー追加ヘルパー"""
        global wire_count
        try:
            sch.add_wire_between_pins(ref1, pin1, ref2, pin2)
            wire_count += 1
        except Exception as e:
            print(f"  WARN: {label} wire failed ({ref1}.{pin1} → {ref2}.{pin2}): {e}")

    # --- I2C0 信号線 ---
    # J1.6 (GP4/SDA) → R1.1, R1.1 → J3.3 (SDA)
    add_wire("J1", "6", "R1", "1", "I2C0 SDA from Pico")
    add_wire("R1", "1", "J3", "3", "I2C0 SDA to Grove")
    # J1.7 (GP5/SCL) → R2.1, R2.1 → J3.4 (SCL)
    add_wire("J1", "7", "R2", "1", "I2C0 SCL from Pico")
    add_wire("R2", "1", "J3", "4", "I2C0 SCL to Grove")
    print(f"  I2C0 signal: {wire_count} wires")

    # --- I2C1 信号線 ---
    add_wire("J1", "9", "R3", "1", "I2C1 SDA from Pico")
    add_wire("R3", "1", "J4", "3", "I2C1 SDA to Grove")
    add_wire("J1", "10", "R4", "1", "I2C1 SCL from Pico")
    add_wire("R4", "1", "J4", "4", "I2C1 SCL to Grove")
    print(f"  I2C1 signal: {wire_count} wires")

    # --- 1-Wire 信号線 ---
    add_wire("J1", "4", "R5", "1", "1-Wire from Pico")
    add_wire("R5", "1", "J8", "2", "1-Wire to connector")
    print(f"  1-Wire signal: {wire_count} wires")

    # --- ADC 信号線 ---
    add_wire("J2", "11", "J5", "3", "ADC0")
    add_wire("J2", "12", "J6", "3", "ADC1")
    add_wire("J2", "14", "J7", "3", "ADC2")
    print(f"  ADC signal: {wire_count} wires")

    # --- Fan PWM ---
    add_wire("J2", "9", "R6", "1", "Fan PWM from Pico")
    add_wire("R6", "2", "Q1", "1", "Gate resistor to MOSFET")
    add_wire("R6", "2", "R7", "1", "Gate to pulldown junction")
    add_wire("Q1", "2", "D1", "1", "Drain to flyback cathode")
    add_wire("D1", "1", "J9", "1", "Flyback to Fan+")
    add_wire("J2", "19", "J9", "1", "VSYS to Fan+")
    print(f"  Fan PWM: {wire_count} wires")

    # --- 3V3 電源分配 ---
    # プルアップ抵抗の上端 → 3V3
    add_wire("J2", "16", "R1", "2", "3V3 to R1 pullup")
    add_wire("J2", "16", "R2", "2", "3V3 to R2 pullup")
    add_wire("J2", "16", "R3", "2", "3V3 to R3 pullup")
    add_wire("J2", "16", "R4", "2", "3V3 to R4 pullup")
    add_wire("J2", "16", "R5", "2", "3V3 to R5 pullup")
    # Grove VCC
    add_wire("J2", "16", "J3", "2", "3V3 to I2C0 VCC")
    add_wire("J2", "16", "J4", "2", "3V3 to I2C1 VCC")
    # センサー電源
    add_wire("J2", "16", "J5", "2", "3V3 to ADC0")
    add_wire("J2", "16", "J6", "2", "3V3 to ADC1")
    add_wire("J2", "16", "J7", "2", "3V3 to ADC2")
    add_wire("J2", "16", "J8", "3", "3V3 to 1-Wire VDD")
    print(f"  3V3 power: {wire_count} wires")

    # --- GND 分配 ---
    add_wire("J1", "3", "J3", "1", "GND to I2C0")
    add_wire("J1", "8", "J4", "1", "GND to I2C1")
    add_wire("J2", "3", "J5", "1", "GND to ADC0")
    add_wire("J2", "8", "J6", "1", "GND to ADC1")
    add_wire("J2", "13", "J7", "1", "GND to ADC2")
    add_wire("J2", "18", "J8", "1", "GND to 1-Wire")
    add_wire("J2", "18", "J9", "2", "GND to Fan-")
    # パッシブのGND
    add_wire("J2", "18", "C1", "2", "GND to C1")
    add_wire("J2", "18", "C2", "2", "GND to C2")
    add_wire("J2", "18", "C3", "2", "GND to C3")
    add_wire("J2", "18", "Q1", "3", "GND to MOSFET Source")
    add_wire("J2", "18", "R7", "2", "GND to R7 pulldown")
    print(f"  GND: {wire_count} wires")

    # --- ADC_VREF ---
    add_wire("J2", "15", "C3", "1", "ADC_VREF to C3")
    print(f"  ADC_VREF: {wire_count} wires")

    # --- Flyback Diode ---
    add_wire("D1", "2", "J2", "19", "Flyback anode to VSYS")
    print(f"  Total wires: {wire_count}")

    # =========================================================================
    # 4. 保存 (KiCad 7互換)
    # =========================================================================
    output_file = output_dir / "grove_shield_v3.kicad_sch"
    sch.save_as(str(output_file))
    print(f"\n  Saved: {output_file}")

    # KiCad 7 互換変換
    import re
    with open(output_file, 'r') as f:
        content = f.read()

    content = content.replace('(version 20250114)', '(version 20230121)')
    content = re.sub(r'\s*\(generator_version \"9\.0\"\)\n', '\n', content)
    content = content.replace('(generator "eeschema")', '(generator eeschema)')
    content = re.sub(r'\s*\(exclude_from_sim \w+\)\n', '\n', content)
    content = re.sub(r'\s*\(fields_autoplaced \w+\)\n', '\n', content)
    content = re.sub(r'\s*\(embedded_fonts \w+\)\n', '\n', content)

    # KiCad 7互換ファイルとして保存
    output_kicad7 = output_dir / "grove_shield_v3_kicad7.kicad_sch"
    with open(output_kicad7, 'w') as f:
        f.write(content)
    print(f"  Saved KiCad 7 compatible: {output_kicad7}")

    # 統計
    wire_count_file = content.count('(wire')
    symbol_count = content.count('(symbol (') if '(symbol (' in content else content.count('(symbol\n')
    print(f"\n  File stats: {wire_count_file} wires, {symbol_count} symbols")

    if wire_count_file >= 40:
        print("  PASSED: Wire count OK")
    else:
        print(f"  WARNING: Only {wire_count_file} wires (expected 44+)")

except Exception as e:
    print(f"\n  FAILED: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("\n=== Done ===")
