"""unipi-daemon: UniPi AgriHA 制御デーモン。

5つのasyncioタスクで構成:
  sensor_loop  : DS18B20/気象センサー読み取り → MQTT publish (10秒周期)
  mqtt_loop    : MqttRelayBridge + CommandGate.gate() でリレー制御コマンド処理
  gpio_watch   : GPIOWatcher で DI edge detection → CommandGate.handle_gpio_event()
  rest_api     : FastAPI REST-MQTT コンバータ (LINE Bot / LLM 向け)
  ccm_loop     : UECS-CCM UDP multicast 受信 → MQTT publish

緊急オーバーライド:
  物理スイッチ (DI07-DI14) ON → CommandGate が I2C 直接リレー制御 + 300秒 LLM ロックアウト
  ロックアウト中は MQTT 経由の LLM コマンドを _GatedRelay 経由でドロップする
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import sys
import time
from pathlib import Path
from typing import Any, Optional

try:
    import yaml
    _YAML_AVAILABLE = True
except ImportError:
    _YAML_AVAILABLE = False

try:
    import paho.mqtt.client as paho_mqtt
    _MQTT_AVAILABLE = True
except ImportError:
    _MQTT_AVAILABLE = False

from .i2c_relay import MCP23008Relay
from .gpio_watch import GPIOWatcher
from .emergency_override import CommandGate
from .mqtt_relay_bridge import MqttRelayBridge
from .sensor_loop import SensorLoop
from .ccm_receiver import CcmReceiver
from .rest_api import RestApi

logger = logging.getLogger(__name__)


# ------------------------------------------------------------------ #
# 設定ロード
# ------------------------------------------------------------------ #

def load_config(config_path: str) -> dict[str, Any]:
    """設定YAMLを読み込む。"""
    if not _YAML_AVAILABLE:
        raise ImportError("pyyaml is required. Install with: pip install pyyaml")
    path = Path(config_path)
    if not path.exists():
        raise FileNotFoundError(f"Config file not found: {config_path}")
    with path.open() as f:
        return yaml.safe_load(f) or {}


# ------------------------------------------------------------------ #
# GatedRelay アダプタ
# ------------------------------------------------------------------ #

class _GatedRelay:
    """CommandGate 経由で MCP23008Relay の WRITE 操作をゲートするアダプタ。

    MqttRelayBridge に渡すことで、MQTT (LLM) コマンドを CommandGate でゲーティングする。
    緊急オーバーライド中 (ロックアウト中) はリレー制御コマンドをドロップする。
    READ 操作 (get_state/get_relay) は常に素通しする。
    """

    def __init__(self, relay: MCP23008Relay, gate: CommandGate) -> None:
        self._relay = relay
        self._gate = gate

    def set_relay(self, ch: int, on: bool) -> None:
        self._gate.gate(self._relay.set_relay, ch, on)

    def get_state(self) -> int:
        return self._relay.get_state()

    def get_relay(self, ch: int) -> bool:
        return self._relay.get_relay(ch)

    def all_off(self) -> None:
        self._gate.gate(self._relay.all_off)


# ------------------------------------------------------------------ #
# デーモン本体
# ------------------------------------------------------------------ #

class UnipiDaemon:
    """unipi-daemon メインデーモン。

    asyncio で3タスクを並行実行し、SIGTERM/SIGINTでgraceful shutdownする。

    Attributes:
        _config: 設定辞書
        _running: タスクループ継続フラグ
    """

    def __init__(self, config: dict[str, Any]) -> None:
        self._config = config
        self._running = False

    # ------------------------------------------------------------------ #
    # asyncio タスク
    # ------------------------------------------------------------------ #

    async def sensor_loop(self, mqtt_client: Any) -> None:
        """センサーデータ取得・MQTT publish ループ。

        SensorLoop クラスに処理を委譲する。
        DS18B20 (1-Wire) と Misol WH65LP (UART RS485) を読み取り、
        MQTT に publish する (10 秒周期)。
        """
        logger.info("sensor_loop started")
        loop_obj = SensorLoop(config=self._config, mqtt_client=mqtt_client)
        loop_obj.setup()
        try:
            await loop_obj.run()
        except asyncio.CancelledError:
            raise
        finally:
            loop_obj.teardown()
            logger.info("sensor_loop stopped")

    async def mqtt_loop(self, relay: MCP23008Relay, gate: CommandGate) -> None:
        """MQTT subscribe 待機ループ (MqttRelayBridge + CommandGate.gate() ゲーティング)。

        _GatedRelay でリレー操作を CommandGate 経由にラップし、
        緊急オーバーライド中は LLM コマンドをドロップする。
        """
        logger.info("mqtt_loop started")

        mqtt_cfg = self._config.get("mqtt", {})
        daemon_cfg = self._config.get("daemon", {})

        gated_relay = _GatedRelay(relay, gate)
        bridge = MqttRelayBridge(
            relay=gated_relay,
            broker=mqtt_cfg.get("broker", "localhost"),
            port=int(mqtt_cfg.get("port", 1883)),
            house_id=daemon_cfg.get("house_id", "h01"),
            client_id=mqtt_cfg.get("client_id", "unipi-daemon"),
            keepalive=int(mqtt_cfg.get("keepalive", 60)),
        )
        bridge.connect()
        try:
            while self._running:
                await asyncio.sleep(1)
        finally:
            bridge.disconnect()

        logger.info("mqtt_loop stopped")

    async def ccm_loop(self, mqtt_client: Any) -> None:
        """UECS-CCM multicast 受信 → MQTT publish ループ。

        ArSprout ノードから送信される CCM パケット (InAirTemp, InAirHumid 等) を
        UDP マルチキャストで受信し、MQTT に publish する。
        """
        logger.info("ccm_loop started")
        receiver = CcmReceiver(config=self._config, mqtt_client=mqtt_client)
        try:
            await receiver.run()
        except asyncio.CancelledError:
            raise
        finally:
            logger.info("ccm_loop stopped")

    async def gpio_watch(self, gate: CommandGate) -> None:
        """GPIO DI edge detection ループ (GPIOWatcher + CommandGate 緊急割り込み)。

        DI07-DI14 の edge event を GPIOWatcher で検出し、
        CommandGate.handle_gpio_event() に渡す。
        物理スイッチ ON → I2C 直接リレー制御 + ロックアウト開始。
        """
        logger.info("gpio_watch started")

        gpio_cfg = self._config.get("gpio", {})
        di_pins: list[int] = gpio_cfg.get("di_lines") or list(range(7, 15))
        chip_path: str = gpio_cfg.get("chip", "/dev/gpiochip0")

        watcher = GPIOWatcher(
            chip_path=chip_path,
            di_pins=di_pins,
            callback=gate.handle_gpio_event,
        )
        await watcher.watch()

        logger.info("gpio_watch stopped")

    # ------------------------------------------------------------------ #
    # 起動・停止
    # ------------------------------------------------------------------ #

    async def run(self) -> None:
        """5タスク並行実行 + graceful shutdown。

        Tasks:
          sensor_loop  : DS18B20/Misol センサー読み取り → MQTT publish
          mqtt_loop    : MqttRelayBridge でリレー制御コマンド処理
          gpio_watch   : GPIO DI edge detection → CommandGate 緊急制御
          rest_api     : FastAPI REST-MQTT コンバータ (LINE Bot / LLM 向け)
          ccm_loop     : UECS-CCM UDP マルチキャスト受信 → MQTT publish
        """
        self._running = True
        loop = asyncio.get_running_loop()

        stop_event = asyncio.Event()

        def _on_signal() -> None:
            logger.info("Shutdown signal received")
            stop_event.set()

        loop.add_signal_handler(signal.SIGTERM, _on_signal)
        loop.add_signal_handler(signal.SIGINT, _on_signal)

        # I2C relay + CommandGate 初期化
        i2c_cfg = self._config.get("i2c", {})
        daemon_cfg = self._config.get("daemon", {})
        mqtt_cfg = self._config.get("mqtt", {})
        house_id: str = daemon_cfg.get("house_id", "h01")

        relay = MCP23008Relay(
            bus_num=int(i2c_cfg.get("bus", 1)),
            addr=int(i2c_cfg.get("mcp23008_addr", 0x20)),
        )

        # CommandGate 用 MQTT クライアント (緊急オーバーライド通知)
        gate_mqtt_client: Optional[Any] = None
        if _MQTT_AVAILABLE:
            gate_mqtt_client = paho_mqtt.Client(client_id="unipi-daemon-emergency")
            try:
                gate_mqtt_client.connect(
                    mqtt_cfg.get("broker", "localhost"),
                    int(mqtt_cfg.get("port", 1883)),
                    keepalive=int(mqtt_cfg.get("keepalive", 60)),
                )
                gate_mqtt_client.loop_start()
            except Exception as exc:
                logger.warning("CommandGate MQTT connect failed (emergency publish disabled): %s", exc)
                gate_mqtt_client = None

        gate = CommandGate(
            relay=relay,
            mqtt_client=gate_mqtt_client,
            house_id=house_id,
        )

        # センサー MQTT クライアント (SensorLoop が MQTT publish に使用)
        sensor_mqtt_client: Optional[Any] = None
        if _MQTT_AVAILABLE:
            sensor_mqtt_client = paho_mqtt.Client(client_id="unipi-daemon-sensor")
            try:
                sensor_mqtt_client.connect(
                    mqtt_cfg.get("broker", "localhost"),
                    int(mqtt_cfg.get("port", 1883)),
                    keepalive=int(mqtt_cfg.get("keepalive", 60)),
                )
                sensor_mqtt_client.loop_start()
            except Exception as exc:
                logger.warning("SensorLoop MQTT connect failed (sensor publish disabled): %s", exc)
                sensor_mqtt_client = None

        # _GatedRelay アダプタ (REST API のリレー状態読み取り用)
        gated_relay = _GatedRelay(relay, gate)

        # REST API 初期化
        start_time = time.monotonic()
        rest_api = RestApi(
            config=self._config,
            gate=gate,
            gated_relay=gated_relay,
            start_time=start_time,
        )

        logger.info("unipi-daemon starting (house_id=%s)", house_id)

        tasks = [
            asyncio.create_task(
                self.sensor_loop(sensor_mqtt_client), name="sensor_loop"
            ),
            asyncio.create_task(self.mqtt_loop(relay, gate), name="mqtt_loop"),
            asyncio.create_task(self.gpio_watch(gate), name="gpio_watch"),
            asyncio.create_task(rest_api.run(), name="rest_api"),
            asyncio.create_task(
                self.ccm_loop(sensor_mqtt_client), name="ccm_loop"
            ),
        ]

        await stop_event.wait()

        logger.info("Stopping daemon...")
        self._running = False

        for task in tasks:
            task.cancel()
        results = await asyncio.gather(*tasks, return_exceptions=True)

        for task, result in zip(tasks, results):
            if isinstance(result, Exception) and not isinstance(result, asyncio.CancelledError):
                logger.error("Task %s raised: %s", task.get_name(), result)

        # クリーンアップ
        if gate_mqtt_client is not None:
            gate_mqtt_client.loop_stop()
            gate_mqtt_client.disconnect()
        if sensor_mqtt_client is not None:
            sensor_mqtt_client.loop_stop()
            sensor_mqtt_client.disconnect()
        relay.close()

        logger.info("unipi-daemon stopped")


# ------------------------------------------------------------------ #
# エントリポイント
# ------------------------------------------------------------------ #

def main() -> None:
    parser = argparse.ArgumentParser(
        description="unipi-daemon: UniPi AgriHA control daemon"
    )
    parser.add_argument(
        "--config",
        default="/etc/agriha/unipi_daemon.yaml",
        help="Path to config YAML file",
    )
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(asctime)s %(name)-20s %(levelname)-8s %(message)s",
        stream=sys.stdout,
    )

    try:
        config = load_config(args.config)
    except FileNotFoundError as e:
        logger.error("%s", e)
        sys.exit(1)

    daemon = UnipiDaemon(config)
    asyncio.run(daemon.run())


if __name__ == "__main__":
    main()
