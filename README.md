# uecs-hardwares — ArSprout拡張ハードウェア集

**ArSprout / UECS環境に追加できる** センサーノード・リレーノードの  
ファームウェアとドライバをまとめたリポジトリ。

既存のArSproutネットワーク（UECS-CCM UDP multicast）にそのまま参加でき、  
ArSprout側の設定変更は不要。PCやRaspberry Piなしで単体動作する。

---

## Arduino FWノード

| ディレクトリ | ボード | 概要 |
|-------------|--------|------|
| `arduino/ccm_rp2350_relay/` | Waveshare RP2350-ETH-8DI-8RO | **8chリレー + 8ch DI + センサー** — CCMノード |
| `arduino/rp2350_relay/` | 同上 | MQTT版（Home Assistant / uecs-llm向け） |

### ccm_rp2350_relay の主な機能

- **UECS-CCM** (UDP 224.0.0.1:16520) で ArSprout と直接通信
- 8ch リレー制御 + 8ch フォトカプラ絶縁デジタル入力
- **DI→リレー連動** — ネットワーク断でもローカルで安全動作（フロートスイッチ等）
- **無通信ウォッチドッグ** — CCM受信途絶でリレー強制OFF（ch別、デフォルト60秒）
- センサー: SHT40 (I2C), DS18B20 (1-Wire), SEN0575降水量 (RS485)
- **WebUI**: ダッシュボード / CCMマッピング / ネットワーク設定 / OTA FW更新
- **OTA**: ブラウザから FW アップロード → 自動リブート（10台超の運用に対応）
- WS2812 RGB LED 状態表示（緑=正常 / 黄=リレー稼働 / 赤=Ethernet断）
- mDNS, NTP, 3段Watchdog (HW/SW/定期リブート)
- PlatformIO / Arduino CLI 両対応

詳細 → [`arduino/ccm_rp2350_relay/README.md`](arduino/ccm_rp2350_relay/README.md)

---

## Python デーモン (Raspberry Pi / UniPi向け)

```
src/uecs_hardwares/
├── main.py               # daemonエントリポイント
├── ccm_receiver.py       # UECS-CCM XMLマルチキャスト受信
├── ds18b20.py            # 1-Wire温度センサー DS18B20ドライバ
├── emergency_override.py # 緊急オーバーライド (DI → リレー直結)
├── gpio_watch.py         # GPIO DI監視 (gpiod)
├── i2c_relay.py          # I2C MCP23008リレー制御 (8ch)
├── mqtt_relay_bridge.py  # MQTT↔リレーブリッジ (paho-mqtt)
├── rest_api.py           # FastAPI REST→MQTTコンバータ
├── sensor_loop.py        # センサーポーリングループ
└── wh65lp_reader.py      # Misol WH65LP気象ステーション UARTドライバ
```

Raspberry Pi + UniPi Neuron等のI2Cリレーボードで動作。  
MQTT/REST APIでuecs-llm（LLM三層制御）と連携。

### セットアップ

```bash
git clone <this-repo> uecs-hardwares
cd uecs-hardwares
python3 -m venv .venv && source .venv/bin/activate
pip install -e .
cp config/unipi_daemon.example.yaml config/unipi_daemon.yaml
# → 環境に合わせて編集
python3 -m uecs_hardwares.main --config config/unipi_daemon.yaml
```

### テスト

```bash
pip install -e ".[dev]"
python3 -m pytest tests/daemon/ -v
```

---

## 位置づけ

```
ArSprout (既存CCMネットワーク)
    │
    ├── [本リポ: Arduino FWノード] ← CCM multicastで直接参加
    │     Waveshare RP2350 / ESP32 系ボード
    │
    └── [本リポ: Python daemon] ← MQTT経由でuecs-llmと連携
          Raspberry Pi + UniPi
                │
          [uecs-llm: LLM三層制御 + Web UI]
```

Arduino FWノードはArSproutネットワークに**設定不要で参加**。  
Python daemonは[uecs-llm](https://github.com/oi-yasu/uecs-llm)と組み合わせて使う。

---

## ライセンス

MIT
