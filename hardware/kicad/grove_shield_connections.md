# Grove Shield for Pi Pico 代替基板 - 接続定義書

**バージョン**: 1.0
**作成日**: 2026-02-08
**用途**: KiCad schematic API (kicad-sch-api) による回路図自動生成用の接続定義

---

## 1. コンポーネント一覧

| RefDes | lib_id | Value | Position (mm) | 備考 |
|--------|--------|-------|--------------|------|
| J1 | Connector_Generic:Conn_01x20_Pin | - | (50, 50) | Pico 左側ヘッダ（GP0-GP15） |
| J2 | Connector_Generic:Conn_01x20_Pin | - | (150, 50) | Pico 右側ヘッダ（GP16-GP28, 電源） |
| J3 | Connector:Conn_01x04_Female | Grove I2C0 | (50, 150) | Grove HY2.0-4P I2C0 |
| J4 | Connector:Conn_01x04_Female | Grove I2C1 | (150, 150) | Grove HY2.0-4P I2C1 |
| J5 | TerminalBlock:TerminalBlock_bornier-3_P5.08mm | ADC0 | (50, 200) | 土壌水分センサー |
| J6 | TerminalBlock:TerminalBlock_bornier-3_P5.08mm | ADC1 | (100, 200) | 日射センサー |
| J7 | TerminalBlock:TerminalBlock_bornier-3_P5.08mm | ADC2 | (150, 200) | 予備ADC |
| J8 | TerminalBlock:TerminalBlock_bornier-3_P5.08mm | 1-Wire | (200, 200) | DS18B20土壌温度 |
| J9 | TerminalBlock:TerminalBlock_bornier-2_P5.08mm | Fan | (250, 200) | 30mm 5V Fan |
| R1 | Device:R | 4.7k | (70, 100) | I2C0 SDA プルアップ |
| R2 | Device:R | 4.7k | (80, 100) | I2C0 SCL プルアップ |
| R3 | Device:R | 4.7k | (170, 100) | I2C1 SDA プルアップ |
| R4 | Device:R | 4.7k | (180, 100) | I2C1 SCL プルアップ |
| R5 | Device:R | 4.7k | (210, 100) | 1-Wire DQ プルアップ |
| R6 | Device:R | 100 | (260, 120) | Fan MOSFET ゲート抵抗 |
| R7 | Device:R | 10k | (260, 140) | Fan MOSFET プルダウン |
| C1 | Device:C | 100n | (60, 130) | I2C0 デカップリング |
| C2 | Device:C | 100n | (160, 130) | I2C1 デカップリング |
| C3 | Device:C | 100n | (120, 180) | ADC デカップリング |
| Q1 | Device:Q_NMOS_GSD | 2N7002 | (270, 150) | Fan Drive MOSFET |
| D1 | Device:D | 1N4148 | (280, 160) | Flyback Protection |

---

## 2. Picoピン配置テーブル（J1/J2とGPIOの対応）

### J1（左側ヘッダ、ピン1-20）

| J1 Pin | GPIO/Signal | 用途 | 接続先 |
|--------|------------|------|--------|
| 1 | GP0 | Reserved | - |
| 2 | GP1 | Reserved | - |
| 3 | GND | Ground | GND Net |
| 4 | GP2 | 1-Wire DQ | R5 pin1, J8 pin2 |
| 5 | GP3 | Reserved | - |
| 6 | GP4 | I2C0 SDA | R1 pin1, J3 pin3 |
| 7 | GP5 | I2C0 SCL | R2 pin1, J3 pin4 |
| 8 | GND | Ground | GND Net |
| 9 | GP6 | I2C1 SDA | R3 pin1, J4 pin3 |
| 10 | GP7 | I2C1 SCL | R4 pin1, J4 pin4 |
| 11 | GP8 | Reserved | - |
| 12 | GP9 | Reserved | - |
| 13 | GND | Ground | GND Net |
| 14 | GP10 | Reserved | - |
| 15 | GP11 | Reserved | - |
| 16 | GP12 | Reserved | - |
| 17 | GP13 | Reserved | - |
| 18 | GND | Ground | GND Net |
| 19 | GP14 | Reserved | - |
| 20 | GP15 | Reserved | - |

### J2（右側ヘッダ、ピン1-20）

