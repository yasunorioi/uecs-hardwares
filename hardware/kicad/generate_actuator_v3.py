#!/usr/bin/env python3
"""
Pico 2 W アクチュエータノード基板 - 回路図自動生成スクリプト
Version: 3.0
Created: 2026-02-08

4ch リレー駆動基板
各チャンネル: GPIO → R(330Ω) → PC817 → R(100Ω) → 2N7002(Gate)
              2N7002(Drain) → リレーコイル → 5V
              + フライバックダイオード + プルダウン + LED

レイアウト:
  上部: Pico ピンヘッダ (J1左, J2右)
  中段: 4ch フォトカプラ+MOSFET+リレー (横並び)
  下段: 出力端子台 (J3-J6) + 電源コンデンサ
"""

import sys
import os
import re
from pathlib import Path

sys.path.insert(0, "/home/yasu/unipi-agri-ha/.venv/lib/python3.12/site-packages")
import kicad_sch_api as ksa

os.environ["KICAD_SYMBOL_DIR"] = "/home/yasu/unipi-agri-ha/hardware/kicad/kicad-symbols"

output_dir = Path("/home/yasu/unipi-agri-ha/hardware/kicad")

print("=== Actuator Board v3 - 4ch Relay Driver ===\n")

try:
    sch = ksa.create_schematic("Pico 2W Actuator Board")
    sch.set_title_block(
        title="Pico 2W Actuator Board - 4ch Relay",
        date="2026-02-08",
        rev="3.0",
        company="AgriHA Project"
    )

    # =========================================================================
    # レイアウト定数 (mm) - A4横: 297 x 210mm
    # =========================================================================

    # --- Pico Headers (上部) ---
    PICO_Y = 30.0
    J1_X = 40.0       # 左ヘッダ
    J2_X = 80.0        # 右ヘッダ (近く配置、ほとんどNCなので)

    # --- チャンネル配置 (中段 横並び) ---
    CH_START_X = 120.0  # CH0開始X
    CH_SPACING = 40.0   # チャンネル間隔
    CH_Y_PHOTOCOUPLER = 35.0   # フォトカプラ行
    CH_Y_MOSFET = 75.0         # MOSFET行
    CH_Y_RELAY = 110.0         # リレー行
    CH_Y_TERMINAL = 150.0      # 端子台行

    # --- 電源 (右下) ---
    PWR_X = 40.0
    PWR_Y = 150.0

    # チャンネル名
    CH_NAMES = ["Irri", "Valve", "VenFan", "Relay"]
    # 対応GPIO (Pico Left header の pin番号)
    # GP10=J1 pin14, GP11=J1 pin15, GP12=J1 pin16, GP13=J1 pin17
    CH_GPIO_PINS = ["14", "15", "16", "17"]

    # =========================================================================
    # 1. コンポーネント配置
    # =========================================================================
    print("[1] Adding components...")

    # --- Pico Headers ---
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
    print("  Pico Headers (J1, J2)")

    # --- 電源コンデンサ ---
    c1 = sch.components.add(
        lib_id="Device:C_Polarized", reference="C1", value="100u",
        position=(PWR_X, PWR_Y), rotation=0.0
    )
    c2 = sch.components.add(
        lib_id="Device:C", reference="C2", value="100n",
        position=(PWR_X + 12, PWR_Y), rotation=0.0
    )
    # 外部電源端子
    j7 = sch.components.add(
        lib_id="Connector_Generic:Conn_01x02",
        reference="J7", value="5V_EXT",
        position=(PWR_X + 25, PWR_Y), rotation=0.0
    )
    print("  Power caps (C1, C2) + J7")

    # --- 4チャンネル繰り返し ---
    for ch in range(4):
        cx = CH_START_X + ch * CH_SPACING  # チャンネルのX位置
        n = ch + 1  # 1-based index

        # フォトカプラ入力抵抗 (R13-R16: 330Ω, GPIO → PC817 LED)
        sch.components.add(
            lib_id="Device:R", reference=f"R{12+n}", value="330",
            position=(cx, CH_Y_PHOTOCOUPLER), rotation=0.0
        )

        # フォトカプラ (U2-U5: PC817)
        sch.components.add(
            lib_id="Isolator:PC817", reference=f"U{1+n}", value="PC817",
            position=(cx, CH_Y_PHOTOCOUPLER + 18), rotation=0.0
        )

        # ゲート抵抗 (R1-R4: 100Ω)
        sch.components.add(
            lib_id="Device:R", reference=f"R{n}", value="100",
            position=(cx, CH_Y_MOSFET - 5), rotation=0.0
        )

        # プルダウン抵抗 (R5-R8: 10kΩ)
        sch.components.add(
            lib_id="Device:R", reference=f"R{4+n}", value="10k",
            position=(cx + 8, CH_Y_MOSFET + 5), rotation=0.0
        )

        # MOSFET (Q1-Q4: 2N7002)
        sch.components.add(
            lib_id="Device:Q_NMOS_GSD", reference=f"Q{n}", value="2N7002",
            position=(cx + 15, CH_Y_MOSFET), rotation=0.0
        )

        # フライバックダイオード (D1-D4: 1N4148)
        sch.components.add(
            lib_id="Device:D", reference=f"D{n}", value="1N4148",
            position=(cx + 22, CH_Y_RELAY - 5), rotation=90.0
        )

        # LED電流制限抵抗 (R9-R12: 330Ω)
        sch.components.add(
            lib_id="Device:R", reference=f"R{8+n}", value="330",
            position=(cx + 28, CH_Y_MOSFET), rotation=0.0
        )

        # インジケータLED (LED1-LED4)
        sch.components.add(
            lib_id="Device:LED", reference=f"LED{n}", value="Red",
            position=(cx + 28, CH_Y_MOSFET + 12), rotation=0.0
        )

        # リレー (K1-K4) - 汎用リレーシンボルとして Conn_01x02 (コイル側)
        sch.components.add(
            lib_id="Relay:SANYOU_SRD_Form_C", reference=f"K{n}", value=CH_NAMES[ch],
            position=(cx + 15, CH_Y_RELAY), rotation=0.0
        )

        # 出力端子台 (J3-J6: NO/COM/NC)
        sch.components.add(
            lib_id="Connector_Generic:Conn_01x03",
            reference=f"J{2+n}", value=f"OUT_{CH_NAMES[ch]}",
            position=(cx + 15, CH_Y_TERMINAL), rotation=0.0
        )

        print(f"  CH{ch} ({CH_NAMES[ch]}): R{12+n}, U{1+n}, R{n}, R{4+n}, Q{n}, D{n}, R{8+n}, LED{n}, K{n}, J{2+n}")

    # =========================================================================
    # 2. ネットラベル
    # =========================================================================
    print("\n[2] Adding net labels...")
    sch.add_label(text="3V3", position=(J2_X + 15, PICO_Y + 38))
    sch.add_label(text="GND", position=(J1_X - 10, PICO_Y + 5))
    sch.add_label(text="VSYS", position=(J2_X + 15, PICO_Y + 46))
    print("  Added power labels")

    # =========================================================================
    # 3. ワイヤー接続
    # =========================================================================
    print("\n[3] Adding wire connections...")

    wire_count = 0

    def add_wire(ref1, pin1, ref2, pin2, label=""):
        global wire_count
        try:
            sch.add_wire_between_pins(ref1, pin1, ref2, pin2)
            wire_count += 1
        except Exception as e:
            print(f"  WARN: {label}: {ref1}.{pin1} → {ref2}.{pin2}: {e}")

    # --- 各チャンネルの配線 ---
    for ch in range(4):
        n = ch + 1
        gpio_pin = CH_GPIO_PINS[ch]  # J1のピン番号

        # === 1次側 (3.3V制御) ===
        # GPIO → R13-16(pin1) フォトカプラ入力抵抗
        add_wire("J1", gpio_pin, f"R{12+n}", "1", f"CH{ch} GPIO→R")

        # R13-16(pin2) → U2-5(pin1) フォトカプラ LED アノード
        add_wire(f"R{12+n}", "2", f"U{1+n}", "1", f"CH{ch} R→PC817 anode")

        # U2-5(pin2) → GND  フォトカプラ LED カソード
        add_wire(f"U{1+n}", "2", "J1", "3", f"CH{ch} PC817 cathode→GND")

        # === 2次側 (5V リレー駆動) ===
        # U2-5(pin4) → 5V (VSYS)  フォトカプラ コレクタ
        add_wire(f"U{1+n}", "4", "J2", "19", f"CH{ch} PC817 collector→5V")

        # U2-5(pin3) → R1-4(pin1)  フォトカプラ エミッタ → ゲート抵抗
        add_wire(f"U{1+n}", "3", f"R{n}", "1", f"CH{ch} PC817 emitter→Rgate")

        # R1-4(pin2) → Q1-4(pin1)  ゲート抵抗 → MOSFET Gate
        add_wire(f"R{n}", "2", f"Q{n}", "1", f"CH{ch} Rgate→Gate")

        # R5-8(pin1) → Q1-4(pin1)  プルダウン上端 → Gate
        add_wire(f"R{4+n}", "1", f"Q{n}", "1", f"CH{ch} Rpulldown→Gate")

        # R5-8(pin2) → GND  プルダウン下端
        add_wire(f"R{4+n}", "2", "J1", "3", f"CH{ch} Rpulldown→GND")

        # Q1-4(pin3) → GND  MOSFET Source
        add_wire(f"Q{n}", "3", "J1", "3", f"CH{ch} Source→GND")

        # Q1-4(pin2) → K1-4(coil-)  MOSFET Drain → リレーコイル
        # SANYOU_SRD_Form_C: pin1=coil+, pin2=coil-, pin3=COM, pin4=NO, pin5=NC
        add_wire(f"Q{n}", "2", f"K{n}", "2", f"CH{ch} Drain→Relay coil-")

        # K1-4(coil+) → 5V  リレーコイル+
        add_wire(f"K{n}", "1", "J2", "19", f"CH{ch} Relay coil+→5V")

        # D1-4 フライバックダイオード (リレーコイル並列)
        # D anode(pin2) → Q Drain(pin2), D cathode(pin1) → 5V(K coil+)
        add_wire(f"D{n}", "2", f"Q{n}", "2", f"CH{ch} Flyback anode→Drain")
        add_wire(f"D{n}", "1", f"K{n}", "1", f"CH{ch} Flyback cathode→5V")

        # LED + 抵抗 (Drain分岐で状態表示)
        # R9-12(pin1) → Q Drain(pin2)
        add_wire(f"R{8+n}", "1", f"Q{n}", "2", f"CH{ch} RLED→Drain")

        # R9-12(pin2) → LED anode(pin1)
        add_wire(f"R{8+n}", "2", f"LED{n}", "1", f"CH{ch} RLED→LED anode")

        # LED cathode(pin2) → GND
        add_wire(f"LED{n}", "2", "J1", "3", f"CH{ch} LED cathode→GND")

        # リレー接点 → 端子台
        # K pin3(COM) → J pin1
        add_wire(f"K{n}", "3", f"J{2+n}", "1", f"CH{ch} COM→terminal")
        # K pin4(NO) → J pin2
        add_wire(f"K{n}", "4", f"J{2+n}", "2", f"CH{ch} NO→terminal")
        # K pin5(NC) → J pin3
        add_wire(f"K{n}", "5", f"J{2+n}", "3", f"CH{ch} NC→terminal")

        print(f"  CH{ch} ({CH_NAMES[ch]}): {wire_count} wires total")

    # --- 電源配線 ---
    # C1(100uF) 5V電源平滑
    add_wire("J2", "19", "C1", "1", "5V→C1+")
    add_wire("C1", "2", "J1", "3", "C1-→GND")

    # C2(100nF) 3.3Vデカップリング
    add_wire("J2", "16", "C2", "1", "3V3→C2")
    add_wire("C2", "2", "J1", "3", "C2→GND")

    # J7(外部5V入力) - オプション
    add_wire("J7", "1", "J2", "19", "EXT 5V→VSYS")
    add_wire("J7", "2", "J1", "3", "EXT GND")

    print(f"\n  Total wires: {wire_count}")

    # =========================================================================
    # 4. 保存 (KiCad 9 + KiCad 7互換)
    # =========================================================================
    output_file = output_dir / "actuator_board_v3.kicad_sch"
    sch.save_as(str(output_file))
    print(f"\n  Saved: {output_file}")

    # KiCad 7互換変換
    with open(output_file, 'r') as f:
        content = f.read()

    content = content.replace('(version 20250114)', '(version 20230121)')
    content = re.sub(r'\s*\(generator_version \"9\.0\"\)\n', '\n', content)
    content = content.replace('(generator "eeschema")', '(generator eeschema)')
    content = re.sub(r'\s*\(exclude_from_sim \w+\)\n', '\n', content)
    content = re.sub(r'\s*\(fields_autoplaced \w+\)\n', '\n', content)
    content = re.sub(r'\s*\(embedded_fonts \w+\)\n', '\n', content)

    output_kicad7 = output_dir / "actuator_board_v3_kicad7.kicad_sch"
    with open(output_kicad7, 'w') as f:
        f.write(content)
    print(f"  Saved KiCad 7: {output_kicad7}")

    # 統計
    wire_file = content.count('(wire')
    sym_count = len(re.findall(r'\(symbol\s*\n\s*\(lib_id', content))
    print(f"\n  Stats: {wire_file} wires, {sym_count} component instances")

except Exception as e:
    print(f"\n  FAILED: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("\n=== Done ===")
