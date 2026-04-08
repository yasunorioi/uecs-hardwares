# ccm_rp2350_relay — ArSprout拡張 8chリレーノード (RP2350B)

Arduino firmware for **Waveshare RP2350-ETH-8DI-8RO / RP2350-POE-ETH-8DI-8RO**.

ArSproutのCCMネットワークに**設定不要で参加**。UECS-CCM (UDP multicast) でリレー制御・センサー送信を行う。

## 機能一覧

| 機能 | 実装 |
|------|------|
| 8ch リレー制御 | GPIO17-24 直接制御 |
| 8ch デジタル入力 | GPIO9-16, フォトカプラ絶縁, アクティブLOW, 割り込み検知 |
| DI→リレー連動 | ch毎にDI紐付け、反転モード対応（フロートスイッチ→灌水停止等） |
| 無通信ウォッチドッグ | CCM受信途絶でリレー強制OFF (ch別、デフォルト60秒) |
| 通信 | **UECS-CCM** (UDP multicast 224.0.0.1:16520) |
| CCMマッピング | WebUI `/ccm` で ch⇔CCMタイプ/Room/Region/Order を設定 + Bulk Set |
| CCM送信 | リレー状態 + センサー値をCCM XMLでbroadcast (10秒間隔) |
| CCM受信 | type+room+region+orderマッチでリレー制御 (priority考慮) |
| I2Cセンサー | SHT40 自動検出 → InAirTemp/InAirHumid としてCCM送信 |
| 日射センサー | PVSS-03 + M5Stack ADC Unit (ADS1110 I2C) → InRadiation としてCCM送信 |
| 日射比例灌水 | 積算日射量(MJ/m²)閾値で灌水リレー自動ON/OFF (2ルール対応) |
| 1-Wire温度 | DS18B20 (GPIO3) 自動検出 → InAirTemp region=12 としてCCM送信 |
| RS485排水センサー | DFRobot SEN0575 → WRainfallAmt としてCCM送信 |
| WebUI | ダッシュボード + CCMマッピング + Greenhouse + Irrigation + OTA |
| OTA更新 | WebUI `/ota` からブラウザ経由でFW書き込み（10台超の運用に対応） |
| RGB LED | WS2812 (GPIO2) 状態表示: 緑=正常 / 黄=リレー稼働 / 赤=Ethernet断 |
| USB-UARTデバッグ | `status` / `help` / `reboot` コマンド対応 |
| mDNS | `{hostname}.local` |
| Watchdog 3段 | HW WDT 8s / SW WDT / 定期リブート 10分 |
| 設定永続化 | LittleFS /config.json + /ccm_map.json + /gh_ctrl.json + /irri_ctrl.json |

---

## クイックスタート

1. **初回のみ** USB-CでPCに接続、BOOTSELモードでFW書き込み
2. LANケーブル接続 → DHCP で IP 取得
3. ブラウザで `http://{ip}/` → ダッシュボード表示
4. `/ccm` ページで各chにCCMタイプ/Room/Region を設定
5. ArSproutのCCMネットワークに自動参加 — **ArSprout側の設定変更は不要**
6. 2回目以降のFW更新は `/ota` ページからブラウザでアップロード

---

## ボード設定

| 項目 | 値 |
|------|-----|
| Board | **Generic RP2350** |
| Variant Chip | **RP2530B** (RP2350B — GPIO33-36使用のため必須) |
| Flash Size | **16MB (Sketch: 14MB, FS: 2MB)** |
| Upload Method | USB (UF2) 初回 / OTA (以降) |

## 必要ライブラリ

| ライブラリ | バージョン | 用途 |
|-----------|-----------|------|
| arduino-pico | 4.5.2+ | Earle Philhower版コア |
| ArduinoJson | v7.x | JSON設定・API |
| NTPClient | 3.2.1 | 時刻同期 |
| SensirionI2cSht4x | latest | SHT40温湿度 (optional) |
| Adafruit NeoPixel | 1.15+ | WS2812 RGB LED |
| OneWire | 2.3.8 | DS18B20 (optional) |
| DallasTemperature | 4.0+ | DS18B20 (optional) |
| W5500lwIP, LEAmDNS, Wire, LittleFS, Updater | arduino-pico内蔵 | |

---

## ビルド

### Arduino CLI

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

### PlatformIO (VSCode)

`platformio.ini` 同梱。VSCodeで開いて `pio run` でビルド可能。

---

## WebUI

| パス | メソッド | 説明 |
|------|---------|------|
| `/` | GET | ダッシュボード (リレー+DI+センサー+Greenhouse+Irrigation) |
| `/config` | GET | ネットワーク設定 (IP/mDNS) |
| `/ccm` | GET | CCMマッピング設定 (Bulk Set / DI連動 / WDTタイマー) |
| `/greenhouse` | GET | 温度比例制御設定 (4ルール) |
| `/irrigation` | GET | 日射比例灌水設定 (2ルール) |
| `/ota` | GET | FW更新ページ (ブラウザからbin選択→アップロード→自動リブート) |
| `/api/state` | GET | 状態JSON (リレー/DI/センサー/CCM/Greenhouse/Irrigation) |
| `/api/config` | GET | ネットワーク設定JSON |
| `/api/config` | POST | ネットワーク設定保存 → リブート |
| `/api/ccm` | POST | CCMマッピング保存 (リブート不要) |
| `/api/greenhouse` | POST | Greenhouse制御設定保存 |
| `/api/irrigation` | POST | Irrigation制御設定保存 |
| `/api/relay/{ch}` | POST | リレー手動制御 |
| `/api/ota` | POST | FWバイナリ受信 → フラッシュ → リブート |

