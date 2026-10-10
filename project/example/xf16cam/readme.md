# XF16Cam RTSP camera

This isolated example turns the XF16/XR872ET camera JPEG path into a
single-client RTSP stream without PSRAM or an SD card.

- First boot starts the open setup AP `XF16CAM` at `192.168.4.1`.
- The setup AP is intentionally unauthenticated; anyone in radio range can
  access its web and media services while AP mode is active.
- Its DHCP server leases `192.168.4.100` to the setup client.
- Open `http://192.168.4.1/` to scan for Wi-Fi and save STA credentials.
- The same page accepts a streamed, verified OTA image and then reboots.
- OTA has been validated end-to-end on XR872 hardware, including retained Wi-Fi
  settings, camera reprobe, audio restart, and management-page recovery.
- Saved settings use a dedicated FDCM sector and survive firmware reflashing.
- Schema-1 settings migrate in place with network and media choices preserved;
  resolution starts conservatively at QVGA because its old reserved byte was undefined.
- Leaving the password blank for the currently saved secured SSID preserves
  that credential; a blank password for a different SSID selects an open network.
- A failed STA connection falls back to the open setup AP.
- Serial recovery: `wifi ap` or `wifi sta <ssid> <password>` saves the same
  configuration as the web page and reboots without exposing the password.
- The serial `upgrade` command is an unconditional last-resort BootROM handoff;
  it remains available even if media, storage, or configuration state is stuck.
- `ptz_netlog` / `no_ptz_netlog` builds (`XF16CAM_NETLOG`) mirror the whole
  console to a UDP broadcast on port 5514 and to `GET /api/log`, for boards
  whose serial pins are not reachable. Run `python tools/xf16cam/udplog.py`
  on a PC in the same network to receive it; the UART output is unchanged.
- Stream: `rtsp://<device-ip>:8554/stream`
- The responsive web console has Live, Network, Storage, and System tabs, with
  a focal embedded MJPEG view that can switch exclusively with RTSP mode.
- Browser MJPEG frames are terminated as standalone, immutable JPEG images;
  both web and RTSP clients finish sending one frame before the next capture.
- Device information reports flash identity, the known XF16 pin map, uptime,
  SDK-cached boot cause, and XF16 chip temperature.
- Runtime diagnostics retain the minimum spare stack values for the HTTP,
  audio, and board workers.
- Demand-started AMIC capture publishes PCMU silence during its 2.1-second
  analogue settling period, preserving the media clock before live samples.
- Browser MJPEG reconnects after a transient failure, and transmit-only MJPEG
  and PCMU workers detect a closed browser without waiting for TCP retries.
- PA15 short-press switches Web/RTSP mode; PA20 held for three seconds restores
  the open setup AP. PA21 blinks during startup and stays on when services are ready
  in case of NO_PTZ. PB20 blinks during startup and will be turned off when services
  are ready in case of PTZ version, because it is a white luming LED.
- An optional one-bit SD card can be mounted, inspected, and safely ejected from
  the web page. Camera and storage share the PA23 rail through reference-counted
  ownership; the page reports total/free space and can explicitly format FAT32.
- Transport: RTP/JPEG (RFC 2435) interleaved over RTSP/TCP
- Optional two-way audio (`ptz_talk` / `no_ptz_talk` builds, `XF16CAM_TALK`):
  an RTSP client that sends `Require: www.onvif.org/ver20/backchannel` in
  DESCRIBE is offered an extra `a=sendonly` PCMU/8000 track (`track3`) and can
  push audio into it over the same TCP connection; the camera plays it on the
  speaker through the XR872 codec line-out. One talker at a time; the
  microphone is silenced while the speaker plays because there is no echo
  canceller. go2rtc, Home Assistant and Frigate use this; VLC and ffplay only
  receive. See `tests/xf16cam/talk-go2rtc-test-plan.md` and
  `tools/xf16cam/rtsp_talk.py`.
- RTSP input is framed across fragmented/coalesced TCP reads, rejects unsupported
  transports, accepts case-insensitive RTSP header/transport tokens, and applies
  bounded connection and send waits.
- Image: 320 x 240 by default, with persistent native 640 x 480 selection for
  VGA-capable landscape sensors; SP0828 remains portrait 240 x 320. JPEG quality 60.
- Sensor probing and driver dispatch use a small descriptor registry and one
  register-table backend; adding a compatible sensor does not require changes
  to the shared camera core.
- A missing or unsupported sensor is non-fatal: Wi-Fi, HTTP setup, OTA, audio,
  SD management, and diagnostics remain available while video services report
  that the camera is offline.

## Camera sensors

- GC0328 (`0x9d`): factory QVGA table, hardware validated on XF16.
- GC0329 (`0xc0`): exact XF16 factory 24 MHz VGA table, hardware validated on
  XF16; QVGA is the default and native VGA is selectable.
