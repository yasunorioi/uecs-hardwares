"""Tests for gpio_watch.py using gpiod mock.

gpiod が未インストール環境でも実行できるよう、
unittest.mock で gpiod をモックする。
"""

from __future__ import annotations

import asyncio
import sys
from unittest.mock import MagicMock, call, patch

import pytest

# gpiod を mock として登録 (既に登録済みの場合は既存 mock を使用)
# ※ test_emergency.py が先に実行されると sys.modules["gpiod"] が既に存在する場合があるため、
#    sys.modules.setdefault ではなく取得/作成を明示的に行う
if "gpiod" not in sys.modules:
    sys.modules["gpiod"] = MagicMock()

# gpio_watch が使用している gpiod mock に sentinel 値を設定
# (どのテスト順序でも gpio_watch.gpiod と同一オブジェクトになる)
_gpiod_used = sys.modules["gpiod"]
_FALLING = object()
_RISING = object()
_INACTIVE = object()
_gpiod_used.EdgeEvent.Type.FALLING_EDGE = _FALLING
_gpiod_used.EdgeEvent.Type.RISING_EDGE = _RISING
_gpiod_used.line.Value.INACTIVE = _INACTIVE

from agriha.daemon.gpio_watch import (  # noqa: E402
    DI_GPIO_MAP,
    GPIO_DI_MAP,
    GPIOEvent,
    GPIOWatcher,
    GPIOD_AVAILABLE,
)


# ------------------------------------------------------------------ #
# DI ↔ GPIO マッピング
# ------------------------------------------------------------------ #

class TestDIGPIOMappings:
    """DI ピン ↔ GPIO line offset マッピングのテスト。"""

    def test_di07_maps_to_gpio11(self):
        assert DI_GPIO_MAP[7] == 11

    def test_di08_maps_to_gpio7(self):
        assert DI_GPIO_MAP[8] == 7

    def test_di09_maps_to_gpio8(self):
        assert DI_GPIO_MAP[9] == 8

    def test_di10_maps_to_gpio9(self):
        assert DI_GPIO_MAP[10] == 9

    def test_di11_maps_to_gpio25(self):
        assert DI_GPIO_MAP[11] == 25

    def test_di12_maps_to_gpio10(self):
        assert DI_GPIO_MAP[12] == 10

    def test_di13_maps_to_gpio31(self):
        assert DI_GPIO_MAP[13] == 31

    def test_di14_maps_to_gpio30(self):
        assert DI_GPIO_MAP[14] == 30

    def test_all_8_pins_mapped(self):
        """DI07-DI14 の8ピンが全てマッピングされていること。"""
        assert set(DI_GPIO_MAP.keys()) == {7, 8, 9, 10, 11, 12, 13, 14}

    def test_reverse_map_covers_all_gpio_lines(self):
        """GPIO_DI_MAP が DI_GPIO_MAP の逆引きを完全にカバーすること。"""
        assert set(GPIO_DI_MAP.keys()) == set(DI_GPIO_MAP.values())

    def test_reverse_map_di07_from_gpio11(self):
        assert GPIO_DI_MAP[11] == 7

    def test_reverse_map_di14_from_gpio30(self):
        assert GPIO_DI_MAP[30] == 14


# ------------------------------------------------------------------ #
# GPIOEvent データクラス
# ------------------------------------------------------------------ #

class TestGPIOEvent:
    """GPIOEvent データクラスのテスト。"""

    def test_fields(self):
        event = GPIOEvent(di_pin=7, gpio_line=11, value=1, timestamp_ns=123456789)
        assert event.di_pin == 7
        assert event.gpio_line == 11
        assert event.value == 1
        assert event.timestamp_ns == 123456789

    def test_value_0_for_off(self):
        event = GPIOEvent(di_pin=8, gpio_line=7, value=0, timestamp_ns=0)
        assert event.value == 0


# ------------------------------------------------------------------ #
# GPIOWatcher ユニットテスト
# ------------------------------------------------------------------ #

