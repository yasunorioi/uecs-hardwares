"""UECS-CCM receiver unit tests.

Tests:
  1. XML parsing (parse_ccm_xml)
  2. CCM type classification
  3. CCM suffix stripping
  4. MQTT topic generation
  5. Invalid XML handling
"""

import pytest

from agriha.daemon.ccm_receiver import (
    parse_ccm_xml,
    classify_ccm_type,
    strip_ccm_suffix,
    CcmReceiver,
)


# -------------------------------------------------------------------
# XML parse tests
# -------------------------------------------------------------------

SAMPLE_XML = b"""\
<?xml version="1.0"?>
<UECS ver="1.00-E10">
  <DATA type="InAirTemp.mC" room="1" region="1" order="1" priority="29" lv="S" cast="uni">23.5</DATA>
  <DATA type="InAirHumid.mC" room="1" region="1" order="1" priority="29" lv="S" cast="uni">65.2</DATA>
  <IP>10.10.0.50</IP>
</UECS>
"""

SAMPLE_MULTI_ROOM = b"""\
<?xml version="1.0"?>
<UECS ver="1.00-E10">
  <DATA type="InAirTemp" room="2" region="1" order="1" priority="29" lv="S" cast="uni">18.0</DATA>
  <DATA type="SoilTemp" room="2" region="1" order="2" priority="29" lv="S" cast="uni">12.5</DATA>
</UECS>
"""

ACTUATOR_XML = b"""\
<?xml version="1.0"?>
<UECS ver="1.00-E10">
  <DATA type="VenFan" room="1" region="1" order="1" priority="10" lv="A" cast="uni">1</DATA>
</UECS>
"""


class TestParseCcmXml:
    def test_basic_parse(self):
        packets = parse_ccm_xml(SAMPLE_XML, source_ip="10.10.0.50")
        assert len(packets) == 2

    def test_temperature_value(self):
        packets = parse_ccm_xml(SAMPLE_XML, source_ip="10.10.0.50")
        temp = packets[0]
        assert temp["ccm_type"] == "InAirTemp"
        assert temp["value"] == pytest.approx(23.5)
        assert temp["room"] == 1

    def test_humidity_value(self):
        packets = parse_ccm_xml(SAMPLE_XML, source_ip="10.10.0.50")
        humid = packets[1]
        assert humid["ccm_type"] == "InAirHumid"
        assert humid["value"] == pytest.approx(65.2)

    def test_source_ip(self):
        packets = parse_ccm_xml(SAMPLE_XML, source_ip="10.10.0.50")
        assert all(p["source_ip"] == "10.10.0.50" for p in packets)

    def test_timestamp_present(self):
        packets = parse_ccm_xml(SAMPLE_XML)
        assert all("timestamp" in p for p in packets)

    def test_multi_room(self):
        packets = parse_ccm_xml(SAMPLE_MULTI_ROOM)
        assert len(packets) == 2
        assert all(p["room"] == 2 for p in packets)

    def test_actuator_parse(self):
        packets = parse_ccm_xml(ACTUATOR_XML)
        assert len(packets) == 1
        pkt = packets[0]
        assert pkt["ccm_type"] == "VenFan"
        assert pkt["value"] == pytest.approx(1.0)
        assert pkt["priority"] == 10
        assert pkt["level"] == "A"

    def test_invalid_xml(self):
        packets = parse_ccm_xml(b"not xml at all")
        assert packets == []

    def test_empty_bytes(self):
        packets = parse_ccm_xml(b"")
        assert packets == []

    def test_xml_no_data_elements(self):
        xml = b'<?xml version="1.0"?><UECS ver="1.00-E10"><IP>10.10.0.1</IP></UECS>'
        packets = parse_ccm_xml(xml)
        assert packets == []


# -------------------------------------------------------------------
# Suffix stripping tests
# -------------------------------------------------------------------

class TestStripCcmSuffix:
    def test_strip_mC(self):
        assert strip_ccm_suffix("InAirTemp.mC") == "InAirTemp"

    def test_strip_cMC(self):
        assert strip_ccm_suffix("InAirHumid.cMC") == "InAirHumid"

    def test_strip_MC(self):
        assert strip_ccm_suffix("SoilTemp.MC") == "SoilTemp"

    def test_no_suffix(self):
        assert strip_ccm_suffix("InAirTemp") == "InAirTemp"

    def test_other_suffix_unchanged(self):
        assert strip_ccm_suffix("InAirTemp.foo") == "InAirTemp.foo"


# -------------------------------------------------------------------
# Classification tests
# -------------------------------------------------------------------

class TestClassifyCcmType:
    def test_sensor_types(self):
        for t in ["InAirTemp", "InAirHumid", "InAirCO2", "SoilTemp", "SoilEC"]:
            assert classify_ccm_type(t) == "sensor", f"{t} should be sensor"

    def test_actuator_types(self):
        for t in ["Irri", "VenFan", "CirHoriFan", "AirHeatBurn"]:
            assert classify_ccm_type(t) == "actuator", f"{t} should be actuator"

    def test_weather_types(self):
        for t in ["WAirTemp", "WAirHumid", "WWindSpeed"]:
            assert classify_ccm_type(t) == "weather", f"{t} should be weather"

    def test_unknown_type(self):
        assert classify_ccm_type("UnknownType") == "other"


# -------------------------------------------------------------------
# CcmReceiver topic generation tests
# -------------------------------------------------------------------

class TestCcmReceiverTopics:
    def test_sensor_topic(self):
        receiver = CcmReceiver(
            config={"daemon": {"house_id": "1"}, "ccm": {"enabled": False}},
            mqtt_client=None,
        )
        topic = receiver._make_topic("sensor", "InAirTemp")
        assert topic == "agriha/1/ccm/sensor/InAirTemp"

    def test_actuator_topic(self):
        receiver = CcmReceiver(
            config={"daemon": {"house_id": "h01"}, "ccm": {"enabled": False}},
            mqtt_client=None,
        )
        topic = receiver._make_topic("actuator", "VenFan")
        assert topic == "agriha/h01/ccm/actuator/VenFan"

    def test_default_house_id(self):
        receiver = CcmReceiver(
            config={"ccm": {"enabled": False}},
            mqtt_client=None,
        )
        topic = receiver._make_topic("sensor", "InAirTemp")
        assert topic == "agriha/h01/ccm/sensor/InAirTemp"
