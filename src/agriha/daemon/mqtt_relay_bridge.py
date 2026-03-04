"""MQTT relay bridge - paho-mqtt によるリレー制御 pub/sub。

Topics:
  Publish:   agriha/{house_id}/relay/state
             payload: {"ch1":0,"ch2":0,...,"ch8":0,"ts":1740000000}
             QoS=1, retain=True

  Subscribe: agriha/{house_id}/relay/{ch}/set
             payload: {"value":1,"duration_sec":180,"reason":"..."}
             QoS=1, retain=False

duration_sec 対応:
  value=1 かつ duration_sec>0 の場合、指定秒後に自動OFFする。
  新しいコマンドを受信した場合は既存タイマーをキャンセルする。
"""

from __future__ import annotations

import json
import logging
import threading
import time
from typing import TYPE_CHECKING

import paho.mqtt.client as mqtt

if TYPE_CHECKING:
    from .i2c_relay import MCP23008Relay

logger = logging.getLogger(__name__)


class MqttRelayBridge:
    """paho-mqtt によるリレー制御ブリッジ。

    Usage:
        bridge = MqttRelayBridge(relay, broker="localhost", house_id="h01")
        bridge.connect()
        bridge.publish_state()
        # ... メインループ ...
        bridge.disconnect()
    """

    def __init__(
        self,
        relay: "MCP23008Relay",
        broker: str,
        port: int = 1883,
        house_id: str = "h01",
        client_id: str = "unipi-daemon-relay",
        keepalive: int = 60,
    ) -> None:
        self._relay = relay
        self._broker = broker
        self._port = port
        self._house_id = house_id
        self._keepalive = keepalive

        self._client = mqtt.Client(client_id=client_id)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.on_disconnect = self._on_disconnect

        # duration_sec タイマー管理 (ch → Timer)
        self._timers: dict[int, threading.Timer] = {}
        self._timers_lock = threading.Lock()

    # ------------------------------------------------------------------ #
    # 接続管理
    # ------------------------------------------------------------------ #

    def connect(self) -> None:
        """MQTTブローカーに接続してバックグラウンドループを開始する。"""
        logger.info("Connecting to MQTT broker %s:%d", self._broker, self._port)
        self._client.connect(self._broker, self._port, keepalive=self._keepalive)
        self._client.loop_start()

    def disconnect(self) -> None:
        """全タイマーをキャンセルしてMQTT切断する。"""
        with self._timers_lock:
            for timer in self._timers.values():
                timer.cancel()
            self._timers.clear()
        self._client.loop_stop()
        self._client.disconnect()
        logger.info("MQTT disconnected")

    # ------------------------------------------------------------------ #
    # 状態publish
    # ------------------------------------------------------------------ #

    def publish_state(self) -> None:
        """全チャンネルの現在状態をpublishする。"""
        raw = self._relay.get_state()
        payload: dict = {
            f"ch{ch}": int(bool(raw & (1 << (8 - ch))))
            for ch in range(1, 9)
        }
        payload["ts"] = int(time.time())
        topic = f"agriha/{self._house_id}/relay/state"
        self._client.publish(topic, json.dumps(payload), qos=1, retain=True)
        logger.debug("published relay state: %s", payload)

    # ------------------------------------------------------------------ #
    # paho コールバック
    # ------------------------------------------------------------------ #

    def _on_connect(self, client: mqtt.Client, userdata: object, flags: dict, rc: int) -> None:
        if rc == 0:
            topic = f"agriha/{self._house_id}/relay/+/set"
            client.subscribe(topic, qos=1)
            logger.info("MQTT connected. Subscribed: %s", topic)
            # 接続時に現在状態をpublish
            self.publish_state()
        else:
            logger.error("MQTT connection failed: rc=%d", rc)

    def _on_disconnect(self, client: mqtt.Client, userdata: object, rc: int) -> None:
        if rc != 0:
            logger.warning("MQTT unexpected disconnect: rc=%d", rc)

    def _on_message(self, client: mqtt.Client, userdata: object, msg: mqtt.MQTTMessage) -> None:
        """agriha/{house_id}/relay/{ch}/set メッセージを処理する。"""
        try:
            # トピックからch番号を抽出: agriha/{house_id}/relay/{ch}/set
            parts = msg.topic.split("/")
            # parts = ["agriha", house_id, "relay", ch, "set"]
            if len(parts) < 5 or parts[-1] != "set":
                logger.warning("Unexpected topic: %s", msg.topic)
                return

            ch = int(parts[-2])
            if not 1 <= ch <= 8:
                logger.warning("Invalid channel: %d", ch)
                return

            payload = json.loads(msg.payload.decode())
            value = int(payload.get("value", 0))
            duration_sec = float(payload.get("duration_sec", 0))
            reason = payload.get("reason", "")

            logger.info(
                "relay cmd: ch%d value=%d duration=%.1fs reason=%r",
                ch, value, duration_sec, reason,
            )

            # 既存タイマーをキャンセル
            with self._timers_lock:
                existing = self._timers.pop(ch, None)
            if existing is not None:
                existing.cancel()
                logger.debug("Cancelled existing timer for ch%d", ch)

            # リレー操作
            self._relay.set_relay(ch, bool(value))
            self.publish_state()

            # duration_sec 指定時: 指定秒後に自動OFF
            if value and duration_sec > 0:
                def _auto_off(channel: int = ch) -> None:
                    logger.info("relay ch%d auto-off (duration elapsed)", channel)
                    try:
                        self._relay.set_relay(channel, False)
                        self.publish_state()
                    except Exception as e:
                        logger.error("auto-off error ch%d: %s", channel, e)
                    finally:
                        with self._timers_lock:
                            self._timers.pop(channel, None)

                timer = threading.Timer(duration_sec, _auto_off)
                timer.daemon = True
                with self._timers_lock:
                    self._timers[ch] = timer
                timer.start()
                logger.debug("Set auto-off timer: ch%d in %.1fs", ch, duration_sec)

        except (ValueError, KeyError, json.JSONDecodeError) as e:
            logger.error("Failed to process relay command [%s]: %s", msg.topic, e)
        except Exception as e:
            logger.error("Unexpected error in relay command handler: %s", e)
