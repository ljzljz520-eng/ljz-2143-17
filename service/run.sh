#!/usr/bin/env bash
# 启动输入策略 HTTP 服务 + 网页配置台
set -euo pipefail
cd "$(dirname "$0")/.."
DB="${INPUT_STRATEGY_DB:-service/data/strategy.db}"
HOST="${INPUT_STRATEGY_HOST:-0.0.0.0}"
PORT="${INPUT_STRATEGY_PORT:-8080}"
DEVICE="${INPUT_STRATEGY_DEVICE:-dev-001}"
exec python3 -m service.server --db "$DB" --host "$HOST" --port "$PORT" --seed-device "$DEVICE"
