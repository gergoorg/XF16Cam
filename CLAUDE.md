# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A fork of the **XRADIO Skylark SDK** (vendor C SDK for XR872/XR808 Cortex-M4
Wi-Fi SoCs) carrying one application: **XF16Cam**, network-camera firmware for
the 1 MiB-flash XR872ET/XF16 A9 camera board.

Everything XF16Cam-specific lives in `project/example/xf16cam/`. The rest of the
tree (`src/`, `include/`, `lib/`, `project/common/`, `chip.mk`, `gcc.mk`,
`config.mk`) is vendor SDK with its original BSD notices — **keep camera
behavior inside `project/example/xf16cam/` wherever possible** rather than
patching the SDK.

`project/example/xf16cam/readme.md` is the project guide (sensor list and
validation status, pin map, flash layout, power/battery plan). Read it before
touching sensors, GPIO, or the flash budget — but **it lags the code**: it was
last updated at `aaa71ca` and still describes single-client RTSP, with no
mention of automatic day/night switching, PTZ control, or capture-stall
recovery. Trust the source over the readme, and update the readme when you
change behavior it documents.

`FEATURES.md` at the repo root is a code-derived table of every UI, API, and
automatic behavior — useful as an index before changing the HTTP surface.

## Build

The only supported toolchain is **Arm GNU Toolchain 8-2019-q3**. There is no
lint step and no test runner beyond one host-compiled unit test.

Windows (Docker, produces `dist/`):

```bat
buildXF16Cam.bat ptz      REM or: no_ptz, ptz_talk, ptz_netlog, ptz_talk_netlog, ...
```

Linux/CI-equivalent, with `arm-none-eabi-gcc` on `PATH`:

```sh
printf '%s\n' '__CONFIG_CHIP_TYPE ?= xr872' '__CONFIG_HOSC_TYPE ?= 40' > .config
chmod +x tools/mkimage
make -C project/example/xf16cam/gcc \
  CC_DIR="$(dirname "$(command -v arm-none-eabi-gcc)")" \
  PRJ_EXTRA_SYMBOLS="" image      # add -DNO_PTZ for the fixed-camera board,
                                  # -DXF16CAM_TALK for the RTSP audio backchannel
make -C project/example/xf16cam/gcc \
  CC_DIR="$(dirname "$(command -v arm-none-eabi-gcc)")" \
  PRJ_EXTRA_SYMBOLS="" image_xz
```

`.config` is generated (`configure.sh` interactively, or the `printf` above) and
is not committed. Outputs land in `project/example/xf16cam/image/xr872/`:
`xr_system.img` (serial flashing) and `xr_system_img_xz.img` (web OTA). Never
upload the full serial image through the web updater.

`make clean` / `make image` from `project/example/xf16cam/gcc`. Build knobs that
matter live in `gcc/localconfig.mk` (XIP on, JPEG on, PSRAM off, OTA policy) and
`prj_config.h`.

Unused SDK features are switched off **without editing the SDK**, so the SDK
tree can be updated without losing anything:

- `prj_config.h` sets `XF16CAM_LWIP_DNS_EN` and `XF16CAM_LWIP_IGMP_EN` to 0.
  lwIP itself is still compiled with both on; `xf16cam_lwip_stubs.c` defines
  every symbol the image imports from `dns.o` and `igmp.o`, so the linker never
  extracts those members and their code, strings and buffers drop out. Set one
  to 1 to link the real code back in with no library rebuild: DNS is needed to
  reach a broker or NTP server by hostname, IGMP to be found by ONVIF
  WS-Discovery or mDNS, or to receive multicast. A future lwIP that imports an
  unstubbed symbol fails the link loudly with duplicate definitions; one that
  renames them is caught by the CI sentinels below.
- **Power management stays compiled in on both variants**, although PTZ boards
  never hibernate. `__CONFIG_PM := n` looked like a free 4 KB of app slot on
  `ptz`, but `hal_flashctrl.c` keys its SBUS re-initialisation workaround
  (`FLASHC_TEMP_FIXED`) to `CONFIG_PM`: without it the flash controller is
  not re-initialised before programmatic access, and the first flash write
  after boot — an OTA piece or a settings save — hangs the device. A `ptz`
  image built that way (v0.17.17) could no longer install updates and had to
  be serial-flashed. CI requires `flashc_suspend`, the hook that exists only
  when that code is compiled, in SRAM on both variants so this is not repeated. The hibernate route, button and function are still
  `#ifdef NO_PTZ`; that part costs nothing and is purely about not exposing a
  control the board cannot honour.

