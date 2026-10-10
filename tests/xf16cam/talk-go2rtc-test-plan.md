# XF16Cam RTSP audio backchannel: device test plan (go2rtc on Windows)

Covers the `ptz_talk` / `no_ptz_talk` builds (`XF16CAM_TALK`): the camera
offers an ONVIF-style audio backchannel over RTSP and plays what the client
sends on the speaker. No serial console is needed: every criterion below is
read from the web console, `GET /api/system`, or the scripted client, which
polls the API itself and prints a PASS/FAIL verdict.

Run the scripted client first; it is deterministic and tells you exactly which
RTSP step failed. go2rtc is the real-world client and is what Home Assistant
and Frigate use. VLC and ffplay cannot send a backchannel; they are used only
to prove the existing receive path is unchanged.

All commands below assume the camera at **192.168.0.108** and are meant to be
pasted as they are, from the repository root, in a Windows terminal. Windows
10 ships `curl.exe`, and `python` must be Python 3.

## What to have ready

- Camera flashed with a `*_talk` image: `buildXF16Cam.bat ptz_talk` (or
  `no_ptz_talk`) produces `dist/xf16cam-ptz_talk-xr872-v0.17.16-ota.img`,
  which uploads through the System tab from any earlier firmware. For the
  runs that end in a reboot use `ptz_talk_netlog`, which adds the console
  mirror (next section) at no change to the talk code.
- Camera in **RTSP media mode** (Live tab, "RTSP mode"), joined to your LAN in
  STA mode. The backchannel does not exist in browser-video mode.
- Speaker connected; headphones or a second device to listen to the camera's
  own microphone through go2rtc.
- go2rtc: `go2rtc_win64.exe` from <https://github.com/AlexxIT/go2rtc/releases>,
  in a folder of its own with this `go2rtc.yaml` next to it:

```yaml
streams:
  xf16cam: rtsp://192.168.0.108:8554/stream
```

  The backchannel is on by default for an RTSP source. If you ever add a `#`
  option to that URL (for example `#timeout=30`), go2rtc turns the backchannel
  off unless you also append `#backchannel=1`.

- VLC, and ffmpeg only if you want to convert a WAV file for T4.

## How to read the camera without a serial console

```text
curl.exe -s http://192.168.0.108/api/system
```

On a talk build the JSON contains
`"talk":{"ok":true,"active":false,"packets":0,"dropped":0,"underruns":0,"errors":0,"stack":N}`.
`active` is true while the speaker is playing; `packets` counts RTP payloads
accepted; `dropped` counts payloads thrown away because the jitter ring was
full; `underruns` counts 20 ms silence frames written while the speaker was
open, and because the speaker stays open for 1.5 s after the last packet
every talk burst ends with about 75 of them by design, so only growth beyond
that means audible gaps; `errors` counts codec write failures; `stack` is the playback task's
minimum spare stack in bytes. The System tab shows the same as a Speaker row
in the Audio card, and `media.clients` is the number of connected stream
clients. To watch it live:

```text
for /l %i in (1,0,2) do @(curl.exe -s http://192.168.0.108/api/system & echo. & timeout /t 1 >nul)
```

(Ctrl+C stops it. In PowerShell use
`while ($true) { curl.exe -s http://192.168.0.108/api/system; ""; Start-Sleep 1 }`.)

Serial-log equivalents, for reference: "speaker on/off" is `talk.active`;
"PLAY with backchannel" is `talk.packets` increasing while `media.clients`
is at least 1; the media rail is checked indirectly in T7.

## Reading the console anyway: the `_netlog` build

A `ptz_talk_netlog` image mirrors every console line to a UDP broadcast on
port 5514 and keeps the last 2 KiB at `GET /api/log`. Start the listener in
a second terminal before any test whose outcome is a reboot (the idle re-run
in T4, T7, T11) and leave it running; allow Python through the Windows
firewall when asked:

```text
python tools/xf16cam/udplog.py
```

It prints each line with a timestamp, marks every boot with
"===== camera booted =====" and appends everything to `xf16cam-<date>.log`
in the current folder. The line to look for after an unexpected restart is
the capture-stall watchdog's "no camera frames for ... rebooting", printed
just before the reset; anything else before a boot marker is a crash to
report with the surrounding lines. Without the listener,
`curl.exe -s http://192.168.0.108/api/log` shows the same 2 KiB, but only
since the last boot.

## Tests

### T1. Non-talk build is unchanged (regression)

Flash the plain `ptz` or `no_ptz` image of the same source, then:

1. VLC: Media > Open Network Stream, `rtsp://192.168.0.108:8554/stream`. If
   it does not play, force RTSP over TCP in Preferences > Input/Codecs.
