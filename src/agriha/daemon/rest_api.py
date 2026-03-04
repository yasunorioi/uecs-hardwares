"""REST-MQTT コンバータ API (FastAPI + uvicorn asyncio 統合)

unipi-daemon の asyncio ループ内で uvicorn.Server として動作する。
LINE Bot / LLM から REST API でリレー制御・センサー取得を行う。

MQTT subscribe/cache:
  agriha/{house_id}/sensor/#  … ハウス固有センサーデータをキャッシュ
  agriha/farm/weather/misol   … 農場気象データをキャッシュ
  agriha/{house_id}/ccm/#     … UECS-CCM 内気象/アクチュエータデータをキャッシュ

REST→MQTT:
  POST /api/relay/{ch}  →  agriha/{house_id}/relay/{ch}/set を publish
  MqttRelayBridge が subscribe して実際のリレー制御を実行

認証: X-API-Key ヘッダー (config.yaml の rest_api.api_key で設定、空文字で認証スキップ)

Endpoints:
  POST /api/relay/{ch}       リレー ch ON/OFF → MQTT publish
  GET  /api/sensors          最新センサーキャッシュ
  GET  /api/status           デーモン状態 + ロックアウト状態
  POST /api/emergency/clear  ロックアウト手動解除
"""

from __future__ import annotations

import copy
import json
import logging
import threading
import time
from typing import Any, Optional

logger = logging.getLogger(__name__)

try:
    from fastapi import Depends, FastAPI, Header, HTTPException, Path
    from fastapi.responses import JSONResponse
    from pydantic import BaseModel, Field
    import uvicorn
    _FASTAPI_AVAILABLE = True
except ImportError:
    _FASTAPI_AVAILABLE = False

try:
    import paho.mqtt.client as mqtt
    _MQTT_AVAILABLE = True
except ImportError:
    _MQTT_AVAILABLE = False


# ---------------------------------------------------------------------------
# センサーキャッシュ
# ---------------------------------------------------------------------------

