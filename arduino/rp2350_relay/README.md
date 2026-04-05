# rp2350_relay — Waveshare 8ch Relay + DI Node FW (RP2350B)

Arduino firmware for **Waveshare RP2350-ETH-8DI-8RO / RP2350-POE-ETH-8DI-8RO**.

2棟目ハウス(h2)統合制御ノード。Ethernet(PoE)のみ。WiFiなし。

## 機能概要

| 機能 | 実装 |
|------|------|
| 8ch リレー制御 | **GPIO17-24 直接制御** (I2Cエキスパンダ不要) |
| 8ch デジタル入力 | GPIO9-16, フォトカプラ絶縁, アクティブLOW, **割り込み検知** |
| 通信 | **Ethernet W5500 SPI1のみ** (WiFiなし / PoE給電) |
| MQTT | PubSubClient 2.8+, subscribe wildcard |
| HA Auto Discovery | 8ch switch + 8ch binary_sensor + I2Cセンサー + RS485排水センサー |
| duration_sec | 指定秒後自動OFF (`mqtt_relay_bridge.py` 互換) |
| 時刻 | PCF85063 RTC (I2C0: SDA=GPIO6, SCL=GPIO7) + NTP |
| I2Cセンサー | SHT40 自動検出 (sensor_registry.h) / バス固着耐性 / 連続3エラーで自動無効化 |
| RS485排水センサー | UART1 (GPIO4/5), Modbus RTU FC03 ポーリングスタブ (センサー未接続時も安全) |
| mDNS | `{node_id}.local` でアクセス可 (LEAmDNS / config で無効化可) |
| WebUI config | `/config` ページから静的IP・MQTT・house_id/node_id を設定 → 保存後自動リブート |
| Watchdog 3段 | HW WDT 8s (Pico SDK) / SW WDT sw_watchdog.h / 定期リブート 10分 |
| 設定 | LittleFS /config.json (静的IP or DHCP, mdns_enabled) |
| フレームワーク | arduino-pico (Earle Philhower) — solar_node.ino と同じ技術スタック |

---

## ボード設定 (Arduino IDE)

| 項目 | 値 |
|------|-----|
| Board Manager URL | `https://github.com/earlephilhower/arduino-pico/releases/download/4.5.2/package_rp2040_index.json` |
| Board | **Waveshare RP2350-ETH-8DI-8RO** (または **Generic RP2350**) |
| Flash Size | **16MB** |
| Upload Method | **USB (UF2)** または Picoprobe |

## 必要ライブラリ

| ライブラリ | バージョン | 備考 |
|-----------|-----------|------|
| arduino-pico | 4.5.2+ | Earle Philhower版 |
| ArduinoJson | **v7.x** | v6不可 |
| PubSubClient | 2.8+ | |
| NTPClient | 3.2.1 | Fabrice Weinberg |
| SensirionI2cSht4x | latest | SHT40センサー用 (optional) |
| W5500lwIP | arduino-pico内蔵 | 追加不要 |
| LEAmDNS | arduino-pico内蔵 | 追加不要 (mDNS) |
| Wire, LittleFS | arduino-pico内蔵 | 追加不要 |

---

## Arduino CLI ビルド確認

```bash
# ボードインストール
arduino-cli core install rp2040:rp2040 \
  --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/4.5.2/package_rp2040_index.json

# ライブラリインストール
arduino-cli lib install "ArduinoJson@7"
arduino-cli lib install "PubSubClient@2.8"
arduino-cli lib install "NTPClient@3.2.1"
arduino-cli lib install "Sensirion I2C SHT4x"

# ビルド (Generic RP2350で代用)
arduino-cli compile \
  --fqbn "rp2040:rp2040:generic:flash=16777216_65536" \
  arduino/rp2350_relay/
```

---

## 書き込み手順 (UF2 drag-and-drop)

1. BOOTSELボタンを押しながらUSB Type-C接続
2. `RPI-RP2` ドライブとしてマウント
3. ビルドで生成された `.uf2` ファイルをドライブにコピー
4. 自動リブート → シリアルモニタ 115200bps で起動ログ確認

---

## 設定ファイル (LittleFS /config.json)

`ip` フィールドがあれば静的IP、なければDHCP。  
WebUI `/config` ページから編集 → Save & Reboot で自動反映。