| J2 Pin | GPIO/Signal | 用途 | 接続先 |
|--------|------------|------|--------|
| 1 | GP16 | W5500_SPI_RESERVED | No Connect (NC) |
| 2 | GP17 | W5500_SPI_RESERVED | No Connect (NC) |
| 3 | GND | Ground | GND Net |
| 4 | GP18 | W5500_SPI_RESERVED | No Connect (NC) |
| 5 | GP19 | W5500_SPI_RESERVED | No Connect (NC) |
| 6 | GP20 | W5500_SPI_RESERVED | No Connect (NC) |
| 7 | GP21 | W5500_SPI_RESERVED | No Connect (NC) |
| 8 | GND | Ground | GND Net |
| 9 | GP22 | Fan PWM | R6 pin1 |
| 10 | RUN | Reset | - |
| 11 | GP26 (ADC0) | ADC0 Input | J5 pin3 |
| 12 | GP27 (ADC1) | ADC1 Input | J6 pin3 |
| 13 | AGND | Analog Ground | GND Net |
| 14 | GP28 (ADC2) | ADC2 Input | J7 pin3 |
| 15 | ADC_VREF | ADC Reference | C3 pin1 |
| 16 | 3V3(OUT) | 3.3V Power | 3V3 Net |
| 17 | 3V3_EN | 3.3V Enable | - |
| 18 | GND | Ground | GND Net |
| 19 | VSYS | 5V System | J9 pin1 (Fan +) |
| 20 | VBUS | USB 5V | - |

---

## 3. ネット接続テーブル

### 3.1 電源ネット（3V3, GND, VSYS）

| From RefDes | From Pin | To RefDes | To Pin | Net Name | 備考 |
|-------------|----------|-----------|--------|----------|------|
| J2 | 16 | R1 | 2 | 3V3 | I2C0 SDA プルアップ電源 |
| J2 | 16 | R2 | 2 | 3V3 | I2C0 SCL プルアップ電源 |
| J2 | 16 | R3 | 2 | 3V3 | I2C1 SDA プルアップ電源 |
| J2 | 16 | R4 | 2 | 3V3 | I2C1 SCL プルアップ電源 |
| J2 | 16 | R5 | 2 | 3V3 | 1-Wire DQ プルアップ電源 |
| J2 | 16 | J3 | 2 | 3V3 | Grove I2C0 VCC |
| J2 | 16 | J4 | 2 | 3V3 | Grove I2C1 VCC |
| J2 | 16 | J5 | 2 | 3V3 | ADC0 センサー電源 |
| J2 | 16 | J6 | 2 | 3V3 | ADC1 センサー電源 |
| J2 | 16 | J7 | 2 | 3V3 | ADC2 センサー電源 |
| J2 | 16 | J8 | 3 | 3V3 | 1-Wire DS18B20 VDD |
| J2 | 16 | C1 | 1 | 3V3 | I2C0 デカップリング |
| J2 | 16 | C2 | 1 | 3V3 | I2C1 デカップリング |
| J1 | 3 | J3 | 1 | GND | Grove I2C0 GND |
| J1 | 8 | J4 | 1 | GND | Grove I2C1 GND |
| J1 | 13 | J5 | 1 | GND | ADC0 センサー GND |
| J1 | 18 | J6 | 1 | GND | ADC1 センサー GND |
| J2 | 3 | J7 | 1 | GND | ADC2 センサー GND |
| J2 | 8 | J8 | 1 | GND | 1-Wire DS18B20 GND |
| J2 | 13 | C3 | 2 | GND | ADC デカップリング GND |
| J2 | 18 | R7 | 2 | GND | MOSFET プルダウン GND |
| J2 | 18 | Q1 | 3 | GND | MOSFET Source |
| J2 | 18 | C1 | 2 | GND | I2C0 デカップリング GND |
| J2 | 18 | C2 | 2 | GND | I2C1 デカップリング GND |
| J2 | 18 | J9 | 2 | GND | Fan - (GND経由) |
| J2 | 15 | C3 | 1 | ADC_VREF | ADC基準電圧 |
| J2 | 19 | D1 | 2 | VSYS | Flyback保護（アノード） |

### 3.2 I2C0 接続（GP4/GP5）

| From RefDes | From Pin | To RefDes | To Pin | Net Name | 備考 |
|-------------|----------|-----------|--------|----------|------|
| J1 | 6 | R1 | 1 | GP4_SDA | GP4 → R1 |
| R1 | 1 | J3 | 3 | GP4_SDA | R1 → Grove I2C0 SDA |
| J1 | 7 | R2 | 1 | GP5_SCL | GP5 → R2 |
| R2 | 1 | J3 | 4 | GP5_SCL | R2 → Grove I2C0 SCL |

### 3.3 I2C1 接続（GP6/GP7）

