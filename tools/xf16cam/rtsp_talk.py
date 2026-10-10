#!/usr/bin/env python3
"""Push audio into the XF16Cam RTSP audio backchannel (XF16CAM_TALK builds).

Speaks just enough RTSP to exercise the ONVIF-style backchannel the camera
offers when DESCRIBE carries "Require: www.onvif.org/ver20/backchannel":

  OPTIONS, DESCRIBE (with Require), SETUP track1 (video, required for PLAY),
  SETUP track3 (the a=sendonly audio track), PLAY, then interleaved RTP/PCMU
  at 50 packets/s on the backchannel data channel, then TEARDOWN.

Every RTSP response is checked, incoming video is drained and discarded, and
the camera's /api/system is polled while sending, so the speaker turning on
and off, the video frame counter and a watchdog reboot are all visible here
without a serial console. The summary at the end says what the camera
accepted. Deterministic, so it is the first thing to run before involving
go2rtc.

    python tools/xf16cam/rtsp_talk.py --tone 5
    python tools/xf16cam/rtsp_talk.py hello.wav
    python tools/xf16cam/rtsp_talk.py --tone 5 --watch      # full timeline
    python tools/xf16cam/rtsp_talk.py --video-only 8        # control run, no talk
    python tools/xf16cam/rtsp_talk.py --camera 192.168.0.42 --tone 5

The WAV must be 8 kHz mono 16-bit PCM (ffmpeg -i in.wav -ar 8000 -ac 1 out.wav).
No third-party modules are needed.
"""
import argparse
import json
import math
import socket
import struct
import sys
import threading
import time
import urllib.request
import wave

DEFAULT_CAMERA = "192.168.0.108"
RTSP_PORT = 8554
BACKCHANNEL = "www.onvif.org/ver20/backchannel"
SAMPLES_PER_PACKET = 160
PACKET_SECONDS = SAMPLES_PER_PACKET / 8000.0
VIDEO_CHANNEL = 0
TALK_CHANNEL = 4
LINGER_SECONDS = 35.0   # longer than the camera's 30 s capture-stall watchdog
# PLAY re-powers the camera rail and reloads the whole sensor register table
# over SCCB when no client has held the camera, which can take several seconds
# from cold. Wait well past that so a slow-but-healthy cold start is not
# mistaken for a hang (and not abandoned, which would strand the client slot).
HANDSHAKE_TIMEOUT = 20.0


def mulaw_encode(sample):
    """ITU-T G.711 mu-law, same algorithm as the firmware encoder."""
    ends = (0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF)
    if sample < 0:
        sample = -sample
        mask = 0x7F
    else:
        mask = 0xFF
    sample = min(sample, 32635) + 132
    segment = 0
    while segment < 8 and sample > ends[segment]:
        segment += 1
    return ((segment << 4) | ((sample >> (segment + 3)) & 0x0F)) ^ mask


def tone_samples(seconds, frequency=440.0, amplitude=0.4):
    total = int(seconds * 8000)
    for i in range(total):
        # Gentle fade in/out so the speaker does not click.
        envelope = min(1.0, i / 400.0, (total - i) / 400.0)
        yield int(amplitude * envelope * 32767 * math.sin(2 * math.pi * frequency * i / 8000.0))


def wav_samples(path):
    with wave.open(path, "rb") as wav:
        if wav.getnchannels() != 1 or wav.getframerate() != 8000 or wav.getsampwidth() != 2:
            raise SystemExit(f"{path}: need 8 kHz mono 16-bit PCM "
                             "(ffmpeg -i in.wav -ar 8000 -ac 1 out.wav)")
        while True:
            frames = wav.readframes(4096)
            if not frames:
                return
            for (sample,) in struct.iter_unpack("<h", frames):
                yield sample


def system_status(camera):
    """The camera's /api/system JSON plus the request latency, or (None, ms)."""
    started = time.monotonic()
    try:
        with urllib.request.urlopen(f"http://{camera}/api/system", timeout=3) as reply:
            data = json.load(reply)
    except (OSError, ValueError):
        data = None
    return data, (time.monotonic() - started) * 1000.0


