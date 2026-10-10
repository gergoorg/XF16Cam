# XF16Cam functionality

Every capability exposed by the firmware, and whether it is reachable from the
web console, the HTTP API, or both. Derived from the code on this branch
(`project/example/xf16cam/xf16cam_http.c`, `xf16cam_board.c`, `command.c`), not
from `readme.md` — the readme was last updated at `aaa71ca` and predates the
multi-client, day/night, and stall-recovery work.

## How the page gets its data

Almost everything is **server-rendered into `GET /`** as C string literals: LED
button labels, SD capacity, and the whole System tab are a snapshot taken when
the page is built, and they stay frozen until you reload. The page itself never
polls for them — `GET /api/system` returns the same diagnostics as JSON, but it
exists for external clients and nothing in the UI calls it.

Only four things change without a reload:

| What | Trigger | Request |
| --- | --- | --- |
| Mic peak level | Automatic, every 2 s while the Live tab is visible | `GET /api/audio` |
| Wi-Fi scan list | "Scan networks" button | `GET /api/scan` |
| Battery voltage | "Measure voltage" button | `POST /api/power` |
| LED button labels | The toggle itself, relabelled from the POST response | `POST /api/led`, `/api/ir_led` |

`GET /api/led` and `GET /api/ir_led` are **not used by the page at all** — the
UI shows LED state through the server-rendered button label ("Turn LED off"
means it is currently on). Those two endpoints exist only for external clients.

## Features

Legend: OK = supported, "-" = not exposed there.
UI = visible or actionable in the web console. API = has a dedicated endpoint.
Variant: *PTZ* = PTZ boards only, *NO_PTZ* = fixed-camera boards only, blank = both.