- GC0308 (`0x9b`): compacted XR872 SDK VGA table, QVGA by default with selectable
  native VGA; compiled but awaiting matching-sensor validation.
- GC0309 (`0xa0`): exact XF16 factory 24 MHz VGA table, hardware validated on
  XF16; QVGA is the default and native VGA is selectable.
- GC0310 (ID `0xa3:0x10`): exact XF16 factory 24 MHz VGA table, hardware
  validated on XF16; QVGA is the default and native VGA is selectable.
- GC0311 (`0xbb`): exact XF16 factory 24 MHz VGA table, hardware validated on
  XF16; QVGA is the default and native VGA is selectable.
- GC0312 (ID `0xb3:0x10`): exact XF16 factory 24 MHz VGA table, hardware
  validated on XF16; QVGA is the default and native VGA is selectable.
- HI704 (`0x96`): factory FTY/X5/X6 VGA table, hardware validated on XF16;
  QVGA is the default, native VGA is selectable, and its profile selects the
  sensor's required rising PCLK edge.
- OV7690 (ID `0x76:0x91`): exact XF16 factory 24 MHz VGA table, QVGA by default
  with selectable native VGA; compiled but awaiting matching-sensor validation.
- SP0A19 (`0xa6`): exact XF16 factory 24 MHz VGA table, hardware validated on
  XF16; QVGA is the default and native VGA is selectable.
- SP0A20 (`0x2b`): factory HQT6 VGA table, hardware validated on XF16; QVGA is
  the default and native VGA is selectable.
- SP0A39 (ID `0x0a:0x39`): exact XF16 factory 24 MHz VGA table, QVGA by default
  with selectable native VGA; compiled but awaiting matching-sensor validation.
- SP0828 (`0x0c`): factory FTY/X5/X6 24 MHz portrait table at 240 x 320;
  hardware validated on XF16.

Each sensor has a compact CSI profile for byte order, PCLK/HREF/VREF polarity,
and sync type, so new sensors can override these without changing the SDK.
Optional descriptor-owned probe tables also cover sensors such as GC0329 whose
factory probe uses a short reset or wake sequence before reading the ID.

The GC0309, GC0310, GC0311, GC0312, GC0329, OV7690, SP0A19, and SP0A39
sequences are byte-exact from XF16 factory firmware; GC0310 and GC0312 share
their byte-identical table. The Taixin-derived HI704, SP0A20, and SP0828 tables
are deliberately limited to byte-exact sequences corroborated by the supplied
factory-firmware research bundle. Alternative SP0828 tables are not included:
the FTY/X5/X6 variant matches the XR872 A9 family and its fixed 24 MHz sensor
clock. Register tables live in XIP flash and share one retrying SCCB writer,
avoiding per-sensor code and runtime state. Those three tables were adapted from
the camera-driver evidence associated with `NonPIayerCharacter/OpenTXW81X`; its
repository currently has no visible top-level licence, so provenance should be
resolved before redistributing them beyond this research firmware.

For VLC, force RTSP-over-TCP if it does not select it automatically. For
FFmpeg/ffplay use `-rtsp_transport tcp`.

All sensors, including GC0328, use the same probe and retrying table loader;
GC0328 retains its validated power-cycle, register-delay, and settle timings.
The 104,046-byte capture arena is the checked worst-case bound for one aligned
100 KiB JPEG buffer and contains no YUV framebuffer. Still capture keeps that
buffer immutable while a frame is transmitted, so the larger per-frame ceiling
costs slightly less SRAM than the former pair of 50 KiB buffers.
Frames are acquired one at a time so a slow network client cannot race the
hardware encoder and observe a buffer while it is being overwritten.
The shared PA23 camera/SD rail, capture arena, and AMIC are demand-driven: boot
probes the sensor, then releases resources. Camera and mounted storage hold
separate rail references; ejecting the card permits power-down when media is idle.
XF16Cam owns camera power, CSI/JPEG, and its media listeners; the SDK platform
starts the underlying Wi-Fi/lwIP services. `PRJCONF_CONSOLE_EN` remains enabled
for serial recovery and reflashing.

## Confirmed XF16 hardware

- Camera CSI: PA0-PA11; camera control: PA14; camera power rail: PA23
- Factory status LED in case of NO_PTZ version: PA21; mode button: PA15; setup/reset button: PA20
- White/fill/"luming" LED in case of PTZ version: PB20; no mode button; setup/reset button: PB19
- IR illumination LED in case of PTZ version: PA22
- Microphone: XR872 internal codec analog microphone (AMIC) input, not a GPIO
- Speaker: XR872 internal codec line-out into an on-board amplifier whose
  enable is PB21, active low (talk builds only). The pin was recovered from
  the factory firmware; the SDK evaluation-board config would drive PA23
  instead, which is the camera/SD rail here, so talk builds use the project's
  copy of the board config with the switch moved to PB21.
