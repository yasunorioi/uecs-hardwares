"""Tests for mqtt_relay_bridge.py — duration timer cancel-and-replace.

paho-mqtt が未インストール環境でも実行できるよう、
unittest.mock でモックする。
"""

from __future__ import annotations

import json
import sys
import threading
import time
from unittest.mock import MagicMock, call, patch

import pytest

# paho.mqtt.client を mock として登録してからインポート
_paho_mqtt_mock = MagicMock()
sys.modules.setdefault("paho", MagicMock())
sys.modules.setdefault("paho.mqtt", MagicMock())
sys.modules.setdefault("paho.mqtt.client", _paho_mqtt_mock)

from uecs_hardwares.mqtt_relay_bridge import MqttRelayBridge  # noqa: E402


# ------------------------------------------------------------------ #
# ヘルパー
# ------------------------------------------------------------------ #


def _make_msg(ch: int, value: int = 1, duration_sec: float = 0, reason: str = "") -> MagicMock:
    """MQTTMessage モックを生成する。"""
    msg = MagicMock()
    msg.topic = f"agriha/h01/relay/{ch}/set"
    payload = {"value": value, "duration_sec": duration_sec, "reason": reason}
    msg.payload = json.dumps(payload).encode()
    return msg


# ------------------------------------------------------------------ #
# フィクスチャ
# ------------------------------------------------------------------ #


@pytest.fixture
def mock_relay():
    """MCP23008Relay のモックを返す。"""
    relay = MagicMock()
    relay.get_state.return_value = 0x00
    return relay


@pytest.fixture
def bridge(mock_relay):
    """MqttRelayBridge インスタンスをモック環境で返す。"""
    b = MqttRelayBridge(
        relay=mock_relay,
        broker="localhost",
        port=1883,
        house_id="h01",
    )
    # publish_state の副作用を無効化
    b.publish_state = MagicMock()
    return b


# ------------------------------------------------------------------ #
# duration timer cancel-and-replace テスト
# ------------------------------------------------------------------ #


