#!/usr/bin/env python3
"""
Grove Shield for Pi Pico 代替基板 - 回路図自動生成スクリプト (kicad-sch-api v2)
Version: 2.0
Created: 2026-02-08
Reference: grove_shield_connections.md v1.0
"""

import sys
import os
from pathlib import Path

# venv内のkicad-sch-apiを使用
sys.path.insert(0, "/home/yasu/unipi-agri-ha/.venv/lib/python3.12/site-packages")

import kicad_sch_api as ksa

print(f"kicad-sch-api version: {ksa.__version__ if hasattr(ksa, '__version__') else 'unknown'}")

# KiCadシンボルライブラリパスを設定
os.environ["KICAD_SYMBOL_DIR"] = "/home/yasu/unipi-agri-ha/hardware/kicad/kicad-symbols"
print(f"KICAD_SYMBOL_DIR set to: {os.environ['KICAD_SYMBOL_DIR']}")

# 出力ディレクトリ
output_dir = Path("/home/yasu/unipi-agri-ha/hardware/kicad")
output_dir.mkdir(parents=True, exist_ok=True)

# =============================================================================
# Grove Shield 回路図生成
# =============================================================================
print("\n=== Generating Grove Shield for Pi Pico Schematic ===")

try:
    # 新しい回路図を作成
    sch = ksa.create_schematic("Grove Shield for Pi Pico Alternative")
    sch.set_title_block(
        title="Grove Shield for Pi Pico Alternative",
        date="2026-02-08",
        rev="2.0",
        company="AgriHA Project"
    )

    print("Creating schematic...")

    # =========================================================================
    # 1. コンポーネント配置
    # =========================================================================
    print("\n[1] Adding components...")

    # J1, J2: Pico ピンヘッダ (Conn_01x20)
    j1 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x20",
        reference="J1",
        value="Pico_Left",
        position=(50.0, 50.0),
        rotation=0.0
    )
    print("  Added J1 (Pico Left Header)")

    j2 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x20",
        reference="J2",
        value="Pico_Right",
        position=(150.0, 50.0),
        rotation=0.0
    )
    print("  Added J2 (Pico Right Header)")

    # J3, J4: Grove I2C コネクタ (Conn_01x04)
    j3 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x04",
        reference="J3",
        value="Grove_I2C0",
        position=(50.0, 150.0),
        rotation=0.0
    )
    print("  Added J3 (Grove I2C0)")

    j4 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x04",
        reference="J4",
        value="Grove_I2C1",
        position=(150.0, 150.0),
        rotation=0.0
    )
    print("  Added J4 (Grove I2C1)")

    # J5, J6, J7: ADC ターミナルブロック (Conn_01x03)
    j5 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J5",
        value="ADC0",
        position=(50.0, 200.0),
        rotation=0.0
    )
    print("  Added J5 (ADC0)")

    j6 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J6",
        value="ADC1",
        position=(100.0, 200.0),
        rotation=0.0
    )
    print("  Added J6 (ADC1)")

    j7 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J7",
        value="ADC2",
        position=(150.0, 200.0),
        rotation=0.0
    )
    print("  Added J7 (ADC2)")

    # J8: 1-Wire ターミナルブロック (Conn_01x03)
    j8 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x03",
        reference="J8",
        value="1-Wire",
        position=(200.0, 200.0),
        rotation=0.0
    )
    print("  Added J8 (1-Wire)")

    # J9: Fan コネクタ (Conn_01x02)
    j9 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x02",
        reference="J9",
        value="Fan",
        position=(250.0, 200.0),
        rotation=0.0
    )
    print("  Added J9 (Fan)")

    # R1-R7: 抵抗
    r1 = sch.components.add(lib_id="Device:R", reference="R1", value="4.7k", position=(70.0, 100.0), rotation=90.0)
    r2 = sch.components.add(lib_id="Device:R", reference="R2", value="4.7k", position=(80.0, 100.0), rotation=90.0)
    r3 = sch.components.add(lib_id="Device:R", reference="R3", value="4.7k", position=(170.0, 100.0), rotation=90.0)
    r4 = sch.components.add(lib_id="Device:R", reference="R4", value="4.7k", position=(180.0, 100.0), rotation=90.0)
    r5 = sch.components.add(lib_id="Device:R", reference="R5", value="4.7k", position=(210.0, 100.0), rotation=90.0)
    r6 = sch.components.add(lib_id="Device:R", reference="R6", value="100", position=(260.0, 120.0), rotation=0.0)
    r7 = sch.components.add(lib_id="Device:R", reference="R7", value="10k", position=(260.0, 140.0), rotation=90.0)
    print("  Added R1-R7 (Resistors)")

    # C1-C3: コンデンサ
    c1 = sch.components.add(lib_id="Device:C", reference="C1", value="100n", position=(60.0, 130.0), rotation=90.0)
    c2 = sch.components.add(lib_id="Device:C", reference="C2", value="100n", position=(160.0, 130.0), rotation=90.0)
    c3 = sch.components.add(lib_id="Device:C", reference="C3", value="100n", position=(120.0, 180.0), rotation=90.0)
    print("  Added C1-C3 (Capacitors)")

    # Q1: MOSFET (Q_NMOS_GSD)
    q1 = sch.components.add(lib_id="Device:Q_NMOS_GSD", reference="Q1", value="2N7002", position=(270.0, 150.0), rotation=0.0)
    print("  Added Q1 (MOSFET 2N7002)")

    # D1: Diode
    d1 = sch.components.add(lib_id="Device:D", reference="D1", value="1N4148", position=(280.0, 160.0), rotation=90.0)
    print("  Added D1 (Diode 1N4148)")

    # =========================================================================
    # 2. ラベル配置
    # =========================================================================
    print("\n[2] Adding power labels...")

    # 電源ラベル (座標は仮。実際のピン位置に合わせて調整が必要)
    sch.add_label(text="3V3", position=(150.0, 30.0))
    sch.add_label(text="GND", position=(50.0, 30.0))
    sch.add_label(text="VSYS", position=(150.0, 20.0))

    print("  Added power labels (3V3, GND, VSYS)")

    # =========================================================================
    # 3. ワイヤー接続
    # =========================================================================
    print("\n[3] Adding wire connections...")

    # I2C0 接続 (GP4/GP5)
    # J1 pin 6 (GP4) → R1 pin 1 → J3 pin 3 (SDA)
    # J1 pin 7 (GP5) → R2 pin 1 → J3 pin 4 (SCL)
    # R1 pin 2 → 3V3, R2 pin 2 → 3V3
    try:
        sch.add_wire_between_pins("J1", "6", "R1", "1")
        sch.add_wire_between_pins("R1", "1", "J3", "3")
        sch.add_wire_between_pins("J1", "7", "R2", "1")
        sch.add_wire_between_pins("R2", "1", "J3", "4")
        print("  Added I2C0 signal wires (GP4/GP5 → J3)")
    except Exception as e:
        print(f"  ⚠️ Warning: I2C0 wiring failed: {e}")

    # I2C1 接続 (GP6/GP7)
    # J1 pin 9 (GP6) → R3 pin 1 → J4 pin 3 (SDA)
    # J1 pin 10 (GP7) → R4 pin 1 → J4 pin 4 (SCL)
    try:
        sch.add_wire_between_pins("J1", "9", "R3", "1")
        sch.add_wire_between_pins("R3", "1", "J4", "3")
        sch.add_wire_between_pins("J1", "10", "R4", "1")
        sch.add_wire_between_pins("R4", "1", "J4", "4")
        print("  Added I2C1 signal wires (GP6/GP7 → J4)")
    except Exception as e:
        print(f"  ⚠️ Warning: I2C1 wiring failed: {e}")

    # 1-Wire 接続 (GP2)
    # J1 pin 4 (GP2) → R5 pin 1 → J8 pin 2 (DQ)
    try:
        sch.add_wire_between_pins("J1", "4", "R5", "1")
        sch.add_wire_between_pins("R5", "1", "J8", "2")
        print("  Added 1-Wire signal wires (GP2 → J8)")
    except Exception as e:
        print(f"  ⚠️ Warning: 1-Wire wiring failed: {e}")

    # ADC 接続 (GP26/GP27/GP28)
    # J2 pin 11 (GP26) → J5 pin 3
    # J2 pin 12 (GP27) → J6 pin 3
    # J2 pin 14 (GP28) → J7 pin 3
    try:
        sch.add_wire_between_pins("J2", "11", "J5", "3")
        sch.add_wire_between_pins("J2", "12", "J6", "3")
        sch.add_wire_between_pins("J2", "14", "J7", "3")
        print("  Added ADC signal wires (GP26/27/28 → J5/6/7)")
    except Exception as e:
        print(f"  ⚠️ Warning: ADC wiring failed: {e}")

    # ファン PWM 接続 (GP22)
    # J2 pin 9 (GP22) → R6 pin 1
    # R6 pin 2 → Q1 pin 1 (Gate)
    # R6 pin 2 → R7 pin 1 (プルダウン接続点)
    # Q1 pin 2 (Drain) → D1 pin 1 (Cathode)
    # D1 pin 1 → J9 pin 1 (Fan +)
    # J2 pin 19 (VSYS) → J9 pin 1 (Fan + 電源)
    try:
        sch.add_wire_between_pins("J2", "9", "R6", "1")
        sch.add_wire_between_pins("R6", "2", "Q1", "1")
        sch.add_wire_between_pins("R6", "2", "R7", "1")
        sch.add_wire_between_pins("Q1", "2", "D1", "1")
        sch.add_wire_between_pins("D1", "1", "J9", "1")
        sch.add_wire_between_pins("J2", "19", "J9", "1")
        print("  Added Fan PWM circuit wires (GP22 → MOSFET → J9)")
    except Exception as e:
        print(f"  ⚠️ Warning: Fan PWM wiring failed: {e}")

    # 電源接続（3V3）- 主要な接続のみ（ラベル活用を推奨）
    # J2 pin 16 (3V3) → R1/R2/R3/R4/R5 pin 2 (プルアップ電源)
    # J2 pin 16 → J3/J4 pin 2 (Grove VCC)
    # J2 pin 16 → J5/J6/J7/J8 pin 2 (センサー電源)
    try:
        # プルアップ抵抗への3V3供給
        sch.add_wire_between_pins("J2", "16", "R1", "2")
        sch.add_wire_between_pins("J2", "16", "R2", "2")
        sch.add_wire_between_pins("J2", "16", "R3", "2")
        sch.add_wire_between_pins("J2", "16", "R4", "2")
        sch.add_wire_between_pins("J2", "16", "R5", "2")
        # Grove/センサーへの3V3供給
        sch.add_wire_between_pins("J2", "16", "J3", "2")
        sch.add_wire_between_pins("J2", "16", "J4", "2")
        sch.add_wire_between_pins("J2", "16", "J5", "2")
        sch.add_wire_between_pins("J2", "16", "J6", "2")
        sch.add_wire_between_pins("J2", "16", "J7", "2")
        sch.add_wire_between_pins("J2", "16", "J8", "3")  # J8は pin3がVDD
        print("  Added 3V3 power distribution wires")
    except Exception as e:
        print(f"  ⚠️ Warning: 3V3 power wiring failed: {e}")

    # 電源接続（GND）- 主要な接続のみ
    # J1 pin 3/8/13/18 → GND
    # J2 pin 3/8/13/18 → GND
    # J3/J4/J5/J6/J7/J8/J9 pin 1 → GND
    # C1/C2/C3 pin 2 → GND
    # Q1 pin 3 (Source) → GND
    # R7 pin 2 → GND
    try:
        # Grove/センサーのGND
        sch.add_wire_between_pins("J1", "3", "J3", "1")
        sch.add_wire_between_pins("J1", "8", "J4", "1")
        sch.add_wire_between_pins("J2", "3", "J5", "1")
        sch.add_wire_between_pins("J2", "8", "J6", "1")
        sch.add_wire_between_pins("J2", "13", "J7", "1")
        sch.add_wire_between_pins("J2", "18", "J8", "1")
        sch.add_wire_between_pins("J2", "18", "J9", "2")
        # コンデンサ/MOSFET/プルダウンのGND
        sch.add_wire_between_pins("J2", "18", "C1", "2")
        sch.add_wire_between_pins("J2", "18", "C2", "2")
        sch.add_wire_between_pins("J2", "18", "C3", "2")
        sch.add_wire_between_pins("J2", "18", "Q1", "3")
        sch.add_wire_between_pins("J2", "18", "R7", "2")
        print("  Added GND distribution wires")
    except Exception as e:
        print(f"  ⚠️ Warning: GND wiring failed: {e}")

    # ADC_VREF 接続
    # J2 pin 15 (ADC_VREF) → C3 pin 1
    try:
        sch.add_wire_between_pins("J2", "15", "C3", "1")
        print("  Added ADC_VREF wire (J2-15 → C3)")
    except Exception as e:
        print(f"  ⚠️ Warning: ADC_VREF wiring failed: {e}")

    # Flyback Diode 接続
    # D1 pin 2 (Anode) → J2 pin 19 (VSYS)
    try:
        sch.add_wire_between_pins("D1", "2", "J2", "19")
        print("  Added Flyback diode wire (D1 Anode → VSYS)")
    except Exception as e:
        print(f"  ⚠️ Warning: Flyback diode wiring failed: {e}")

    # =========================================================================
    # 4. ファイル保存
    # =========================================================================
    output_file = output_dir / "grove_shield_v2.kicad_sch"
    sch.save_as(str(output_file))
    print(f"\n✓ Saved: {output_file}")

    # ファイルサイズ・統計確認
    file_size = output_file.stat().st_size
    print(f"  File size: {file_size} bytes")

    with open(output_file, 'r') as f:
        content = f.read()
        wire_count = content.count('(wire')
        symbol_count = content.count('(symbol')
        label_count = content.count('(label')
        print(f"  Wire count: {wire_count}")
        print(f"  Symbol count: {symbol_count}")
        print(f"  Label count: {label_count}")

    if wire_count >= 20 and symbol_count >= 15:
        print("  ✅ Grove Shield Schematic Generation PASSED")
    else:
        print(f"  ⚠️ WARNING: Wire count={wire_count}, Symbol count={symbol_count}")

except Exception as e:
    print(f"  ❌ Schematic Generation FAILED: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("\n" + "="*60)
print("✅ Grove Shield Schematic Generation Completed!")
print("="*60)
print(f"\nOutput file: {output_file}")
print("Next step: Open in KiCad 8 for visual verification and ERC check")
