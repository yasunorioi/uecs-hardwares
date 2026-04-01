# uecs-hardwares

UniPi daemon + センサードライバ + リレー制御 — UECS HW通信基盤

## 概要

`uecs-hardwares` は [uecs-llm](https://github.com/oi-yasu/uecs-llm) リポジトリから
`git filter-repo` で **HW通信基盤のみを履歴付き切り出し** した独立リポジトリ。

**担当範囲: 物理HWとのインターフェース層（Layer 1）**

```
[F9P / DS18B20 / WH65LP / UniPi I2C-Relay / GPIOスイッチ]
          ↓ このリポジトリが担当
    [uecs_hardwares daemon]
          ↓ MQTT / REST API (localhost:8080)
    [uecs-llm: LLM三層制御]
```

元リポとの通信は **MQTTブローカー (mosquitto, localhost:1883)** と **REST API** のみ。
Python インポート依存はゼロ（daemon ↔ control 間の直接 import なし）。

---

## モジュール構成

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

**依存ライブラリ: `paho-mqtt` のみ（サードパーティ依存最小）**

---

## セットアップ

```bash
# 1. リポジトリ取得
git clone <this-repo> uecs-hardwares
cd uecs-hardwares

# 2. 設定ファイル準備
cp config/unipi_daemon.example.yaml config/unipi_daemon.yaml
# → config/unipi_daemon.yaml を環境に合わせて編集

# 3. 仮想環境作成とインストール
python3 -m venv .venv
source .venv/bin/activate
pip install -e .

# 4. MQTTブローカー起動 (Docker)
docker compose -f docker/docker-compose.yaml up -d

# 5. daemon起動
python3 -m uecs_hardwares.main --config config/unipi_daemon.yaml
```

### systemd 登録

```bash
# systemd/unipi-daemon.service の __REPO_DIR__ を実際のパスに置換
sudo cp systemd/unipi-daemon.service /etc/systemd/system/
sudo systemctl enable --now unipi-daemon
```

### cron 登録 (Layer 1 緊急制御)

```bash
# systemd/uecs-hardwares-cron の __REPO_DIR__/__USER__ を置換
sudo cp systemd/uecs-hardwares-cron /etc/cron.d/uecs-hardwares
```

---

## テスト

```bash
pip install -e ".[dev]"
python3 -m pytest tests/daemon/ -v
# → 209 passed
```

---

## 元リポとの関係

| 項目 | uecs-hardwares (本リポ) | uecs-llm |
|------|------------------------|---------|
| 担当 | HW通信基盤 (Layer 1) | LLM三層制御 (Layer 1-3) + Web UI |
| 依存 | paho-mqtt のみ | anthropic, fastapi, httpx, 他多数 |
| 通信 | MQTT / REST API で疎結合 | ← 同上 |
| 切り出し元 | `src/agriha/daemon/` → `src/uecs_hardwares/` | 残留 |

`uecs-llm` 側の `daemon/` 削除は別コマンドで指示がある際に実施。