| Feature | UI | API | Endpoint / control | Variant | Notes |
| --- | :--: | :--: | --- | :--: | --- |
| Web console page | OK | OK | `GET /` | | Tabs: Live, Network, Storage, System |
| MJPEG video stream | OK | OK | `GET /stream.mjpeg` | | Needs sensor + Web media mode; up to 3 clients; auto-retries after failure |
| PCMU audio stream | OK | OK | `GET /stream.pcmu` | | AMIC; up to 3 clients; decoded in-browser via WebAudio |
| Mic peak meter | OK | OK | `GET /api/audio` | | Live tab, polled every 2 s; JSON also has active, mean, packets, errors |
| Console log mirror | - | OK | UDP broadcast to port 5514, `GET /api/log` | NETLOG | `XF16CAM_NETLOG` builds only; every printf, last 2 KiB in RAM; `tools/xf16cam/udplog.py` receives on a PC |
| RTSP video stream | - | OK | `rtsp://<ip>:8554/stream` | | RTP/JPEG over RTSP/TCP; up to 3 clients; separate listener |
| RTSP audio backchannel (talk) | - | OK | `rtsp://<ip>:8554/stream`, `track3` | TALK | `XF16CAM_TALK` builds only; ONVIF `Require` header in DESCRIBE adds an `a=sendonly` PCMU track; one talker; mic muted while playing; amplifier enable PB21 (active low) switched by the SDK around playback; counters in `/api/system` `talk` |
| Switch media mode (web/rtsp) | OK | OK | `POST /api/media` | | Modes are exclusive; reboots |
| Switch resolution | OK | OK | `POST /api/resolution` | | `qvga`/`vga`; 409 if sensor lacks VGA; reboots |
| Wi-Fi scan | OK | OK | `GET /api/scan` | | Sorted by RSSI, click a result to fill the SSID field |
| Join Wi-Fi network (STA) | OK | OK | `POST /api/wifi` | | Blank password keeps saved credential; reboots |
| Restore open setup AP | OK | OK | `POST /api/ap` | | Also via serial `wifi ap`; reboots |
| White/illumination LED state | OK | OK | `GET /api/led` | PTZ | UI shows it as the button label; GET endpoint unused by the page |
| Toggle white/illumination LED | OK | OK | `POST /api/led` | PTZ | API works on both variants; button renders on PTZ only |
| IR LED state | OK | OK | `GET /api/ir_led` | PTZ | UI shows it as the button label; GET endpoint unused by the page |
| Toggle IR LED | OK | OK | `POST /api/ir_led` | PTZ | Also switches SP0A39 to night/mono; auto day/night may revert it within 5 s |
| Automatic day/night switching | - | - | CDS light sensor, ADC5 | PTZ | Every 5 s; avg of 10 samples > 1500 = dark, drives IR LED + sensor night mode |
| PTZ move / home | OK | OK | `POST /api/ptz` | PTZ | `mode=up\|down\|left\|right\|home`; 501 under NO_PTZ |
| PTZ idle power-down | - | - | automatic | PTZ | Motors powered down 10 s after the last move |
| Measure battery voltage | OK | OK | `POST /api/power` | | PA16/ADC6, median of 11; uncalibrated, charging always "Unknown" |
| Hibernate | OK | OK | `POST /api/hibernate` | NO_PTZ | Wakes on PA20; quiesces media first; PTZ builds have no route or button (404) |
| Mount / check SD card | OK | OK | `POST /api/sd/refresh` | | 503 when no readable FAT card |
| Safely eject SD card | OK | OK | `POST /api/sd/eject` | | Releases the shared PA23 rail |
| Format SD card (FAT32) | OK | OK | `POST /api/sd/format` | | Destructive |
| SD capacity / mount status | OK | OK | Storage tab, `/api/system` | | Mounted flag, total and free MiB |
| Firmware update (OTA) | OK | OK | `POST /api/ota` | | Streamed, verified before the bootloader selects it |
| Reboot | OK | OK | `POST /api/reboot` | | System tab Power card; 409 while an update holds the lock |
| All diagnostics as JSON | - | OK | `GET /api/system` | | Everything below in one response; streamed, no Content-Length |
| Device info | OK | OK | System tab, `/api/system` | | Flash JEDEC/size, MAC, network mode, IP, heap |
| DHCP hostname | OK | OK | System tab, `/api/system` (`host`) | | `XF16CAM-<last 3 MAC bytes>`, derived from the eFuse MAC; read-only, sent as DHCP option 12 in STA mode |
| Runtime info | OK | OK | System tab, `/api/system` | | Uptime, boot reason, chip temperature (tenths degC, `null` if unavailable) |
| Stack diagnostics | OK | OK | System tab, `/api/system` | | Min spare stack for HTTP, audio, board workers |
| Media counters | OK | OK | Live tab, `/api/system` | | Frames, largest JPEG, capture errors, capacity, last-frame age, client count |
| Pin map | OK | - | System tab | | Confirmed XF16 assignments, varies by variant |
| Button state | OK | OK | System tab, `/api/system` | | `btn.mode` is `null` on PTZ boards, which have no mode button |
| Capture-stall recovery | - | - | automatic | | Reboots if a session holds the camera and no frame for 30 s, counted from the camera being acquired |
| Mode button (Web/RTSP toggle) | - | - | PA15 short press | NO_PTZ | Physical control |
| Setup/reset to AP | - | - | PA20 / PB19 hold 3 s | | Physical control; PA20 on NO_PTZ, PB19 on PTZ |
| Status / illumination LED | - | - | PA21 / PB20 | | NO_PTZ: blinks on boot, solid when ready. PTZ: off when ready |

## Serial console

Recovery path over PB0/PB1, kept working independently of media, storage, and
configuration state. Replies are `<ACK> <code> <text>`.

| Command | Description |
| --- | --- |
| `wifi ap` | Save open-AP mode and reboot |
| `wifi sta <ssid> <password>` | Save station credentials and reboot; password never echoed |
| `upgrade` | Unconditional BootROM handoff for UART reflashing |

## Client limits

Three parallel clients per media type (`XF16CAM_MAX_PARALLEL_CLIENTS`), each on
its own thread, with a 4th getting `503`. In **AP mode the DHCP pool is a single
lease**, so only one device can associate at a time regardless of that limit.
A single capture lock serializes frame acquisition, so one slow client cannot
observe a buffer another client is still reading.
