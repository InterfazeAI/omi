#!/usr/bin/env python3
"""Show why pairing did or did not work, and hand the device to a new owner.

    pairing.py             read status (works on an unpaired link)
    pairing.py --pair      force the encrypted read that makes the host raise its pairing prompt
    pairing.py --release   give up the bond slot, so another device can pair

The status read deliberately does not need pairing. Everything else on the device does, which is
the whole point: the diagnostic has to work when the security does not.
"""
import argparse
import asyncio

from bleak import BleakClient

import omi_sd


async def show(client):
    st = await omi_sd.read_pairing_status(client)

    print(f"  build         SMP {'on' if st.smp_enabled else 'OFF'}, "
          f"bondable {'yes' if st.bondable else 'no'}, "
          f"persistent bonds {'yes' if st.settings_enabled else 'no'}")
    print(f"  bonds         {st.bond_count} of {st.max_bonds} slots used")
    print(f"  this link     security level {st.current_security_level} "
          f"({'encrypted' if st.link_encrypted else 'NOT encrypted'})")
    print(f"  counters      {st.connections:,} connections, "
          f"{st.pairing_successes:,} pairings succeeded, {st.pairing_failures:,} failed")

    if st.pairing_failures or st.last_pairing_err:
        print(f"  last pairing  {st.describe_pairing_err()}")
    if st.last_security_err:
        print(f"  last security {st.describe_security_err()} (reached level {st.last_security_level})")
    if st.unbond_requests:
        print(f"  releases      {st.unbond_requests:,} requested, last result {st.last_unbond_result}")

    # The failure this is most likely to catch. One slot, no eviction, and a bond the host has
    # forgotten looks identical to a broken stack from the host side.
    if st.slots_full and not st.link_encrypted:
        print()
        print("  >> Every bond slot is taken and this link is not the bonded one.")
        print("     New pairing attempts will be refused until the slot is released.")
        print("     Pair from the bonded device and run --release, or flash the unbond image.")
    return st


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", action="store_true",
                    help="give up the bond slot AND erase every recording")
    ap.add_argument("--pair", action="store_true", help="trigger the host's pairing prompt")
    ap.add_argument("--force", action="store_true", help="skip the confirmation for --release")
    args = ap.parse_args()

    dev = await omi_sd.find_device()
    if not dev:
        print("device not advertising")
        return 1

    async with BleakClient(dev, timeout=30.0) as client:
        st = await show(client)

        if args.pair:
            print("\n  reading an encrypted characteristic -- accept the prompt on your host")
            try:
                await omi_sd.read_info(client)
                print("  encrypted read OK -- paired")
            except Exception as exc:
                print(f"  encrypted read failed: {type(exc).__name__}: {exc}")
            print()
            await show(client)

        if args.release:
            if not st.link_encrypted:
                print("\n  refusing: releasing the bond needs the encrypted link, so only the")
                print("  current owner can do it. Pair first, or flash the unbond image.")
                return 1

            info = await omi_sd.read_info(client)
            print(f"\n  RELEASING THIS DEVICE ERASES ALL {info.newest_bytes:,} BYTES OF AUDIO.")
            print("  A new owner must not inherit your recordings, so the card is wiped first.")
            print("  Pull anything you want to keep before continuing (pull_last.py).")
            if not args.force:
                if input("  type ERASE to continue: ").strip() != "ERASE":
                    print("  cancelled, nothing changed")
                    return 1

            await omi_sd.release_bond(client)
            print("\n  wiping the card, then releasing the bond; this connection will drop")
            print("  the host keeps a stale bond -- forget the device there before re-pairing")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