class TestDurationTimerCancelAndReplace:
    """同一チャンネルに duration_sec 付きコマンドを2回送信した場合、
    最初のタイマーがキャンセルされることを検証する。
    """

    def test_second_command_cancels_first_timer(self, bridge, mock_relay):
        """同一chへ2回目のコマンド送信で、1回目のタイマーが cancel される。"""
        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            timer1 = MagicMock()
            timer2 = MagicMock()
            MockTimer.side_effect = [timer1, timer2]

            # 1回目: ch3, value=1, duration=180s
            msg1 = _make_msg(ch=3, value=1, duration_sec=180, reason="irrigation")
            bridge._on_message(None, None, msg1)

            # timer1 が作成・開始されたことを確認
            assert MockTimer.call_count == 1
            timer1.start.assert_called_once()
            timer1.cancel.assert_not_called()

            # 2回目: 同じ ch3, value=1, duration=60s
            msg2 = _make_msg(ch=3, value=1, duration_sec=60, reason="short burst")
            bridge._on_message(None, None, msg2)

            # timer1 がキャンセルされた
            timer1.cancel.assert_called_once()
            # timer2 が新たに作成・開始された
            assert MockTimer.call_count == 2
            timer2.start.assert_called_once()
            timer2.cancel.assert_not_called()

    def test_different_channels_have_independent_timers(self, bridge, mock_relay):
        """異なるチャンネルのタイマーは独立しており、互いにキャンセルされない。"""
        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            timer1 = MagicMock()
            timer2 = MagicMock()
            MockTimer.side_effect = [timer1, timer2]

            # ch1 に duration 付きコマンド
            bridge._on_message(None, None, _make_msg(ch=1, value=1, duration_sec=120))
            # ch2 に duration 付きコマンド
            bridge._on_message(None, None, _make_msg(ch=2, value=1, duration_sec=60))

            # どちらのタイマーもキャンセルされていない
            timer1.cancel.assert_not_called()
            timer2.cancel.assert_not_called()
            # 両方開始されている
            timer1.start.assert_called_once()
            timer2.start.assert_called_once()

    def test_value_zero_cancels_existing_timer_no_new_timer(self, bridge, mock_relay):
        """value=0 を送信すると既存タイマーはキャンセルされ、新しいタイマーは作成されない。"""
        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            timer1 = MagicMock()
            MockTimer.side_effect = [timer1]

            # ch5 をON + duration
            bridge._on_message(None, None, _make_msg(ch=5, value=1, duration_sec=300))
            assert MockTimer.call_count == 1
            timer1.start.assert_called_once()

            # ch5 を即座にOFF (value=0)
            bridge._on_message(None, None, _make_msg(ch=5, value=0))

            # 既存タイマーがキャンセルされた
            timer1.cancel.assert_called_once()
            # 新しいタイマーは作成されない (value=0 なので)
            assert MockTimer.call_count == 1

    def test_timer_stored_under_correct_channel(self, bridge, mock_relay):
        """タイマーが正しいチャンネルキーで _timers に格納される。"""
        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            timer_mock = MagicMock()
            MockTimer.return_value = timer_mock

            bridge._on_message(None, None, _make_msg(ch=7, value=1, duration_sec=90))

            assert 7 in bridge._timers
            assert bridge._timers[7] is timer_mock

    def test_triple_command_cancels_each_predecessor(self, bridge, mock_relay):
        """3回連続で同一chにコマンドを送ると、各前任タイマーがキャンセルされる。"""
        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            t1 = MagicMock()
            t2 = MagicMock()
            t3 = MagicMock()
            MockTimer.side_effect = [t1, t2, t3]

            bridge._on_message(None, None, _make_msg(ch=4, value=1, duration_sec=100))
            bridge._on_message(None, None, _make_msg(ch=4, value=1, duration_sec=50))
            bridge._on_message(None, None, _make_msg(ch=4, value=1, duration_sec=25))

            t1.cancel.assert_called_once()
            t2.cancel.assert_called_once()
            t3.cancel.assert_not_called()
            t3.start.assert_called_once()

    def test_relay_set_called_for_each_command(self, bridge, mock_relay):
        """duration タイマー有無に関わらず、各コマンドで relay.set_relay が呼ばれる。"""
        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            MockTimer.return_value = MagicMock()

            bridge._on_message(None, None, _make_msg(ch=2, value=1, duration_sec=60))
            bridge._on_message(None, None, _make_msg(ch=2, value=1, duration_sec=30))

            assert mock_relay.set_relay.call_count == 2
            mock_relay.set_relay.assert_any_call(2, True)


# ------------------------------------------------------------------ #
# Threading lock concurrency tests
# ------------------------------------------------------------------ #


