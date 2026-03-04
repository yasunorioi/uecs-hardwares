"""Tests for i2c_relay.py using smbus2 mock.

smbus2 が未インストール環境でも実行できるよう、
unittest.mock でSMBusをモックする。
"""

from __future__ import annotations

import sys
from unittest.mock import MagicMock, call, patch

import pytest

# smbus2 を mock として登録してから i2c_relay をインポート
smbus2_mock = MagicMock()
smbus2_mock.SMBus = MagicMock
sys.modules.setdefault("smbus2", smbus2_mock)

from agriha.daemon.i2c_relay import MCP23008Relay  # noqa: E402


# ------------------------------------------------------------------ #
# フィクスチャ
# ------------------------------------------------------------------ #

@pytest.fixture
def mock_bus():
    """smbus2.SMBus モックを返す。"""
    return MagicMock()


@pytest.fixture
def relay(mock_bus):
    """MCP23008Relayのインスタンスをmock busで返す。"""
    with patch("agriha.daemon.i2c_relay.smbus2") as mock_smbus2:
        mock_smbus2.SMBus.return_value = mock_bus
        r = MCP23008Relay(bus_num=1, addr=0x20)
        r._bus = mock_bus  # 直接差し替え
        yield r


# ------------------------------------------------------------------ #
# ch_to_bit: 逆順配線マッピングのテスト
# ------------------------------------------------------------------ #

class TestChToBit:
    """ch_to_bit 静的メソッドのユニットテスト。"""

    def test_ch1_is_bit7(self):
        """ch1 → bit7 (0x80)"""
        assert MCP23008Relay.ch_to_bit(1) == 0x80

    def test_ch2_is_bit6(self):
        """ch2 → bit6 (0x40)"""
        assert MCP23008Relay.ch_to_bit(2) == 0x40

    def test_ch3_is_bit5(self):
        """ch3 → bit5 (0x20)"""
        assert MCP23008Relay.ch_to_bit(3) == 0x20

    def test_ch4_is_bit4(self):
        """ch4 → bit4 (0x10)"""
        assert MCP23008Relay.ch_to_bit(4) == 0x10

    def test_ch5_is_bit3(self):
        """ch5 → bit3 (0x08)"""
        assert MCP23008Relay.ch_to_bit(5) == 0x08

    def test_ch6_is_bit2(self):
        """ch6 → bit2 (0x04)"""
        assert MCP23008Relay.ch_to_bit(6) == 0x04

    def test_ch7_is_bit1(self):
        """ch7 → bit1 (0x02)"""
        assert MCP23008Relay.ch_to_bit(7) == 0x02

    def test_ch8_is_bit0(self):
        """ch8 → bit0 (0x01)"""
        assert MCP23008Relay.ch_to_bit(8) == 0x01

    def test_invalid_channel_0(self):
        """ch0 は ValueError"""
        with pytest.raises(ValueError):
            MCP23008Relay.ch_to_bit(0)

    def test_invalid_channel_9(self):
        """ch9 は ValueError"""
        with pytest.raises(ValueError):
            MCP23008Relay.ch_to_bit(9)


# ------------------------------------------------------------------ #
# MCP23008Relay 初期化
# ------------------------------------------------------------------ #

class TestInit:
    """初期化時のI2C操作テスト。"""

    def test_init_sets_iodir_to_output(self, mock_bus):
        """初期化時に IODIR=0x00 (全ピン出力) を書き込む。"""
        with patch("agriha.daemon.i2c_relay.smbus2") as mock_smbus2:
            mock_smbus2.SMBus.return_value = mock_bus
            MCP23008Relay(bus_num=1, addr=0x20)
        mock_bus.write_byte_data.assert_called_once_with(0x20, MCP23008Relay.IODIR, 0x00)

    def test_initial_olat_is_zero(self, relay):
        """初期OLATシャドーレジスタは0x00。"""
        assert relay._olat == 0x00


# ------------------------------------------------------------------ #
# set_relay
# ------------------------------------------------------------------ #

