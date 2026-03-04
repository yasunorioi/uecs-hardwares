#!/usr/bin/env python3
"""
emergency_override.py - CommandGate パターンによる緊急オーバーライド

物理スイッチ (DI07-DI14) が ON になると:
  1. I2C 直接リレー制御 (MCP23008Relay) — MQTT relay bridge を経由しない
  2. MQTT publish (agriha/{house_id}/emergency/override, QoS=1, retain=True)
  3. LLM コマンドのロックアウト 300 秒 (gate() で通過可否を判定)

DI ピン → リレーチャンネルマッピング (UniPi 1.1 実配線):
  DI07 → relay ch1   (MCP23008 GP7, bit7)
  DI08 → relay ch2   (MCP23008 GP6, bit6)
  DI09 → relay ch3   (MCP23008 GP5, bit5)
  DI10 → relay ch4   (MCP23008 GP4, bit4)
  DI11 → relay ch5   (MCP23008 GP3, bit3)
  DI12 → relay ch6   (MCP23008 GP2, bit2)
  DI13 → relay ch7   (MCP23008 GP1, bit1)
  DI14 → relay ch8   (MCP23008 GP0, bit0)

ビットマッピングは i2c_relay.MCP23008Relay.ch_to_bit() に準拠:
  ch1=0x80, ch2=0x40, ..., ch8=0x01 (逆順配線)
"""

from __future__ import annotations

import json
import logging
import time
from typing import TYPE_CHECKING, Any, Callable, Optional

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None  # type: ignore[assignment]

from .gpio_watch import GPIOEvent
from .i2c_relay import MCP23008Relay

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# 定数
# ---------------------------------------------------------------------------

LOCKOUT_SECONDS = 300

# DI ピン番号 → リレーチャンネルマッピング
DI_RELAY_MAP: dict[int, int] = {
    7:  1,   # DI07 → relay ch1
    8:  2,   # DI08 → relay ch2
    9:  3,   # DI09 → relay ch3
    10: 4,   # DI10 → relay ch4
    11: 5,   # DI11 → relay ch5
    12: 6,   # DI12 → relay ch6
    13: 7,   # DI13 → relay ch7
    14: 8,   # DI14 → relay ch8
}


# ---------------------------------------------------------------------------
# CommandGate
# ---------------------------------------------------------------------------

class CommandGate:
    """
    緊急オーバーライドの CommandGate パターン実装。

    物理スイッチ (DI07-DI14) イベントを受け取り:
      1. MCP23008Relay を直接制御 (MqttRelayBridge を経由しない)
      2. agriha/{house_id}/emergency/override に MQTT publish
      3. スイッチ ON 時にロックアウト開始 (LLM コマンドを gate() でブロック)

    gate() を使って LLM コマンドの通過可否を制御する:
        allowed = gate.gate(relay.set_relay, ch, True)
        # ロックアウト中は allowed=False でコマンドはドロップされる
    """

    def __init__(
        self,
        relay: MCP23008Relay,
        mqtt_client: Optional[Any] = None,  # paho.mqtt.client.Client
        house_id: str = "h01",
        lockout_seconds: int = LOCKOUT_SECONDS,
    ) -> None:
        """
        Args:
            relay:           MCP23008Relay インスタンス (open() 済み)
            mqtt_client:     paho MQTT クライアント (None でスキップ)
            house_id:        MQTT トピックのハウスID
            lockout_seconds: 緊急割り込み後のロックアウト秒数
        """
        self.relay = relay
        self.mqtt_client = mqtt_client
        self.house_id = house_id
        self.lockout_seconds = lockout_seconds
        self._lockout_until: float = 0.0  # time.monotonic() ベース

    # ------------------------------------------------------------------
    # ロックアウト状態
    # ------------------------------------------------------------------

    def is_locked_out(self) -> bool:
        """LLM コマンドのロックアウト中かどうかを返す。"""
        return time.monotonic() < self._lockout_until

    def remaining_lockout(self) -> float:
        """ロックアウト残り秒数 (0.0 以上)。"""
        return max(0.0, self._lockout_until - time.monotonic())

    def clear_lockout(self) -> None:
        """ロックアウトを手動で解除する（管理・テスト用）。"""
        self._lockout_until = 0.0
        logger.info("Emergency lockout cleared manually")

    # ------------------------------------------------------------------
    # LLM コマンドゲート
    # ------------------------------------------------------------------

    def gate(self, command_fn: Callable, *args: Any, **kwargs: Any) -> bool:
        """
        LLM コマンドを CommandGate 経由で実行する。

        ロックアウト中はコマンドをドロップして False を返す。
        通常時は command_fn(*args, **kwargs) を呼び出して True を返す。

        Args:
            command_fn: 実行するコマンド関数
            *args:      command_fn に渡す引数
            **kwargs:   command_fn に渡すキーワード引数

        Returns:
            True=実行済み, False=ロックアウトでドロップ
        """
        if self.is_locked_out():
            logger.warning(
                "LLM command rejected by CommandGate "
                "(lockout %.0fs remaining)",
                self.remaining_lockout(),
            )
            return False
        command_fn(*args, **kwargs)
        return True

    # ------------------------------------------------------------------
    # GPIO イベントハンドラ (GPIOWatcher callback)
    # ------------------------------------------------------------------

    def handle_gpio_event(self, event: GPIOEvent) -> None:
        """
        GPIOWatcher からの edge イベントを受け取り緊急制御を実行する。

        Args:
            event: GPIOEvent (value=1=スイッチON/FALLING_EDGE,
                              value=0=スイッチOFF/RISING_EDGE)
        """
        relay_ch = DI_RELAY_MAP.get(event.di_pin)
        if relay_ch is None:
            logger.debug("DI%02d has no relay mapping, ignoring", event.di_pin)
            return

        state = bool(event.value)  # True=ON, False=OFF
        logger.info(
            "[Emergency] DI%02d → relay ch%d %s",
            event.di_pin, relay_ch, "ON" if state else "OFF",
        )

        # 1. I2C 直接リレー制御 (MCP23008Relay, 逆順配線対応済み)
        try:
            self.relay.set_relay(relay_ch, state)
        except Exception as exc:
            logger.error("Relay control failed: %s", exc, exc_info=True)

        # 2. MQTT publish
        self._publish_override(event, relay_ch, state)

        # 3. スイッチ ON 時にロックアウト更新
        if state:
            self._lockout_until = time.monotonic() + self.lockout_seconds
            logger.warning(
                "LLM command lockout started: %ds (DI%02d triggered)",
                self.lockout_seconds, event.di_pin,
            )

    # ------------------------------------------------------------------
    # 内部メソッド
    # ------------------------------------------------------------------

    def _publish_override(self, event: GPIOEvent, relay_ch: int, state: bool) -> None:
        """緊急オーバーライドを MQTT で通知する。"""
        if self.mqtt_client is None:
            return
        topic = f"agriha/{self.house_id}/emergency/override"
        payload = json.dumps({
            "di_pin": event.di_pin,
            "relay_ch": relay_ch,
            "state": state,
            "timestamp": time.time(),
            "lockout_sec": self.lockout_seconds if state else 0,
        })
        try:
            self.mqtt_client.publish(topic, payload, qos=1, retain=True)
            logger.debug("MQTT published: %s", topic)
        except Exception as exc:
            logger.error("MQTT publish failed: %s", exc, exc_info=True)
