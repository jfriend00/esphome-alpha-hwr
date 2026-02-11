#!/usr/bin/env python3
"""
Test schedule write with Python reference implementation to capture actual response.
"""

import asyncio
import logging
import sys

sys.path.insert(0, "reference/alpha-hwr/src")

from alpha_hwr.client import AlphaHWRClient
from alpha_hwr.models import ScheduleEntry

# Enable debug logging to see all protocol details
logging.basicConfig(
    level=logging.DEBUG, format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)


async def main():
    # Device address
    address = "3C:E0:02:50:98:BF"
    pin = "280118"

    print(f"\n{'=' * 60}")
    print(f"Testing Schedule Write with Python Reference Implementation")
    print(f"{'=' * 60}\n")

    # Create client
    client = AlphaHWRClient(address, pin=pin)

    try:
        # Connect and authenticate
        print("Connecting to pump...")
        await client.connect()
        print("✓ Connected\n")

        # Read current schedule first
        print("Reading current schedule on layer 0...")
        current_entries = await client.schedule.read_entries(layer=0)
        print(f"✓ Current schedule has {len(current_entries)} entries\n")
        for entry in current_entries:
            print(f"  {entry}")

        # Create test schedule (single entry for Monday)
        test_entry = ScheduleEntry(
            day="Monday",
            begin_hour=6,
            begin_minute=0,
            end_hour=8,
            end_minute=0,
            action=0x02,  # Auto mode
            enabled=True,
            layer=0,
        )

        print(f"\n{'=' * 60}")
        print("Writing test schedule:")
        print(f"  {test_entry}")
        print(f"{'=' * 60}\n")

        # Write schedule (this will log the full request/response)
        success = await client.schedule.write_entries([test_entry], layer=0)

        print(f"\n{'=' * 60}")
        print(f"Write result: {'SUCCESS' if success else 'FAILED'}")
        print(f"{'=' * 60}\n")

        if success:
            # Wait a moment, then read back
            print("Waiting 1 second before reading back...")
            await asyncio.sleep(1)

            print("\nReading schedule back...")
            verify_entries = await client.schedule.read_entries(layer=0)
            print(f"✓ Readback has {len(verify_entries)} entries\n")
            for entry in verify_entries:
                print(f"  {entry}")

            if len(verify_entries) == 1:
                print("\n✓✓✓ SUCCESS: Schedule write persisted!")
            else:
                print("\n✗✗✗ FAILURE: Schedule write did NOT persist!")

    except Exception as e:
        print(f"\n✗ Error: {e}")
        import traceback

        traceback.print_exc()

    finally:
        print("\nDisconnecting...")
        await client.disconnect()
        print("✓ Disconnected\n")


if __name__ == "__main__":
    asyncio.run(main())