class RtspClient:
    def __init__(self, camera, verbose):
        self.url = f"rtsp://{camera}:{RTSP_PORT}/stream"
        self.verbose = verbose
        self.cseq = 0
        self.sock = socket.create_connection((camera, RTSP_PORT), timeout=5)
        # Reads (notably the PLAY reply, which waits for a cold camera start)
        # get a generous timeout so a slow re-init is not read as a hang.
        self.sock.settimeout(HANDSHAKE_TIMEOUT)
        self.buffer = b""
        self.session = None

    def send(self, method, url=None, headers=()):
        """Send a request without waiting for the reply. After PLAY the socket
        also carries interleaved video, so a reply cannot be parsed reliably;
        TEARDOWN is sent this way and the camera closes the session anyway."""
        self.cseq += 1
        lines = [f"{method} {url or self.url} RTSP/1.0", f"CSeq: {self.cseq}"]
        if self.session:
            lines.append(f"Session: {self.session}")
        lines.extend(headers)
        lines.append("User-Agent: xf16cam-rtsp-talk")
        data = ("\r\n".join(lines) + "\r\n\r\n").encode()
        if self.verbose:
            print(">>", data.decode().replace("\r\n", "\n").rstrip())
        self.sock.sendall(data)

    def request(self, method, url=None, headers=()):
        self.send(method, url, headers)
        return self.response()

    def response(self):
        # The handshake happens before PLAY, so no interleaved data is mixed in.
        while b"\r\n\r\n" not in self.buffer:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise SystemExit(
                    f"no reply within {HANDSHAKE_TIMEOUT:.0f} s. If this was PLAY, the "
                    "camera is either doing an unusually slow cold sensor init or is "
                    "hung; watch /api/system for a reboot in the next ~30 s.")
            except OSError as exc:
                raise SystemExit(
                    f"camera reset the connection during the RTSP handshake ({exc}). "
                    "It is usually wedged from a previous unclean disconnect and all "
                    "three client slots are taken; wait ~30 s for its watchdog to "
                    "reboot it, then retry.")
            if not chunk:
                raise SystemExit(
                    "camera closed the connection during the RTSP handshake: it is "
                    "usually at its 3-client limit from a previous unclean disconnect. "
                    "Wait ~30 s for its watchdog to reboot it, then retry.")
            self.buffer += chunk
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        head = head.decode(errors="replace")
        headers = {}
        for line in head.split("\r\n")[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        while len(self.buffer) < length:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SystemExit("connection closed reading RTSP body")
            self.buffer += chunk
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        status = int(head.split(" ", 2)[1])
        if self.verbose:
            print("<<", head.replace("\r\n", "\n").rstrip())
            if body:
                print(body.decode(errors="replace").rstrip())
        if "session" in headers:
            self.session = headers["session"].split(";")[0]
        return status, headers, body.decode(errors="replace")


def expect(status, wanted, what):
    if status != wanted:
        raise SystemExit(f"{what}: expected {wanted}, got {status}")
    print(f"{what}: {status}")


def drain(sock, stop, stats):
    """Read and discard the camera's interleaved video so TCP keeps flowing."""
    sock.settimeout(1.0)
    while not stop.is_set():
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            return
        if not chunk:
            stats["closed"] = True
            return
        stats["received"] += len(chunk)


class Watcher(threading.Thread):
    """Polls /api/system twice a second. Always reports speaker on/off
    transitions and a reboot; with verbose=True prints the whole timeline:
    API latency, talk counters, video frame counter and frame age."""

    def __init__(self, camera, start, verbose):
        super().__init__(daemon=True)
        self.camera = camera
        self.start_time = start
        self.verbose = verbose
        self.stop = threading.Event()
        self.events = []        # (t, active)
        self.rebooted = False
        self.api_failures = 0
        self.frames = []        # (t, frames)
        self.last_active = None
        self.last_up = None

    def run(self):
        while not self.stop.is_set():
            data, latency = system_status(self.camera)
            t = time.monotonic() - self.start_time
            if data is None:
                self.api_failures += 1
                print(f"  [{t:5.1f} s] api: no reply within 3 s")
            else:
                talk = data.get("talk") or {}
                media = data.get("media", {})
                up = data.get("up")
                if self.last_up is not None and up is not None and up < self.last_up:
                    self.rebooted = True
                    print(f"  [{t:5.1f} s] *** camera REBOOTED (uptime {up} s, boot: {data.get('boot')}) ***")
                self.last_up = up
                active = talk.get("active")
                if active != self.last_active:
                    self.last_active = active
                    self.events.append((t, active))
                    print(f"  [{t:5.1f} s] speaker {'ON' if active else 'off'}")
                self.frames.append((t, media.get("frames")))
                if self.verbose:
                    # last_ms is the last frame's absolute uptime timestamp,
                    # re-stamped when the camera is acquired; the stall
                    # watchdog reboots when a session holds the camera and
                    # up*1000 - last_ms passes 30000. Before that fix a
                    # client arriving 30 s after the previous session's last
                    # frame was rebooted on the spot.
                    last_ms = media.get("last_ms")
                    clients = media.get("clients")
                    stale = up * 1000 - last_ms if (up is not None and last_ms is not None) else None
                    margin = "" if not clients or stale is None else f" stale={stale} ms (reboot at 30000)"
                    print(f"  [{t:5.1f} s] api {latency:4.0f} ms  talk packets={talk.get('packets')} "
                          f"dropped={talk.get('dropped')} underruns={talk.get('underruns')} "
                          f"active={active}  video frames={media.get('frames')} "
                          f"clients={clients} up={up}{margin}")
            self.stop.wait(0.5)


def print_frame_summary(watcher):
    samples = [f for f in watcher.frames if f[1] is not None]
    if len(samples) < 2:
        return
    first, last = samples[0], samples[-1]
    gained = last[1] - first[1]
    span = last[0] - first[0]
    rate = gained / span if span > 0 else 0.0
    # Longest stretch with no new frame.
    longest, since = 0.0, samples[0]
    for sample in samples[1:]:
        if sample[1] != since[1]:
            since = sample
        longest = max(longest, sample[0] - since[0])
    print(f"video: {gained} frames in {span:.1f} s ({rate:.1f} fps), "
          f"longest gap without a frame {longest:.1f} s")


def close_session(client, stop, reader):
    stop.set()
    reader.join(timeout=2)
    try:
        client.send("TEARDOWN")
    except OSError:
        pass
    client.sock.close()


def linger_watch(linger):
    if linger > 0:
        print(f"session closed; watching the camera for {linger:.0f} s more (watchdog window)...")
        time.sleep(linger)


def play_session(client, samples, start, linger):
    stop = threading.Event()
    stats = {"received": 0, "closed": False}
    reader = threading.Thread(target=drain, args=(client.sock, stop, stats), daemon=True)
    reader.start()

    sequence = 1
    timestamp = 0
    ssrc = 0x54414C4B  # "TALK"
    sent = 0
    packet = bytearray(SAMPLES_PER_PACKET)
    fill = 0
    print("sending...")
    try:
        for sample in samples:
            packet[fill] = mulaw_encode(sample)
            fill += 1
            if fill < SAMPLES_PER_PACKET:
                continue
            fill = 0
            header = struct.pack("!BBHII", 0x80, 0, sequence & 0xFFFF, timestamp & 0xFFFFFFFF, ssrc)
            frame = b"$" + bytes((TALK_CHANNEL,)) + struct.pack("!H", 12 + SAMPLES_PER_PACKET) + header + packet
            client.sock.sendall(frame)
            sequence += 1
            timestamp += SAMPLES_PER_PACKET
            sent += 1
            # Pace to real time so the camera's jitter ring never overflows.
            due = start + sent * PACKET_SECONDS
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            if stats["closed"]:
                raise SystemExit("camera closed the connection while sending")
        # Keep the session open long enough to see the speaker switch off
        # (the camera closes it 1.5 s after the last packet).
        time.sleep(2.5)
    finally:
        close_session(client, stop, reader)
    elapsed = time.monotonic() - start
    print(f"sent {sent} PCMU packets ({sent * PACKET_SECONDS:.1f} s of audio) in {elapsed:.1f} s; "
          f"received {stats['received']} bytes of video")
    linger_watch(linger)
    return sent


def video_only_session(client, seconds, linger):
    stop = threading.Event()
    stats = {"received": 0, "closed": False}
    reader = threading.Thread(target=drain, args=(client.sock, stop, stats), daemon=True)
    reader.start()
    print(f"receiving video only for {seconds:.0f} s...")
    try:
        time.sleep(seconds)
    finally:
        close_session(client, stop, reader)
    print(f"received {stats['received']} bytes of video")
    linger_watch(linger)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("wav", nargs="?", help="8 kHz mono 16-bit WAV to play")
    parser.add_argument("--camera", default=DEFAULT_CAMERA, metavar="IP",
                        help=f"camera address (default {DEFAULT_CAMERA})")
    parser.add_argument("--tone", type=float, metavar="SECONDS",
                        help="send a 440 Hz tone instead of a WAV file")
    parser.add_argument("--video-only", type=float, metavar="SECONDS",
                        help="control run: PLAY video for SECONDS without any backchannel")
    parser.add_argument("--no-require", action="store_true",
                        help="omit the Require header to confirm the track is not offered")
    parser.add_argument("--watch", action="store_true",
                        help="print the full /api/system timeline every 0.5 s")
    parser.add_argument("--linger", type=float, default=None, metavar="SECONDS",
                        help=f"keep polling after the session closes (default {LINGER_SECONDS:.0f} "
                             "with --watch or --video-only, else 0)")
    parser.add_argument("--verbose", "-v", action="store_true", help="print every RTSP message")
    args = parser.parse_args()
    if args.tone is None and not args.wav and args.video_only is None:
        parser.error("give a WAV file, --tone SECONDS or --video-only SECONDS")
    linger = args.linger
    if linger is None:
        linger = LINGER_SECONDS if (args.watch or args.video_only is not None) else 0.0

    before, _ = system_status(args.camera)
    before_talk = None if before is None else before.get("talk")
    if before is None:
        print(f"note: http://{args.camera}/api/system is unreachable")
    elif before_talk is None:
        print("note: /api/system has no talk object (not a talk build?)")
    else:
        print(f"camera up {before['up']} s, boot: {before.get('boot')}, "
              f"video frames so far {before['media']['frames']}")
        print("talk before:", json.dumps(before_talk))

    client = RtspClient(args.camera, args.verbose)
    status, headers, _ = client.request("OPTIONS")
    expect(status, 200, "OPTIONS")

    want_talk = args.video_only is None and not args.no_require
    require = (f"Require: {BACKCHANNEL}",) if want_talk else ()
    status, headers, sdp = client.request("DESCRIBE", headers=("Accept: application/sdp",) + require)
    expect(status, 200, "DESCRIBE")
    offered = "a=control:track3" in sdp and "a=sendonly" in sdp
    print(f"backchannel track offered: {'yes' if offered else 'no'}")
    if args.no_require:
        if offered:
            raise SystemExit("FAIL: track3 offered without the Require header")
        print("PASS: no backchannel without Require")
        client.request("TEARDOWN")
        return
    if want_talk and not offered:
        raise SystemExit("FAIL: DESCRIBE did not offer the sendonly track3 "
                         "(is this an XF16CAM_TALK build in RTSP mode?)")
    base = headers.get("content-base", client.url + "/")

    status, _, _ = client.request(
        "SETUP", base + "track1",
        headers=(f"Transport: RTP/AVP/TCP;unicast;interleaved={VIDEO_CHANNEL}-{VIDEO_CHANNEL + 1}",))
    expect(status, 200, "SETUP track1 (video)")
    if want_talk:
        status, _, _ = client.request(
            "SETUP", base + "track3",
            headers=(f"Transport: RTP/AVP/TCP;unicast;interleaved={TALK_CHANNEL}-{TALK_CHANNEL + 1}",))
        if status == 503:
            raise SystemExit("SETUP track3: 503, another client already holds the talk slot")
        expect(status, 200, "SETUP track3 (backchannel)")
    status, _, _ = client.request("PLAY", headers=("Range: npt=0.000-",))
    expect(status, 200, "PLAY")

    start = time.monotonic()
    watcher = Watcher(args.camera, start, args.watch)
    if before is not None:
        watcher.start()

    if args.video_only is not None:
        video_only_session(client, args.video_only, linger)
        watcher.stop.set()
        watcher.join(timeout=4)
        print_frame_summary(watcher)
        if watcher.rebooted:
            print("FAIL: the camera rebooted during or after a video-only session")
            sys.exit(1)
        print("video-only run finished; compare its frame rate with a --tone run")
        return

    samples = tone_samples(args.tone) if args.tone is not None else wav_samples(args.wav)
    sent = play_session(client, samples, start, linger)
    watcher.stop.set()
    watcher.join(timeout=4)

    after, _ = system_status(args.camera)
    if before_talk is None or after is None or after.get("talk") is None:
        return
    after_talk = after["talk"]
    print("talk after: ", json.dumps(after_talk))
    print_frame_summary(watcher)
    if watcher.rebooted:
        print("FAIL: the camera rebooted (the 30 s capture-stall watchdog fires when a client is "
              "connected and no frame is captured; see the frame timeline above)")
        sys.exit(1)
    accepted = after_talk["packets"] - before_talk["packets"]
    dropped = after_talk["dropped"] - before_talk["dropped"]
    underruns = after_talk["underruns"] - before_talk["underruns"]
    errors = after_talk["errors"] - before_talk["errors"]
    on = [t for t, active in watcher.events if active]
    off = [t for t, active in watcher.events if not active and t > 0.5]
    verdict = []
    if accepted != sent:
        verdict.append(f"camera accepted {accepted} of {sent} packets")
    if dropped:
        verdict.append(f"{dropped} dropped")
    if errors:
        verdict.append(f"{errors} write errors")
    # The firmware keeps the DAC open for 1.5 s after the last packet and
    # counts every silent 20 ms frame it writes meanwhile, so one talk burst
    # ends with about 75 underruns by design. More than that means gaps.
    if underruns > 100:
        verdict.append(f"{underruns} underruns (about 75 are the silence tail, the rest are gaps)")
    if watcher.api_failures:
        verdict.append(f"{watcher.api_failures} API polls got no reply")
    if not on:
        verdict.append("speaker never reported active")
    elif not off:
        verdict.append("speaker did not switch off after the audio ended")
    if verdict:
        print("FAIL:", "; ".join(verdict))
        sys.exit(1)
    print(f"PASS: {accepted} packets played, {underruns} underruns (about 75 is the normal "
          f"silence tail), speaker on at {on[0]:.1f} s and off at {off[0]:.1f} s")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