class SensorCache:
    """スレッドセーフなセンサーデータキャッシュ。

    paho MQTT コールバック（バックグラウンドスレッド）から更新され、
    FastAPI エンドポイント（asyncio スレッド）から読み取られる。
    threading.Lock でデータ競合を防ぐ。
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._data: dict[str, Any] = {}
        self._updated_at: float = 0.0

    def update(self, key: str, value: Any) -> None:
        """センサーデータをキーで更新する。"""
        with self._lock:
            self._data[key] = value
            self._updated_at = time.time()

    def get_all(self) -> dict[str, Any]:
        """全センサーデータのディープコピーを返す。"""
        with self._lock:
            return copy.deepcopy(self._data)

    def get_updated_at(self) -> float:
        """最終更新時刻 (UNIX timestamp)。データなしの場合 0.0。"""
        with self._lock:
            return self._updated_at


# ---------------------------------------------------------------------------
# リクエストスキーマ
# ---------------------------------------------------------------------------

if _FASTAPI_AVAILABLE:
    class RelaySetRequest(BaseModel):
        value: int = Field(..., ge=0, le=1, description="0=OFF, 1=ON")
        duration_sec: float = Field(0.0, ge=0.0, description="自動OFF秒数 (0=タイマーなし)")
        reason: str = Field("", description="制御理由 (ログ用)")


# ---------------------------------------------------------------------------
# RestApi
# ---------------------------------------------------------------------------

class RestApi:
    """FastAPI REST-MQTT コンバータ。

    unipi-daemon の asyncio ループ内で uvicorn.Server として動作する。

    自身の paho MQTT クライアントを持ち:
      - センサートピックを subscribe してキャッシュを更新する
      - リレーコマンドを relay/{ch}/set に publish する

    共有オブジェクト (main.py から注入):
      - gate:        CommandGate (ロックアウト状態・解除)
      - gated_relay: _GatedRelay (リレー状態読み取り)

    Args:
        config:       unipi-daemon 設定辞書
        gate:         CommandGate インスタンス
        gated_relay:  _GatedRelay インスタンス (get_state() のみ使用)
        start_time:   daemon 起動時刻 (time.monotonic())
    """

    def __init__(
        self,
        config: dict[str, Any],
        gate: Any,
        gated_relay: Any,
        start_time: float,
    ) -> None:
        if not _FASTAPI_AVAILABLE:
            raise ImportError(
                "fastapi/uvicorn is required. "
                "Install with: pip install fastapi uvicorn"
            )

        self._config = config
        self._gate = gate
        self._gated_relay = gated_relay
        self._start_time = start_time
        self._sensor_cache = SensorCache()

        daemon_cfg = config.get("daemon", {})
        self._house_id: str = daemon_cfg.get("house_id", "h01")

        api_cfg = config.get("rest_api", {})
        self._api_key: str = str(api_cfg.get("api_key", ""))
        self._host: str = str(api_cfg.get("host", "0.0.0.0"))
        self._port: int = int(api_cfg.get("port", 8080))

        mqtt_cfg = config.get("mqtt", {})
        self._broker: str = str(mqtt_cfg.get("broker", "localhost"))
        self._mqtt_port: int = int(mqtt_cfg.get("port", 1883))
        self._keepalive: int = int(mqtt_cfg.get("keepalive", 60))

        self._mqtt_client: Optional[Any] = None  # paho client

        self._app = FastAPI(
            title="unipi-daemon REST API",
            description="UniPi AgriHA リレー制御・センサー取得 REST-MQTT コンバータ",
            version="1.0.0",
        )
        self._setup_routes()

    # ------------------------------------------------------------------
    # MQTT setup
    # ------------------------------------------------------------------

    def _setup_mqtt(self) -> None:
        """paho MQTT クライアントを初期化する。"""
        if not _MQTT_AVAILABLE:
            logger.warning("paho-mqtt 未インストール: REST API の MQTT 機能は無効")
            return

        house_id = self._house_id
        cache = self._sensor_cache

        client = mqtt.Client(client_id="unipi-daemon-rest-api")

        def on_connect(c: Any, userdata: Any, flags: dict, rc: int) -> None:
            if rc == 0:
                # センサートピックを subscribe してキャッシュを更新する
                c.subscribe(f"agriha/{house_id}/sensor/#", qos=1)
                c.subscribe("agriha/farm/weather/misol", qos=1)
                c.subscribe(f"agriha/{house_id}/relay/state", qos=1)
                c.subscribe(f"agriha/{house_id}/ccm/#", qos=0)
                logger.info(
                    "RestApi MQTT connected: subscribed to %s/sensor/#, weather/misol, relay/state, ccm/#",
                    house_id,
                )
            else:
                logger.error("RestApi MQTT connect failed: rc=%d", rc)

        def on_message(c: Any, userdata: Any, msg: Any) -> None:
            try:
                data = json.loads(msg.payload.decode())
                cache.update(msg.topic, data)
                logger.debug("SensorCache updated: %s", msg.topic)
            except Exception as exc:
                logger.warning("RestApi MQTT message parse error [%s]: %s", msg.topic, exc)

        def on_disconnect(c: Any, userdata: Any, rc: int) -> None:
            if rc != 0:
                logger.warning("RestApi MQTT unexpected disconnect: rc=%d", rc)

        client.on_connect = on_connect
        client.on_message = on_message
        client.on_disconnect = on_disconnect
        self._mqtt_client = client

    def _connect_mqtt(self) -> None:
        """MQTT ブローカーに接続してバックグラウンドループを開始する。"""
        if self._mqtt_client is None:
            return
        try:
            self._mqtt_client.connect(self._broker, self._mqtt_port, self._keepalive)
            self._mqtt_client.loop_start()
            logger.info("RestApi MQTT connecting to %s:%d", self._broker, self._mqtt_port)
        except Exception as exc:
            logger.warning("RestApi MQTT connect failed: %s (センサーキャッシュ無効)", exc)
            self._mqtt_client = None

    def _disconnect_mqtt(self) -> None:
        """MQTT を切断する。"""
        if self._mqtt_client is not None:
            try:
                self._mqtt_client.loop_stop()
                self._mqtt_client.disconnect()
            except Exception:
                pass
            self._mqtt_client = None

    # ------------------------------------------------------------------
    # API key dependency factory
    # ------------------------------------------------------------------

    def _make_api_key_dep(self):
        """API キー検証用の FastAPI Depends 依存関数を生成する。"""
        api_key = self._api_key

        async def _check(x_api_key: str = Header(default="")) -> None:
            if api_key and x_api_key != api_key:
                raise HTTPException(status_code=403, detail="Invalid API key")

        return _check

    # ------------------------------------------------------------------
    # Routes
    # ------------------------------------------------------------------

    def _setup_routes(self) -> None:
        """FastAPI ルートを設定する。"""
        app = self._app
        check_key = self._make_api_key_dep()

        # ---- POST /api/relay/{ch} ----

        @app.post("/api/relay/{ch}", summary="リレー制御")
        async def set_relay(
            ch: int = Path(..., ge=1, le=8, description="リレーチャンネル (1-8)"),
            body: RelaySetRequest = ...,
            _: None = Depends(check_key),
        ) -> JSONResponse:
            """リレー ch を ON/OFF する。

            ロックアウト中 (緊急スイッチ ON 後 300 秒) はリレー操作を拒否する。
            通常時は agriha/{house_id}/relay/{ch}/set に MQTT publish し、
            MqttRelayBridge が非同期でリレーを操作する。
            """
            if self._gate.is_locked_out():
                return JSONResponse(
                    status_code=423,
                    content={
                        "error": "locked_out",
                        "message": "緊急スイッチによりロックアウト中",
                        "remaining_sec": round(self._gate.remaining_lockout(), 1),
                    },
                )

            topic = f"agriha/{self._house_id}/relay/{ch}/set"
            payload = json.dumps({
                "value": body.value,
                "duration_sec": body.duration_sec,
                "reason": body.reason,
            })

            if self._mqtt_client is not None:
                self._mqtt_client.publish(topic, payload, qos=1)
                logger.info(
                    "REST relay cmd: ch%d value=%d duration=%.1fs → %s",
                    ch, body.value, body.duration_sec, topic,
                )
                return JSONResponse(
                    status_code=202,
                    content={"ch": ch, "value": body.value, "queued": True},
                )
            else:
                logger.error("REST relay cmd: MQTT client unavailable")
                return JSONResponse(
                    status_code=503,
                    content={"error": "mqtt_unavailable", "message": "MQTT ブローカー未接続"},
                )

        # ---- GET /api/sensors ----

        @app.get("/api/sensors", summary="センサーデータ取得")
        async def get_sensors(
            _: None = Depends(check_key),
        ) -> JSONResponse:
            """最新センサーデータキャッシュを返す。

            MQTT subscribe で受信したデータをキャッシュ。
            updated_at は UNIX timestamp (0.0 はデータなし)。
            age_sec はキャッシュの経過秒数。
            """
            data = self._sensor_cache.get_all()
            updated_at = self._sensor_cache.get_updated_at()
            age_sec = round(time.time() - updated_at, 1) if updated_at > 0 else None

            return JSONResponse(content={
                "sensors": data,
                "updated_at": updated_at,
                "age_sec": age_sec,
            })

        # ---- GET /api/status ----

        @app.get("/api/status", summary="デーモン状態")
        async def get_status(
            _: None = Depends(check_key),
        ) -> JSONResponse:
            """デーモンの状態を返す。

            locked_out が True の間はリレー制御 API は 423 を返す。
            relay_state が null の場合は I2C 読み取りエラー。
            """
            relay_state = None
            try:
                raw = self._gated_relay.get_state()
                relay_state = {
                    f"ch{ch}": bool(raw & (1 << (8 - ch)))
                    for ch in range(1, 9)
                }
            except Exception as exc:
                logger.warning("relay get_state failed: %s", exc)

            return JSONResponse(content={
                "house_id": self._house_id,
                "uptime_sec": round(time.monotonic() - self._start_time),
                "locked_out": self._gate.is_locked_out(),
                "lockout_remaining_sec": round(self._gate.remaining_lockout(), 1),
                "relay_state": relay_state,
                "ts": time.time(),
            })

        # ---- POST /api/emergency/clear ----

        @app.post("/api/emergency/clear", summary="ロックアウト解除")
        async def emergency_clear(
            _: None = Depends(check_key),
        ) -> JSONResponse:
            """緊急スイッチによるロックアウトを手動解除する。

            物理スイッチによる安全確保のために設けられたロックアウトを
            強制解除する。通常は 300 秒後に自動解除される。
            """
            was_locked = self._gate.is_locked_out()
            self._gate.clear_lockout()
            logger.info("Emergency lockout cleared via REST API (was_locked=%s)", was_locked)
            return JSONResponse(content={
                "cleared": True,
                "was_locked_out": was_locked,
            })

    # ------------------------------------------------------------------
    # asyncio タスク
    # ------------------------------------------------------------------

    @property
    def app(self) -> "FastAPI":
        """FastAPI アプリケーションインスタンス (テスト用)。"""
        return self._app

    @property
    def sensor_cache(self) -> SensorCache:
        """センサーキャッシュへのアクセサ (テスト用)。"""
        return self._sensor_cache

    async def run(self) -> None:
        """uvicorn.Server として asyncio イベントループ内で動作する。

        main.py から asyncio.create_task(rest_api.run()) で起動する。
        asyncio.CancelledError でシャットダウンする。
        """
        if not _FASTAPI_AVAILABLE:
            logger.warning("fastapi/uvicorn 未インストール: REST API は無効")
            return

        self._setup_mqtt()
        self._connect_mqtt()

        config = uvicorn.Config(
            self._app,
            host=self._host,
            port=self._port,
            loop="none",        # 既存の asyncio ループを使用
            log_level="warning",  # uvicorn ログを抑制
            access_log=False,
        )
        server = uvicorn.Server(config)
        logger.info("REST API starting on %s:%d", self._host, self._port)

        try:
            await server.serve()
        except Exception as exc:
            logger.error("REST API error: %s", exc)
        finally:
            self._disconnect_mqtt()
            logger.info("REST API stopped")