### Test

One host test, run before the firmware build in both CI and the Dockerfile:

```sh
cc -std=c11 -Wall -Wextra -Werror -Iinclude -Iproject/example/xf16cam \
  tests/xf16cam/test_rtsp_parser.c project/example/xf16cam/xf16cam_rtsp_parser.c \
  -o /tmp/xf16cam-rtsp-parser-test && /tmp/xf16cam-rtsp-parser-test
```

The RTSP request parser is deliberately factored out of `main.c` into
`xf16cam_rtsp_parser.c` so it can be compiled and tested on the host. Keep new
pure-logic code testable the same way.

### Gates CI enforces (run these locally before claiming a change fits)

```sh
python3 tools/xf16cam/check_symbol_placement.py \
  --elf project/example/xf16cam/gcc/xf16cam.axf \
  --require-sram xf16cam_http_flash_info --require-sram xf16cam_http_ota \
  --require-sram flashc_suspend --require-xip xf16cam_http_start \
  --require-absent dns_table --require-absent igmp_group_list

python3 tools/xf16cam/check_image_budget.py \
  --config project/example/xf16cam/image/xr872/image_auto_cal.cfg \
  --image-dir project/example/xf16cam/image/xr872
```

Budget minimums: 8 KiB free in the SRAM-loaded app slot, 64 KiB free in each of
the XIP and compressed-OTA areas. The placement check exists because code that
queries or writes flash must not be executing *from* flash — that is why
`xf16cam_http_flash_info` and `xf16cam_http_ota` must stay in SRAM and uninlined.
The absent sentinels prove the lwIP link-out still takes effect after an SDK
update. `flashc_suspend`, the flash controller's PM hook, must be in SRAM on
both variants: power management cannot be compiled out (see Build).

The variant name is `ptz` or `no_ptz` plus optional suffixes, in this order:
`_talk`, then `_netlog`. CI builds `ptz`, `no_ptz`, `ptz_talk`, `no_ptz_talk`
and `ptz_talk_netlog`; a change must compile under all of them.

`XF16CAM_TALK` (the `_talk` suffix) adds speaker playback for the RTSP audio
backchannel. It is opt-in because it costs flash and because it swaps the
board config: the SDK's evaluation-board file drives PA23 high as a
speaker-amplifier enable, but on the XF16 PA23 is the camera/SD rail and the
amplifier enable is **PB21, active low**. That pin is not on any pin map; it
was read out of the factory (Z-IoT) firmware, whose "talk open spk" routine
writes PB21 low and "talk close spk" writes it high (`HAL_GPIO_WritePin` is
in ROM, so the call is unambiguous in a disassembly). Talk builds therefore
link `project/example/xf16cam/board/`, a copy of
`project/common/board/xr872_evb_ai` whose only change is that PA-switch
entry (`GPIO_PORT_B`, `GPIO_PIN_21`, `on_state = GPIO_PIN_LOW`) on the
internal codec; the SDK sound card then drives the pin around every
`PCM_OUT` open and close and never for capture. The Makefile picks the copy
when `PRJ_EXTRA_SYMBOLS` contains `XF16CAM_TALK`, and non-talk builds keep
the SDK file. Re-copy and re-apply that one change when the SDK board config
is updated. The codec itself was verified to be blameless before that pin was
found: the DAC FIFO counter, a register dump against the XR872 user manual
and the line-out-to-line-in loopback all checked out with the speaker still
silent, so do not go back down that path if the speaker ever goes quiet
again; check PB21 first.

`XF16CAM_NETLOG` (the `_netlog` suffix) compiles in the console mirror:
`xf16cam_log.c` installs its own libc stdout writer, so every `printf` in the
system still reaches the UART and is also kept in a 2 KiB RAM ring that the
board task broadcasts over UDP port 5514 and `GET /api/log` returns as text.
It exists because the board has no serial connection without disassembly;
`tools/xf16cam/udplog.py` is the PC side. The writer itself must stay in SRAM
(no `__xip_text`), since printf runs while flash is disabled during OTA and
settings writes. Nothing survives a reset, so every deliberate reboot calls
`xf16cam_log_flush()` first and the reason it printed leaves the board; a
hard fault does not reboot at all (the ROM handler halts, and the hardware
watchdog is not enabled), so a silent hang with no boot marker is a crash.
Measured cost on
`ptz`: 120 bytes of app slot, 376 bytes of XIP, plus the 2 KiB ring in `.bss`.

## Memory discipline

