"""tests/test_ds18b20.py — DS18B20 ドライバのユニットテスト（sysfs ファイル mock）

tmp_path を使って実際のファイルシステム上にダミーの w1 デバイス構造を作り、
実デバイスなしでテストする。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from agriha.daemon.ds18b20 import DS18B20, DS18B20Error, DEFAULT_DEVICE_ID


# ------------------------------------------------------------------ #
# ヘルパー
# ------------------------------------------------------------------ #

def _make_device(tmp_path: Path, device_id: str, raw_value: str) -> Path:
    """テスト用 w1 デバイスディレクトリを作成し、temperature ファイルを書く。"""
    device_dir = tmp_path / device_id
    device_dir.mkdir()
    (device_dir / "temperature").write_text(raw_value + "\n")
    return device_dir


# ------------------------------------------------------------------ #
# TestDS18B20ReadCelsius
# ------------------------------------------------------------------ #

class TestDS18B20ReadCelsius:
    def test_normal_positive(self, tmp_path):
        """24500 millidegrees → 24.5°C"""
        _make_device(tmp_path, DEFAULT_DEVICE_ID, "24500")
        sensor = DS18B20(device_id=DEFAULT_DEVICE_ID, base_path=str(tmp_path))
        assert sensor.read_celsius() == pytest.approx(24.5)

    def test_negative_temperature(self, tmp_path):
        """-5125 millidegrees → -5.125°C"""
        _make_device(tmp_path, DEFAULT_DEVICE_ID, "-5125")
        sensor = DS18B20(device_id=DEFAULT_DEVICE_ID, base_path=str(tmp_path))
        assert sensor.read_celsius() == pytest.approx(-5.125)

    def test_zero(self, tmp_path):
        """0 millidegrees → 0.0°C"""
        _make_device(tmp_path, DEFAULT_DEVICE_ID, "0")
        sensor = DS18B20(device_id=DEFAULT_DEVICE_ID, base_path=str(tmp_path))
        assert sensor.read_celsius() == 0.0

    def test_high_temperature(self, tmp_path):
        """125000 millidegrees → 125.0°C（DS18B20 最大値）"""
        _make_device(tmp_path, DEFAULT_DEVICE_ID, "125000")
        sensor = DS18B20(device_id=DEFAULT_DEVICE_ID, base_path=str(tmp_path))
        assert sensor.read_celsius() == pytest.approx(125.0)

    def test_trailing_whitespace_stripped(self, tmp_path):
        """ファイル末尾の空白・改行が無視されること"""
        _make_device(tmp_path, DEFAULT_DEVICE_ID, "20000  ")
        sensor = DS18B20(device_id=DEFAULT_DEVICE_ID, base_path=str(tmp_path))
        assert sensor.read_celsius() == pytest.approx(20.0)

    def test_device_not_found(self, tmp_path):
        """デバイスが存在しない → DS18B20Error"""
        sensor = DS18B20(device_id="28-nonexistent", base_path=str(tmp_path))
        with pytest.raises(DS18B20Error, match="デバイスが見つかりません"):
            sensor.read_celsius()

    def test_invalid_value(self, tmp_path):
        """temperature ファイルの値が数値でない → DS18B20Error"""
        _make_device(tmp_path, DEFAULT_DEVICE_ID, "invalid")
        sensor = DS18B20(device_id=DEFAULT_DEVICE_ID, base_path=str(tmp_path))
        with pytest.raises(DS18B20Error, match="不正な温度値"):
            sensor.read_celsius()

    def test_ds18b20_error_is_oserror(self):
        """DS18B20Error は OSError のサブクラスであること"""
        assert issubclass(DS18B20Error, OSError)


# ------------------------------------------------------------------ #
# TestDS18B20Discover
# ------------------------------------------------------------------ #

class TestDS18B20Discover:
    def test_discover_finds_devices(self, tmp_path):
        """28-* パターンのデバイスが見つかること"""
        _make_device(tmp_path, "28-aabbccddeeff", "20000")
        _make_device(tmp_path, "28-001122334455", "22000")

        found = DS18B20.discover(base_path=str(tmp_path))
        assert len(found) == 2
        device_ids = {d.device_id for d in found}
        assert device_ids == {"28-aabbccddeeff", "28-001122334455"}

    def test_discover_empty(self, tmp_path):
        """デバイスがない場合は空リスト"""
        found = DS18B20.discover(base_path=str(tmp_path))
        assert found == []

    def test_discover_ignores_non_28_prefix(self, tmp_path):
        """28- 以外のデバイスは除外されること"""
        _make_device(tmp_path, "28-valid", "21000")
        other = tmp_path / "10-notds18b20"
        other.mkdir()
        (other / "temperature").write_text("20000\n")

        found = DS18B20.discover(base_path=str(tmp_path))
        assert len(found) == 1
        assert found[0].device_id == "28-valid"

    def test_discover_ignores_missing_temperature_file(self, tmp_path):
        """temperature ファイルがない 28-* は除外されること"""
        device_dir = tmp_path / "28-notworking"
        device_dir.mkdir()
        # temperature ファイルなし

        found = DS18B20.discover(base_path=str(tmp_path))
        assert found == []

    def test_discovered_sensor_readable(self, tmp_path):
        """discover() で得たセンサーから read_celsius() できること"""
        _make_device(tmp_path, "28-readable", "18500")
        found = DS18B20.discover(base_path=str(tmp_path))
        assert len(found) == 1
        assert found[0].read_celsius() == pytest.approx(18.5)
