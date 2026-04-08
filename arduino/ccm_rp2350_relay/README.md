# ccm_rp2350_relay — UECS-CCM版 Waveshare 8ch Relay Node (RP2350B)

Arduino firmware for **Waveshare RP2350-ETH-8DI-8RO / RP2350-POE-ETH-8DI-8RO**.

**ArSproutユーザー向け** — UECS-CCM (UDP multicast) で既存CCMネットワークに参加。MQTTなし。

## MQTT版との違い

| | rp2350_relay (MQTT版) | ccm_rp2350_relay (CCM版) |
|---|---|---|
| **通信** | MQTT + HA Discovery | UECS-CCM (UDP 224.0.0.1:16520) |
| **制御** | MQTT subscribe | CCM XML受信 (type+room+region+order match) |
| **状態送信** | MQTT publish (retain) | CCM XML broadcast |
| **設定** | house_id + MQTT broker | CCMマッピング (type/room/region/order per ch) |
| **ターゲット** | HA/uecs-llm環境 | ArSprout既存ネットワーク |
| **IP** | 静的IP or DHCP | DHCP推奨 |
| **依存ライブラリ** | PubSubClient | 不要 (UDP標準) |

## 機能概要

| 機能 | 実装 |
|------|------|
| 8ch リレー制御 | GPIO17-24 直接制御 |
| 8ch デジタル入力 | GPIO9-16, フォトカプラ絶縁, アクティブLOW, 割り込み検知 |
| 通信 | **UECS-CCM** (UDP multicast 224.0.0.1:16520) |
| CCMマッピング | WebUI `/ccm` ページから ch⇔CCMタイプ(Irri,VenFan等)/Room/Region/Order を設定 |
| CCM送信 | リレー状態 + I2Cセンサー + 降水量をCCM XMLでbroadcast (10秒間隔) |
| CCM受信 | type+room+region+orderマッチでリレー制御 (priority考慮) |
| I2Cセンサー | SHT40 自動検出 → InAirTemp/InAirHumid としてCCM送信 |
| RS485排水センサー | DFRobot SEN0575 → WRainfallAmt としてCCM送信 |
| WebUI | ダッシュボード + CCMマッピング設定 + ネットワーク設定 + OTA更新 |
| OTA更新 | WebUI `/ota` からブラウザ経由でFW書き込み (Updaterクラス) |
| USB-UARTデバッグ | `status` / `help` / `reboot` コマンド対応 |
| mDNS | `{node_id}.local` |
| Watchdog 3段 | HW WDT 8s / SW WDT / 定期リブート 10分 |
| 設定 | LittleFS /config.json (ネットワーク) + /ccm_map.json (CCMマッピング) |

---

## ボード設定 (Arduino IDE)

| 項目 | 値 |
|------|-----|
| Board | **Generic RP2350** |
| Variant Chip | **RP2530B** (RP2350B — GPIO33-36使用のため必須) |
| Flash Size | **16MB (Sketch: 14MB, FS: 2MB)** |
| Upload Method | USB (UF2) 初回 / OTA (以降) |

## 必要ライブラリ

| ライブラリ | バージョン | 備考 |
|-----------|-----------|------|
| arduino-pico | 4.5.2+ | Earle Philhower版 |
| ArduinoJson | **v7.x** | |
| NTPClient | 3.2.1 | |
| SensirionI2cSht4x | latest | optional |
| W5500lwIP, LEAmDNS, Wire, LittleFS | arduino-pico内蔵 | |

> **PubSubClient 不要** — CCMはUDP multicastなのでMQTTライブラリは使わない。

---

## Arduino CLI ビルド

```bash
# ビルド
arduino-cli compile \
  --fqbn "rp2040:rp2040:generic_rp2350:variantchip=RP2530B,flash=16777216_2097152,psramcs=GPIOnone,psram=0mb,freq=150,arch=arm,opt=Small,os=none,profile=Disabled,rtti=Disabled,stackprotect=Disabled,exceptions=Disabled,dbgport=Serial,dbglvl=None,usbstack=picosdk,ipbtstack=ipv4only,uploadmethod=default" \
  arduino/ccm_rp2350_relay/

# .binエクスポート (OTA用)
arduino-cli compile --output-dir /tmp/ccm_ota \
  --fqbn "rp2040:rp2040:generic_rp2350:variantchip=RP2530B,flash=16777216_2097152,psramcs=GPIOnone,psram=0mb,freq=150,arch=arm,opt=Small,os=none,profile=Disabled,rtti=Disabled,stackprotect=Disabled,exceptions=Disabled,dbgport=Serial,dbglvl=None,usbstack=picosdk,ipbtstack=ipv4only,uploadmethod=default" \
  arduino/ccm_rp2350_relay/

# 初回USB書き込み
arduino-cli upload -p /dev/ttyACM0 \
  --fqbn "rp2040:rp2040:generic_rp2350:variantchip=RP2530B,flash=16777216_2097152,..." \
  arduino/ccm_rp2350_relay/

# OTA更新 (2回目以降)
curl --data-binary @/tmp/ccm_ota/ccm_rp2350_relay.ino.bin \
  -H "Content-Type: application/octet-stream" \
  http://{device-ip}/api/ota
```

---

## WebUI エンドポイント