This is a 1 MiB-flash, no-PSRAM target and the budget is genuinely tight. Two
habits are load-bearing:

- Mark new, non-flash-touching functions `__xip_text` so they stay in XIP flash
  instead of consuming the small SRAM-loaded slot. Most of `main.c` and
  `xf16cam_http.c` already is.
- **`__xip_text` moves only the code.** `appos.ld` collects `.rodata` into the
  same `> RAM` output section as `.text`, so every plain string literal —
  `snprintf` format strings especially — lands in the 64 KiB app slot that CI
  only leaves 8 KiB free in, no matter how the enclosing function is marked.
  Only `.xip_rodata` reaches flash. `xf16cam_xip.h` exists to fix that:
  `XF16CAM_XIP_FORMAT` parks a format string there and is used by
  `xf16cam_http.c`, `main.c` and `xf16cam_net.c`. `xf16cam_http.c` adds two of
  its own on the same pattern — `XF16CAM_HTTP_SEND_LITERAL` for static markup
  and `XF16CAM_HTTP_MESSAGE` for the status/body pair of an error page. **No
  new string literal in those files should be written without one of the
  three.** Reading a format string from XIP is safe — only *disabling* flash
  while executing from it is not.
- Converting the string literals freed **5,200 bytes of the app slot**; the lwIP
  link-out (see Build) then freed about 2.6 KB of XIP on both variants, and
  dropping the hibernate route from PTZ builds another 840 bytes of their app
  slot. The three reserves are now close to level: `ptz` has 20,680 free in the
  app slot, 77,484 in XIP and 78,008 in the OTA area against floors of 8, 64
  and 64 KiB; `no_ptz` has 20,248 / 79,588 / 78,584. Moving more from the app
  slot into XIP only shifts which gate trips first. What adds real room is
  removing things, as the link-out does, or revisiting the floors, not further
  relocation. The `XF16CAM_TALK` backchannel costs about 500 bytes of app
  slot and 2.1 KB of XIP on top of that (`ptz_talk` 0.17.16: 20,184 /
  75,228 / 76,556; `no_ptz_talk` 19,752 / 77,332 / 77,060), plus a 4 KiB
  playback ring in `.bss`.
- The macros keep gcc's `-Wformat` checking: GCC 8 follows a `static const
  char[]` initialised from a literal, so a mismatched argument still warns, as
  verified with the project toolchain. Read the build log, though, since the
  SDK is not built with `-Werror`. What nothing catches is a mangled literal —
  a lost tag or a wrong JSON key — so diff the literals
  (`grep -o '"[^"]*"' file | sort`) before and after a bulk conversion.
- A `#ifdef` cannot appear inside a macro invocation, so resolve a build-variant
  difference to a local first (`mode_button` in `xf16cam_http_page()` and
  `xf16cam_http_system_json()`) rather than leaving the call as a bare
  `snprintf`.
- There is exactly one **104,046-byte capture arena** holding a single aligned
  ~100 KiB JPEG buffer, and no YUV framebuffer. Frames are captured one at a
  time and each buffer stays immutable until fully transmitted, so a slow client
  can never observe a buffer mid-overwrite. Do not add a second frame buffer.

Sensor register tables live in XIP flash (`xf16cam_sensor_tables.c`, ~40 KB) and
share one retrying SCCB writer. Adding a sensor means adding a descriptor plus a
table — not new code in the camera core.

Task stacks are small and explicitly sized (`XF16CAM_HTTP_STACK_SIZE`,
`XF16CAM_BOARD_STACK_SIZE`, `XF16CAM_AUDIO_STACK_SIZE`,
`XF16CAM_RTSP_CLIENT_STACK`, MJPEG 3 KiB). The System tab reports minimum spare
stack per worker — use it rather than guessing when adding stack pressure.

## Architecture

Boot (`main.c`) brings up the SDK platform (Wi-Fi/lwIP/console), loads config,
probes the sensor, then **releases** camera power. Resources are demand-driven.

- **`xf16cam_config.c`** — settings in a dedicated FDCM sector at 1016 KiB, so
  they survive reflashing. Versioned schema with in-place migration. Also owns
  the `xf16cam_update_begin/end` mutual exclusion that serializes reboot-causing
  operations (OTA, mode change, AP reset).