| From RefDes | From Pin | To RefDes | To Pin | Net Name | 備考 |
|-------------|----------|-----------|--------|----------|------|
| J1 | 9 | R3 | 1 | GP6_SDA | GP6 → R3 |
| R3 | 1 | J4 | 3 | GP6_SDA | R3 → Grove I2C1 SDA |
| J1 | 10 | R4 | 1 | GP7_SCL | GP7 → R4 |
| R4 | 1 | J4 | 4 | GP7_SCL | R4 → Grove I2C1 SCL |

### 3.4 ADC接続（GP26/GP27/GP28）

| From RefDes | From Pin | To RefDes | To Pin | Net Name | 備考 |
|-------------|----------|-----------|--------|----------|------|
| J2 | 11 | J5 | 3 | GP26_ADC0 | GP26 → ADC0 入力 |
| J2 | 12 | J6 | 3 | GP27_ADC1 | GP27 → ADC1 入力 |
| J2 | 14 | J7 | 3 | GP28_ADC2 | GP28 → ADC2 入力 |

### 3.5 1-Wire接続（GP2）

| From RefDes | From Pin | To RefDes | To Pin | Net Name | 備考 |
|-------------|----------|-----------|--------|----------|------|
| J1 | 4 | R5 | 1 | GP2_DQ | GP2 → R5 |
| R5 | 1 | J8 | 2 | GP2_DQ | R5 → DS18B20 DQ |

### 3.6 ファンPWM接続（GP22）

| From RefDes | From Pin | To RefDes | To Pin | Net Name | 備考 |
|-------------|----------|-----------|--------|----------|------|
| J2 | 9 | R6 | 1 | GP22_PWM | GP22 → R6 ゲート抵抗 |
| R6 | 2 | Q1 | 1 | GATE | R6 → MOSFET Gate |
| R6 | 2 | R7 | 1 | GATE | R6/R7 接続点 → Gate |
| Q1 | 2 | D1 | 1 | DRAIN | MOSFET Drain → Flyback カソード |
| D1 | 1 | J9 | 1 | DRAIN | Flyback → Fan + 接続点 |
| J2 | 19 | J9 | 1 | VSYS | VSYS → Fan + |

---

## 4. ラベル一覧

| Label Name | Net Name | 配置箇所 |
|------------|----------|---------|
| 3V3 | 3V3 | J2-16 |
| GND | GND | J1-3, J1-8, J1-13, J1-18, J2-3, J2-8, J2-13, J2-18 |
| VSYS | VSYS | J2-19 |
| GP4_SDA | GP4_SDA | J1-6, R1-1, J3-3 |
| GP5_SCL | GP5_SCL | J1-7, R2-1, J3-4 |
| GP6_SDA | GP6_SDA | J1-9, R3-1, J4-3 |
| GP7_SCL | GP7_SCL | J1-10, R4-1, J4-4 |
| GP22_PWM | GP22_PWM | J2-9, R6-1 |
| GP26_ADC0 | GP26_ADC0 | J2-11, J5-3 |
| GP27_ADC1 | GP27_ADC1 | J2-12, J6-3 |
| GP28_ADC2 | GP28_ADC2 | J2-14, J7-3 |
| GP2_DQ | GP2_DQ | J1-4, R5-1, J8-2 |
| ADC_VREF | ADC_VREF | J2-15, C3-1 |
| GATE | GATE | R6-2, Q1-1, R7-1 |
| DRAIN | DRAIN | Q1-2, D1-1, J9-1 |

---

## 5. No Connect（NC）一覧

以下のピンは **W5500 SPI予約** のため接続しない（No Connect Flag配置）：

| RefDes | Pin | GPIO | 理由 |
|--------|-----|------|------|
| J2 | 1 | GP16 | W5500 SPI MISO |
| J2 | 2 | GP17 | W5500 SPI CS |
| J2 | 4 | GP18 | W5500 SPI SCK |
| J2 | 5 | GP19 | W5500 SPI MOSI |
| J2 | 6 | GP20 | W5500 RST |
| J2 | 7 | GP21 | W5500 INT |

その他の予約ピン（将来拡張用、NC）：
| RefDes | Pin | GPIO |
|--------|-----|------|
| J1 | 1 | GP0 |
| J1 | 2 | GP1 |
| J1 | 5 | GP3 |
| J1 | 11 | GP8 |
| J1 | 12 | GP9 |
| J1 | 14 | GP10 |
| J1 | 15 | GP11 |
| J1 | 16 | GP12 |
| J1 | 17 | GP13 |
| J1 | 19 | GP14 |
| J1 | 20 | GP15 |
| J2 | 10 | RUN |
| J2 | 17 | 3V3_EN |
| J2 | 20 | VBUS |

---

## 6. 接続生成用スクリプト例（kicad-sch-api）

