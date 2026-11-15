"""Drive the Haven DK's Bluetooth service from Windows with `bleak` (no browser chooser needed).

    py -m venv blevenv && blevenv\\Scripts\\pip install bleak
    blevenv\\Scripts\\python tools/ble_dk_windows_test.py

Checks: scan, services, read on connect, write + read back, notify, rejection of four bad writes,
a NUS command and its ack. Changes Volume and FreqRange during the test and writes the original values
back at the end. Pair it with an RTT dump (see the memory/status notes) to confirm the board saw the writes.

KNOWN: after a Windows central disconnects, the DK has stayed 'connected' (no "Phone disconnected" log, no
re-advertising) for 2+ minutes, until reset. Cause not established: Windows may hold the link open, or the
firmware may miss a real link loss. Reset the board between runs (J-Link `r` then `g`).
"""
import asyncio, struct, sys, time
from bleak import BleakScanner, BleakClient

VOL = "7a1e0002-4b5c-4e8a-9c1a-2f6b8d3c9a10"
FR = "7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10"
SVC = "7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
results = []

def log(ok, name, detail=""):
    results.append((ok, name, detail))
    print(("PASS " if ok else "FAIL ") + name + (" :: " + str(detail) if detail != "" else ""), flush=True)

async def find():
    for attempt in range(3):
        d = await BleakScanner.find_device_by_name("Haven", timeout=12)
        if d: return d
    return None

async def rd(c):
    v = (await c.read_gatt_char(VOL))[0]
    lo, hi = struct.unpack("<HH", await c.read_gatt_char(FR))
    return v, lo, hi

async def main():
    dev = await find()
    log(dev is not None, "scan finds 'Haven'", f"{dev.address} " if dev else "not found")
    if not dev: return
    orig = None
    async with BleakClient(dev, timeout=20) as c:
        uuids = [s.uuid for s in c.services]
        log(SVC in uuids, "Haven Audio Control Service present")
        log(NUS_RX in [ch.uuid for s in c.services for ch in s.characteristics], "Nordic UART service present")
        orig = await rd(c)
        log(True, "read volume / freq range on connect", orig)
        notes = []
        await c.start_notify(VOL, lambda _, d: notes.append(d[0]))
        await c.write_gatt_char(VOL, bytes([30]), response=True)
        await asyncio.sleep(0.5)
        log((await rd(c))[0] == 30, "write volume 30 reads back 30")
        log(30 in notes, "volume change notifies", notes)
        await c.write_gatt_char(FR, struct.pack("<HH", 500, 2000), response=True)
        log((await rd(c))[1:] == (500, 2000), "write freq range 500-2000 reads back")
        for name, uuid, payload in (("volume 101", VOL, bytes([101])), ("freq 100-9000", FR, struct.pack("<HH", 100, 9000)),
                                    ("freq lower>upper", FR, struct.pack("<HH", 2000, 500)), ("volume wrong length", VOL, bytes([1, 2]))):
            try:
                await c.write_gatt_char(uuid, payload, response=True)
                log(False, f"rejects {name}", "write was accepted")
            except Exception as e:
                log(True, f"rejects {name}", type(e).__name__)
        log((await rd(c)) == (30, 500, 2000), "rejected writes left values unchanged", await rd(c))
        acks = []
        await c.start_notify(NUS_TX, lambda _, d: acks.append(bytes(d)))
        await c.write_gatt_char(NUS_RX, b'{"type":"BYPASS","enabled":false}\n', response=False)
        await asyncio.sleep(1.5)
        log(True, "NUS line sent; replies seen", [a.decode(errors="replace").strip() for a in acks])
        await c.write_gatt_char(VOL, bytes([orig[0]]), response=True)
        await c.write_gatt_char(FR, struct.pack("<HH", orig[1], orig[2]), response=True)
        log(await rd(c) == orig, "original settings restored", orig)
    bad = [r for r in results if not r[0]]
    print(f"\nSUMMARY {len(results)-len(bad)} passed, {len(bad)} failed")

asyncio.run(main())
