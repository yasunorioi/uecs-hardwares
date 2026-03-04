"""統合テスト: REST API (relay + sensor + emergency) + CommandGate 連携

FastAPI TestClient を使い、実際の MQTT / I2C ハードウェアなしでテストする。
CommandGate / _GatedRelay を MagicMock で差し替える。

カバレッジ:
  - POST /api/relay/{ch}  (normal / locked_out / mqtt_unavailable)
  - GET  /api/sensors     (empty / with cache)
  - GET  /api/status      (relay_state ok / i2c error)
  - POST /api/emergency/clear (locked / not locked)
  - APIキー認証 (with key / wrong key / no auth)
"""

from __future__ import annotations

import json
import sys
import time
from typing import Any
from unittest.mock import MagicMock, patch

import pytest

# smbus2 / gpiod / paho を mock として登録
smbus2_mock = MagicMock()
smbus2_mock.SMBus = MagicMock
sys.modules.setdefault("smbus2", smbus2_mock)

gpiod_mock = MagicMock()
sys.modules.setdefault("gpiod", gpiod_mock)

paho_mock = MagicMock()
paho_client_mock = MagicMock()
paho_mock.Client = MagicMock(return_value=paho_client_mock)
sys.modules.setdefault("paho", paho_mock)
sys.modules.setdefault("paho.mqtt", paho_mock)
sys.modules.setdefault("paho.mqtt.client", paho_mock)

from fastapi.testclient import TestClient  # noqa: E402
from agriha.daemon.rest_api import RestApi, SensorCache  # noqa: E402


# ---------------------------------------------------------------------------
# ヘルパー
# ---------------------------------------------------------------------------

def make_config(api_key: str = "", port: int = 8080) -> dict:
    return {
        "daemon": {"house_id": "h01", "sensor_interval_sec": 10},
        "mqtt": {"broker": "localhost", "port": 1883, "keepalive": 60},
        "rest_api": {"host": "127.0.0.1", "port": port, "api_key": api_key},
    }


def make_gate(locked: bool = False, remaining: float = 0.0) -> MagicMock:
    gate = MagicMock()
    gate.is_locked_out.return_value = locked
    gate.remaining_lockout.return_value = remaining
    return gate


def make_gated_relay(state_bitmask: int = 0x00) -> MagicMock:
    relay = MagicMock()
    relay.get_state.return_value = state_bitmask
    return relay


def make_rest_api(
    api_key: str = "",
    gate_locked: bool = False,
    relay_state: int = 0x00,
    start_time: float = 0.0,
) -> RestApi:
    gate = make_gate(locked=gate_locked, remaining=300.0 if gate_locked else 0.0)
    gated_relay = make_gated_relay(relay_state)
    api = RestApi(
        config=make_config(api_key=api_key),
        gate=gate,
        gated_relay=gated_relay,
        start_time=start_time or time.monotonic(),
    )
    # MQTT クライアントをモック化 (テスト中は接続しない)
    api._mqtt_client = MagicMock()
    return api


# ---------------------------------------------------------------------------
# フィクスチャ
# ---------------------------------------------------------------------------

@pytest.fixture
def api() -> RestApi:
    return make_rest_api()


@pytest.fixture
def client(api) -> TestClient:
    return TestClient(api.app)


@pytest.fixture
def api_with_key() -> RestApi:
    return make_rest_api(api_key="secret-key")


@pytest.fixture
def client_with_key(api_with_key) -> TestClient:
    return TestClient(api_with_key.app)


# ---------------------------------------------------------------------------
# SensorCache 単体テスト
# ---------------------------------------------------------------------------

