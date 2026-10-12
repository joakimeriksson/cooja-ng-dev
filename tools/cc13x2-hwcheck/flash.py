#!/usr/bin/env python3
"""Program a CC1352R1 LaunchPad through its XDS110 and log the UART.

usage: tools/cc13x2-hwcheck/flash.py ELF SECONDS [PORT]
       tools/cc13x2-hwcheck/flash.py --no-flash SECONDS [PORT]

Flashes ELF (catch.tcl, so standby images work), resets through the core,
and prints every UART line for SECONDS with the host time since the reset.
The host times carry USB-serial latency (tens of ms, lines arrive in
bursts); hwcheck takes its timings on the chip for that reason.

PORT defaults to the first /dev/cu.usbmodem*1 (the XDS110's application
UART is its lower-numbered interface).  Needs openocd 0.12 and pyserial.
"""
import glob
import os
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = ['openocd', '-f', 'board/ti_cc13x2_launchpad.cfg']


def flash(elf):
    out = ''
    for _ in range(4):   # the catch is occasionally lost; openocd may assert
        r = subprocess.run(BOARD + ['-f', os.path.join(HERE, 'catch.tcl'), '-c',
                                    f'flash write_image erase {elf}; verify_image {elf}; '
                                    'echo FLASH-VERIFIED; exit'],
                           capture_output=True, text=True)
        out = r.stdout + r.stderr
        if 'FLASH-VERIFIED' in out:
            return
    sys.exit('flash failed:\n' + out[-2000:])


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    elf = None if args[0] == '--no-flash' else os.path.abspath(args[0])
    secs = float(args[1])
    ports = [args[2]] if len(args) > 2 else sorted(glob.glob('/dev/cu.usbmodem*1'))
    if not ports:
        sys.exit('no XDS110 UART found')
    if elf:
        flash(elf)
    s = serial.Serial(ports[0], 115200, timeout=0.05)
    s.reset_input_buffer()
    t0 = time.monotonic()
    subprocess.run(BOARD + ['-c', 'init; reset run; exit'], capture_output=True)
    buf = b''
    while time.monotonic() - t0 < secs:
        buf += s.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            print(f'{time.monotonic() - t0:10.4f} {line.decode(errors="replace").rstrip()}',
                  flush=True)


if __name__ == '__main__':
    main()