---

## センサー接続

| センサー | インターフェース | GPIO | CCM送信 | 備考 |
|---------|----------------|------|---------|------|
| SHT40 | I2C1 (Grove) | 6/7 | InAirTemp, InAirHumid | 自動検出 |
| PVSS-03 + ADS1110 | I2C1 (Grove) | 6/7 | InRadiation | M5Stack ADC Unit V1.1経由。0-1V=0-1000W/m² |
| DS18B20 | 1-Wire | 3 | InAirTemp (region=12) | Grove基板から引出し |
| SEN0575 | RS485 (Modbus RTU) | 4/5 | WRainfallAmt | 排水センサー |

センサーは全て **自動検出**。未接続でも正常動作する。

---

## 日射比例灌水

WebUI `/irrigation` ページで設定。M5Stack ADC Unit V1.1 (ADS1110) + PVSS-03日射センサーをGrove I2Cに接続。

**動作**: 日射量を積算 → 閾値(MJ/m²)到達 → 灌水リレーON → 設定秒数後OFF → カウンタリセット → 再積算

| 設定項目 | デフォルト | 説明 |
|---------|-----------|------|
| Threshold | 0.5 MJ/m² | 灌水トリガーの積算日射量 (成長段階で調整) |
| Duration | 120秒 | 1回の灌水時間 |
| Min W/m² | 50 | この値未満は積算しない (夜間ノイズ除外) |

### Threshold (積算日射量) の目安

作物の成長段階で調整する。閾値が小さいほど灌水頻度が高い。

| 段階 | Threshold | 灌水頻度 (晴天時) |
|------|----------|-----------------|
| 定植直後 | 0.3 MJ/m² | 多い (少量頻回) |
| 生育中期 | 0.5 MJ/m² | 標準 |
| 収穫期 | 0.8-1.0 MJ/m² | 少ない |

### Min W/m² (ノイズフィルター)

PVSS-03は0W/m²付近でもADCノイズで数W/m²の値が出る。  
この値未満の日射読み取りは積算から除外し、夜間の誤灌水を防止する。

| 設定値 | 用途 |
|--------|------|
| 50 (デフォルト) | 通常運用。曇天でもある程度拾う |
| 20-30 | 曇天続きで灌水不足になる場合に下げる |
| 100 | 晴天のみ積算したい場合 |

### 運用ノート

- 2ルール対応 (e.g. 点滴灌水 + ミスト)
- CCM `InRadiation` としてArSproutにも日射値をbroadcast
- ADS1110未接続でも他機能は正常動作（灌水ルール有効でも安全）
- 灌水中は積算を停止する（灌水直後に再トリガーしない）
- WebUI `/irrigation` で積算量・灌水回数・残り秒数をリアルタイム監視可能

---

## 安全機能

### DI→リレー連動

WebUI `/ccm` ページで各リレーchにDI入力を紐付け。ネットワーク断でもローカルで即座に動作。

| 設定 | 用途例 |
|------|--------|
| DI1 → CH1 (normal) | 手元スイッチで換気扇ON |
| DI2 → CH3 (invert) | フロートスイッチON → 灌水ポンプOFF（水位上限） |

### 無通信ウォッチドッグ

ch毎に設定可能（0=無効、デフォルト60秒）。  
CCM受信が途絶えるとリレーを **安全側（OFF）** に強制。  
ArSprout 10秒間隔なら6回連続不達で停止。

---

## CCMマッピング

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

### デフォルト設定

| CH | CCMタイプ | Room | Region | Order |
|----|-----------|------|--------|-------|
| 1 | Relay | 2 | 61 | 1 |
| 2 | Relay | 2 | 61 | 2 |
| ... | Relay | 2 | 61 | ... |
| 8 | Relay | 2 | 61 | 8 |

Room=2（ArSprout=Room 1との共存）、Order=CH番号。

---

## ファイル構成

```
arduino/ccm_rp2350_relay/
├── ccm_rp2350_relay.ino   # メインFW
├── sw_watchdog.h           # SW WDT
├── sensor_registry.h      # I2Cセンサー定義
├── platformio.ini          # PlatformIO設定
└── README.md
```

---

## 実機検証

- [x] W5500 SPI0 (GPIO33-36) 動作確認
- [x] Ethernet DHCP取得 + 通信確認
- [x] mDNS `uecs-ccm-01.local` アクセス確認
- [x] CCM multicast join + XML送信確認
- [x] WebUI 全ページ動作確認
- [x] OTA更新 動作確認 (248KB / ~7秒)
- [x] USB-UART デバッグコマンド確認
- [ ] 全8chリレー ON/OFF
- [ ] DI 8ch 割り込み検知
- [ ] DI→リレー連動
- [ ] CCM XML受信 → リレー制御
- [ ] SHT40 CCM送信
- [ ] DS18B20 CCM送信
- [ ] SEN0575 CCM送信
- [ ] WS2812 RGB LED 点灯確認
- [ ] ArSprout側で受信確認

### RP2350Bピンマップ注意事項

| 機能 | GPIO | バス | 注意 |
|------|------|------|------|
| I2C SDA/SCL | 6/7 | **I2C1** (I2C0ではない) | `Wire1` を使用 |
| W5500 SPI | 33-36 | **SPI0** (SPI1ではない) | `SPI` を使用 |
| DS18B20 | 3 | 1-Wire | Grove基板から引出し |
| WS2812 | 2 | NeoPixel | ボード上実装済み |

`variantchip=RP2530B` (RP2350B用) でないとGPIO33-36が使えず、ビルドは通るが実行時PANICする。
