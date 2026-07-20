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
- **Keys:** `remote_key_inject{key_code, direction}`; keycode names/numbers are the `RemoteKeyCode` enum in `remotemessage.proto`. `SHORT` for taps; `START_LONG`/`END_LONG` press-release pair for press-and-hold auto-repeat. `POST /api/key` takes an optional `direction`; the web UI uses pointerdown/up for d-pad holds.
- **Volume does NOT work on this Chromecast HD via this protocol — confirmed with the reference Python library itself** (its `volume_info` reports `max: 0`): the device delegates volume to the TV over HDMI-CEC and its physical remote uses IR, a path this protocol doesn't have. No key/direction combination changes volume; do not chase this as a firmware bug again. `remote_set_volume_level.volume_max == 0` is the runtime signal; the web UI greys volume buttons when it sees it. Also: **rapid `START_LONG`/`END_LONG` pairs make the Chromecast close the control connection** (first seen with mute, then with fast d-pad tapping). Taps must send a single `SHORT`; send the long pair only for a genuine hold (web UI uses a 300 ms threshold before `START_LONG`).
- **Keepalive reality:** the Chromecast HD does NOT ping every 5 s as the reference assumes — it can stay silent >16 s. Never use a fixed idle disconnect; probe with our own `remote_ping_request` after ~10 s of silence and reconnect only if the reply doesn't arrive (implemented in `remote.c`).
- Both TLS connections: present our self-signed RSA-2048 client cert (`certs/`), skip server verification. Same cert for pairing and control — regenerating it breaks the TV's trust.

### Samsung TV fallback (volume)

Since volume can't be controlled via the Android TV Remote protocol on this hardware, `main/samsung_tv.c` talks directly to a Samsung Tizen TV's own local remote-control API as a fallback — verified against `xchwarze/samsung-tv-ws-api` (the library behind Home Assistant's `samsungtv` integration):

- **Endpoint:** `wss://<CONFIG_ATV_SAMSUNG_TV_IP>:8002/api/v2/channels/samsung.remote.control?name=<base64 app name>`, TLS with server-cert verification skipped (`skip_cert_common_name_check=true`, no CA configured — same trust posture as the Chromecast's cert in `net_tls.c`). `name` is base64 of a display string shown on the TV's pairing prompt; this firmware hardcodes `RVNQMzIgUmVtb3Rl` = `base64("ESP32 Remote")`. **Confirmed on hardware (2020 Q60T, Tizen):** the plain `ws://:8001` variant that some references describe as simpler does *not* work here — the TV replies instantly with `{"event":"ms.channel.unauthorized"}` and closes the socket without ever showing the on-screen prompt. Only `wss://:8002` actually triggers the prompt on this firmware.
- **First connection:** the TV shows an on-screen "Allow this device?" prompt. On approval it sends `{"event":"ms.channel.connect","data":{"token":"..."}}`; save the token (NVS, `store_get/set_samsung_token`) and append `&token=<token>` to the URL on future connections to skip the prompt.
- **Key press:** `{"method":"ms.remote.control","params":{"Cmd":"Click","DataOfCmd":"KEY_VOLUP","Option":"false","TypeOfRemote":"SendRemoteKey"}}` (`DataOfCmd` is `KEY_VOLUP`/`KEY_VOLDOWN`/`KEY_MUTE`). Click-only — no `START_LONG`/`END_LONG` equivalent, so a held volume button steps once per press rather than auto-repeating.
- Routing lives in `webserver.c:key_post_handler`: a `VOLUME_UP`/`VOLUME_DOWN`/`VOLUME_MUTE` key is sent to the Samsung TV instead of the Chromecast only when `g_atv_status.vol_max == 0` **and** `g_samsung_status.connected` — everything else is unaffected.
- Unlike `net_tls.c`, `esp_websocket_client_send_text()` is safe to call from any task (library-internal locking), so the HTTP handler calls it directly — no dedicated writer task/queue for this path.
- Empty `CONFIG_ATV_SAMSUNG_TV_IP` (Kconfig) disables the feature entirely; `samsung_tv_init()` no-ops.

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
- [x] Phase 4 — pairing state machine + secret (live-paired with the Chromecast HD via the web form). NOTE: if `tools/gen_cert.sh` ever regenerates the cert, rerun `test/gen_pairing_oracle.py` — `test/golden_pairing.inc` is derived from the local client cert.
- [x] Phase 5 — control channel + keepalive (verified: stays connected, auto-reconnects)
- [x] Phase 6 — key injection (verified: web buttons drive the TV)
- [x] Phase 7 — web UI polish (SVG icon remote layout, Netflix/YouTube via `POST /api/app`; verified on phone)
- [x] Phase 8 — robustness (reconnect w/ backoff ≤15 s, ping-probe keepalive, WiFi PS off for latency, command requeue across reconnects, LWIP sockets 16, LRU purge, immediate WiFi retry)
