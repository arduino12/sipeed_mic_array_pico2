"""Flash the firmware by UF2 copy (picotool has no WinUSB driver on this PC).

    python tools/flash.py [pico2|zero|path/to/firmware.uf2]   (default: pico2)

If no RP2350 drive is mounted, the board is put into BOOTSEL with the 1200 bps
touch first. Waits for the board to come back and prints its COM port.
"""
import ctypes
import os
import shutil
import string
import sys
import time

import serial

sys.path.insert(0, os.path.dirname(__file__))
import micarray  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def uf2_path(arg):
    """A PlatformIO env name (pico2, zero) or a UF2 file."""
    if arg.lower().endswith(".uf2"):
        return arg
    return os.path.join(ROOT, ".pio", "build", arg, "firmware.uf2")


def find_drive(label="RP2350"):
    buf = ctypes.create_unicode_buffer(64)
    for letter in string.ascii_uppercase:
        root = f"{letter}:\\"
        if not os.path.exists(root):
            continue
        if ctypes.windll.kernel32.GetVolumeInformationW(root, buf, 64, None, None, None, None, 0):
            if buf.value == label:
                return root
    return None


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        r = fn()
        if r:
            return r
        time.sleep(0.25)
    return None


def main():
    uf2 = uf2_path(sys.argv[1] if len(sys.argv) > 1 else "pico2")
    if not os.path.exists(uf2):
        sys.exit(f"missing {uf2} -- run `pio run -e <env>` first")
    drive = find_drive()
    if not drive:
        port = micarray.find_port()
        if not port:
            sys.exit("no RP2350 drive and no board COM port: hold BOOTSEL while plugging in")
        print(f"1200 bps touch on {port}")
        try:
            s = serial.Serial(port, 1200)
            s.dtr = False
            s.close()
        except serial.SerialException as e:
            print(f"  ({e})")
        drive = wait_for(find_drive, 15)
        if not drive:
            sys.exit("board did not enter BOOTSEL")
    print(f"copying {os.path.basename(uf2)} -> {drive}")
    shutil.copyfile(uf2, os.path.join(drive, "firmware.uf2"))
    time.sleep(1.0)
    port = wait_for(lambda: micarray.find_port(), 20)
    print(f"board is up on {port}" if port else "board did not re-enumerate a COM port")
    sys.exit(0 if port else 1)


if __name__ == "__main__":
    main()