- SD card: PB16 CMD, PB17 D0, PB18 CLK
- Console: PB0 TX, PB1 RX; SIP flash: PB8-PB13 (never drive them; PTZ motors
  use PB2-PB7 and PB14/PB15)

The button pins and roles were recovered from the factory application's board
configuration. Both inputs are active-low and use internal pull-ups.

## Linux build

The GitHub Actions workflow builds with Arm GNU Toolchain 8-2019-q3 on
Ubuntu 22.04. Each run uploads an `xf16cam-xr872` artifact containing:

- `xf16cam-xr872-v<version>.img`: the complete image for serial flashing
- `xf16cam-xr872-v<version>-ota.img`: compressed image for the web updater
- `SHA256SUMS`: image checksum
- `size.txt`: linked application memory usage
- `sections.txt`: per-section linked memory usage
- `symbols.txt`: linked symbols sorted by size
- `xf16cam.map.gz`: compressed linker map for placement and growth analysis
- `image-budget.txt`: app, XIP, OTA, and reserved-tail usage/headroom

For a local Linux build with `arm-none-eabi-gcc` on `PATH`:

```sh
printf '%s\n' \
  '__CONFIG_CHIP_TYPE ?= xr872' \
  '__CONFIG_HOSC_TYPE ?= 40' > .config
chmod +x tools/mkimage
make -C project/example/xf16cam/gcc \
  CC_DIR="$(dirname "$(command -v arm-none-eabi-gcc)")" \
  image
make -C project/example/xf16cam/gcc \
  CC_DIR="$(dirname "$(command -v arm-none-eabi-gcc)")" image_xz
```

The flashable result is
`project/example/xf16cam/image/xr872/xr_system.img`; its web-update partner is
`xr_system_img_xz.img` in the same directory.

## 1 MiB flash layout

- `0-32 KiB`: bootloader and reserved space
- `32-96 KiB`: SRAM-loaded application
- `96-597 KiB`: XIP application
- `597-635 KiB`: WLAN firmware
- `635-636 KiB`: reserved primary-image growth
- `636-640 KiB`: guard space
- `640-644 KiB`: SDK OTA metadata
- `644-1016 KiB`: compressed, verified OTA staging image
- `1016-1020 KiB`: XF16Cam configuration (FDCM)
- `1020-1024 KiB`: SDK system information

An upload is written directly to the staging area in 2 KiB pieces. It is not
selected by the bootloader until its image structure and MD5 have passed SDK
verification, so a failed or interrupted upload leaves the current firmware
bootable. Do not upload the full serial image through the web page.

CI additionally requires at least 8 KiB free in the SRAM-loaded app slot and
64 KiB free in both the XIP and compressed-OTA areas. This prevents ordinary
feature growth from silently consuming the final usable bytes. A symbol-placement
check also requires the flash identity query to remain in SRAM and uninlined,
preventing code from disabling XIP while it is executing from flash.

## Battery and power-management plan

Factory-firmware analysis identifies PA16/ADC6 as the battery-divider input
(2.5 V ADC reference, approximately 1.7:1 divider) and PA20 as wake input 6.
The factory averages ten ADC samples after dropping the minimum and maximum,
warns at 3.4 V, and hibernates after repeated readings at or below 3.3 V.
PA23 controls the camera/peripheral rail; it is not a main battery relay.
No reliable charger-status GPIO has been found, and PA21 is the status LED.

The System tab can take an explicit raw/approximate millivolt reading from
PA16 and can enter hibernation on request; PA20 is configured as its falling-edge
wake source. Charging state remains unknown. XF16Cam deliberately does not yet
estimate battery percentage or sleep automatically. The ADC display uses the
median of eleven conversions: hardware testing found occasional analogue
outliers while Wi-Fi, camera and AMIC were active, whereas the median tracked
the attached battery consistently. A missing battery can produce a zero, floating, or
charger-regulated ADC value, so copying the factory cutoff before calibration
could make USB-powered devices repeatedly hibernate. The safe implementation
order is:

1. Add read-only PA16 raw/millivolt telemetry and report charging as unknown.
   (Implemented; calibration pending.)
2. Add explicit hibernation with PA20 wake. (Implemented; battery-hardware
   validation and wake-reason diagnostics pending.)
3. Make AMIC capture and the camera rail demand-driven. (Implemented.)
4. Treat OTA, settings/SD writes, and active media clients as sleep inhibitors.
5. Calibrate against a multimeter with the battery attached, then enable
   percentage estimates and repeated-sample low-voltage hibernation.

These features can remain local to XF16Cam; the current SDK already provides
standby, hibernation, wake-I/O, wake-timer, and wake-reason APIs.
