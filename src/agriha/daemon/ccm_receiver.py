"""UECS-CCM multicast receiver -> MQTT publisher.

Receives UECS-CCM UDP multicast (224.0.0.1:16520), parses XML payloads,
and publishes sensor/actuator/weather data to MQTT.

MQTT topics:
  agriha/{house_id}/ccm/sensor/{ccm_type}    ... InAirTemp, InAirHumid, etc.
  agriha/{house_id}/ccm/actuator/{ccm_type}  ... Irri, VenFan, etc.
  agriha/{house_id}/ccm/weather/{ccm_type}   ... WAirTemp, WAirHumid, etc.

Based on ccm_bridge.py from uecs-ccm-mcp.
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
import socket
import struct
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from typing import Any

logger = logging.getLogger(__name__)

# ── CCM Protocol Constants ───────────────────────────────────────────

MULTICAST_ADDR = "224.0.0.1"
MULTICAST_PORT = 16520
BUFFER_SIZE = 4096

_CCM_SUFFIX_RE = re.compile(r"\.(mC|cMC|MC)$")

# ── CCM Type Classification ──────────────────────────────────────────

SENSOR_TYPES = {
    "InAirTemp", "InAirHumid", "InAirCO2", "SoilTemp",
    "InRadiation", "SoilEC", "SoilWC", "Pulse",
    "InAirHD", "InAirAbsHumid", "InAirDP", "IntgRadiation",
}

ACTUATOR_TYPES = {
    "Irri", "VenFan", "CirHoriFan", "AirHeatBurn", "AirHeatHP",
    "CO2Burn", "VenRfWin", "VenSdWin", "ThCrtn", "LsCrtn",
    "AirCoolHP", "AirHumFog",
}

WEATHER_TYPES = {
    "WAirTemp", "WAirHumid", "WWindSpeed", "WWindDir16",
    "WRainfall", "WRainfallAmt", "WLUX",
}


def strip_ccm_suffix(ccm_type: str) -> str:
    return _CCM_SUFFIX_RE.sub("", ccm_type)


def classify_ccm_type(ccm_type: str) -> str:
    if ccm_type in SENSOR_TYPES:
        return "sensor"
    if ccm_type in ACTUATOR_TYPES:
        return "actuator"
    if ccm_type in WEATHER_TYPES:
        return "weather"
    return "other"


def parse_ccm_xml(xml_bytes: bytes, source_ip: str = "") -> list[dict]:
    """Parse UECS XML payload into list of dicts."""
    packets: list[dict] = []
    try:
        text = xml_bytes.decode("utf-8", errors="ignore")
        root = ET.fromstring(text)
    except (ET.ParseError, UnicodeDecodeError):
        return packets

    now = datetime.now(timezone.utc).isoformat()

    for elem in root.findall("DATA"):
        raw_type = elem.get("type", "")
        raw_value = (elem.text or "").strip()
        try:
            value: float | str = float(raw_value)
        except ValueError:
            value = raw_value

        def _int(attr: str, default: int) -> int:
            try:
                return int(elem.get(attr, str(default)))
            except (ValueError, TypeError):
                return default

        packets.append({
            "ccm_type": strip_ccm_suffix(raw_type),
            "raw_type": raw_type,
            "value": value,
            "room": _int("room", 1),
            "region": _int("region", 1),
            "order": _int("order", 1),
            "priority": _int("priority", 29),
            "level": elem.get("lv", "S"),
            "cast": elem.get("cast", "uni"),
            "source_ip": source_ip,
            "timestamp": now,
        })
    return packets


# ── Asyncio CCM Receiver ─────────────────────────────────────────────

class CcmReceiver:
    """Receives CCM multicast and publishes to MQTT.

    Args:
        config:      unipi-daemon config dict
        mqtt_client: connected paho.mqtt.client.Client instance
    """

    def __init__(self, config: dict[str, Any], mqtt_client: Any) -> None:
        self._config = config
        self._mqtt = mqtt_client

        daemon_cfg = config.get("daemon", {})
        self._house_id: str = daemon_cfg.get("house_id", "h01")

        ccm_cfg = config.get("ccm", {})
        self._multicast_addr: str = ccm_cfg.get("multicast_addr", MULTICAST_ADDR)
        self._multicast_port: int = int(ccm_cfg.get("multicast_port", MULTICAST_PORT))
        self._enabled: bool = ccm_cfg.get("enabled", True)

    def _make_topic(self, category: str, ccm_type: str) -> str:
        return f"agriha/{self._house_id}/ccm/{category}/{ccm_type}"

    def _publish(self, packet: dict) -> None:
        """Publish a single CCM packet to MQTT."""
        if self._mqtt is None:
            return

        ccm_type = packet["ccm_type"]
        category = classify_ccm_type(ccm_type)
        topic = self._make_topic(category, ccm_type)

        payload = json.dumps({
            "ccm_type": ccm_type,
            "value": packet["value"],
            "room": packet["room"],
            "region": packet["region"],
            "order": packet["order"],
            "priority": packet["priority"],
            "level": packet["level"],
            "source_ip": packet["source_ip"],
            "timestamp": packet["timestamp"],
        })

        self._mqtt.publish(topic, payload, qos=0, retain=True)

    async def run(self) -> None:
        """Main receiver loop. Runs until cancelled."""
        if not self._enabled:
            logger.info("CCM receiver disabled by config")
            return

        loop = asyncio.get_running_loop()

        # Create UDP multicast socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", self._multicast_port))

        mreq = struct.pack(
            "4sL", socket.inet_aton(self._multicast_addr), socket.INADDR_ANY
        )
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        sock.setblocking(False)

        logger.info(
            "CCM receiver listening on %s:%d",
            self._multicast_addr, self._multicast_port,
        )

        try:
            while True:
                try:
                    data, addr = await loop.sock_recvfrom(sock, BUFFER_SIZE)
                except OSError as e:
                    logger.error("CCM socket error: %s", e)
                    await asyncio.sleep(1.0)
                    continue

                packets = parse_ccm_xml(data, source_ip=addr[0])
                for pkt in packets:
                    self._publish(pkt)

                if packets:
                    logger.debug(
                        "CCM: %d packets from %s (%s)",
                        len(packets),
                        addr[0],
                        ", ".join(p["ccm_type"] for p in packets),
                    )
        except asyncio.CancelledError:
            raise
        finally:
            sock.close()
            logger.info("CCM receiver stopped")
