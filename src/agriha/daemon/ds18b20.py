"""DS18B20 温度センサードライバ（Linux sysfs 1-Wire 経由）

DS2482-100 I2C-to-1-Wire ブリッジ（I2C 0x18）経由で接続された DS18B20 温度センサーを読み取る。

前提条件:
  /boot/config.txt に dtoverlay=ds2482 が設定済み
  Linux カーネルの w1_therm モジュールがロード済み

読み取り先:
  /sys/bus/w1/devices/{device_id}/temperature
  値は millidegrees Celsius (例: 24500 → 24.5°C)
"""

from __future__ import annotations

import glob
import logging
from pathlib import Path

logger = logging.getLogger(__name__)

DEFAULT_DEVICE_ID = "28-00000de13271"
W1_BASE = "/sys/bus/w1/devices"


class DS18B20Error(OSError):
    """DS18B20 読み取り失敗"""


class DS18B20:
    """DS18B20 Linux sysfs 1-Wire ドライバ。

    /sys/bus/w1/devices/{device_id}/temperature を読み取り、摂氏 (°C) で返す。
    """

    def __init__(
        self,
        device_id: str = DEFAULT_DEVICE_ID,
        base_path: str = W1_BASE,
    ) -> None:
        self.device_id = device_id
        self._temp_path = Path(base_path) / device_id / "temperature"

    def read_celsius(self) -> float:
        """温度を読み取り、摂氏 (float) で返す。

        Raises:
            DS18B20Error: デバイスが見つからない、または値が不正な場合
        """
        try:
            raw = self._temp_path.read_text().strip()
        except FileNotFoundError:
            raise DS18B20Error(f"デバイスが見つかりません: {self._temp_path}")
        except OSError as e:
            raise DS18B20Error(f"sysfs 読み取りエラー: {e}") from e

        try:
            return int(raw) / 1000.0
        except ValueError:
            raise DS18B20Error(f"不正な温度値: {raw!r}")

    @classmethod
    def discover(cls, base_path: str = W1_BASE) -> list[DS18B20]:
        """接続されている DS18B20 を全て検索して返す。

        /sys/bus/w1/devices/28-* パターンで検索し、
        temperature ファイルが存在するデバイスのみ返す。
        """
        found = []
        for device_dir in glob.glob(str(Path(base_path) / "28-*")):
            temp_path = Path(device_dir) / "temperature"
            if temp_path.exists():
                device_id = Path(device_dir).name
                found.append(cls(device_id=device_id, base_path=base_path))
        logger.debug("DS18B20 discover: %d devices in %s", len(found), base_path)
        return found
