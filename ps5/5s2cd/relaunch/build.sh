#!/bin/bash
# relaunch.elf: goes to /data/PCSX2/relaunch.elf (5S2CD sends it to the ELF loader when you go back to the menu).
set -e
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the ps5-payload-dev SDK}"
cd "$(dirname "$0")"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -O2 -o relaunch.elf relaunch.c -lSceSystemService -lSceUserService
ls -la relaunch.elf
