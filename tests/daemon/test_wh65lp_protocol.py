"""
MISOL WH65LP プロトコルパーサー ユニットテスト

テスト内容:
  1. チェックサム検証
  2. 17バイト基本フレームのパース
  3. 21バイト拡張フレーム（気圧付き）のパース
  4. 無効値センチネル検出
  5. バイト3ビットフィールド（風向高位・風速高位・温度高位・バッテリー）
  6. フレーム長不足の例外
"""

import pytest

from agriha.daemon.wh65lp_reader import (
    verify_checksum,
    parse_frame,
    SENTINEL_WIND_DIR,
    SENTINEL_TEMP,
    SENTINEL_WIND,
    SENTINEL_GUST,
    SENTINEL_UV,
    SENTINEL_LIGHT,
    FRAME_LEN_BASE,
    FRAME_LEN_EXT,
)


# -------------------------------------------------------------------
# ヘルパー: テストフレーム生成
# -------------------------------------------------------------------

def make_frame(
    wind_dir_raw: int = 90,
    temp_raw: int = 596,      # (596-400)/10 = 19.6°C
    humidity: int = 70,
    wind_raw: int = 16,       # (16/8)*1.12 = 2.24 m/s
    gust_raw: int = 8,        # 8*1.12 = 8.96 m/s
    rain_raw: int = 10,       # 10*0.3 = 3.0 mm
    uv_raw: int = 100,        # 100/10 = 10.0 W/m2
    light_raw: int = 400,     # 400/10 = 40.0 lux
    battery_low: bool = False,
    pressure_raw: int | None = None,
) -> bytes:
    """
    指定したフィールド値から正しいチェックサム付きフレームを生成する。
    wind_dir_raw, temp_raw, wind_raw はビットフィールドに分解して格納。
    """
    data = bytearray(FRAME_LEN_BASE)
    data[0] = 0x24   # sync
    data[1] = 0x00   # sensor ID

    # Byte 2: wind_dir 下位8bit
    data[2] = wind_dir_raw & 0xFF

    # Byte 3: 複合ビットフィールド
    b3 = 0x00
    if wind_dir_raw & 0x100:   b3 |= 0x80  # 風向 bit8
    b3 |= (temp_raw >> 8) & 0x07            # 温度 bits10-8
    if wind_raw & 0x100:       b3 |= 0x10  # 風速 bit8
    if battery_low:            b3 |= 0x08  # バッテリー
    data[3] = b3

    # Byte 4: 温度 下位8bit
    data[4] = temp_raw & 0xFF

    # Byte 5: 湿度
    data[5] = humidity

    # Byte 6: 風速 下位8bit
    data[6] = wind_raw & 0xFF

    # Byte 7: 突風
    data[7] = gust_raw & 0xFF

    # Byte 8-9: 降雨量
    data[8] = (rain_raw >> 8) & 0xFF
    data[9] = rain_raw & 0xFF

    # Byte 10-11: UV
    data[10] = (uv_raw >> 8) & 0xFF
    data[11] = uv_raw & 0xFF

    # Byte 12-14: 照度
    data[12] = (light_raw >> 16) & 0xFF
    data[13] = (light_raw >> 8) & 0xFF
    data[14] = light_raw & 0xFF

    # Byte 15: reserved (0)
    data[15] = 0x00

    # Byte 16: チェックサム
    data[16] = sum(data[0:16]) & 0xFF

    if pressure_raw is not None:
        # 拡張フレーム: 4バイト追加（Byte17-19が気圧、Byte20は0）
        ext = bytearray(4)
        ext[0] = (pressure_raw >> 16) & 0xFF
        ext[1] = (pressure_raw >> 8) & 0xFF
        ext[2] = pressure_raw & 0xFF
        ext[3] = 0x00
        return bytes(data) + bytes(ext)

    return bytes(data)


# -------------------------------------------------------------------
# チェックサムテスト
# -------------------------------------------------------------------

class TestChecksum:
    def test_valid_checksum(self):
        frame = make_frame()
        assert verify_checksum(frame) is True

    def test_invalid_checksum(self):
        frame = bytearray(make_frame())
        frame[16] ^= 0xFF   # チェックサムを壊す
        assert verify_checksum(bytes(frame)) is False

    def test_short_frame(self):
        assert verify_checksum(bytes(10)) is False

    def test_checksum_all_zeros(self):
        """全バイト0のフレームはチェックサムも0。"""
        data = bytearray(FRAME_LEN_BASE)
        data[16] = 0x00
        assert verify_checksum(bytes(data)) is True

    def test_checksum_wrap_around(self):
        """チェックサムが256を超えても下位8bitで正しく計算される。"""
        # 各バイトを0xFFにすると sum(0xFF * 16) = 4080, 4080 & 0xFF = 0xF0
        data = bytearray(FRAME_LEN_BASE)
        for i in range(16):
            data[i] = 0xFF
        data[16] = (0xFF * 16) & 0xFF  # = 0xF0
        assert verify_checksum(bytes(data)) is True