class TestSensorCache:
    def test_initial_empty(self):
        cache = SensorCache()
        assert cache.get_all() == {}
        assert cache.get_updated_at() == 0.0

    def test_update_and_get(self):
        cache = SensorCache()
        cache.update("sensor/ds18b20", {"temp_c": 20.5})
        data = cache.get_all()
        assert data["sensor/ds18b20"]["temp_c"] == 20.5

    def test_updated_at_changes_on_update(self):
        cache = SensorCache()
        before = time.time()
        cache.update("key", {"value": 1})
        after = time.time()
        assert before <= cache.get_updated_at() <= after

    def test_get_all_returns_copy(self):
        """get_all() は内部 dict のコピーを返す (変更が反映されない)。"""
        cache = SensorCache()
        cache.update("k", {"v": 1})
        copy = cache.get_all()
        copy["k"]["v"] = 999
        assert cache.get_all()["k"]["v"] == 1

    def test_update_overwrites(self):
        cache = SensorCache()
        cache.update("k", {"v": 1})
        cache.update("k", {"v": 2})
        assert cache.get_all()["k"]["v"] == 2


# ---------------------------------------------------------------------------
# POST /api/relay/{ch}
# ---------------------------------------------------------------------------

class TestSetRelay:
    def test_relay_on_success(self, api, client):
        """通常時: リレー ON コマンドが MQTT publish されること。"""
        resp = client.post("/api/relay/1", json={"value": 1, "duration_sec": 60, "reason": "test"})
        assert resp.status_code == 202
        body = resp.json()
        assert body["ch"] == 1
        assert body["value"] == 1
        assert body["queued"] is True
        # MQTT publish が呼ばれること
        api._mqtt_client.publish.assert_called_once()
        call_args = api._mqtt_client.publish.call_args
        assert "agriha/h01/relay/1/set" in call_args[0][0]

    def test_relay_off(self, api, client):
        """リレー OFF コマンドが publish されること。"""
        resp = client.post("/api/relay/1", json={"value": 0})
        assert resp.status_code == 202
        payload = json.loads(api._mqtt_client.publish.call_args[0][1])
        assert payload["value"] == 0

    def test_relay_with_duration(self, api, client):
        """duration_sec が payload に含まれること。"""
        resp = client.post("/api/relay/3", json={"value": 1, "duration_sec": 180.0})
        assert resp.status_code == 202
        payload = json.loads(api._mqtt_client.publish.call_args[0][1])
        assert payload["duration_sec"] == 180.0

    def test_relay_publish_topic_correct(self, api, client):
        """MQTT トピックが agriha/h01/relay/{ch}/set であること。"""
        client.post("/api/relay/5", json={"value": 1})
        topic = api._mqtt_client.publish.call_args[0][0]
        assert topic == "agriha/h01/relay/5/set"

    def test_relay_all_channels(self, api, client):
        """チャンネル 1-8 が全て受け付けられること。"""
        for ch in range(1, 9):
            api._mqtt_client.reset_mock()
            resp = client.post(f"/api/relay/{ch}", json={"value": 1})
            assert resp.status_code == 202

    def test_relay_invalid_channel_0(self, client):
        """ch=0 は 422 (FastAPI バリデーションエラー)。"""
        resp = client.post("/api/relay/0", json={"value": 1})
        assert resp.status_code == 422

    def test_relay_invalid_channel_9(self, client):
        """ch=9 は 422 (FastAPI バリデーションエラー)。"""
        resp = client.post("/api/relay/9", json={"value": 1})
        assert resp.status_code == 422

    def test_relay_invalid_value(self, client):
        """value=2 は 422 (ge=0, le=1 バリデーション)。"""
        resp = client.post("/api/relay/1", json={"value": 2})
        assert resp.status_code == 422

    def test_relay_locked_out(self):
        """ロックアウト中は 423 を返すこと。"""
        api = make_rest_api(gate_locked=True)
        client = TestClient(api.app)
        resp = client.post("/api/relay/1", json={"value": 1})
        assert resp.status_code == 423
        body = resp.json()
        assert body["error"] == "locked_out"
        assert body["remaining_sec"] == 300.0

    def test_relay_locked_out_no_mqtt_publish(self):
        """ロックアウト中は MQTT publish が呼ばれないこと。"""
        api = make_rest_api(gate_locked=True)
        client = TestClient(api.app)
        client.post("/api/relay/1", json={"value": 1})
        api._mqtt_client.publish.assert_not_called()

    def test_relay_mqtt_unavailable(self):
        """MQTT クライアントが None のとき 503 を返すこと。"""
        api = make_rest_api()
        api._mqtt_client = None
        client = TestClient(api.app)
        resp = client.post("/api/relay/1", json={"value": 1})
        assert resp.status_code == 503
        assert resp.json()["error"] == "mqtt_unavailable"


