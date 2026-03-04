"""Tests for emergency_override.py (CommandGate パターン).

MCP23008Relay と paho.mqtt.client を mock して、
smbus2/gpiod 未インストール環境でも実行できるようにする。
"""

from __future__ import annotations

import json
import sys
import time
from unittest.mock import MagicMock, call, patch

import pytest

# smbus2 を mock として登録 (i2c_relay の import に必要)
smbus2_mock = MagicMock()
smbus2_mock.SMBus = MagicMock
sys.modules.setdefault("smbus2", smbus2_mock)

# gpiod を mock として登録 (gpio_watch の import に必要)
gpiod_mock = MagicMock()
sys.modules.setdefault("gpiod", gpiod_mock)

from agriha.daemon.gpio_watch import GPIOEvent               # noqa: E402
from agriha.daemon.emergency_override import CommandGate, DI_RELAY_MAP, LOCKOUT_SECONDS  # noqa: E402


# ------------------------------------------------------------------ #
# 定数テスト
# ------------------------------------------------------------------ #

class TestConstants:
    def test_lockout_seconds_is_300(self):
        assert LOCKOUT_SECONDS == 300

    def test_di_relay_map_covers_di07_to_di14(self):
        assert set(DI_RELAY_MAP.keys()) == {7, 8, 9, 10, 11, 12, 13, 14}

    def test_di07_maps_to_ch1(self):
        assert DI_RELAY_MAP[7] == 1

    def test_di14_maps_to_ch8(self):
        assert DI_RELAY_MAP[14] == 8

    def test_di_relay_map_channels_are_1_to_8(self):
        assert set(DI_RELAY_MAP.values()) == {1, 2, 3, 4, 5, 6, 7, 8}


# ------------------------------------------------------------------ #
# フィクスチャ
# ------------------------------------------------------------------ #

@pytest.fixture
def mock_relay():
    return MagicMock()


@pytest.fixture
def mock_mqtt():
    return MagicMock()


@pytest.fixture
def gate(mock_relay, mock_mqtt):
    """MQTTクライアントあり CommandGate。"""
    return CommandGate(relay=mock_relay, mqtt_client=mock_mqtt, house_id="h01")


@pytest.fixture
def gate_no_mqtt(mock_relay):
    """MQTTクライアントなし CommandGate。"""
    return CommandGate(relay=mock_relay, mqtt_client=None, house_id="h01")


def make_event(di_pin: int, value: int) -> GPIOEvent:
    return GPIOEvent(di_pin=di_pin, gpio_line=0, value=value, timestamp_ns=0)


# ------------------------------------------------------------------ #
# ロックアウト状態
# ------------------------------------------------------------------ #

