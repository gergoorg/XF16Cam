#!/usr/bin/env python3
"""Exercise an XF16Cam's ESPHome native API from a PC.

Connects with aioesphomeapi (the library Home Assistant uses), prints the
device info and entity list, follows state updates, and can request an
image, toggle a light, press a button or provision a key. Needs
aioesphomeapi (pip install aioesphomeapi).

    python tools/xf16cam/esphome_check.py --host 192.168.0.108
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --key BASE64KEY
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --image out.jpg
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --stream 20
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --light led on
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --button reboot
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --provision
    python tools/xf16cam/esphome_check.py --host 192.168.0.108 --clear-key --key BASE64KEY

--provision connects with the all-zero key an unprovisioned device accepts,
sends a fresh random key, prints it, and reconnects with it.
"""
import argparse
import asyncio
import base64
import os
import sys
import time

try:
    import aioesphomeapi
    from aioesphomeapi import APIClient
except ImportError:
    sys.exit("aioesphomeapi is required: pip install aioesphomeapi")

ZERO_KEY = base64.b64encode(bytes(32)).decode()
WATCH_SECONDS = 15


def entity_map(entities):
    """object_id -> entity for every listed entity."""
    return {e.object_id: e for e in entities}


async def connect(host, port, key):
    cli = APIClient(host, port, None, noise_psk=key, client_info="xf16cam-check")
    await cli.connect(login=True)
    return cli


async def run(args):
    if args.provision:
        print("provisioning: connecting with the zero key")
        cli = await connect(args.host, args.port, ZERO_KEY)
        new_key = base64.b64encode(os.urandom(32)).decode()
        # The request carries the key's base64 text, as Home Assistant sends it.
        ok = await cli.noise_encryption_set_key(new_key.encode())
        await cli.disconnect()
        if not ok:
            sys.exit("device refused the key")
        print(f"key set, reconnect with --key {new_key}")
        args.key = new_key

    cli = await connect(args.host, args.port, args.key)
    info = await cli.device_info()
    print(f"device: {info.name} mac={info.mac_address} model={info.model} "
          f"manufacturer={info.manufacturer} firmware={info.esphome_version} "
          f"api_encryption_supported={info.api_encryption_supported} "
          f"webserver_port={info.webserver_port}")
    print(f"api version: {cli.api_version}")

    entities, services = await cli.list_entities_services()
    by_id = entity_map(entities)
    for e in entities:
        kind = type(e).__name__.replace("Info", "")
        extra = ""
        if hasattr(e, "unit_of_measurement") and e.unit_of_measurement:
            extra = f" [{e.unit_of_measurement}]"
        print(f"  {kind:14} key={e.key:2} {e.object_id:10} '{e.name}'{extra}"
              f"{' (disabled)' if e.disabled_by_default else ''}")

    if args.clear_key:
        ok = await cli.noise_encryption_set_key(b"")
        print(f"key cleared: {ok}")
        await cli.disconnect()
        return

    if args.light:
        name, state = args.light
        e = by_id.get(name)
        if e is None:
            sys.exit(f"no light '{name}'")
        cli.light_command(e.key, state=(state.lower() == "on"))
        print(f"light {name} -> {state}")

    if args.button:
        e = by_id.get(args.button)
        if e is None:
            sys.exit(f"no button '{args.button}'")
        cli.button_command(e.key)
        print(f"button {args.button} pressed")

    images = []
    started = time.monotonic()

    def on_state(state):
        if isinstance(state, aioesphomeapi.CameraState):
            images.append(state.data)
            print(f"  image {len(state.data)} bytes at +{time.monotonic() - started:.2f}s")
            return
        key = getattr(state, "key", None)
        name = next((e.object_id for e in entities if e.key == key), key)
        value = getattr(state, "state", None)
        if getattr(state, "missing_state", False):
            value = "unavailable"
        print(f"  {name}: {value}")

    cli.subscribe_states(on_state)

    if args.image:
        cli.request_single_image()
        while not images and time.monotonic() - started < 10:
            await asyncio.sleep(0.1)
        if not images:
            sys.exit("no image within 10 s")
        with open(args.image, "wb") as f:
            f.write(images[0])
        print(f"saved {args.image}")

    if args.stream:
        want = args.stream
        t0 = time.monotonic()
        while len(images) < want and time.monotonic() - t0 < want * 3:
            seen = len(images)
            cli.request_image_stream()
            while len(images) == seen and time.monotonic() - t0 < want * 3:
                await asyncio.sleep(0.02)
        elapsed = time.monotonic() - t0
        print(f"{len(images)} frames in {elapsed:.1f} s = {len(images) / elapsed:.1f} fps")

    if not (args.image or args.stream or args.light or args.button):
        print(f"watching states for {WATCH_SECONDS} s")
        await asyncio.sleep(WATCH_SECONDS)
    else:
        await asyncio.sleep(1)
    await cli.disconnect()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=6053)
    parser.add_argument("--key", help="base64 Noise key; omit for a plaintext device")
    parser.add_argument("--provision", action="store_true",
                        help="set a fresh key over the zero-key Noise session")
    parser.add_argument("--clear-key", action="store_true")
    parser.add_argument("--image", metavar="FILE", help="request one image and save it")
    parser.add_argument("--stream", type=int, metavar="N", help="request N stream frames")
    parser.add_argument("--light", nargs=2, metavar=("ID", "ON|OFF"))
    parser.add_argument("--button", metavar="ID")
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except aioesphomeapi.InvalidEncryptionKeyAPIError as e:
        sys.exit(f"invalid encryption key: {e}")
    except aioesphomeapi.RequiresEncryptionAPIError:
        sys.exit("device requires encryption: pass --key")
    except aioesphomeapi.APIConnectionError as e:
        sys.exit(f"connection failed: {e}")


if __name__ == "__main__":
    main()