2. `python tools/xf16cam/rtsp_talk.py --tone 3`
3. `curl.exe -s http://192.168.0.108/api/system`

Pass: VLC plays video and audio as before. The script prints the note that
the API has no talk object and stops at DESCRIBE with "FAIL: DESCRIBE did not
offer the sendonly track3", which is the expected result on this build. The
JSON has no `talk` object.

### T2. Talk build boots and reports the speaker

Flash the `*_talk` image, then:

```text
curl.exe -s http://192.168.0.108/api/system
```

Pass: the JSON has `"talk":{"ok":true,"active":false,...}`. The System tab
Audio card shows "Speaker: ready (RTSP backchannel, on demand)". Sensor name,
Wi-Fi, SD card and OTA behave as on the non-talk build. Write down `heap`
and `talk.stack` from the JSON for T9.

### T3. Clients that do not ask for a backchannel see nothing new

1. VLC as in T1.
2. `python tools/xf16cam/rtsp_talk.py --no-require --tone 1`

Pass: VLC plays video and audio exactly as before. The script prints
"backchannel track offered: no" and "PASS: no backchannel without Require".

### T4. Scripted backchannel (the deterministic test)

```text
python tools/xf16cam/rtsp_talk.py --tone 5
```

Pass: every RTSP step prints 200 and "backchannel track offered: yes". A
clean 440 Hz tone plays from the speaker for about 5 s, starting well under a
second after "sending...", with no clicks or stutter. The script prints
`speaker ON` at roughly 0.2 to 0.7 s, `speaker off` about 1.5 s after the
tone ends, and finishes with a PASS line such as
"PASS: 250 packets played, 66 underruns (about 75 is the normal silence
tail), speaker on at 0.6 s and off at 6.9 s". It exits with FAIL if the
camera dropped packets, reported write errors, had more than 100 underruns,
or never switched the speaker on and off.

Then judge quality and volume with speech. Windows can synthesize the test
phrase itself; run this once in PowerShell from the repository root and it
writes `speech8k.wav` in the format the script needs:

```text
Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; $f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(8000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono); $s.SetOutputToWaveFile("speech8k.wav", $f); $s.Rate = -1; $s.Speak("This is the X F sixteen camera speaker test. One, two, three, four, five. If you can hear every word clearly, the audio backchannel is working."); $s.Dispose()
```

(Any recording works instead: `ffmpeg -i speech.wav -ar 8000 -ac 1
speech8k.wav`.) Then:

```text
python tools/xf16cam/rtsp_talk.py speech8k.wav
```

Run it twice back to back. Both must pass with no reboot between them
(`camera up` in the second run's header should be larger than in the first,
not reset to a few seconds). A reboot between two clean runs would mean an
unclean-disconnect leak, covered by T11.

Then run it once more after leaving the camera idle for at least a minute.
This used to reboot the camera: the stall watchdog measured from the previous
session's last frame, so any client arriving 30 s or more after it was
rebooted on the first board poll, while the cold sensor re-init was still
running. The fix stamps the clock when the camera is acquired and counts
only sessions that hold the camera. The idle re-run must now pass, with the
listener (next section) showing "media rail: PA23 on" and "SP0A39 init
complete" after "RTSP client connected" and no boot marker. Repeat with
about 25 s and about 35 s of idle, since the old bug sat exactly on the 30 s
line.

If the level is wrong for the speaker, `XF16CAM_TALK_VOLUME` in
`project/example/xf16cam/xf16cam_audio.c` is the knob (0 to 31, currently 28).

### T5. go2rtc two-way audio

1. Start `go2rtc_win64.exe` from its folder and open
   <http://localhost:1984/api/streams?src=xf16cam>
   Pass: the producer lists a video track, the camera's PCMU audio track, and
   a PCMU/8000 track marked `sendonly`. That last one is the backchannel.
2. Open
   <http://localhost:1984/webrtc.html?src=xf16cam&media=audio+microphone>
   and allow the microphone when the browser asks. The microphone only works
   from a secure origin, and `localhost` counts, so keep the browser on the
   go2rtc PC. The camera's own audio should play. Do not ask this page for
   video: the camera sends Motion JPEG, which WebRTC has no codec for, so a
   `media=video+...` URL shows a spinner forever while the audio still works
   underneath. For the picture open go2rtc's MJPEG output in a second tab:
   <http://localhost:1984/stream.html?src=xf16cam&mode=mjpeg>
3. Speak into the PC microphone while the live API loop from above runs in a
   terminal.