# -------------------------------------------------------------------
# 17バイト基本フレームのパーステスト
# -------------------------------------------------------------------

class TestParseBasicFrame:
    def test_wind_direction(self):
        frame = make_frame(wind_dir_raw=90)
        result = parse_frame(frame)
        assert result["wind_dir_deg"] == 90

    def test_wind_direction_high_bit(self):
        """風向 bit8 (Byte3 bit7) を使う角度 (256°)。"""
        frame = make_frame(wind_dir_raw=256)
        result = parse_frame(frame)
        assert result["wind_dir_deg"] == 256

    def test_wind_direction_max(self):
        """風向最大 255°（bit8なし）。"""
        frame = make_frame(wind_dir_raw=255)
        result = parse_frame(frame)
        assert result["wind_dir_deg"] == 255

    def test_temperature_positive(self):
        """温度: raw=596 → (596-400)/10 = 19.6°C"""
        frame = make_frame(temp_raw=596)
        result = parse_frame(frame)
        assert result["temperature_c"] == pytest.approx(19.6, abs=0.05)

    def test_temperature_zero(self):
        """温度: raw=400 → 0.0°C"""
        frame = make_frame(temp_raw=400)
        result = parse_frame(frame)
        assert result["temperature_c"] == pytest.approx(0.0, abs=0.05)

    def test_temperature_negative(self):
        """温度: raw=150 → (150-400)/10 = -25.0°C"""
        frame = make_frame(temp_raw=150)
        result = parse_frame(frame)
        assert result["temperature_c"] == pytest.approx(-25.0, abs=0.05)

    def test_temperature_high_bits(self):
        """温度 high bits (Byte3 bits2-0) を使う温度。raw = (3<<8)|0 = 768 → 36.8°C"""
        frame = make_frame(temp_raw=768)
        result = parse_frame(frame)
        assert result["temperature_c"] == pytest.approx(36.8, abs=0.05)

    def test_humidity(self):
        frame = make_frame(humidity=65)
        result = parse_frame(frame)
        assert result["humidity_pct"] == 65

    def test_wind_speed(self):
        """風速: raw=16 → (16/8.0)*1.12 = 2.24 m/s"""
        frame = make_frame(wind_raw=16)
        result = parse_frame(frame)
        assert result["wind_speed_ms"] == pytest.approx(2.24, abs=0.01)

    def test_wind_speed_zero(self):
        frame = make_frame(wind_raw=0)
        result = parse_frame(frame)
        assert result["wind_speed_ms"] == pytest.approx(0.0, abs=0.01)

    def test_wind_speed_high_bit(self):
        """風速 bit8 (Byte3 bit4): raw=256 → (256/8.0)*1.12 = 35.84 m/s"""
        frame = make_frame(wind_raw=256)
        result = parse_frame(frame)
        assert result["wind_speed_ms"] == pytest.approx(35.84, abs=0.01)

    def test_gust_speed(self):
        """突風: raw=8 → 8*1.12 = 8.96 m/s"""
        frame = make_frame(gust_raw=8)
        result = parse_frame(frame)
        assert result["gust_speed_ms"] == pytest.approx(8.96, abs=0.01)

    def test_rainfall(self):
        """降雨量: raw=10 → 10*0.3 = 3.0 mm"""
        frame = make_frame(rain_raw=10)
        result = parse_frame(frame)
        assert result["rainfall_mm"] == pytest.approx(3.0, abs=0.05)

    def test_rainfall_16bit(self):
        """降雨量: raw=1000 → 1000*0.3 = 300.0 mm（16bit使用）"""
        frame = make_frame(rain_raw=1000)
        result = parse_frame(frame)
        assert result["rainfall_mm"] == pytest.approx(300.0, abs=0.05)

    def test_uv(self):
        """UV: raw=100 → 100/10.0 = 10.0 W/m2"""
        frame = make_frame(uv_raw=100)
        result = parse_frame(frame)
        assert result["uv_wm2"] == pytest.approx(10.0, abs=0.05)

    def test_illuminance(self):
        """照度: raw=400 → 400/10.0 = 40.0 lux"""
        frame = make_frame(light_raw=400)
        result = parse_frame(frame)
        assert result["light_lux"] == pytest.approx(40.0, abs=0.05)

    def test_illuminance_large(self):
        """照度: raw=2000000 → 200000.0 lux（24bit使用）"""
        frame = make_frame(light_raw=2_000_000)
        result = parse_frame(frame)
        assert result["light_lux"] == pytest.approx(200_000.0, abs=0.5)

    def test_battery_low_false(self):
        frame = make_frame(battery_low=False)
        result = parse_frame(frame)
        assert result["battery_low"] is False

    def test_battery_low_true(self):
        frame = make_frame(battery_low=True)
        result = parse_frame(frame)
        assert result["battery_low"] is True

    def test_no_pressure_in_basic_frame(self):
        """17バイトフレームでは気圧はNone。"""
        frame = make_frame()
        assert len(frame) == FRAME_LEN_BASE
        result = parse_frame(frame)
        assert result["pressure_hpa"] is None


