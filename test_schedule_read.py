#!/usr/bin/env python3
"""Quick script to test schedule reading from pump."""
import asyncio
import sys
sys.path.insert(0, 'reference/alpha-hwr/src')

from alpha_hwr.client import AlphaHWRClient

async def main():
    address = "3C:E0:02:50:98:BF"
    print(f"Connecting to pump at {address}...")
    
    async with AlphaHWRClient(address) as client:
        print("Connected! Reading schedule from layer 0...")
        entries = await client.schedule.read_entries(layer=0)
        
        if entries:
            print(f"\nFound {len(entries)} schedule entries:")
            for entry in entries:
                print(f"  {entry.day}: {entry.begin_time}-{entry.end_time} "
                      f"(enabled={entry.enabled}, action=0x{entry.action:02X})")
        else:
            print("\nNo schedule entries found on layer 0")

if __name__ == "__main__":
    asyncio.run(main())
