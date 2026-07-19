# ESP32 Android TV Remote

ESP-IDF firmware (built with PlatformIO) that pairs with and controls a Chromecast with Google TV over the Android TV Remote protocol v2, exposing a browser-based remote UI on port 80. Full spec: `PLAN.md` — read it before large changes; it defines the phases, risks, and acceptance tests.

## Build / flash / monitor

```
pio run                 # build
pio run -t upload       # flash
pio run -t monitor      # serial log (115200)
pio run -t menuconfig   # set WiFi SSID/password (menu "Android TV Remote")
```

WiFi credentials live in Kconfig (`ATV_WIFI_SSID` / `ATV_WIFI_PASSWORD`, set via menuconfig → stored in `sdkconfig.esp32doit-devkit-v1`, gitignored local state).

Target board: DOIT ESP32 DevKit v1 (`esp32doit-devkit-v1`, WROOM-32, 4 MB flash, no PSRAM).

## Layout

- `main/` — app sources (one `.c/.h` pair per subsystem, per PLAN.md §layout)
- `main/proto_gen/` — **generated** nanopb code; never edit by hand. Regenerate with `tools/gen_proto.sh` (needs `pip install nanopb==0.4.9.1`; generator version must match the runtime in `components/nanopb/`).
- `components/nanopb/` — vendored nanopb 0.4.9.1 runtime (pb_common/pb_encode/pb_decode).
- `proto/` — `.proto` + `.options` files copied verbatim from `tronikos/androidtvremote2`. Do not edit the `.proto` files; tune sizes only in `.options`.
- `web/index.html` — the single-page remote UI, embedded into the binary.

## Protocol ground truth

Reference checkout: clone `https://github.com/tronikos/androidtvremote2`; the files that matter are `src/androidtvremote2/{pairing,remote,base}.py` and the two `.proto` files. **The Python source wins over any prose, including this file.**

Facts already verified against `pairing.py` / `remote.py` (v2 protocol):

- **Framing** (both ports): varint message length, then the protobuf bytes. Handle partial TCP reads.
- **Pairing (port 6467, `OuterMessage`, always `protocol_version=2`, `status=STATUS_OK=200`):**
  1. send `pairing_request` (service_name=`"atvremote"`, client_name arbitrary) → recv `pairing_request_ack`
  2. send `options` (preferred_role=ROLE_TYPE_INPUT, one input_encoding {HEXADECIMAL, symbol_length 6}) → recv `options`
  3. send `configuration` (client_role=INPUT, same encoding) → recv `configuration_ack` → TV now shows the code
  4. user submits 6-hex-digit code → compute secret → send `secret` → recv `secret_ack` = paired
- **Pairing secret** (`pairing.py:async_finish_pairing`): `SHA256(client_N ++ client_E ++ server_N ++ server_E ++ nonce)` where each modulus/exponent is the byte sequence of Python `bytes.fromhex(f"{n:X}")` — i.e. **minimal big-endian, no leading zero, but the exponent is formatted as `f"0{e:X}"`** (65537 → `010001`, 3 bytes). Nonce = hex-decode of code chars [2:6] (2 bytes). Verify `hash[0] == int(code[0:2],16)` before sending. Server cert comes from the live TLS session (DER), client cert is our own.
- **Control (port 6466, `RemoteMessage`):** TV sends `remote_configure` first → reply with our `remote_configure` (`code1` = active feature bits, device_info{unknown1=1, unknown2="1", package_name="atvremote", app_version="1.0.0"}). TV sends `remote_set_active` → reply `remote_set_active{active=features}`. `remote_start.started` signals ready. Answer every `remote_ping_request` with `remote_ping_response{val1=req.val1}` (TV pings ~5 s; 3 misses = disconnect; reference disconnects itself after 16 s idle).
- **Keys:** `remote_key_inject{key_code, direction=SHORT}`; keycode names/numbers are the `RemoteKeyCode` enum in `remotemessage.proto`.
- Both TLS connections: present our self-signed RSA-2048 client cert (`certs/`), skip server verification. Same cert for pairing and control — regenerating it breaks the TV's trust.

## Constraints & conventions

- Only the TV-session FreeRTOS task writes to the TLS socket. HTTP handlers post commands to its queue (PLAN.md §4.5).
- No TLS/auth on the port-80 web UI — trusted-LAN only, by design.
- `RemoteVoicePayload.samples` and `RemoteError.message` are nanopb callbacks (unused; voice is out of scope on ESP32).
- Before implementing the pairing secret in C, build the Python hash oracle (PLAN.md §5) and unit-test against it.

## Phase status

- [x] Phase 0 — scaffolding (PlatformIO + espidf, nanopb vendored, codegen, WiFi bring-up)
- [x] Phase 1 — mutual TLS to :6467 (verified against the real Chromecast HD)
- [x] Phase 2 — protobuf framing (host tests: `tools/test_host.sh`, golden bytes from reference lib in `test/`)
- [x] Phase 3 — web server bring-up (+mDNS, verified from LAN via androidtv-remote.local)
- [ ] Phase 4 — pairing state machine + secret (code + oracle selftest done; live pairing pending). NOTE: if `tools/gen_cert.sh` ever regenerates the cert, rerun `test/gen_pairing_oracle.py` — `test/golden_pairing.inc` is derived from the local client cert.
- [ ] Phase 5 — control channel + keepalive
- [ ] Phase 6 — key injection
- [ ] Phase 7 — web UI polish
- [ ] Phase 8 — robustness
