#!/bin/bash
# agriha-capture.sh — 定点カメラ撮影スクリプト
# Usage: /usr/local/bin/agriha-capture.sh
# cron: */5 * * * * agriha /usr/local/bin/agriha-capture.sh >> /var/log/agriha/capture.log 2>&1
set -euo pipefail

PHOTO_DIR="/var/lib/agriha/photos"
GROWTH_RT="/var/lib/agriha/growth_log/realtime"
LATEST="${PHOTO_DIR}/latest.jpg"
TS=$(date +%Y%m%d_%H%M%S)
ARCHIVE="${PHOTO_DIR}/${TS}.jpg"

mkdir -p "$PHOTO_DIR"
mkdir -p "$GROWTH_RT"

# Raspberry Pi カメラ撮影（rpicam-still）
rpicam-still --nopreview --output "$LATEST" --timeout 2000 --quality 85

# タイムスタンプ付きアーカイブ（直近24時間分: 288枚×5分 = 1440分）
cp "$LATEST" "$ARCHIVE"

# growth_log: 画像コピー + センサーJSON取得
cp "$LATEST" "${GROWTH_RT}/${TS}.jpg"
curl -sf http://localhost:8080/api/sensors \
  | python3 -c "
import sys, json
from datetime import datetime
raw = json.load(sys.stdin)
s = raw.get('sensors', {})
def ccm(t): v = s.get(f'agriha/h01/ccm/sensor/{t}', {}); return v.get('value')
misol = s.get('agriha/farm/weather/misol', {})
relay = s.get('agriha/h01/relay/state', {})
out = {
  'timestamp': datetime.now().astimezone().isoformat(),
  'unix_ts': int(datetime.now().timestamp()),
  'camera': {'resolution': '640x480', 'quality': 85},
  'sensors': {
    'in_air_temp': ccm('InAirTemp'), 'in_air_humid': ccm('InAirHumid'),
    'in_air_co2': ccm('InAirCO2'), 'in_radiation': ccm('InRadiation'),
    'intg_radiation': ccm('IntgRadiation'), 'soil_temp': ccm('SoilTemp'),
    'soil_ec': ccm('SoilEC'), 'soil_wc': ccm('SoilWC'), 'pulse': ccm('Pulse')
  },
  'weather': {
    'temperature_c': misol.get('temperature_c'), 'humidity_pct': misol.get('humidity_pct'),
    'light_lux': misol.get('light_lux'), 'rainfall_mm': misol.get('rainfall_mm'),
    'wind_speed_ms': misol.get('wind_speed_ms')
  },
  'relay': {
    'ch4_irrigation': relay.get('ch4', 0), 'ch1_heater': relay.get('ch1', 0),
    'ch5_north_win_open': relay.get('ch5', 0), 'ch8_south_win_open': relay.get('ch8', 0)
  }
}
json.dump(out, open('${GROWTH_RT}/${TS}.json', 'w'), ensure_ascii=False)
" 2>/dev/null || echo "$(date -Iseconds) WARN: sensor取得失敗（画像のみ保存）"

# 古いアーカイブを削除（24時間超）
find "$PHOTO_DIR" -name "*.jpg" ! -name "latest.jpg" -mmin +1440 -delete

echo "$(date -Iseconds) 撮影完了: $LATEST"
