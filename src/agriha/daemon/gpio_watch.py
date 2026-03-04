#!/usr/bin/env python3
"""
gpio_watch.py - gpiod v2 による GPIO edge detection モジュール

UniPi 1.1 の DI07-DI14 (デジタル入力) を監視し、
asyncio fd 統合でイベント駆動の検出を行う。

GPIO マッピング（UniPi 1.1 DI → gpiochip0 line offset）:
  DI07 → GPIO11
  DI08 → GPIO7
  DI09 → GPIO8
  DI10 → GPIO9
  DI11 → GPIO25
  DI12 → GPIO10
  DI13 → GPIO31
  DI14 → GPIO30

物理: DI ピンは pull-up 付き → 通常 HIGH、スイッチ ON で LOW (FALLING_EDGE)。
"""

import asyncio
import logging
from dataclasses import dataclass
from typing import Callable, Optional

try:
    import gpiod
    GPIOD_AVAILABLE = True
except ImportError:
    GPIOD_AVAILABLE = False

logger = logging.getLogger(__name__)

# DI ピン番号 → GPIO chip line offset マッピング
DI_GPIO_MAP: dict[int, int] = {
    7:  11,   # DI07 → GPIO11
    8:  7,    # DI08 → GPIO7
    9:  8,    # DI09 → GPIO8
    10: 9,    # DI10 → GPIO9
    11: 25,   # DI11 → GPIO25
    12: 10,   # DI12 → GPIO10
    13: 31,   # DI13 → GPIO31
    14: 30,   # DI14 → GPIO30
}

# 逆引き: GPIO line offset → DI ピン番号
GPIO_DI_MAP: dict[int, int] = {v: k for k, v in DI_GPIO_MAP.items()}


@dataclass
class GPIOEvent:
    """GPIO エッジイベント。"""
    di_pin: int       # DI07-DI14 のピン番号
    gpio_line: int    # gpiochip0 上の line offset
    value: int        # 1=スイッチ ON (FALLING_EDGE), 0=スイッチ OFF (RISING_EDGE)
    timestamp_ns: int # イベントタイムスタンプ (nanoseconds)


# コールバック型
GPIOCallback = Callable[[GPIOEvent], None]


class GPIOWatcher:
    """
    gpiod v2 を使った DI07-DI14 の edge detection ウォッチャー。

    asyncio loop.add_reader() でイベント駆動統合を行う。
    pull-up 付きの DI ピン:
      - スイッチ ON (FALLING_EDGE) → value=1
      - スイッチ OFF (RISING_EDGE) → value=0
    """

    def __init__(
        self,
        chip_path: str = "/dev/gpiochip0",
        di_pins: Optional[list[int]] = None,
        callback: Optional[GPIOCallback] = None,
    ) -> None:
        """
        Args:
            chip_path: GPIO チップデバイスパス
            di_pins:   監視する DI ピン番号リスト (None で全 DI07-DI14)
            callback:  GPIOEvent を受け取るコールバック関数
        """
        self.chip_path = chip_path
        self.di_pins = di_pins if di_pins is not None else list(DI_GPIO_MAP.keys())
        self.callback = callback
        self._chip: Optional["gpiod.Chip"] = None
        self._request: Optional["gpiod.LineRequest"] = None

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    def open(self) -> None:
        """GPIO チップを開き、line を要求する。"""
        if not GPIOD_AVAILABLE:
            raise RuntimeError("gpiod not installed. Run: pip install gpiod")

        self._chip = gpiod.Chip(self.chip_path)

        # line offset → LineSettings のマッピングを構築
        line_config: dict[int, "gpiod.LineSettings"] = {}
        for di_pin in self.di_pins:
            gpio_line = DI_GPIO_MAP.get(di_pin)
            if gpio_line is None:
                logger.warning(f"Unknown DI pin: DI{di_pin:02d}, skipping")
                continue
            line_config[gpio_line] = gpiod.LineSettings(
                direction=gpiod.line.Direction.INPUT,
                bias=gpiod.line.Bias.PULL_UP,
                edge_detection=gpiod.line.Edge.BOTH,
            )

        if not line_config:
            raise ValueError("No valid DI pins specified")

        self._request = self._chip.request_lines(
            config=line_config,
            consumer="unipi-daemon-gpio-watch",
        )
        logger.info(
            f"GPIO watch opened: {self.chip_path}, "
            f"DI pins={self.di_pins}, fd={self._request.fd}"
        )

    def close(self) -> None:
        """GPIO リソースを解放する。"""
        if self._request is not None:
            self._request.release()
            self._request = None
        if self._chip is not None:
            self._chip.close()
            self._chip = None
        logger.info("GPIO watch closed")

    @property
    def fd(self) -> int:
        """asyncio add_reader 用のファイルディスクリプタ。"""
        if self._request is None:
            raise RuntimeError("GPIOWatcher is not open")
        return self._request.fd

    def read_events(self) -> list[GPIOEvent]:
        """
        保留中の edge イベントを全て読み取り GPIOEvent リストで返す。

        pull-up 配線の変換:
          FALLING_EDGE (GPIO HIGH→LOW, スイッチ閉) → value=1
          RISING_EDGE  (GPIO LOW→HIGH, スイッチ開) → value=0
        """
        if self._request is None:
            return []

        events: list[GPIOEvent] = []
        for raw in self._request.read_edge_events():
            di_pin = GPIO_DI_MAP.get(raw.line_offset)
            if di_pin is None:
                continue
            # pull-up: FALLING = スイッチ ON
            if raw.event_type == gpiod.EdgeEvent.Type.FALLING_EDGE:
                value = 1
            else:
                value = 0
            events.append(GPIOEvent(
                di_pin=di_pin,
                gpio_line=raw.line_offset,
                value=value,
                timestamp_ns=raw.timestamp_ns,
            ))
        return events

    def get_value(self, di_pin: int) -> int:
        """
        指定した DI ピンの現在値を返す。

        Returns:
            1=スイッチ ON (LOW), 0=スイッチ OFF (HIGH)
        """
        if self._request is None:
            raise RuntimeError("GPIOWatcher is not open")
        gpio_line = DI_GPIO_MAP.get(di_pin)
        if gpio_line is None:
            raise ValueError(f"Unknown DI pin: {di_pin}")
        # pull-up: GPIO LOW → スイッチ ON → value 1
        raw_val = self._request.get_value(gpio_line)
        return 1 if raw_val == gpiod.line.Value.INACTIVE else 0

    # ------------------------------------------------------------------
    # asyncio 統合
    # ------------------------------------------------------------------

    async def watch(self, loop: Optional[asyncio.AbstractEventLoop] = None) -> None:
        """
        asyncio loop.add_reader() でイベント駆動 GPIO 監視を開始する。

        callback に GPIOEvent が渡される。
        キャンセルされるまで無限に動作する。
        """
        loop = loop or asyncio.get_running_loop()
        self.open()

        stop_event = asyncio.Event()

        def _on_readable() -> None:
            events = self.read_events()
            if self.callback:
                for event in events:
                    try:
                        self.callback(event)
                    except Exception as exc:
                        logger.error(f"GPIO callback error: {exc}", exc_info=True)

        loop.add_reader(self.fd, _on_readable)
        logger.info("GPIO watch started (asyncio add_reader)")

        try:
            await stop_event.wait()  # キャンセルされるまでここで待機
        except asyncio.CancelledError:
            logger.info("GPIO watch cancelled")
            raise
        finally:
            loop.remove_reader(self.fd)
            self.close()