- **`main.c`** — camera manager (reference-counted PA23 rail shared with the SD
  card), JPEG capture, MJPEG client tasks, and the RTSP/RTP-JPEG server on port
  8554 (RFC 2435, interleaved over RTSP/TCP). One thread per client, up to
  `XF16CAM_MAX_PARALLEL_CLIENTS` (3) per media type, slots claimed with a CAS
  on `active`. A capture lock serializes frame acquisition so one client cannot
  overwrite a buffer another is still reading. In AP mode the DHCP pool is one
  lease, so only one device associates regardless of that limit.
- **`xf16cam_http.c`** — the entire web console and API, generated as string
  literals directly into the socket (no filesystem, no asset bundle). Editing
  the UI means editing C string literals in `xf16cam_http_page()`.
- **`xf16cam_sensor.c` + `xf16cam_sensor_tables.c`** — descriptor registry,
  probe (some sensors need a reset/wake sequence first), and one table-driven
  backend. Each sensor carries a small CSI profile (byte order, PCLK/HREF/VREF
  polarity, sync type). `xf16cam_sensor_switch_cam_sensor_mode()` is the
  exception to the table-driven rule: it is SP0A39-only and no-ops on every
  other sensor. Night mode is sticky across a reload via `g_sensor_night_mode`.
- **`xf16cam_board.c`** — buttons, LEDs, and the `NO_PTZ` split: PTZ boards use
  PB20 (white LED), PA22 (IR), PB19 (reset) and have no mode button; NO_PTZ
  boards use PA21 (status), PA15 (mode), PA20 (reset). Its poll loop also runs
  two automatic behaviors: **capture-stall recovery** (reboot if a session
  holds the camera, `xf16cam_media_capturing()`, and `last_frame_ms` is 30 s
  stale; the stamp is reset whenever the camera is acquired, because it used
  to keep the previous session's last frame and any client arriving 30 s
  later was rebooted during the cold sensor re-init — the older unconditional
  2-hour reboot is commented out, leave it that way) and, on PTZ builds,
  **day/night switching** from a CDS sensor on ADC5 every 5 s.
- **`xf16cam_audio.c`** — on-demand AMIC capture; publishes PCMU silence during
  the 2.1 s analogue settling window so the media clock stays intact. Under
  `XF16CAM_TALK` it also owns speaker playback: a lock-free single-producer
  PCMU ring fed by `xf16cam_talk_push()`, a playback task that opens the codec
  output once 200 ms is buffered and closes it 1.5 s after the last packet,
  and half-duplex muting of the microphone while the speaker plays (no echo
  canceller exists). `xf16cam_talk_acquire()` admits one talker at a time.
- **`xf16cam_storage.c`**, **`xf16cam_power.c`**, **`xf16cam_ptz.c`**,
  **`xf16cam_net.c`**, **`xf16cam_rail.c`** — SD card, PA16 battery ADC and
  hibernation, PTZ motion, Wi-Fi bring-up, shared rail refcount.
- **`xf16cam_lwip_stubs.c`** — link-time stubs that keep lwIP's DNS client and
  IGMP out of the image without touching the SDK (see Build).
- **`xf16cam_log.c`** — `XF16CAM_NETLOG` only: console mirror to a RAM ring,
  UDP broadcast and `/api/log` (see Build).

The DHCP hostname is `XF16CAM-<last three eFuse MAC bytes>`, built once in
`xf16cam_net_start()` before `net_switch_mode()` and handed to the SDK's
`ethernetif_set_hostname()`. It is deliberately not configurable: the eFuse MAC
makes it unique per board from a single firmware image, with no provisioning
step and nothing an OTA or a reflash can overwrite. Adding an editable name
would mean growing `XF16CamConfig`, and `xf16cam_config_storage_valid()` rejects
any record whose `length` differs from `sizeof(XF16CamConfig)` — so that change
needs a real schema-3 migration or it silently resets every device to AP mode.
Flash is fully allocated (settings at 1016 KiB, SDK sysinfo at 1020 KiB), so
there is no spare sector to store it in separately.

A missing or unsupported sensor is non-fatal by design: Wi-Fi, HTTP, OTA, audio,
SD, and diagnostics all stay up while video reports the camera offline. Preserve
that property.

### Media mode is exclusive

`media_mode` is persisted config (`XF16CAM_MEDIA_WEB` or `XF16CAM_MEDIA_RTSP`)
and switching it **reboots**. Browser MJPEG and RTSP do not run concurrently —
an earlier attempt at simultaneous modes was reverted. Do not reintroduce it
without addressing the single-buffer capture constraint.

## Web UI

One responsive page at `http://<device-ip>/`, four tabs — **Live**, **Network**,
**Storage**, **System** — rendered server-side into the socket, with small
inline JS helpers (`tab()`, `scan()`, `toggleAudio()`, `videoStop()`,
`measurePower()`, `update()`).