```json
{
  "mqtt_broker": "192.168.15.14",
  "mqtt_port": 1883,
  "house_id": "h2",
  "node_id": "waveshare_relay_01",
  "ip": "192.168.15.50",
  "subnet": "255.255.255.0",
  "gateway": "192.168.15.1",
  "dns": "8.8.8.8",
  "sensor_interval": 10,
  "rs485_baud": 9600,
  "mdns_enabled": true
}
```

LittleFSアップロード: `arduino-pico` の "Upload Filesystem Image" メニュー (data/ フォルダに配置)

---

## WebUI エンドポイント

| パス | メソッド | 説明 |
|------|---------|------|
| `/` | GET | ダッシュボード (IP/Subnet/GW/DNS/mDNS/MAC/センサー状態表示) |
| `/config` | GET | 設定フォーム (MQTT/IP/house_id/node_id/mDNS) |
| `/api/state` | GET | 状態JSON (relay, di, sensor, network情報, MAC, mDNS hostname) |
| `/api/config` | GET | 現在設定JSON |
| `/api/config` | POST | 設定保存 (form-encoded) → 自動リブート |
| `/api/relay/{ch}` | POST | リレー制御JSON |

### mDNS

`mdns_enabled: true` (デフォルト) の場合、`http://{node_id}.local` でアクセス可能。  
例: `http://waveshare_relay_01.local`

---

## GPIO マップ

### リレー出力 (GPIO直接制御)

| CH | GPIO | 備考 |
|----|------|------|
| Relay 1 | 17 | |
| Relay 2 | 18 | |
| Relay 3 | 19 | |
| Relay 4 | 20 | |
| Relay 5 | 21 | |
| Relay 6 | 22 | |
| Relay 7 | 23 | |
| Relay 8 | 24 | |

### デジタル入力 (active LOW)

| CH | GPIO | 備考 |
|----|------|------|
| DI1 | 9 | |
| DI2 | 10 | |
| DI3 | 11 | |
| DI4 | 12 | |
| DI5 | 13 | |
| DI6 | 14 | |
| DI7 | 15 | |
| DI8 | 16 | |

### W5500 SPI1

| 機能 | GPIO | 備考 |
|------|------|------|
| CS | 33 | |
| RST | 25 | |
| SCK | 34 | SPI1 |
| MOSI | 35 | SPI1 TX |
| MISO | 36 | SPI1 RX |
| INT | — | GPIO8の可能性あり (実機確認TODO) |

> **⚠️ 実機検証TODO**: GPIO33-36がRP2350BでSPI1かSPI0かの確認が必要。
> `SPI1.setSCK(34)` で動作しない場合は `SPI` (SPI0) に切り替えよ。

### I2C0 (RTC + センサー)

| デバイス | I2Cアドレス | GPIO |
|---------|------------|------|
| PCF85063 RTC | 0x51 | SDA=6, SCL=7 |
| SHT40 | 0x44 | 同上 |

### RS485 UART1 (排水センサー)

| 機能 | GPIO | 備考 |
|------|------|------|
| RS485 TX | 4 | UART1 TX |
| RS485 RX | 5 | UART1 RX |

> DE/RE ピンはボード上に配線なし。トランシーバーによる自動方向制御。センサー未接続時も動作に影響なし。

### その他予約

| 機能 | GPIO |
|------|------|
| WS2812 RGB LED | 2 |
| ブザー | 3 |

---

## MQTTトピック

| トピック | 方向 | retain | 説明 |
|---------|------|--------|------|
| `agriha/{house_id}/relay/{ch}/set` | Sub | — | リレー制御 (QoS=1) |
| `agriha/{house_id}/relay/state` | Pub | ✓ | 全ch状態 JSON |
| `agriha/{house_id}/di/state` | Pub | ✓ | DI全ch状態 (変化時即送信) |
| `agriha/{house_id}/sensor/relay_node/state` | Pub | — | SHT40データ |
| `agriha/{house_id}/sensor/drain/state` | Pub | — | RS485排水センサー (センサー接続時のみ送信) |
| `agriha/{node_id}/version` | Pub | ✓ | FWバージョン |
| `agriha/heartbeat` | Pub | — | 死活監視 (10秒) |

### リレー制御ペイロード

```json
{"value": 1, "duration_sec": 180, "reason": "irrigation_zone_1"}
```

### リレー状態ペイロード (retain)