# -------------------------------------------------------------------
# 21バイト拡張フレーム（気圧付き）のパーステスト
# -------------------------------------------------------------------

class TestParseExtendedFrame:
    def test_pressure(self):
        """気圧: raw=101325 → 101325/100.0 = 1013.25 hPa"""
        frame = make_frame(pressure_raw=101325)
        assert len(frame) == FRAME_LEN_EXT
        result = parse_frame(frame)
        assert result["pressure_hpa"] == pytest.approx(1013.25, abs=0.05)

    def test_pressure_low(self):
        """低気圧: raw=97000 → 970.0 hPa"""
        frame = make_frame(pressure_raw=97000)
        result = parse_frame(frame)
        assert result["pressure_hpa"] == pytest.approx(970.0, abs=0.05)

    def test_basic_fields_unaffected(self):
        """拡張フレームでも基本フィールドは正常にパースされる。"""
        frame = make_frame(temp_raw=596, humidity=70, pressure_raw=101325)
        result = parse_frame(frame)
        assert result["temperature_c"] == pytest.approx(19.6, abs=0.05)
        assert result["humidity_pct"] == 70
        assert result["pressure_hpa"] is not None


# -------------------------------------------------------------------
# 無効値センチネル検出テスト
# -------------------------------------------------------------------

class TestSentinelValues:
    def test_wind_dir_sentinel(self):
        """風向センチネル: 0x1FF → None"""
        frame = make_frame(wind_dir_raw=SENTINEL_WIND_DIR)
        result = parse_frame(frame)
        assert result["wind_dir_deg"] is None

    def test_temperature_sentinel(self):
        """温度センチネル: 0x7FF → None"""
        frame = make_frame(temp_raw=SENTINEL_TEMP)
        result = parse_frame(frame)
        assert result["temperature_c"] is None

    def test_wind_speed_sentinel(self):
        """風速センチネル: 0x1FF → None"""
        frame = make_frame(wind_raw=SENTINEL_WIND)
        result = parse_frame(frame)
        assert result["wind_speed_ms"] is None

    def test_gust_sentinel(self):
        """突風センチネル: 0xFF → None"""
        frame = make_frame(gust_raw=SENTINEL_GUST)
        result = parse_frame(frame)
        assert result["gust_speed_ms"] is None

    def test_uv_sentinel(self):
        """UVセンチネル: 0xFFFF → None"""
        frame = make_frame(uv_raw=SENTINEL_UV)
        result = parse_frame(frame)
        assert result["uv_wm2"] is None

    def test_light_sentinel(self):
        """照度センチネル: 0xFFFFFF → None"""
        frame = make_frame(light_raw=SENTINEL_LIGHT)
        result = parse_frame(frame)
        assert result["light_lux"] is None

    def test_rainfall_not_sentinel(self):
        """降雨量はセンチネルなし（常にfloat）。"""
        frame = make_frame(rain_raw=0)
        result = parse_frame(frame)
        assert result["rainfall_mm"] == pytest.approx(0.0)


# -------------------------------------------------------------------
# エラーケーステスト
# -------------------------------------------------------------------

class TestErrorCases:
    def test_frame_too_short(self):
        with pytest.raises(ValueError, match="Frame too short"):
            parse_frame(bytes(10))

    def test_exactly_17_bytes(self):
        """17バイトでエラーなくパースできること。"""
        frame = make_frame()
        assert len(frame) == FRAME_LEN_BASE
        result = parse_frame(frame)
        assert isinstance(result, dict)

    def test_exactly_21_bytes(self):
        """21バイトでエラーなくパースできること。"""
        frame = make_frame(pressure_raw=101325)
        assert len(frame) == FRAME_LEN_EXT
        result = parse_frame(frame)
        assert isinstance(result, dict)

    def test_verify_checksum_empty(self):
        assert verify_checksum(b"") is False

    def test_verify_checksum_16_bytes(self):
        """16バイトはFRAME_LEN_BASE未満→False。"""
        assert verify_checksum(bytes(16)) is False