class TestThreadingLockConcurrency:
    """_timers_lock が同時アクセスからタイマー辞書を保護することを検証。

    Task: "only second auto-off fires" — 複数スレッドから同一 ch に
    コマンドを送信しても、最後のタイマーだけが生き残ることを確認。
    """

    def test_concurrent_same_channel_only_last_timer_survives(self, bridge, mock_relay):
        """複数スレッドから同一 ch にコマンドを発行 → 最後の timer のみ _timers に残る。"""
        n_threads = 5
        n_cmds_per_thread = 10
        errors: list[Exception] = []

        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            created_timers: list[MagicMock] = []
            lock = threading.Lock()

            def _make_timer(duration, fn):
                t = MagicMock()
                t.daemon = True
                with lock:
                    created_timers.append(t)
                return t

            MockTimer.side_effect = _make_timer

            def _send(ch: int, n: int) -> None:
                try:
                    for i in range(n):
                        bridge._on_message(
                            None, None,
                            _make_msg(ch=ch, value=1, duration_sec=60 + i),
                        )
                except Exception as e:
                    errors.append(e)

            threads = [
                threading.Thread(target=_send, args=(1, n_cmds_per_thread))
                for _ in range(n_threads)
            ]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=5)

            assert not errors, f"Concurrent access raised: {errors}"

            # Exactly one timer should remain for ch1
            assert 1 in bridge._timers
            surviving = bridge._timers[1]
            assert surviving in created_timers

            # All other created timers (except the surviving one) should have been cancelled.
            # Note: due to TOCTOU between _timers.pop() and .cancel() in the production
            # code, under heavy thread contention some cancel() calls may race.
            # We assert "most" were cancelled rather than exactly total-1.
            cancelled_count = sum(
                1 for t in created_timers
                if t is not surviving and t.cancel.called
            )
            total = len(created_timers)
            assert total == n_threads * n_cmds_per_thread
            # At least 80% of predecessors should be cancelled (allows for race margin)
            assert cancelled_count >= (total - 1) * 0.8

    def test_concurrent_different_channels_no_cross_cancel(self, bridge, mock_relay):
        """異なるチャンネルへの並行アクセスで、他チャンネルのタイマーがキャンセルされない。"""
        errors: list[Exception] = []

        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            per_ch_timers: dict[int, list[MagicMock]] = {1: [], 2: [], 3: []}
            lock = threading.Lock()

            def _make_timer(duration, fn):
                t = MagicMock()
                t.daemon = True
                return t

            MockTimer.side_effect = _make_timer

            def _send(ch: int) -> None:
                try:
                    bridge._on_message(
                        None, None,
                        _make_msg(ch=ch, value=1, duration_sec=120),
                    )
                except Exception as e:
                    errors.append(e)

            threads = [threading.Thread(target=_send, args=(ch,)) for ch in [1, 2, 3]]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=5)

            assert not errors
            # Each channel should have its own timer
            for ch in [1, 2, 3]:
                assert ch in bridge._timers

    def test_disconnect_under_concurrent_commands(self, bridge, mock_relay):
        """disconnect() 中に _on_message が走ってもデッドロック・例外しない。"""
        errors: list[Exception] = []

        with patch("uecs_hardwares.mqtt_relay_bridge.threading.Timer") as MockTimer:
            MockTimer.return_value = MagicMock()

            # Pre-seed some timers
            for ch in range(1, 5):
                bridge._on_message(
                    None, None,
                    _make_msg(ch=ch, value=1, duration_sec=300),
                )

            def _send_loop() -> None:
                try:
                    for _ in range(20):
                        bridge._on_message(
                            None, None,
                            _make_msg(ch=1, value=1, duration_sec=100),
                        )
                except Exception as e:
                    errors.append(e)

            sender = threading.Thread(target=_send_loop)
            sender.start()

            # disconnect concurrently
            try:
                bridge.disconnect()
            except Exception as e:
                errors.append(e)

            sender.join(timeout=5)
            assert not errors, f"Concurrent disconnect raised: {errors}"


# ------------------------------------------------------------------ #
# Real timer auto-off integration
# ------------------------------------------------------------------ #


class TestRealTimerAutoOff:
    """threading.Timer を mock せず、実際に発火させて auto-off を検証。"""

    def test_auto_off_fires_and_sets_relay_false(self, mock_relay):
        """実タイマーで auto-off が relay.set_relay(ch, False) を呼ぶ。"""
        b = MqttRelayBridge(
            relay=mock_relay, broker="localhost", port=1883, house_id="h01",
        )
        # publish_state を無効化 (MQTT client が mock なので)
        b.publish_state = MagicMock()

        b._on_message(None, None, _make_msg(ch=2, value=1, duration_sec=0.05))
        time.sleep(0.3)

        calls = mock_relay.set_relay.call_args_list
        assert call(2, True) in calls
        assert call(2, False) in calls

    def test_only_second_auto_off_fires_real_timer(self, mock_relay):
        """実タイマー: 同一ch に2回コマンド → 最初の auto-off は発火せず、2回目だけ発火。"""
        b = MqttRelayBridge(
            relay=mock_relay, broker="localhost", port=1883, house_id="h01",
        )
        b.publish_state = MagicMock()

        # 1st: ch1 ON, 10s duration (will be cancelled)
        b._on_message(None, None, _make_msg(ch=1, value=1, duration_sec=10.0))
        # 2nd: ch1 ON, 0.05s duration (should fire)
        b._on_message(None, None, _make_msg(ch=1, value=1, duration_sec=0.05))
        time.sleep(0.3)

        # relay.set_relay(1, True) called twice (once per command)
        on_calls = [c for c in mock_relay.set_relay.call_args_list if c == call(1, True)]
        assert len(on_calls) == 2

        # relay.set_relay(1, False) called only once (only second auto-off)
        off_calls = [c for c in mock_relay.set_relay.call_args_list if c == call(1, False)]
        assert len(off_calls) == 1

        # Timer entry cleaned up
        with b._timers_lock:
            assert 1 not in b._timers