Pass: your voice comes out of the camera speaker with about half a second of
delay. `talk.active` is true while you speak and returns to false about 1.5 s
after you stop; `talk.packets` climbs at about 50 per second of speech.
While the speaker plays, the camera's own microphone goes silent in the
browser (half-duplex) and comes back within a second after you stop.
`talk.dropped` stays 0 or grows only slowly; about 75 underruns per pause in
speech are the normal silence tail, `talk.errors` stays 0.

Also open <http://localhost:1984/webrtc.html?src=xf16cam&media=audio> (no
microphone) in a third tab. Pass: it plays the camera audio, and the first
tab keeps its backchannel. Home Assistant and Frigate behave the same way:
they take the backchannel from this RTSP source and get video through their
own MJPEG path or an ffmpeg transcode, not through WebRTC.

### T6. One talker at a time

With the T5 tab open and its microphone allowed, run:

```text
python tools/xf16cam/rtsp_talk.py --tone 3
```

Pass: the script stops with "SETUP track3: 503, another client already holds
the talk slot" and go2rtc keeps working. Close the go2rtc tab, wait ten
seconds, run the same command again: it passes.

### T7. The PA23 rail survives speaker on/off

This is the risk the project-local board config exists for: the SDK board
file would drop PA23, the camera/SD rail, when playback closes, whereas the
XF16's amplifier enable is PB21 (active low) and that is what the project
copy switches. A rail drop shows up as frozen or corrupted video, a lost SD
card, or a capture-stall reboot.

1. Insert an SD card, mount it on the Storage tab, note total and free space.
2. Start VLC on `rtsp://192.168.0.108:8554/stream` and keep it visible.
3. Run ten talk cycles, then read the API again:

   ```text
   for /l %i in (1,1,10) do @python tools/xf16cam/rtsp_talk.py --tone 2
   curl.exe -s http://192.168.0.108/api/system
   ```

Pass: all ten runs print PASS; VLC video never freezes or restarts; the
Storage tab still shows the card mounted with the same figures; `up` in the
JSON is larger than before (no reboot) and `cam.name` is unchanged.

### T8. Updates stop the speaker first

An update never writes flash while the speaker is open: the OTA handler
stops the stream clients, waits for the playback task to close the codec
(which also drops the amplifier enable), and only then starts writing. It
refuses with "Active media could not stop safely" only if that does not
finish within its timeout.

1. Open the T5 tab and keep talking.
2. On the System tab, upload `dist/xf16cam-ptz_talk-xr872-v0.17.16-ota.img`.

Pass: your voice stops within a couple of seconds, the go2rtc tab loses the
stream, the upload proceeds and the camera reboots with settings intact and
the speaker working again afterwards. On the listener, "speaker off" and
"client stopped" appear before "OTA: push init". A refusal message is also a
pass, as long as a second upload after closing the tab succeeds. On
`no_ptz_talk`, the Hibernate button quiesces the same way.

### T9. Soak

Leave the T5 tab open for 30 minutes, talking for a few seconds every couple
of minutes, then:

```text
curl.exe -s http://192.168.0.108/api/system
```

Pass: `up` is about 1800 more than at the start (no reboot), `heap` is within
a few hundred bytes of the T2 value, `talk.stack` is above 300, `talk.errors`
is 0 and `talk.dropped` is not climbing.

### T10. Both talk variants

Repeat T2 and T4 on the other variant (`no_ptz_talk` or `ptz_talk`).

### T11. Recovery from an unclean disconnect

A real client (go2rtc, or a dropped Wi-Fi link) can vanish without a TEARDOWN.
The RTSP server frees a client slot only when its session thread returns, so a
session stuck mid-capture would hold its slot until the 30 s capture-stall
watchdog reboots the board. This test checks the camera recovers on its own.

1. Start a run and kill it in the middle with Ctrl+C:

   ```text
   python tools/xf16cam/rtsp_talk.py --tone 20
   ```

   Press Ctrl+C after a few seconds.
2. Immediately start a clean run:

   ```text
   python tools/xf16cam/rtsp_talk.py --tone 5 --watch
   ```

Pass: the second run connects and passes, or it reports the camera is at its
client limit and then, within about 30 s, the watcher prints a reboot line and
a following run passes. Either way the camera must return to service without
being power-cycled. Repeat the kill three times in a row; the camera should
still recover. If a clean run is refused and no reboot ever comes, the slot
leaked permanently, which is a firmware bug to report with the run output.

## What to send back

- The full console output of T4 (tone and speech runs).
- The `/api/system` JSON after T4 and after T9.
- The T7 result, because it is the only test that can break a stream rather
  than just fail.
- Subjective volume and quality of the speech run in T4.
