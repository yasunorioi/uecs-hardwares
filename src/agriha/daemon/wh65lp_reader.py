#!/usr/bin/env python3
"""
MISOL WH65LP RS485 Weather Station Reader

プロトコル仕様:
  - ボーレート: 9600, 8N1
  - 送信間隔: 約16秒（プッシュ型UART）
  - フレーム長: 17バイト（基本）/ 21バイト（気圧付き）
  - 同期バイト: 0x24（フレーム先頭）
  - チェックサム: sum(data[0:16]) & 0xFF == data[16]

バイトマップ:
  Byte 0      : 0x24 (sync)
  Byte 1      : センサーID
  Byte 2-3    : 風向 deg  = data[2] | ((data[3] & 0x80) << 1)
  Byte 3-4    : 温度      = (data[4] | ((data[3] & 0x07) << 8) - 400) / 10.0 °C
  Byte 5      : 湿度 %
  Byte 3,6    : 風速      = (data[6] | ((data[3] & 0x10) << 4)) / 8.0 * 1.12 m/s
  Byte 7      : 突風      = data[7] * 1.12 m/s
  Byte 8-9    : 降雨量    = ((data[8] << 8) | data[9]) * 0.3 mm (累積)
  Byte 10-11  : UV強度    = ((data[10] << 8) | data[11]) / 10.0 W/m2
  Byte 12-14  : 照度      = ((data[12]<<16)|(data[13]<<8)|data[14]) / 10.0 lux
  Byte 3 bit3 : バッテリー低下フラグ
  Byte 16     : チェックサム
  Byte 17-19  : 気圧      = ((data[17]<<16)|(data[18]<<8)|data[19]) / 100.0 hPa (拡張)

無効値センチネル:
  風向: 0x1FF, 温度: 0x7FF, 風速: 0x1FF, 突風: 0xFF, UV: 0xFFFF, 照度: 0xFFFFFF
"""

import sys
import time
import json
import argparse
import logging

try:
    import serial
except ImportError:
    serial = None  # type: ignore[assignment]

logger = logging.getLogger(__name__)

# -------------------------------------------------------------------
# 定数
# -------------------------------------------------------------------
SYNC_BYTE = 0x24
FRAME_LEN_BASE = 17   # 基本フレーム（チェックサムまで）
FRAME_LEN_EXT  = 21   # 拡張フレーム（気圧付き）

# 無効値センチネル
SENTINEL_WIND_DIR = 0x1FF
SENTINEL_TEMP     = 0x7FF
SENTINEL_WIND     = 0x1FF
SENTINEL_GUST     = 0xFF
SENTINEL_UV       = 0xFFFF
SENTINEL_LIGHT    = 0xFFFFFF


# -------------------------------------------------------------------
# プロトコルパーサー（シリアルポート不要。単体テスト可能）
# -------------------------------------------------------------------

def verify_checksum(data: bytes) -> bool:
    """
    バイト0-15の合計の下位8bitがバイト16と一致するか検証。

    Args:
        data: 17バイト以上のフレームデータ

    Returns:
        チェックサムが一致すればTrue
    """
    if len(data) < FRAME_LEN_BASE:
        return False
    return (sum(data[0:16]) & 0xFF) == data[16]


