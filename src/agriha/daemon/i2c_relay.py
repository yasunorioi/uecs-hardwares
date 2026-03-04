"""MCP23008 8-channel relay control via smbus2.

レジスタ:
  IODIR (0x00): I/O方向設定 (0=出力)
  GPIO  (0x09): GPIO状態読み取り
  OLAT  (0x0A): 出力ラッチ書き込み

配線: 逆順配線（ch1=bit7, ch2=bit6, ... ch8=bit0）
  ch1 → GP7 (0x80)
  ch2 → GP6 (0x40)
  ...
  ch8 → GP0 (0x01)
"""

from __future__ import annotations

import logging

logger = logging.getLogger(__name__)

try:
    import smbus2
    _SMBUS2_AVAILABLE = True
except ImportError:
    _SMBUS2_AVAILABLE = False


class MCP23008Relay:
    """smbus2によるMCP23008 8chリレー制御。

    Usage:
        with MCP23008Relay(bus_num=1, addr=0x20) as relay:
            relay.set_relay(1, True)   # ch1 ON
            state = relay.get_state()  # ビットマスク
            relay.set_all(0b10000001)  # ch1, ch8 ON
    """

    IODIR = 0x00  # I/O Direction register (0=output, 1=input)
    GPIO  = 0x09  # GPIO port register (read)
    OLAT  = 0x0A  # Output Latch register (write)

    def __init__(self, bus_num: int = 1, addr: int = 0x20) -> None:
        if not _SMBUS2_AVAILABLE:
            raise ImportError("smbus2 is required. Install with: pip install smbus2")
        self._bus = smbus2.SMBus(bus_num)
        self._addr = addr
        self._olat: int = 0x00  # shadow register (現在の出力ラッチ値)

        # 全ピンを出力モードに設定
        self._bus.write_byte_data(self._addr, self.IODIR, 0x00)
        logger.debug("MCP23008 initialized at bus=%d addr=0x%02X", bus_num, addr)

    # ------------------------------------------------------------------ #
    # チャンネル → ビット変換
    # ------------------------------------------------------------------ #

    @staticmethod
    def ch_to_bit(channel: int) -> int:
        """チャンネル番号(1-8)をビットマスクに変換する。

        逆順配線: ch1=bit7(0x80), ch2=bit6(0x40), ..., ch8=bit0(0x01)
        """
        if not 1 <= channel <= 8:
            raise ValueError(f"Channel must be 1-8, got {channel}")
        return 1 << (8 - channel)

    # ------------------------------------------------------------------ #
    # 制御API
    # ------------------------------------------------------------------ #

    def set_relay(self, channel: int, on: bool) -> None:
        """個別リレーON/OFF。

        Args:
            channel: チャンネル番号 (1-8)
            on: True=ON, False=OFF
        """
        bit = self.ch_to_bit(channel)
        if on:
            self._olat |= bit
        else:
            self._olat &= (~bit) & 0xFF
        self._bus.write_byte_data(self._addr, self.OLAT, self._olat)
        logger.debug("relay ch%d %s (olat=0x%02X)", channel, "ON" if on else "OFF", self._olat)

    def get_state(self) -> int:
        """全8ch状態をビットマスクで返す。

        Returns:
            OLATレジスタ値 (bit7=ch1, bit0=ch8)
        """
        return self._bus.read_byte_data(self._addr, self.OLAT)

    def get_relay(self, channel: int) -> bool:
        """指定チャンネルの現在状態を返す。

        Args:
            channel: チャンネル番号 (1-8)

        Returns:
            True=ON, False=OFF
        """
        bit = self.ch_to_bit(channel)
        return bool(self.get_state() & bit)

    def set_all(self, bitmask: int) -> None:
        """全8ch一括設定。

        Args:
            bitmask: ビットマスク (bit7=ch1, bit0=ch8)。0x80=ch1 ON のみ。
        """
        self._olat = bitmask & 0xFF
        self._bus.write_byte_data(self._addr, self.OLAT, self._olat)
        logger.debug("relay set_all(0x%02X)", self._olat)

    def all_off(self) -> None:
        """全チャンネルOFF。"""
        self.set_all(0x00)

    # ------------------------------------------------------------------ #
    # コンテキストマネージャ
    # ------------------------------------------------------------------ #

    def close(self) -> None:
        """I2Cバスを閉じる。"""
        try:
            self._bus.close()
        except Exception:
            pass

    def __enter__(self) -> "MCP23008Relay":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()