class TestGPIOWatcher:
    """GPIOWatcher の open/close/read_events/get_value テスト。"""

    @pytest.fixture
    def mock_request(self):
        req = MagicMock()
        req.fd = 5
        return req

    @pytest.fixture
    def watcher_open(self, mock_request):
        """_request を直接セットした GPIOWatcher を返す。"""
        watcher = GPIOWatcher()
        watcher._request = mock_request
        return watcher

    # --- open ---

    def test_open_creates_chip_with_chip_path(self):
        """open() が指定した chip_path で gpiod.Chip を生成すること。"""
        with patch("agriha.daemon.gpio_watch.gpiod") as mock_gpiod:
            mock_gpiod.Chip.return_value = MagicMock()
            watcher = GPIOWatcher(chip_path="/dev/gpiochip0", di_pins=[7])
            watcher.open()
            mock_gpiod.Chip.assert_called_once_with("/dev/gpiochip0")

    def test_open_requests_lines_for_all_di_pins(self):
        """open() が指定した全 DI ピンの GPIO line を request_lines に渡すこと。"""
        with patch("agriha.daemon.gpio_watch.gpiod") as mock_gpiod:
            mock_chip = MagicMock()
            mock_gpiod.Chip.return_value = mock_chip
            watcher = GPIOWatcher(di_pins=[7, 8, 9, 10, 11, 12, 13, 14])
            watcher.open()
            call_kwargs = mock_chip.request_lines.call_args.kwargs
            config = call_kwargs["config"]
            # 8ピン分の line config が渡されること
            assert len(config) == 8
            # DI07→GPIO11, DI08→GPIO7 などが含まれること
            assert 11 in config  # DI07
            assert 7 in config   # DI08
            assert 30 in config  # DI14

    def test_open_sets_chip_and_request(self):
        """open() 後に _chip と _request が設定されること。"""
        with patch("agriha.daemon.gpio_watch.gpiod") as mock_gpiod:
            mock_chip = MagicMock()
            mock_gpiod.Chip.return_value = mock_chip
            watcher = GPIOWatcher(di_pins=[7])
            watcher.open()
            assert watcher._chip is mock_chip
            assert watcher._request is mock_chip.request_lines.return_value

    # --- fd ---

    def test_fd_returns_request_fd(self, watcher_open, mock_request):
        """fd プロパティが _request.fd を返すこと。"""
        assert watcher_open.fd == 5

    def test_fd_raises_if_not_open(self):
        """open() 前に fd を参照すると RuntimeError。"""
        watcher = GPIOWatcher()
        with pytest.raises(RuntimeError):
            _ = watcher.fd

    # --- close ---

    def test_close_releases_request_and_closes_chip(self, watcher_open, mock_request):
        """close() が request.release() と chip.close() を呼ぶこと。"""
        mock_chip = MagicMock()
        watcher_open._chip = mock_chip
        watcher_open.close()
        mock_request.release.assert_called_once()
        mock_chip.close.assert_called_once()

    def test_close_clears_references(self, watcher_open):
        """close() 後に _chip と _request が None になること。"""
        watcher_open._chip = MagicMock()
        watcher_open.close()
        assert watcher_open._request is None
        assert watcher_open._chip is None

    # --- read_events ---

    def _make_raw_event(self, line_offset: int, event_type: object, ts: int = 1000) -> MagicMock:
        raw = MagicMock()
        raw.line_offset = line_offset
        raw.event_type = event_type
        raw.timestamp_ns = ts
        return raw

    def test_read_events_falling_edge_returns_value_1(self, watcher_open, mock_request):
        """FALLING_EDGE (スイッチ ON) → value=1 の GPIOEvent が返ること。"""
        raw = self._make_raw_event(line_offset=11, event_type=_FALLING, ts=999)
        mock_request.read_edge_events.return_value = [raw]

        events = watcher_open.read_events()
        assert len(events) == 1
        assert events[0].di_pin == 7
        assert events[0].gpio_line == 11
        assert events[0].value == 1
        assert events[0].timestamp_ns == 999

    def test_read_events_rising_edge_returns_value_0(self, watcher_open, mock_request):
        """RISING_EDGE (スイッチ OFF) → value=0 の GPIOEvent が返ること。"""
        raw = self._make_raw_event(line_offset=11, event_type=_RISING)
        mock_request.read_edge_events.return_value = [raw]

        events = watcher_open.read_events()
        assert len(events) == 1
        assert events[0].value == 0

    def test_read_events_unknown_line_ignored(self, watcher_open, mock_request):
        """マッピングにない GPIO line のイベントは無視されること。"""
        raw = self._make_raw_event(line_offset=99, event_type=_FALLING)
        mock_request.read_edge_events.return_value = [raw]

        events = watcher_open.read_events()
        assert len(events) == 0

    def test_read_events_multiple(self, watcher_open, mock_request):
        """複数イベントがまとめて返ること。"""
        raws = [
            self._make_raw_event(11, _FALLING),   # DI07 ON
            self._make_raw_event(7, _RISING),      # DI08 OFF
            self._make_raw_event(30, _FALLING),    # DI14 ON
        ]
        mock_request.read_edge_events.return_value = raws

        events = watcher_open.read_events()
        assert len(events) == 3
        assert events[0].di_pin == 7
        assert events[0].value == 1
        assert events[1].di_pin == 8
        assert events[1].value == 0
        assert events[2].di_pin == 14
        assert events[2].value == 1

    def test_read_events_returns_empty_if_not_open(self):
        """open() していない場合は空リストを返すこと。"""
        watcher = GPIOWatcher()
        assert watcher.read_events() == []

    # --- get_value ---

    def test_get_value_inactive_returns_1(self, watcher_open, mock_request):
        """GPIO INACTIVE (LOW, スイッチ ON) → value=1。"""
        mock_request.get_value.return_value = _INACTIVE
        assert watcher_open.get_value(7) == 1

    def test_get_value_active_returns_0(self, watcher_open, mock_request):
        """GPIO HIGH (スイッチ OFF) → value=0。"""
        # INACTIVE 以外の値は Active (HIGH) とみなす
        mock_request.get_value.return_value = MagicMock()  # != _INACTIVE
        assert watcher_open.get_value(7) == 0

    def test_get_value_invalid_pin_raises(self, watcher_open):
        """未知の DI ピンは ValueError。"""
        with pytest.raises(ValueError):
            watcher_open.get_value(99)


