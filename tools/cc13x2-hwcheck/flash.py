#!/usr/bin/env python3
"""Program a CC13x2 LaunchPad through its XDS110 and log the UART.

usage: tools/cc13x2-hwcheck/flash.py [--x7] [--serial SN] ELF SECONDS [PORT]
       tools/cc13x2-hwcheck/flash.py [--x7] [--serial SN] --no-flash SECONDS [PORT]

Flashes ELF (catch.tcl, so standby images work), resets through the core,
and prints every UART line for SECONDS with the host time since the reset.
The host times carry USB-serial latency (tens of ms, lines arrive in
bursts); hwcheck takes its timings on the chip for that reason.

PORT defaults to the XDS110's application UART: of the two serial ports
the probe (USB 0451:bef3) presents, the lower-numbered one
(/dev/cu.usbmodem...1 on macOS, /dev/ttyACM0 on Linux).  Needs openocd 0.12
and pyserial.

--x7      a CC13x2x7/CC26x2x7 part (LP-CC1312R7): its ICEPick ID differs, so
          openocd uses ti_cc13x2x7_launchpad.cfg here instead of the stock
          board file.
--serial  the XDS110's serial number (as `xdsdfu -e` or the USB serial
          string shows it), for when more than one probe is attached: it
          picks both the probe openocd drives and the UART to log.
"""
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = ['openocd', '-f', 'board/ti_cc13x2_launchpad.cfg']
BOARD_X7 = ['openocd', '-f', os.path.join(HERE, 'ti_cc13x2x7_launchpad.cfg')]


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


def xds110_uart(serial_no=None):
    ports = sorted(p.device for p in list_ports.comports()
                   if p.vid == 0x0451 and p.pid == 0xBEF3 and
                   (serial_no is None or p.serial_number == serial_no))
    return ports[:1]


def main():
    global BOARD
    args = sys.argv[1:]
    serial_no = None
    while args and args[0] in ('--x7', '--serial'):
        if args[0] == '--x7':
            BOARD = BOARD_X7
            args = args[1:]
        elif len(args) > 1:
            serial_no = args[1]
            args = args[2:]
        else:
            sys.exit(__doc__)
    if serial_no:
        BOARD = BOARD + ['-c', f'adapter serial {serial_no}']
    if len(args) < 2:
        sys.exit(__doc__)
    elf = None if args[0] == '--no-flash' else os.path.abspath(args[0])
    secs = float(args[1])
    ports = [args[2]] if len(args) > 2 else xds110_uart(serial_no)
    if not ports:
        sys.exit('no XDS110 UART found' +
                 (f' with serial number {serial_no}' if serial_no else ''))
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
