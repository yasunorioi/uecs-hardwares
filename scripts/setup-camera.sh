#!/bin/bash
# カメラセットアップ（オプション）
# Usage: sudo bash scripts/setup-camera.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# agrihaユーザーをvideoグループに追加
usermod -aG video agriha

# 写真ディレクトリ作成
mkdir -p /var/lib/agriha/photos
chown agriha:agriha /var/lib/agriha/photos
ln -sf /var/lib/agriha/photos /var/lib/agriha/pictures

# agriha-capture.sh をデプロイ
cp "$SCRIPT_DIR/scripts/agriha-capture.sh" /usr/local/bin/agriha-capture.sh
chmod +x /usr/local/bin/agriha-capture.sh

# カメラcronエントリ追加（/etc/cron.d/agriha-camera）
cat > /etc/cron.d/agriha-camera <<'CRON'
*/5 * * * * agriha /usr/local/bin/agriha-capture.sh >> /var/log/agriha/capture.log 2>&1
CRON
chmod 644 /etc/cron.d/agriha-camera

echo "カメラセットアップ完了"