# ---------------------------------------------------------------------------
# GET /api/sensors
# ---------------------------------------------------------------------------

class TestGetSensors:
    def test_empty_cache(self, client):
        """キャッシュ空のとき sensors={} が返ること。"""
        resp = client.get("/api/sensors")
        assert resp.status_code == 200
        body = resp.json()
        assert body["sensors"] == {}
        assert body["updated_at"] == 0.0
        assert body["age_sec"] is None

    def test_with_cached_sensor(self, api, client):
        """センサーデータがキャッシュされているとき返ること。"""
        api.sensor_cache.update("agriha/h01/sensor/DS18B20", {"temperature_c": 18.5})
        resp = client.get("/api/sensors")
        assert resp.status_code == 200
        body = resp.json()
        assert "agriha/h01/sensor/DS18B20" in body["sensors"]
        assert body["sensors"]["agriha/h01/sensor/DS18B20"]["temperature_c"] == 18.5
        assert body["updated_at"] > 0.0
        assert body["age_sec"] is not None

    def test_multiple_sensors(self, api, client):
        """複数センサーが全て返ること。"""
        api.sensor_cache.update("agriha/h01/sensor/DS18B20", {"temperature_c": 20.0})
        api.sensor_cache.update("agriha/farm/weather/misol", {"wind_speed_ms": 2.5})
        resp = client.get("/api/sensors")
        body = resp.json()
        assert len(body["sensors"]) == 2


# ---------------------------------------------------------------------------
# GET /api/status
# ---------------------------------------------------------------------------

class TestGetStatus:
    def test_status_not_locked(self, api, client):
        """ロックアウトなしの状態が正しく返ること。"""
        resp = client.get("/api/status")
        assert resp.status_code == 200
        body = resp.json()
        assert body["house_id"] == "h01"
        assert body["locked_out"] is False
        assert body["lockout_remaining_sec"] == 0.0
        assert "uptime_sec" in body
        assert "ts" in body

    def test_status_locked_out(self):
        """ロックアウト中の状態が正しく返ること。"""
        api = make_rest_api(gate_locked=True)
        client = TestClient(api.app)
        resp = client.get("/api/status")
        body = resp.json()
        assert body["locked_out"] is True
        assert body["lockout_remaining_sec"] == 300.0

    def test_status_relay_state(self):
        """relay_state が ch1-8 の dict で返ること。"""
        # ch1 ON = bit7 (0x80)
        api = make_rest_api(relay_state=0x80)
        client = TestClient(api.app)
        resp = client.get("/api/status")
        body = resp.json()
        assert body["relay_state"] is not None
        assert body["relay_state"]["ch1"] is True
        assert body["relay_state"]["ch2"] is False

    def test_status_all_relay_off(self):
        """全チャンネル OFF の状態が正しく返ること。"""
        api = make_rest_api(relay_state=0x00)
        client = TestClient(api.app)
        resp = client.get("/api/status")
        body = resp.json()
        relay_state = body["relay_state"]
        assert all(v is False for v in relay_state.values())

    def test_status_relay_i2c_error(self):
        """I2C エラー時に relay_state=null で 200 を返すこと。"""
        api = make_rest_api()
        api._gated_relay.get_state.side_effect = OSError("I2C error")
        client = TestClient(api.app)
        resp = client.get("/api/status")
        assert resp.status_code == 200
        assert resp.json()["relay_state"] is None

    def test_status_uptime_positive(self, api, client):
        """uptime_sec が正の値であること。"""
        resp = client.get("/api/status")
        assert resp.json()["uptime_sec"] >= 0


# ---------------------------------------------------------------------------
# POST /api/emergency/clear
# ---------------------------------------------------------------------------

