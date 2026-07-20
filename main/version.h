#pragma once

// Single source of truth for the firmware version (SemVer). Logged at boot
// and shown in the web UI footer and /api/status, so a running device
// always says what's actually flashed.
//
// Auto-bumped by semantic-release (tools/bump_version.sh via
// .releaserc.json) on every push to main — don't hand-edit this.
#define FW_VERSION "0.2.0"
