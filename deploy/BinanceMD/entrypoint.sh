#!/bin/sh
set -e

# Implement the config.json symlink invariant.
#
# The binary always reads ./config/config.json relative to its CWD (/app).
# The ENV var selects the config variant:
#
#   ENV=prod  (default) → config/config.prod.json
#   ENV=dev             → config/config.dev.json
#
# Config files are expected to be present in /app/config/ (bind-mounted or
# baked into the image).

CONFIG_VARIANT="${ENV:-prod}"
CONFIG_FILE="/app/config/config.${CONFIG_VARIANT}.json"

if [ ! -f "${CONFIG_FILE}" ]; then
    echo "ERROR: config file not found: ${CONFIG_FILE}" >&2
    exit 1
fi

ln -sf "${CONFIG_FILE}" /app/config/config.json

exec /app/BinanceMD "$@"
