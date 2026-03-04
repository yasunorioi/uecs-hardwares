"""Sensor loop — asyncio periodic sensor read -> MQTT publish.

DS18B20 (1-Wire) and Misol WH65LP (UART RS485) readings are published as JSON.

MQTT topics:
  agriha/{house_id}/sensor/DS18B20  ... DS18B20 temperature (QoS=1, retain=True)
  agriha/farm/weather/misol         ... Misol weather data   (QoS=1, retain=True)

Called from UnipiDaemon in main.py.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Any

from .ds18b20 import DS18B20, DS18B20Error
from .wh65lp_reader import read_frame, parse_frame

try:
    import serial
except ImportError:
    serial = None  # type: ignore[assignment]

logger = logging.getLogger(__name__)


class SensorLoop:
    """DS18B20 + Misol WH65LP sensor reader with MQTT publish.

    Args:
        config:      unipi-daemon config dict (same structure as config.yaml)
        mqtt_client: connected paho.mqtt.client.Client instance
    """

    def __init__(self, config: dict[str, Any], mqtt_client: Any) -> None:
        self._config = config
        self._mqtt = mqtt_client

        daemon_cfg = config.get("daemon", {})
        self._house_id: str = daemon_cfg.get("house_id", "h01")
        self._interval: int = int(daemon_cfg.get("sensor_interval_sec", 10))

        onewire_cfg = config.get("onewire", {})
        self._ds18b20_ids: list[str] = onewire_cfg.get("devices", [])

        uart_cfg = config.get("uart", {})
        self._serial_port: str = uart_cfg.get("weather_port", "/dev/ttyUSB0")
        self._serial_baud: int = int(uart_cfg.get("weather_baud", 9600))

        # MQTT topics
        self._topic_ds18b20 = f"agriha/{self._house_id}/sensor/DS18B20"
        self._topic_weather = "agriha/farm/weather/misol"

        # Sensor instances (lazy init)
        self._ds18b20_sensors: list[DS18B20] = []
        self._misol_serial: Any = None  # serial.Serial or None

    # ------------------------------------------------------------------ #
    # Init / teardown
    # ------------------------------------------------------------------ #

    def setup(self) -> None:
        """Initialize sensors. Call before run()."""
        # DS18B20
        if self._ds18b20_ids:
            self._ds18b20_sensors = [DS18B20(device_id=did) for did in self._ds18b20_ids]
            logger.info("DS18B20: %d device(s): %s", len(self._ds18b20_sensors), self._ds18b20_ids)
        else:
            self._ds18b20_sensors = DS18B20.discover()
            logger.info("DS18B20: discover -> %d device(s)", len(self._ds18b20_sensors))

        # Misol WH65LP (0x24 sync, 16s interval -> need timeout > 16s)
        if serial is None:
            logger.warning("pyserial not installed, Misol disabled")
            return
        try:
            self._misol_serial = serial.Serial(
                port=self._serial_port,
                baudrate=self._serial_baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=2.0,
            )
            logger.info("Misol serial opened: %s @ %d bps", self._serial_port, self._serial_baud)
        except Exception as e:
            logger.warning("Misol serial open failed: %s (skip)", e)
            self._misol_serial = None

    def teardown(self) -> None:
        """Clean up sensors."""
        if self._misol_serial and self._misol_serial.is_open:
            self._misol_serial.close()
            self._misol_serial = None

    # ------------------------------------------------------------------ #
    # Read
    # ------------------------------------------------------------------ #

    def _read_ds18b20(self) -> None:
        """Read all DS18B20 sensors and publish to MQTT."""
        for sensor in self._ds18b20_sensors:
            try:
                temp = sensor.read_celsius()
                payload = json.dumps({
                    "device_id": sensor.device_id,
                    "temperature_c": temp,
                    "timestamp": time.time(),
                })
                self._mqtt.publish(self._topic_ds18b20, payload, qos=1, retain=True)
                logger.info("DS18B20[%s]: %.2f C -> %s", sensor.device_id, temp, self._topic_ds18b20)
            except DS18B20Error as e:
                logger.error("DS18B20[%s] read failed: %s", sensor.device_id, e)

    def _read_misol(self) -> None:
        """Read Misol WH65LP frame and publish to MQTT."""
        if self._misol_serial is None:
            return
        try:
            frame = read_frame(self._misol_serial, sync_timeout=20.0)
            if frame is None:
                logger.debug("Misol: no frame received")
                return
            data = parse_frame(frame)
            data["timestamp"] = time.time()
            self._mqtt.publish(self._topic_weather, json.dumps(data), qos=1, retain=True)
            logger.info(
                "Misol: %.1f C %d%% %.1fm/s -> %s",
                data.get("temperature_c") or 0,
                data.get("humidity_pct") or 0,
                data.get("wind_speed_ms") or 0,
                self._topic_weather,
            )
        except Exception as e:
            logger.error("Misol read failed: %s", e)

    # ------------------------------------------------------------------ #
    # asyncio loop
    # ------------------------------------------------------------------ #

    async def run(self) -> None:
        """Sensor read + MQTT publish loop at configured interval.

        Call setup() before run().
        Stopped by asyncio.CancelledError from main.py.
        """
        loop = asyncio.get_running_loop()
        logger.info(
            "SensorLoop started: house=%s, interval=%ds, DS18B20 x%d, Misol=%s",
            self._house_id, self._interval, len(self._ds18b20_sensors),
            self._serial_port if self._misol_serial else "none",
        )
        while True:
            # DS18B20: sysfs read (fast, no executor needed)
            self._read_ds18b20()

            # Misol: blocking serial.read in executor (up to 20s sync wait)
            await loop.run_in_executor(None, self._read_misol)

            await asyncio.sleep(self._interval)