class TestLockoutState:
    def test_initial_not_locked(self, gate):
        """初期状態はロックアウトなし。"""
        assert gate.is_locked_out() is False

    def test_initial_remaining_is_zero(self, gate):
        """初期状態のロックアウト残り時間は 0.0。"""
        assert gate.remaining_lockout() == 0.0

    def test_clear_lockout_resets(self, gate):
        """clear_lockout() でロックアウトが解除されること。"""
        gate._lockout_until = time.monotonic() + 300
        gate.clear_lockout()
        assert gate.is_locked_out() is False
        assert gate.remaining_lockout() == 0.0

    def test_lockout_active_after_switch_on(self, gate):
        """スイッチ ON イベント後にロックアウトが有効になること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        assert gate.is_locked_out() is True

    def test_remaining_lockout_is_approx_300s(self, gate):
        """スイッチ ON 直後の残り時間が ~300秒であること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        remaining = gate.remaining_lockout()
        assert 299 < remaining <= 300

    def test_switch_off_does_not_change_lockout(self, gate):
        """スイッチ OFF イベントはロックアウト状態を変更しないこと。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        before = gate.remaining_lockout()
        gate.handle_gpio_event(make_event(di_pin=7, value=0))
        after = gate.remaining_lockout()
        # OFF でロックアウトは解除されない (残り時間はほぼ同じかわずかに減少)
        assert after <= before
        assert gate.is_locked_out() is True

    def test_second_switch_on_extends_lockout(self, gate):
        """2回目のスイッチ ON でロックアウトが延長されること。"""
        gate._lockout_until = time.monotonic() + 100  # 残り100秒
        gate.handle_gpio_event(make_event(di_pin=8, value=1))
        assert gate.remaining_lockout() > 200  # 延長されて ~300秒


# ------------------------------------------------------------------ #
# gate() メソッド
# ------------------------------------------------------------------ #

class TestCommandGateGate:
    def test_gate_executes_command_when_not_locked(self, gate):
        """ロックアウトなし → コマンド実行, True を返す。"""
        cmd = MagicMock()
        result = gate.gate(cmd, "arg1", key="val")
        assert result is True
        cmd.assert_called_once_with("arg1", key="val")

    def test_gate_drops_command_when_locked(self, gate):
        """ロックアウト中 → コマンドをドロップ, False を返す。"""
        gate._lockout_until = time.monotonic() + 300
        cmd = MagicMock()
        result = gate.gate(cmd, "arg1")
        assert result is False
        cmd.assert_not_called()

    def test_gate_returns_false_does_not_raise(self, gate):
        """ロックアウト中の gate() は例外を発生させないこと。"""
        gate._lockout_until = time.monotonic() + 300
        gate.gate(lambda: None)  # 問題なく実行できること

    def test_gate_with_relay_set_relay(self, gate, mock_relay):
        """gate(relay.set_relay, ch, on) の典型的な使用パターンのテスト。"""
        result = gate.gate(mock_relay.set_relay, 1, True)
        assert result is True
        mock_relay.set_relay.assert_called_once_with(1, True)

    def test_gate_with_relay_set_relay_blocked(self, gate, mock_relay):
        """ロックアウト中は relay.set_relay が呼ばれないこと。"""
        gate._lockout_until = time.monotonic() + 300
        result = gate.gate(mock_relay.set_relay, 1, True)
        assert result is False
        mock_relay.set_relay.assert_not_called()


# ------------------------------------------------------------------ #
# handle_gpio_event()
# ------------------------------------------------------------------ #

class TestHandleGPIOEvent:
    def test_di07_on_sets_relay_ch1_on(self, gate, mock_relay):
        """DI07 ON → relay ch1 ON が呼ばれること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        mock_relay.set_relay.assert_called_with(1, True)

    def test_di14_on_sets_relay_ch8_on(self, gate, mock_relay):
        """DI14 ON → relay ch8 ON が呼ばれること。"""
        gate.handle_gpio_event(make_event(di_pin=14, value=1))
        mock_relay.set_relay.assert_called_with(8, True)

    def test_di07_off_sets_relay_ch1_off(self, gate, mock_relay):
        """DI07 OFF → relay ch1 OFF が呼ばれること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=0))
        mock_relay.set_relay.assert_called_with(1, False)

    def test_unknown_di_pin_is_ignored(self, gate, mock_relay):
        """DI マッピングにないピンはリレー制御もロックアウトもしないこと。"""
        gate.handle_gpio_event(make_event(di_pin=99, value=1))
        mock_relay.set_relay.assert_not_called()
        assert gate.is_locked_out() is False

    def test_relay_error_does_not_propagate(self, gate, mock_relay):
        """relay.set_relay が例外を投げても handle_gpio_event は例外を伝播しないこと。"""
        mock_relay.set_relay.side_effect = OSError("I2C bus error")
        # 例外が外に出ないこと
        gate.handle_gpio_event(make_event(di_pin=7, value=1))

    def test_all_di_pins_map_to_correct_channels(self, gate_no_mqtt, mock_relay):
        """DI07-DI14 の全ピンが対応チャンネルに正しくマッピングされること。"""
        expected = {
            7: 1, 8: 2, 9: 3, 10: 4,
            11: 5, 12: 6, 13: 7, 14: 8,
        }
        for di_pin, expected_ch in expected.items():
            mock_relay.reset_mock()
            gate_no_mqtt.handle_gpio_event(make_event(di_pin=di_pin, value=1))
            mock_relay.set_relay.assert_called_with(expected_ch, True)


# ------------------------------------------------------------------ #
# MQTT publish
# ------------------------------------------------------------------ #

class TestMQTTPublish:
    def test_publish_called_on_switch_on(self, gate, mock_mqtt):
        """スイッチ ON で MQTT publish が呼ばれること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        mock_mqtt.publish.assert_called_once()

    def test_publish_called_on_switch_off(self, gate, mock_mqtt):
        """スイッチ OFF でも MQTT publish が呼ばれること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=0))
        mock_mqtt.publish.assert_called_once()

    def test_publish_topic(self, gate, mock_mqtt):
        """publish トピックが agriha/h01/emergency/override であること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        topic = mock_mqtt.publish.call_args[0][0]
        assert topic == "agriha/h01/emergency/override"

    def test_publish_qos_1_retain(self, gate, mock_mqtt):
        """QoS=1, retain=True で publish されること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        kwargs = mock_mqtt.publish.call_args.kwargs
        assert kwargs.get("qos") == 1
        assert kwargs.get("retain") is True

    def test_publish_payload_fields_on_switch_on(self, gate, mock_mqtt):
        """スイッチ ON の payload に必要なフィールドが含まれること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
        raw_payload = mock_mqtt.publish.call_args[0][1]
        payload = json.loads(raw_payload)
        assert payload["di_pin"] == 7
        assert payload["relay_ch"] == 1
        assert payload["state"] is True
        assert payload["lockout_sec"] == LOCKOUT_SECONDS
        assert "timestamp" in payload

    def test_publish_payload_lockout_0_on_switch_off(self, gate, mock_mqtt):
        """スイッチ OFF の payload では lockout_sec=0 であること。"""
        gate.handle_gpio_event(make_event(di_pin=7, value=0))
        raw_payload = mock_mqtt.publish.call_args[0][1]
        payload = json.loads(raw_payload)
        assert payload["state"] is False
        assert payload["lockout_sec"] == 0

    def test_no_publish_when_mqtt_client_is_none(self, gate_no_mqtt):
        """mqtt_client=None のとき publish は呼ばれないこと (例外も出ないこと)。"""
        # 例外が出ないこと
        gate_no_mqtt.handle_gpio_event(make_event(di_pin=7, value=1))

    def test_publish_error_does_not_propagate(self, gate, mock_mqtt):
        """publish が例外を投げても handle_gpio_event は例外を伝播しないこと。"""
        mock_mqtt.publish.side_effect = RuntimeError("MQTT error")
        gate.handle_gpio_event(make_event(di_pin=7, value=1))