| パス | メソッド | 説明 |
|------|---------|------|
| `/` | GET | ダッシュボード (リレー状態+CCMマッピング+DI+センサー) |
| `/config` | GET | ネットワーク設定 (IP/mDNS) |
| `/ccm` | GET | **CCMマッピング設定** (ch⇔CCMタイプ/Room/Region/Order) + Bulk Set |
| `/ota` | GET | **FW更新ページ** (ブラウザからbin選択→アップロード→自動リブート) |
| `/api/state` | GET | 状態JSON |
| `/api/config` | GET | ネットワーク設定JSON |
| `/api/config` | POST | ネットワーク設定保存 → リブート |
| `/api/ccm` | POST | CCMマッピング保存 (リブート不要) |
| `/api/relay/{ch}` | POST | リレー手動制御 |
| `/api/ota` | POST | FWバイナリ受信 (application/octet-stream) → フラッシュ → リブート |

---

## CCMマッピング設定

WebUI `/ccm` ページで各リレーchにCCMアクチュエータタイプを割り当てる。

### 利用可能なCCMタイプ

| CCMタイプ | 日本語 | 用途 |
|-----------|--------|------|
| Irri | 灌水 | スイッチON/OFF |
| VenFan | 換気扇 | スイッチON/OFF |
| CirHoriFan | 循環扇 | スイッチON/OFF |
| AirHeatBurn | 暖房(燃焼) | スイッチON/OFF |
| AirHeatHP | 暖房(HP) | スイッチON/OFF |
| CO2Burn | CO2施用 | スイッチON/OFF |
| VenRfWin | 天窓 | 位置制御 |
| VenSdWin | 側窓 | 位置制御 |
| ThCrtn | 保温カーテン | 位置制御 |
| LsCrtn | 遮光カーテン | 位置制御 |
| AirCoolHP | 冷房 | スイッチON/OFF |
| AirHumFog | 加湿 | スイッチON/OFF |
| Relay | 汎用リレー | スイッチON/OFF |

### 設定例

| CH | CCMタイプ | Room | Region | Order | 用途 |
|----|-----------|------|--------|-------|------|
| 1 | Irri | 1 | 61 | 1 | 灌水ポンプ |
| 2 | VenFan | 1 | 61 | 1 | 換気扇 |
| 3 | CirHoriFan | 1 | 61 | 1 | 循環扇 |
| 4-8 | (none) | - | - | - | 未割当 |

### CCM XMLフォーマット (送信例)

```xml
<UECS ver="1.00-E10">
  <DATA type="Irri.cMC" room="1" region="61" order="1" priority="1" lv="S" cast="uni">1</DATA>
  <DATA type="VenFan.cMC" room="1" region="61" order="1" priority="1" lv="S" cast="uni">0</DATA>
  <DATA type="InAirTemp.cMC" room="1" region="11" order="1" priority="29" lv="S" cast="uni">23.5</DATA>
</UECS>
```

---

## 設定ファイル

### /config.json (ネットワーク)

```json
{
  "node_id": "ccm_relay_01",
  "node_name": "UECS-Pi Relay",
  "ip": "",
  "subnet": "255.255.255.0",
  "gateway": "192.168.1.1",
  "dns": "192.168.1.1",
  "mdns_enabled": true,
  "rs485_baud": 9600
}
```

### /ccm_map.json (CCMマッピング)

```json
{
  "channels": [
    {"type": "Irri", "room": 1, "region": 61, "order": 1, "priority": 1, "suffix": ".cMC"},
    {"type": "VenFan", "room": 1, "region": 61, "order": 1, "priority": 1, "suffix": ".cMC"},
    {"type": "", "room": 1, "region": 61, "order": 1, "priority": 1, "suffix": ".cMC"},
    ...
  ]
}
```

---

## ArSproutとの共存

- RP2350ノードはDHCPで同じLAN上に参加
- CCM multicast (224.0.0.1:16520) で自動的にArSproutから見える
- Room/Region/Orderで識別 — ArSprout側の設定変更は不要
- ArSproutのconfig XMLとは独立 (別ノードとして認識される)

---

## ファイル構成

```
arduino/ccm_rp2350_relay/
├── ccm_rp2350_relay.ino   # メインFW (UECS-CCM版)
├── sw_watchdog.h           # SW WDT
├── sensor_registry.h      # I2Cセンサー定義
└── README.md
```

---

## 実機検証

- [x] W5500 SPI0 (GPIO33-36) 動作確認 — RP2350Bピンマップ修正済み
- [x] Ethernet DHCP取得 + 通信確認
- [x] mDNS `uecs-ccm-01.local` アクセス確認
- [x] CCM multicast join (224.0.0.1:16520) 確認
- [x] CCM XML送信確認
- [x] WebUI ダッシュボード + CCM設定 + ネットワーク設定 動作確認
- [x] OTA更新 (WebUI `/ota` + curl) 動作確認 (248KB / ~7秒)
- [x] USB-UART `status` / `help` / `reboot` コマンド確認
- [ ] 全8chリレー ON/OFF (GPIO17-24)
- [ ] DI 8ch 割り込み検知 (GPIO9-16)
- [ ] CCM XML受信 → リレー制御確認 (type+room+region+order match)
- [ ] SHT40 → InAirTemp/InAirHumid CCM送信確認
- [ ] SEN0575 → WRainfallAmt CCM送信確認
- [ ] CCM XML送信 → ArSprout側で受信確認

### RP2350Bピンマップ注意事項

| 機能 | GPIO | バス | 注意 |
|------|------|------|------|
| I2C SDA/SCL | 6/7 | **I2C1** (I2C0ではない) | `Wire1` を使用 |
| W5500 SPI | 33-36 | **SPI0** (SPI1ではない) | `SPI` を使用、RP2530Bバリアント必須 |

`variantchip=RP2530B` (RP2350B用) でないとGPIO33-36が使えずビルドは通るが実行時PANICする。