class TestEmergencyClear:
    def test_clear_when_locked(self):
        """ロックアウト中に clear → cleared=True, was_locked_out=True。"""
        api = make_rest_api(gate_locked=True)
        client = TestClient(api.app)
        resp = client.post("/api/emergency/clear")
        assert resp.status_code == 200
        body = resp.json()
        assert body["cleared"] is True
        assert body["was_locked_out"] is True
        api._gate.clear_lockout.assert_called_once()

    def test_clear_when_not_locked(self, api, client):
        """ロックアウトなしに clear → cleared=True, was_locked_out=False。"""
        resp = client.post("/api/emergency/clear")
        assert resp.status_code == 200
        body = resp.json()
        assert body["cleared"] is True
        assert body["was_locked_out"] is False
        api._gate.clear_lockout.assert_called_once()


# ---------------------------------------------------------------------------
# APIキー認証テスト
# ---------------------------------------------------------------------------

class TestApiKeyAuth:
    def test_no_key_required_passes(self, client):
        """api_key が空の場合は認証なしでアクセスできること。"""
        resp = client.get("/api/status")
        assert resp.status_code == 200

    def test_correct_key_passes(self, client_with_key):
        """正しい API キーでアクセスできること。"""
        resp = client_with_key.get("/api/status", headers={"X-Api-Key": "secret-key"})
        assert resp.status_code == 200

    def test_wrong_key_rejected(self, client_with_key):
        """不正な API キーは 403 になること。"""
        resp = client_with_key.get("/api/status", headers={"X-Api-Key": "wrong-key"})
        assert resp.status_code == 403
        assert resp.json()["detail"] == "Invalid API key"

    def test_no_key_rejected_when_required(self, client_with_key):
        """api_key が設定されているとき、ヘッダーなしは 403 になること。"""
        resp = client_with_key.get("/api/status")
        assert resp.status_code == 403

    def test_relay_rejected_with_wrong_key(self, client_with_key):
        """リレーエンドポイントも認証が必要なこと。"""
        resp = client_with_key.post(
            "/api/relay/1",
            json={"value": 1},
            headers={"X-Api-Key": "bad"},
        )
        assert resp.status_code == 403

    def test_sensors_rejected_with_wrong_key(self, client_with_key):
        """センサーエンドポイントも認証が必要なこと。"""
        resp = client_with_key.get("/api/sensors", headers={"X-Api-Key": "bad"})
        assert resp.status_code == 403

    def test_emergency_clear_rejected_with_wrong_key(self, client_with_key):
        """emergency/clear エンドポイントも認証が必要なこと。"""
        resp = client_with_key.post(
            "/api/emergency/clear",
            headers={"X-Api-Key": "bad"},
        )
        assert resp.status_code == 403


# ---------------------------------------------------------------------------
# relay + gateway 結合テスト
# ---------------------------------------------------------------------------

class TestRelayGatewayIntegration:
    def test_lockout_prevents_relay_and_emergency_clears(self):
        """ロックアウト→リレー拒否→解除→リレー可のフロー。"""
        gate = make_gate(locked=True, remaining=300.0)
        gated_relay = make_gated_relay(0x00)
        api = RestApi(
            config=make_config(),
            gate=gate,
            gated_relay=gated_relay,
            start_time=time.monotonic(),
        )
        api._mqtt_client = MagicMock()
        client = TestClient(api.app)

        # ロックアウト中はリレー拒否
        resp = client.post("/api/relay/1", json={"value": 1})
        assert resp.status_code == 423

        # ロックアウト解除
        gate.is_locked_out.return_value = False
        gate.remaining_lockout.return_value = 0.0
        resp = client.post("/api/emergency/clear")
        assert resp.status_code == 200

        # 解除後はリレー可
        resp = client.post("/api/relay/1", json={"value": 1})
        assert resp.status_code == 202

    def test_sensor_cache_reflected_in_sensors_endpoint(self):
        """SensorCache に入れたデータが /api/sensors に反映されること。"""
        api = make_rest_api()
        client = TestClient(api.app)

        # 最初は空
        resp = client.get("/api/sensors")
        assert resp.json()["sensors"] == {}

        # キャッシュを更新
        api.sensor_cache.update("agriha/h01/sensor/DS18B20", {"temperature_c": 23.1})

        # 更新が反映される
        resp = client.get("/api/sensors")
        assert resp.json()["sensors"]["agriha/h01/sensor/DS18B20"]["temperature_c"] == 23.1
