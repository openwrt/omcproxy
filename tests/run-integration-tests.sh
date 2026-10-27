#!/bin/bash

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OMCPROXY="${1:-/src/build/omcproxy}"

"$SCRIPT_DIR/test-interface-lifecycle.sh" "$OMCPROXY"
"$SCRIPT_DIR/test-dataplane.sh" "$OMCPROXY"