def parse_frame(data: bytes) -> dict:
    """
    17または21バイトのフレームをパースし、測定値の辞書を返す。
    センチネル値（無効測定）はNoneで表現。

    Args:
        data: 17または21バイトのフレームデータ（チェックサム検証済み前提）

    Returns:
        {
            "wind_dir_deg": int|None,
            "temperature_c": float|None,
            "humidity_pct": int,
            "wind_speed_ms": float|None,
            "gust_speed_ms": float|None,
            "rainfall_mm": float,
            "uv_wm2": float|None,
            "light_lux": float|None,
            "pressure_hpa": float|None,
            "battery_low": bool,
        }

    Raises:
        ValueError: フレーム長が不正な場合
    """
    if len(data) < FRAME_LEN_BASE:
        raise ValueError(f"Frame too short: {len(data)} bytes (need {FRAME_LEN_BASE})")

    b3 = data[3]
    result: dict = {}

    # 風向 (9-bit): Byte2 + Byte3[bit7]
    wind_dir_raw = data[2] | ((b3 & 0x80) << 1)
    result["wind_dir_deg"] = None if wind_dir_raw == SENTINEL_WIND_DIR else wind_dir_raw

    # 温度 (11-bit): Byte4 + Byte3[bits2:0]
    temp_raw = data[4] | ((b3 & 0x07) << 8)
    result["temperature_c"] = None if temp_raw == SENTINEL_TEMP else round((temp_raw - 400) / 10.0, 1)

    # 湿度 (8-bit): Byte5
    result["humidity_pct"] = data[5]

    # 風速 (9-bit): Byte6 + Byte3[bit4]
    wind_raw = data[6] | ((b3 & 0x10) << 4)
    result["wind_speed_ms"] = None if wind_raw == SENTINEL_WIND else round((wind_raw / 8.0) * 1.12, 2)

    # 突風 (8-bit): Byte7
    gust_raw = data[7]
    result["gust_speed_ms"] = None if gust_raw == SENTINEL_GUST else round(gust_raw * 1.12, 2)

    # 降雨量累積 (16-bit): Byte8-9
    rain_raw = (data[8] << 8) | data[9]
    result["rainfall_mm"] = round(rain_raw * 0.3, 1)

    # UV強度 (16-bit): Byte10-11
    uv_raw = (data[10] << 8) | data[11]
    result["uv_wm2"] = None if uv_raw == SENTINEL_UV else round(uv_raw / 10.0, 1)

    # 照度 (24-bit): Byte12-14
    light_raw = (data[12] << 16) | (data[13] << 8) | data[14]
    result["light_lux"] = None if light_raw == SENTINEL_LIGHT else round(light_raw / 10.0, 1)

    # バッテリー低下フラグ: Byte3[bit3]
    result["battery_low"] = bool(b3 & 0x08)

    # 気圧 (拡張フレーム: Byte17-19)
    if len(data) >= FRAME_LEN_EXT:
        pressure_raw = (data[17] << 16) | (data[18] << 8) | data[19]
        result["pressure_hpa"] = round(pressure_raw / 100.0, 1)
    else:
        result["pressure_hpa"] = None

    return result


def read_exact(ser: "serial.Serial", n: int) -> bytes | None:
    """
    シリアルポートからちょうどnバイト読み取る。
    タイムアウトが発生した場合はNoneを返す。
    """
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            logger.warning(f"Short read: got {len(buf)}/{n} bytes")
            return None
        buf.extend(chunk)
    return bytes(buf)


def read_frame(ser: "serial.Serial", sync_timeout: float = 60.0) -> bytes | None:
    """
    シリアルポートから1フレームを読み取る。

    1. 0x24同期バイトが見つかるまでバイトを読み捨てる（最大sync_timeout秒）
    2. 同期バイト含む17バイト収集
    3. チェックサム検証
    4. 追加4バイト（気圧）があれば21バイトフレームとして返す

    Args:
        ser:          pyserialのSerialオブジェクト（timeout設定済み）
        sync_timeout: 同期バイト待ちの最大秒数

    Returns:
        17または21バイトのフレームデータ、失敗時はNone
    """
    deadline = time.monotonic() + sync_timeout

    # 0x24同期バイトを探す
    while True:
        if time.monotonic() > deadline:
            logger.warning("Timeout waiting for 0x24 sync byte")
            return None
        b = ser.read(1)
        if not b:
            continue
        if b[0] == SYNC_BYTE:
            logger.debug("Sync byte found")
            break
        else:
            logger.debug(f"Skip: 0x{b[0]:02X}")

    # 残り16バイト読み取り（同期バイト含む17バイト収集）
    rest = read_exact(ser, FRAME_LEN_BASE - 1)
    if rest is None:
        return None

    frame = bytes([SYNC_BYTE]) + rest

    # チェックサム検証
    if not verify_checksum(frame):
        calc = sum(frame[0:16]) & 0xFF
        logger.warning(
            f"Checksum mismatch: calculated=0x{calc:02X}, got=0x{frame[16]:02X} | frame={frame.hex()}"
        )
        return None

    # 拡張フレーム（気圧）を試みる（100ms以内に4バイトあれば）
    orig_timeout = ser.timeout
    ser.timeout = 0.1
    ext = ser.read(4)
    ser.timeout = orig_timeout

    if len(ext) == 4:
        logger.debug("Extended frame (21 bytes) detected")
        return frame + ext
    else:
        logger.debug("Basic frame (17 bytes)")
        return frame


