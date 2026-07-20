#!/usr/bin/env bash
# Rewrites FW_VERSION in main/version.h. Called by semantic-release's exec
# plugin (see .releaserc.json) during the "prepare" step, before it commits
# and tags — not meant to be run by hand except to test the release flow.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <version>" >&2
    exit 1
fi

new_version="$1"
version_h="$(dirname "$0")/../main/version.h"

sed -i "s/#define FW_VERSION \".*\"/#define FW_VERSION \"${new_version}\"/" "$version_h"
grep -q "\"${new_version}\"" "$version_h"  # fail loudly if the sed didn't take