- **Live** — embedded MJPEG `<img>` (or an "RTSP mode" placeholder), Listen
  button for browser audio, Browser-video/RTSP mode switch, and — PTZ builds
  only — white-LED and IR-LED toggles plus a 5-way PTZ pad. Note the IR toggle
  is not purely manual: it also drives SP0A39 night mode, and on PTZ builds the
  automatic CDS check can revert a manual toggle within 5 seconds.
- **Network** — Wi-Fi scan, SSID/password form, and "Return to open AP". A blank
  password for the currently saved secured SSID preserves the stored credential;
  a blank password for a *different* SSID means an open network.
- **Storage** — SD mount status, total/free space, check, safe eject, format
  FAT32.
- **System** — flash identity, pin map, uptime, boot cause, chip temperature,
  free SRAM heap (the SDK heap manager's live figure, `sram_free_heap_size()`;
  the build uses `__CONFIG_MALLOC_MODE` 0x01), per-worker minimum spare
  stack, media/audio counters,
  resolution switch (QVGA to native VGA where the sensor supports it), battery
  voltage measurement, hibernate (NO_PTZ builds only), and streamed OTA upload.

## HTTP API

Streams: `GET /stream.mjpeg`, `GET /stream.pcmu` (both 3-client capped, 503 when
full). RTSP: `rtsp://<device-ip>:8554/stream` — force TCP transport in VLC, or
`-rtsp_transport tcp` with ffplay.

JSON reads: `GET /api/scan`, `/api/audio`, `/api/led`, `/api/ir_led`, and
`/api/system` (every System/Live/Storage diagnostic in one streamed response).
`GET /api/log` (`XF16CAM_NETLOG` builds only) returns the last 2 KiB of
console output as text.

Form-encoded writes: `POST /api/wifi`, `/api/ap`, `/api/media`, `/api/resolution`
(these reboot), `/api/led`, `/api/ir_led`, `/api/ptz` (`mode=up|down|left|right|home`,
501 under `NO_PTZ`), `/api/power` (measure battery), `/api/hibernate` (`NO_PTZ`
only; PTZ builds have neither the route nor the button, so it is a 404 there),
`/api/sd/refresh`, `/api/sd/eject`, `/api/sd/format`, `/api/reboot` (both
variants; 409 while an update holds the lock, no media quiesce), and `POST /api/ota`
(streamed image, written to the staging area in 2 KiB pieces and only selected
by the bootloader after SDK structure + MD5 verification, so a failed upload
leaves the running firmware bootable).

Handlers return `XF16CAM_HTTP_KEEP_RUNNING` or signal a reboot; errors go through
`xf16cam_http_message(fd, status, text)`.

RTSP backchannel (`XF16CAM_TALK` builds): a DESCRIBE carrying
`Require: www.onvif.org/ver20/backchannel` gets a third, `a=sendonly`
PCMU/8000 track (`track3`); SETUP of it claims the single talk slot (503 when
taken) and the client pushes interleaved RTP on that channel. The parser hands
complete binary frames to a sink (`xf16cam_rtsp_parser_set_sink()`), and
`xf16cam_rtp_payload()` unwraps the RTP header; both are host-tested. The
session thread drains the socket only between video frames, which is why the
playback ring is 512 ms and why `xf16cam_rtsp_receive()` reads up to eight
times per pass on talk builds. `/api/system` gains a `talk` object and the
System tab an Audio-card Speaker row. `tools/xf16cam/rtsp_talk.py` is a
scripted client; `tests/xf16cam/talk-go2rtc-test-plan.md` is the device test.

**The setup AP is unauthenticated by design** and so is the API — anyone in radio
range can reach every endpoint above while AP mode is active. Keep that in mind
before adding anything more destructive than what is already exposed.

## Serial console

Recovery path, kept working independently of media/storage/config state
(`command.c`): `wifi ap`, `wifi sta <ssid> <password>` (saves the same config as
the web page, reboots, never echoes the password), and `upgrade` — an
unconditional BootROM handoff. Responses are `<ACK> <code> <text>`.
`PRJCONF_CONSOLE_EN` must stay enabled.

## Versioning

Bump `XF16CAM_VERSION` in `project/example/xf16cam/xf16cam_version.h`; CI parses
it out of that file to name artifacts, and the build fails if it is empty.
`ChangeLog.md` is the vendor SDK changelog, not the XF16Cam one — do not add
XF16Cam entries there.