# ------------------------------------------------------------------ #
# asyncio watch() 統合テスト
# ------------------------------------------------------------------ #

class TestGPIOWatcherAsync:
    """watch() の asyncio 統合テスト。"""

    def test_watch_registers_reader_and_cancels_cleanly(self):
        """watch() が add_reader を呼び、CancelledError でクリーンアップされること。"""
        async def _run():
            with patch("agriha.daemon.gpio_watch.gpiod") as mock_gpiod:
                mock_chip = MagicMock()
                mock_req = MagicMock()
                mock_req.fd = 42
                mock_gpiod.Chip.return_value = mock_chip
                mock_chip.request_lines.return_value = mock_req

                callback = MagicMock()
                watcher = GPIOWatcher(di_pins=[7], callback=callback)

                loop = asyncio.get_running_loop()
                add_reader_calls: list = []
                remove_reader_calls: list = []

                # 実際の epoll には登録しない (fd=42 は OS にとって無効) ため side_effect のみ記録
                with (
                    patch.object(loop, "add_reader",
                                 side_effect=lambda fd, cb: add_reader_calls.append(fd)),
                    patch.object(loop, "remove_reader",
                                 side_effect=lambda fd: remove_reader_calls.append(fd)),
                ):
                    task = asyncio.create_task(watcher.watch(loop))
                    await asyncio.sleep(0)  # watch() が open() + add_reader() を実行するまで待つ

                    assert 42 in add_reader_calls

                    task.cancel()
                    with pytest.raises(asyncio.CancelledError):
                        await task

                    # キャンセル後に remove_reader が呼ばれること
                    assert 42 in remove_reader_calls
                    # close() が呼ばれること
                    mock_req.release.assert_called_once()

        asyncio.run(_run())

    def test_watch_calls_callback_on_readable(self):
        """fd が readable になったとき callback が呼ばれること。"""
        async def _run():
            with patch("agriha.daemon.gpio_watch.gpiod") as mock_gpiod:
                mock_chip = MagicMock()
                mock_req = MagicMock()
                mock_req.fd = 10
                mock_gpiod.Chip.return_value = mock_chip
                mock_chip.request_lines.return_value = mock_req
                mock_gpiod.EdgeEvent.Type.FALLING_EDGE = _FALLING

                raw_ev = MagicMock()
                raw_ev.line_offset = 11  # GPIO11 = DI07
                raw_ev.event_type = _FALLING
                raw_ev.timestamp_ns = 1000
                mock_req.read_edge_events.return_value = [raw_ev]

                callback = MagicMock()
                watcher = GPIOWatcher(di_pins=[7], callback=callback)

                loop = asyncio.get_running_loop()
                _registered_cb: list = []

                with (
                    patch.object(loop, "add_reader",
                                 side_effect=lambda fd, cb: _registered_cb.append(cb)),
                    patch.object(loop, "remove_reader"),
                ):
                    task = asyncio.create_task(watcher.watch(loop))
                    await asyncio.sleep(0)

                    # 登録されたコールバックを手動で呼び出す
                    assert len(_registered_cb) == 1
                    _registered_cb[0]()

                    # callback に GPIOEvent が渡されること
                    callback.assert_called_once()
                    ev = callback.call_args[0][0]
                    assert ev.di_pin == 7
                    assert ev.value == 1

                    task.cancel()
                    try:
                        await task
                    except asyncio.CancelledError:
                        pass

        asyncio.run(_run())