# -------------------------------------------------------------------
# 人間可読出力
# -------------------------------------------------------------------

WIND_DIR_NAMES = [
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
]


def degrees_to_compass(deg: int | None) -> str:
    if deg is None:
        return "N/A"
    idx = round(deg / 22.5) % 16
    return WIND_DIR_NAMES[idx]


def format_human(data: dict, frame_hex: str = "") -> str:
    """測定値を人間可読テキストにフォーマット。"""
    lines = ["─" * 44, "  MISOL WH65LP Weather Data", "─" * 44]

    def _v(val, unit: str, fmt: str = "{}") -> str:
        return "N/A" if val is None else f"{fmt.format(val)} {unit}"

    deg = data["wind_dir_deg"]
    compass = degrees_to_compass(deg)
    lines.append(f"  Wind Direction : {_v(deg, '°')} ({compass})")
    lines.append(f"  Temperature    : {_v(data['temperature_c'], '°C', '{:.1f}')}")
    lines.append(f"  Humidity       : {data['humidity_pct']} %")
    lines.append(f"  Wind Speed     : {_v(data['wind_speed_ms'], 'm/s', '{:.2f}')}")
    lines.append(f"  Gust Speed     : {_v(data['gust_speed_ms'], 'm/s', '{:.2f}')}")
    lines.append(f"  Rainfall (acc) : {data['rainfall_mm']:.1f} mm")
    lines.append(f"  UV Intensity   : {_v(data['uv_wm2'], 'W/m²', '{:.1f}')}")
    lines.append(f"  Illuminance    : {_v(data['light_lux'], 'lux', '{:.1f}')}")
    if data.get("pressure_hpa") is not None:
        lines.append(f"  Pressure       : {data['pressure_hpa']:.1f} hPa")
    lines.append(f"  Battery Low    : {'⚠ YES' if data['battery_low'] else 'OK'}")
    if frame_hex:
        lines.append(f"  Raw Frame      : {frame_hex}")
    lines.append("─" * 44)
    return "\n".join(lines)


# -------------------------------------------------------------------
# CLI エントリーポイント
# -------------------------------------------------------------------

def main() -> None:
    if serial is None:
        print("ERROR: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
        sys.exit(1)
    parser = argparse.ArgumentParser(
        description="MISOL WH65LP RS485 weather station reader",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --port /dev/ttyUSB0
  %(prog)s --port /dev/ttyUSB0 --count 5
  %(prog)s --port /dev/ttyUSB0 --json
  %(prog)s --port /dev/ttyUSB0 --json --count 3 | tee weather.jsonl
""",
    )
    parser.add_argument("--port",    default="/dev/ttyUSB0", help="シリアルポート (default: /dev/ttyUSB0)")
    parser.add_argument("--baud",    default=9600, type=int,  help="ボーレート (default: 9600)")
    parser.add_argument("--count",   default=0,    type=int,  help="受信フレーム数で終了 (0=無限)")
    parser.add_argument("--json",    action="store_true",     help="JSON形式で出力")
    parser.add_argument("--timeout", default=60.0, type=float, help="同期バイト待ちタイムアウト秒 (default: 60)")
    parser.add_argument("--verbose", action="store_true",     help="デバッグログ有効化")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.WARNING,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    try:
        ser = serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=2.0,
        )
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    logger.info(f"Opened {args.port} at {args.baud} baud")
    received = 0

    try:
        while True:
            frame = read_frame(ser, sync_timeout=args.timeout)
            if frame is None:
                print("ERROR: Failed to read frame (timeout or checksum error)", file=sys.stderr)
                sys.exit(1)

            data = parse_frame(frame)
            ts = time.strftime("%Y-%m-%dT%H:%M:%S")

            if args.json:
                output = {"timestamp": ts, **data, "frame_hex": frame.hex()}
                print(json.dumps(output, ensure_ascii=False), flush=True)
            else:
                print(f"\n[{ts}]")
                print(format_human(data, frame.hex()), flush=True)

            received += 1
            if args.count > 0 and received >= args.count:
                break

    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()
