#!/bin/sh
# No shell state or dependency bootstrap is sourced from another owner's scripts.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$SCRIPT_DIR/macos-delivery.py" "$@"
