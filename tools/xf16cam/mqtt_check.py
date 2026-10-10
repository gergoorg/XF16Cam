#!/usr/bin/env python3
"""Watch an XF16Cam's MQTT traffic and sanity-check it.

Subscribes to the camera's topics and Home Assistant discovery, validates
the state document's key set, saves the newest JPEG, reports publish
intervals, and can send commands. Needs paho-mqtt (pip install paho-mqtt).

    python tools/xf16cam/mqtt_check.py --broker 192.168.0.2
    python tools/xf16cam/mqtt_check.py --broker 192.168.0.2 --cmd led ON
    python tools/xf16cam/mqtt_check.py --broker 192.168.0.2 --cmd ptz home
"""
import argparse
import json
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required: pip install paho-mqtt")

STATE_KEYS = {
    "id", "ver", "media", "mjpeg", "stream", "cam", "res", "led", "ir", "ip",
    "up", "temp", "heap", "sd", "bat", "clients", "frames", "ptz", "boot",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--broker", required=True, help="broker IP or hostname")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--user")
    parser.add_argument("--password")
    parser.add_argument("--host", help="camera id (XF16CAM-XXXXXX); default: any")
    parser.add_argument("--image", default="xf16cam-last.jpg",
                        help="where the newest JPEG is written")
    parser.add_argument("--cmd", nargs=2, metavar=("NAME", "PAYLOAD"),
                        help="publish one command (led|ir|ptz|reboot) and exit")
    parser.add_argument("--seconds", type=int, default=0,
                        help="stop after this many seconds (0 = run until Ctrl-C)")
    args = parser.parse_args()

    base = "xf16cam/%s" % (args.host or "+")
    last = {}

    def on_connect(client, userdata, flags, rc, properties=None):
        print("connected to broker, rc=%s" % rc)
        client.subscribe([(base + "/#", 0), ("homeassistant/device/+/config", 0)])
        if args.cmd:
            if not args.host:
                sys.exit("--cmd needs --host")
            topic = "%s/cmd/%s" % (base, args.cmd[0])
            client.publish(topic, args.cmd[1])
            print("sent %s = %s" % (topic, args.cmd[1]))
            client.disconnect()

    def on_message(client, userdata, msg):
        now = time.time()
        leaf = msg.topic.rsplit("/", 1)[-1]
        gap = ""
        if msg.topic in last:
            gap = " (+%.1fs)" % (now - last[msg.topic])
        last[msg.topic] = now
        stamp = time.strftime("%H:%M:%S")
        if leaf == "image":
            data = msg.payload
            ok = data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"
            with open(args.image, "wb") as out:
                out.write(data)
            print("%s %s: %d bytes, %s%s" % (stamp, msg.topic, len(data),
                                             "valid JPEG" if ok else "NOT A JPEG",
                                             gap))
        elif leaf == "state":
            try:
                doc = json.loads(msg.payload.decode())
            except ValueError as error:
                print("%s %s: invalid JSON (%s)%s" % (stamp, msg.topic, error, gap))
                return
            missing = STATE_KEYS - set(doc)
            extra = set(doc) - STATE_KEYS
            verdict = "ok" if not missing and not extra else \
                "missing %s extra %s" % (sorted(missing), sorted(extra))
            print("%s %s: media=%s led=%s ir=%s stream=%s up=%s keys %s%s" % (
                stamp, msg.topic, doc.get("media"), doc.get("led"), doc.get("ir"),
                doc.get("stream") or doc.get("mjpeg"), doc.get("up"), verdict, gap))
        elif leaf == "config":
            try:
                doc = json.loads(msg.payload.decode())
                cmps = sorted(doc.get("cmps", {}))
                print("%s %s: %d bytes, %d components: %s" % (
                    stamp, msg.topic, len(msg.payload), len(cmps), " ".join(cmps)))
            except ValueError as error:
                print("%s %s: invalid JSON (%s)" % (stamp, msg.topic, error))
        else:
            print("%s %s: %s%s" % (stamp, msg.topic, msg.payload.decode(errors="replace"), gap))

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()
    if args.user:
        client.username_pw_set(args.user, args.password)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 60)
    try:
        if args.seconds:
            deadline = time.time() + args.seconds
            while time.time() < deadline:
                client.loop(timeout=1.0)
        else:
            client.loop_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