```python
# add_wire_between_pins() 形式での接続例

# I2C0 接続
sch.add_wire_between_pins("J1", 6, "R1", 1)  # GP4 → R1
sch.add_wire_between_pins("R1", 1, "J3", 3)  # R1 → Grove SDA
sch.add_wire_between_pins("R1", 2, "LABEL_3V3", 1)  # R1 → 3V3

sch.add_wire_between_pins("J1", 7, "R2", 1)  # GP5 → R2
sch.add_wire_between_pins("R2", 1, "J3", 4)  # R2 → Grove SCL
sch.add_wire_between_pins("R2", 2, "LABEL_3V3", 1)  # R2 → 3V3

# I2C1 接続
sch.add_wire_between_pins("J1", 9, "R3", 1)  # GP6 → R3
sch.add_wire_between_pins("R3", 1, "J4", 3)  # R3 → Grove SDA
sch.add_wire_between_pins("R3", 2, "LABEL_3V3", 1)  # R3 → 3V3

sch.add_wire_between_pins("J1", 10, "R4", 1)  # GP7 → R4
sch.add_wire_between_pins("R4", 1, "J4", 4)  # R4 → Grove SCL
sch.add_wire_between_pins("R4", 2, "LABEL_3V3", 1)  # R4 → 3V3

# 1-Wire 接続
sch.add_wire_between_pins("J1", 4, "R5", 1)  # GP2 → R5
sch.add_wire_between_pins("R5", 1, "J8", 2)  # R5 → DS18B20 DQ
sch.add_wire_between_pins("R5", 2, "LABEL_3V3", 1)  # R5 → 3V3

# ADC 接続
sch.add_wire_between_pins("J2", 11, "J5", 3)  # GP26 → ADC0
sch.add_wire_between_pins("J2", 12, "J6", 3)  # GP27 → ADC1
sch.add_wire_between_pins("J2", 14, "J7", 3)  # GP28 → ADC2

# ファン PWM 接続
sch.add_wire_between_pins("J2", 9, "R6", 1)   # GP22 → R6
sch.add_wire_between_pins("R6", 2, "Q1", 1)   # R6 → MOSFET Gate
sch.add_wire_between_pins("R6", 2, "R7", 1)   # R6/R7 接続点
sch.add_wire_between_pins("R7", 2, "LABEL_GND", 1)  # R7 → GND
sch.add_wire_between_pins("Q1", 3, "LABEL_GND", 1)  # MOSFET Source → GND
sch.add_wire_between_pins("Q1", 2, "D1", 1)   # MOSFET Drain → Flyback カソード
sch.add_wire_between_pins("D1", 2, "J2", 19)  # Flyback アノード → VSYS
sch.add_wire_between_pins("D1", 1, "J9", 1)   # Flyback カソード → Fan +
sch.add_wire_between_pins("J2", 19, "J9", 1)  # VSYS → Fan +

# 電源接続（GND）
sch.add_wire_between_pins("J1", 3, "J3", 1)   # GND → Grove I2C0 GND
sch.add_wire_between_pins("J1", 8, "J4", 1)   # GND → Grove I2C1 GND
sch.add_wire_between_pins("J2", 18, "J9", 2)  # GND → Fan -
# ... (他のGND接続も同様)

# 電源接続（3V3）
sch.add_wire_between_pins("J2", 16, "J3", 2)  # 3V3 → Grove I2C0 VCC
sch.add_wire_between_pins("J2", 16, "J4", 2)  # 3V3 → Grove I2C1 VCC
# ... (他の3V3接続も同様)
```

---

## 7. 補足事項

### 7.1 抵抗/コンデンサのピン番号
- **R (抵抗)**: Pin1, Pin2（極性なし）
- **C (コンデンサ)**: Pin1 (Positive側), Pin2 (Negative側/GND側)
- **Q (MOSFET)**: Pin1 (Gate), Pin2 (Drain), Pin3 (Source)
- **D (ダイオード)**: Pin1 (Cathode/カソード), Pin2 (Anode/アノード)

### 7.2 Grove コネクタピン配列（J3/J4）
- Pin1: GND
- Pin2: VCC (3.3V)
- Pin3: SDA
- Pin4: SCL

### 7.3 ターミナルブロックピン配列
- **J5/J6/J7 (ADC)**: Pin1=GND, Pin2=3.3V, Pin3=ADC
- **J8 (1-Wire)**: Pin1=GND, Pin2=DQ, Pin3=3.3V
- **J9 (Fan)**: Pin1=VSYS(5V), Pin2=GND

---

**作成者**: 足軽2号（Ashigaru-2）
**参照**: DESIGN.md v1.0
**次ステップ**: kicad-sch-api を使った回路図自動生成スクリプト作成
