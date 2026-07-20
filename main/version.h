#pragma once

// Single source of truth for the firmware version (SemVer). Bump this on
// every release; it's logged at boot and shown in the web UI footer and
// /api/status, so a running device always says what's actually flashed.
#define FW_VERSION "0.1.0"
