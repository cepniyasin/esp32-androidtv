#!/usr/bin/env bash
# Regenerate nanopb C sources from proto/*.proto into main/proto_gen/.
# Requires the nanopb generator (pip install nanopb==0.4.9.1) — must match
# the runtime version vendored in components/nanopb/.
set -euo pipefail
cd "$(dirname "$0")/.."

GEN="${NANOPB_GENERATOR:-nanopb_generator}"
OUT=main/proto_gen
mkdir -p "$OUT"
"$GEN" --proto-path=proto --output-dir="$OUT" polo.proto remotemessage.proto
echo "Generated:"
ls -la "$OUT"