```json
{
  "ch1": 0, "ch2": 0, "ch3": 1, "ch4": 0,
  "ch5": 0, "ch6": 0, "ch7": 0, "ch8": 0,
  "ts": 1740000000,
  "node_id": "waveshare_relay_01",
  "uptime": 3600
}
```

---

## 既存システムとの互換性

| 項目 | 1棟目 (h1) | 2棟目 (h2) |
|------|-----------|-----------|
| MQTT Subscribe | `agriha/h1/relay/{ch}/set` | `agriha/h2/relay/{ch}/set` |
| MQTT Publish | `agriha/h1/relay/state` | `agriha/h2/relay/state` |
| ペイロード | `{"ch1":0,...}` | 同一 + node_id, uptime |

uecs-llm の LLM制御層は `house_id` の切替のみで両棟を制御可能。

---

## ファイル構成

```
arduino/rp2350_relay/
├── rp2350_relay.ino     # メインFW
├── sw_watchdog.h        # SW WDT (Pico SDK, solar_node.inoから流用)
├── sensor_registry.h   # I2Cセンサーアドレス/HAフィールド定義
└── README.md
```

---

## RS485 雨量/排水センサー (DFRobot SEN0575)

UART1 (TX=GPIO4, RX=GPIO5) に RS485 トランシーバーを介して DFRobot SEN0575 転倒ます型雨量計を接続する。

### 動作仕様

- 電源投入時に `Serial2` を `rs485_baud` (デフォルト 9600bps) で初期化
- 起動時に PID読み取り (FC04, addr=0xC0, reg=0x0000) でセンサー自動検出
- 検出成功時: `sensor_interval` 秒ごとに以下3値を読み取り
  - 累積降水量 (reg 0x0007-0x0008, 32bit ÷10000 = mm)
  - 転倒ます回数 (reg 0x0009-0x000A, 32bit raw)
  - センサー稼働時間 (reg 0x000B, ÷60 = hours)
- 検出失敗時: ポーリングをスキップ (クラッシュ・リブートなし)
- CRC エラー・タイムアウト時も無視して次サイクルで再試行

### MQTT ペイロード

`agriha/{house_id}/sensor/drain/state`:
```json
{"rainfall_mm": 12.50, "tips": 447, "work_hours": 168.5, "ts": 1740000000}
```

### HA Discovery (検出時のみ)

| Entity | val_tpl | unit | icon |
|--------|---------|------|------|
| `{node_id}_rainfall` | `rainfall_mm` | mm | mdi:weather-pouring |
| `{node_id}_tips` | `tips` | tips | mdi:water-pump |
| `{node_id}_work_hours` | `work_hours` | h | mdi:clock-outline |

### SEN0575 Modbus プロトコル概要

| 項目 | 値 |
|------|-----|
| Modbus addr | 0xC0 |
| Function Code | FC04 (Read Input Registers) |
| Baud | 9600 8N1 |
| PID | 0x000100C0 |
| VID | 0x3343 |
| 1tip = | 0.2794mm (設定可能)

---

## 実機検証 TODO

- [ ] W5500 SPI1 (GPIO33-36) 動作確認 → 失敗時はSPI0に切替
- [ ] GPIO8 = W5500 INT? 確認後 `W5500_INT = 8` に修正
- [ ] 全8chリレー ON/OFF 動作確認 (GPIO17-24)
- [ ] DI 8ch 割り込み検知確認 (GPIO9-16)
- [ ] duration_sec 自動OFF タイマー確認
- [ ] PCF85063 RTC I2C確認 (addr=0x51, GPIO6/7) + 日付設定完全実装
- [ ] NTP同期 → ts フィールド確認
- [ ] SHT40 自動検出 + HA Discoveryエンティティ確認
- [ ] SHT40 バス固着時のタイムアウト動作確認 (Wire.setTimeout=10ms)
- [ ] mDNS `{node_id}.local` アクセス確認
- [ ] WebUI `/config` ページで静的IP設定 → リブート後確認
- [ ] PoE給電動作確認 (PoE版)
- [ ] RS485 SEN0575 実機接続確認 (UART1 GPIO4/5, Modbus RTU FC04, addr=0xC0)
- [ ] SEN0575 PID自動検出 → HA Discovery 3エンティティ (rainfall/tips/work_hours) 確認
- [ ] RS485/Modbus UECS CCM連携 (将来)
