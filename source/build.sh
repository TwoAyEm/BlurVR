#!/bin/sh
# Release build of d3d9.dll (32-bit) with Zig (pip install ziglang).  Run from this folder.
python3 -m ziglang c++ -target x86-windows-gnu -O2 -s -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -shared -fno-exceptions -fno-rtti -std=c++17 -Ithird_party \
  -o d3d9.dll src/*.cpp d3d9.def -ld3d11 -ldxgi -lgdi32 -luser32 -lkernel32
