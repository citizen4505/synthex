"""
tools/print_memory.py
PlatformIO post-build skript — vypíše využití Flash a RAM po každém buildu.

Použití v platformio.ini:
    extra_scripts = post:tools/print_memory.py
"""

Import("env")
import subprocess
import os
import re

def print_memory_usage(source, target, env):
    # Cesta k ELF souboru (PlatformIO ho vždy generuje)
    elf_path = str(target[0])
    
    # Cesta k ARM size utility (součást toolchainu PlatformIO)
    size_bin = env.get("SIZETOOL", "arm-none-eabi-size")
    
    try:
        result = subprocess.run(
            [size_bin, "-B", elf_path],
            capture_output=True, text=True
        )
        lines = result.stdout.strip().splitlines()
        if len(lines) < 2:
            return

        # Formát -B:  text   data    bss    dec    hex  filename
        parts = lines[1].split()
        text  = int(parts[0])   # Flash: kód + rodata (wavetables!)
        data  = int(parts[1])   # RAM:   inicializované proměnné
        bss   = int(parts[2])   # RAM:   neinicializované proměnné

        flash_used = text + data
        ram_used   = data + bss

        # Arduino Due: 512 KB Flash, 96 KB RAM
        FLASH_TOTAL = 512 * 1024
        RAM_TOTAL   =  96 * 1024

        flash_pct = flash_used / FLASH_TOTAL * 100
        ram_pct   = ram_used   / RAM_TOTAL   * 100

        bar_width = 30
        def bar(pct):
            filled = int(pct / 100 * bar_width)
            return "█" * filled + "░" * (bar_width - filled)

        print()
        print("┌─────────────────────────────────────────────┐")
        print("│           BareMetalCore — Memory            │")
        print("├─────────────────────────────────────────────┤")
        print(f"│ Flash  {bar(flash_pct)} {flash_pct:5.1f}% │")
        print(f"│        {flash_used:>6} / {FLASH_TOTAL:<6} B"
              f"  ({flash_used//1024} KB / {FLASH_TOTAL//1024} KB)    │")
        print("│                                             │")
        print(f"│ RAM    {bar(ram_pct)} {ram_pct:5.1f}% │")
        print(f"│        {ram_used:>6} / {RAM_TOTAL:<6} B"
              f"  ({ram_used//1024} KB / {RAM_TOTAL//1024} KB)     │")
        print("└─────────────────────────────────────────────┘")
        print()

    except FileNotFoundError:
        print(f"[memory] WARN: '{size_bin}' nenalezen, přeskoč výpis paměti.")
    except Exception as e:
        print(f"[memory] WARN: {e}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", print_memory_usage)
