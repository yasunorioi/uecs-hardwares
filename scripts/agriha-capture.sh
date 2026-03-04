#!/bin/bash
# agriha-capture.sh — 定点カメラ撮影スクリプト
# Usage: /usr/local/bin/agriha-capture.sh
# cron: */5 * * * * agriha /usr/local/bin/agriha-capture.sh >> /var/log/agriha/capture.log 2>&1
set -euo pipefail

PHOTO_DIR="/var/lib/agriha/photos"
LATEST="${PHOTO_DIR}/latest.jpg"
ARCHIVE="${PHOTO_DIR}/$(date +%Y%m%d_%H%M%S).jpg"

mkdir -p "$PHOTO_DIR"

# Raspberry Pi カメラ撮影（rpicam-still）
rpicam-still --nopreview --output "$LATEST" --timeout 2000 --quality 85

# タイムスタンプ付きアーカイブ（直近24時間分: 288枚×5分 = 1440分）
cp "$LATEST" "$ARCHIVE"

# 古いアーカイブを削除（24時間超）
find "$PHOTO_DIR" -name "*.jpg" ! -name "latest.jpg" -mmin +1440 -delete

echo "$(date -Iseconds) 撮影完了: $LATEST"
