# PCB → JLCPCB 発注ワークフロー

## 現在のステータス

| 基板 | サイズ | 回路図 | PCB配置 | ベタ | 自動配線 | 殿の手作業 |
|------|--------|--------|---------|------|----------|-----------|
| Grove Shield v3 | 51x21mm | OK | OK（ヘッダ90°回転済み） | GND(B.Cu)+VCC(F.Cu) | 部分完了(10未配線) | **配置調整+手動配線** |
| Actuator Board v3 | 80x50mm | OK | OK（ヘッダ90°回転+リレー外付け化） | GND(B.Cu)+5V(F.Cu) | ほぼ完了(1未配線) | **配置調整+手動配線** |

## 自動化済みパイプライン

```
回路図(.kicad_sch)
  │ kicad-cli sch export netlist
  ▼
ネットリスト(.net)
  │ generate_pcb_*.py (pcbnew API)
  ▼
PCBレイアウト(.kicad_pcb) ← GND/VCCベタプレーン付き
  │ export_dsn.py
  ▼
Specctra DSN(.dsn)
  │ freerouting.jar (headless CLI)
  ▼
Specctra Session(.ses) ← 殿がpcbnewでインポート
  │ pcbnew GUI: 配置調整 + DRC
  ▼
  │ export_gerber.sh
  ▼
JLCPCB用ガーバーZIP → jlcpcb.com アップロード
```

## 殿の手作業

### Step 1: pcbnew で配置確認・調整

```bash
pcbnew grove_shield_v3.kicad_pcb
pcbnew actuator_board_v3.kicad_pcb
```

確認ポイント:
- **Grove Shield: J1/J2ヘッダが90°回転済み（ピンが長辺51mm方向に並ぶ）**
- ヘッダピンの間隔（Pico互換: 17.78mm、短辺方向）
- コネクタが基板端から出ているか（Grove=左端、ADC/1-Wire/Fan=右端）
- 部品の物理的干渉がないか
- **Grove Shield は 51x21mm で密度が高い → 配置の微調整が重要**

### Step 2: 自動配線

```bash
# Grove Shield (GND/VCC はベタプレーンで処理)
bash autoroute.sh grove_shield_v3.kicad_pcb \
  --exclude 'Net-(J2-Pin_18)' 'Net-(J2-Pin_16)'

# Actuator Board (GND/5V はベタプレーンで処理)
bash autoroute.sh actuator_board_v3.kicad_pcb \
  --exclude 'Net-(GND)' 'Net-(5V)'
```

### Step 3: SES インポート + 手動仕上げ

pcbnew で:
1. `File → Import → Specctra Session` で .ses を読み込み
2. 未配線のラッツネスト（白い線）を手動配線
3. `Inspect → Design Rules Checker` でDRC確認

### Step 4: ガーバー出力

```bash
bash export_gerber.sh grove_shield_v3.kicad_pcb gerber_grove
bash export_gerber.sh actuator_board_v3.kicad_pcb gerber_actuator
```

### Step 5: JLCPCB 発注

1. https://cart.jlcpcb.com/quote にアクセス
2. ZIP をアップロード
3. 設定: Layers=2, Thickness=1.6mm, HASL lead-free, Green

## ファイル一覧

| ファイル | 説明 |
|----------|------|
| `grove_shield_v3_kicad7.kicad_sch` | センサーノード回路図 |
| `actuator_board_v3_kicad7.kicad_sch` | アクチュエータノード回路図 |
| `grove_shield_v3.kicad_pcb` | センサーノードPCB（ベタプレーン付き） |
| `actuator_board_v3.kicad_pcb` | アクチュエータノードPCB（ベタプレーン付き） |
| `generate_pcb_grove.py` | PCB生成（配置+ネット） |
| `generate_pcb_actuator.py` | PCB生成（配置+ネット） |
| `export_dsn.py` | DSNエクスポート（--exclude対応） |
| `autoroute.sh` | DSN生成+FreeRouting一括実行 |
| `export_gerber.sh` | ガーバー+ドリル+CPL一括出力 |
| `freerouting.jar` | FreeRouting v2.1.0（CLI対応） |

## PCB再生成

```bash
python3 generate_pcb_grove.py      # 配置を初期状態に戻す
python3 generate_pcb_actuator.py
```
