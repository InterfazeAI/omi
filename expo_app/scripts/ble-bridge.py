"""Exposes an Omi DevKit v2's storage characteristic over stdio, so the app's own
TypeScript sync engine can be driven against real hardware from Node.

The phone talks to the device through react-native-ble-plx; there is no such
thing on a Mac, so this stands in for it. Everything above the transport --
command encoding, the resume plan, block accounting, the frame parser -- is the
same code that runs on the phone.

Protocol, one JSON object per line each way:

    in   {"op": "info",  "id": N}      read the 21-byte storage info
         {"op": "pairing", "id": N}    read the pairing status, which never needs encryption
         {"op": "write", "id": N, "data": "<base64>"}
         {"op": "quit"}
    out  {"t": "ready",   "address": "...", "mtu": N}
         {"t": "info",    "id": N, "data": "<base64>"}
         {"t": "pairing", "id": N, "data": "<base64>"}
         {"t": "written", "id": N}
         {"t": "notify",  "data": "<base64>"}
         {"t": "error",   "id": N|null, "message": "..."}

Run it with the bleak venv from omi/firmware/scripts/devkit/sd_sync.
"""

import asyncio
import base64
import json
import sys
import threading
import time

from bleak import BleakClient, BleakScanner

AUDIO_SVC = "19b10000-e8f2-537e-4f6c-d104768a1214"
CMD_CHAR = "30295781-4301-eabd-2904-2849adfeae43"
SIZE_CHAR = "30295782-4301-eabd-2904-2849adfeae43"
PAIRING_STATUS_CHAR = "19b10041-e8f2-537e-4f6c-d104768a1214"


def emit(**payload):
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


async def find_device(timeout=60.0):
    """The device only advertises once its SD card has mounted."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = await BleakScanner.discover(timeout=6.0, return_adv=True)
        for _, (device, adv) in found.items():
            if AUDIO_SVC in [uuid.lower() for uuid in (adv.service_uuids or [])]:
                return device
    return None


def stdin_reader(loop, queue):
    for line in sys.stdin:
        line = line.strip()
        if line:
            loop.call_soon_threadsafe(queue.put_nowait, line)
    loop.call_soon_threadsafe(queue.put_nowait, None)


async def main():
    device = await find_device()
    if device is None:
        emit(t="error", id=None, message="no Omi DevKit advertising")
        return 1

    async with BleakClient(device) as client:
        # Notifications must be flowing before the first READ, or its opening
        # blocks arrive with nobody listening.
        await client.start_notify(
            CMD_CHAR,
            lambda _, data: emit(t="notify", data=base64.b64encode(bytes(data)).decode()),
        )
        emit(t="ready", address=str(device.address), mtu=getattr(client, "mtu_size", 0))

        loop = asyncio.get_running_loop()
        queue: asyncio.Queue = asyncio.Queue()
        threading.Thread(target=stdin_reader, args=(loop, queue), daemon=True).start()

        while True:
            line = await queue.get()
            if line is None:
                break

            request = json.loads(line)
            op = request.get("op")
            request_id = request.get("id")

            if op == "quit":
                break

            try:
                if op == "info":
                    raw = await client.read_gatt_char(SIZE_CHAR)
                    emit(t="info", id=request_id, data=base64.b64encode(bytes(raw)).decode())
                elif op == "pairing":
                    raw = await client.read_gatt_char(PAIRING_STATUS_CHAR)
                    emit(t="pairing", id=request_id, data=base64.b64encode(bytes(raw)).decode())
                elif op == "write":
                    await client.write_gatt_char(
                        CMD_CHAR, base64.b64decode(request["data"]), response=True
                    )
                    emit(t="written", id=request_id)
            except Exception as error:  # surfaced to Node, which decides what is fatal
                emit(t="error", id=request_id, message=f"{type(error).__name__}: {error}")

        try:
            await client.stop_notify(CMD_CHAR)
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