class TestSetRelay:
    """set_relay() のテスト。"""

    def test_set_relay_ch1_on(self, relay):
        """ch1 ON → OLAT=0x80 を書き込む。"""
        relay.set_relay(1, True)
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0x80)
        assert relay._olat == 0x80

    def test_set_relay_ch8_on(self, relay):
        """ch8 ON → OLAT=0x01 を書き込む。"""
        relay.set_relay(8, True)
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0x01)

    def test_set_relay_ch1_off_clears_bit(self, relay):
        """ch1 を ON後に OFF → bit7 がクリアされる。"""
        relay._olat = 0xFF  # 全ch ON 状態を想定
        relay.set_relay(1, False)
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0x7F)
        assert relay._olat == 0x7F

    def test_set_relay_multiple_channels(self, relay):
        """複数チャンネルを順番にON → OLATが正しく累積される。"""
        relay.set_relay(1, True)   # 0x80
        relay.set_relay(8, True)   # 0x80 | 0x01 = 0x81
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0x81)
        assert relay._olat == 0x81

    def test_set_relay_invalid_channel(self, relay):
        """不正チャンネルは ValueError。"""
        with pytest.raises(ValueError):
            relay.set_relay(0, True)
        with pytest.raises(ValueError):
            relay.set_relay(9, True)


# ------------------------------------------------------------------ #
# get_state
# ------------------------------------------------------------------ #

class TestGetState:
    """get_state() のテスト。"""

    def test_get_state_reads_olat_register(self, relay):
        """get_state() は OLAT レジスタを読む。"""
        relay._bus.read_byte_data.return_value = 0xAB
        result = relay.get_state()
        relay._bus.read_byte_data.assert_called_once_with(0x20, MCP23008Relay.OLAT)
        assert result == 0xAB

    def test_get_relay_ch1_on(self, relay):
        """ch1 ON 状態のビットが立っているとき get_relay(1) が True を返す。"""
        relay._bus.read_byte_data.return_value = 0x80
        assert relay.get_relay(1) is True

    def test_get_relay_ch1_off(self, relay):
        """ch1 の bit が立っていないとき get_relay(1) が False を返す。"""
        relay._bus.read_byte_data.return_value = 0x00
        assert relay.get_relay(1) is False

    def test_get_relay_ch8_on(self, relay):
        """ch8 ON 状態のビットが立っているとき get_relay(8) が True を返す。"""
        relay._bus.read_byte_data.return_value = 0x01
        assert relay.get_relay(8) is True


# ------------------------------------------------------------------ #
# set_all
# ------------------------------------------------------------------ #

class TestSetAll:
    """set_all() のテスト。"""

    def test_set_all_writes_bitmask(self, relay):
        """set_all(0b10000001) → OLAT=0x81 を書き込む。"""
        relay.set_all(0b10000001)
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0x81)
        assert relay._olat == 0x81

    def test_set_all_masks_to_8bit(self, relay):
        """9bit以上の値は下位8bitにマスクされる。"""
        relay.set_all(0x1FF)  # 0x1FF & 0xFF = 0xFF
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0xFF)

    def test_all_off(self, relay):
        """all_off() → OLAT=0x00 を書き込む。"""
        relay._olat = 0xFF
        relay.all_off()
        relay._bus.write_byte_data.assert_called_with(0x20, MCP23008Relay.OLAT, 0x00)
        assert relay._olat == 0x00


# ------------------------------------------------------------------ #
# context manager
# ------------------------------------------------------------------ #

class TestContextManager:
    """コンテキストマネージャのテスト。"""

    def test_with_statement_calls_close(self, mock_bus):
        """with 文を抜けると close() が呼ばれる。"""
        with patch("agriha.daemon.i2c_relay.smbus2") as mock_smbus2:
            mock_smbus2.SMBus.return_value = mock_bus
            with MCP23008Relay(bus_num=1, addr=0x20) as relay:
                relay._bus = mock_bus
            mock_bus.close.assert_called_once()
