"""Shared helpers: find the board's COM port / audio device, talk to the console."""
import json
import time

import serial
import serial.tools.list_ports

VID = 0x1209
PIDS = {0xA6E1: "stereo", 0xA6E3: "raw8"}
RPI_VID = 0x2E8A  # arduino-pico default before our firmware runs
DEVICE_NAME = "Sipeed Mic Array"


def find_port(timeout=0.0):
    """COM port of the board running this firmware (or a stock arduino-pico)."""
    end = time.time() + timeout
    while True:
        ports = list(serial.tools.list_ports.comports())
        for p in ports:
            if p.vid == VID and p.pid in PIDS:
                return p.device
        for p in ports:
            if p.vid == RPI_VID:
                return p.device
        if time.time() >= end:
            return None
        time.sleep(0.25)


class Console:
    """Line console. cmd() returns the {"ok"/"err"} reply; telemetry lines are
    kept in self.telemetry (most recent last)."""

    def __init__(self, port=None, timeout=3.0):
        port = port or find_port(timeout)
        if not port:
            raise RuntimeError("mic array COM port not found")
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0.02)
        self.ser.dtr = True
        self.telemetry = []
        self._buf = b""
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _lines(self):
        """Parsed lines; each gets "_ht", the host time it was read."""
        self._buf += self.ser.read(4096)
        now = time.time()
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                msg = {"raw": line.decode(errors="replace")}
            msg["_ht"] = now
            yield msg

    def pump(self, seconds):
        """Collect telemetry for `seconds`, return the lines received."""
        out = []
        end = time.time() + seconds
        while True:  # at least one read, so pump(0) drains what is waiting
            for msg in self._lines():
                if "ok" not in msg and "err" not in msg:
                    out.append(msg)
            if time.time() >= end:
                break
        self.telemetry.extend(out)
        return out

    def cmd(self, line, timeout=2.0):
        self.ser.write((line + "\n").encode())
        end = time.time() + timeout
        while time.time() < end:
            for msg in self._lines():
                if "ok" in msg or "err" in msg:
                    return msg
                self.telemetry.append(msg)
        raise TimeoutError(f"no reply to {line!r}")


# WDM-KS talks to the USB audio driver directly: exact device samples, all
# channels. WASAPI shared mode goes through the Windows audio engine, whose
# default "Voice Clarity" effect pack narrows the 8-ch endpoint to 2 channels
# and applies AGC/noise suppression; exclusive mode avoids the effects but
# PortAudio still caps the channel count at the shared-mode format.
HOSTAPI_PREFERENCE = ("Windows WDM-KS", "Windows WASAPI")


def find_input(name=DEVICE_NAME, hostapis=HOSTAPI_PREFERENCE, timeout=15.0):
    """Returns (index, device_info) of the mic array, preferring WDM-KS. The
    audio endpoint appears a few seconds after the COM port, and PortAudio
    only scans devices at init, so rescan until it shows up."""
    import sounddevice as sd

    end = time.time() + timeout
    while True:
        devs = list(enumerate(sd.query_devices()))
        for api in hostapis:
            for i, d in devs:
                if d["max_input_channels"] and name in d["name"] and sd.query_hostapis(d["hostapi"])["name"] == api:
                    d = dict(d)
                    d["hostapi_name"] = api
                    return i, d
        if time.time() >= end:
            return None, None
        time.sleep(1.0)
        sd._terminate()
        sd._initialize()


def find_output(name="Realtek", hostapi="Windows WASAPI"):
    import sounddevice as sd

    for i, d in enumerate(sd.query_devices()):
        if d["max_output_channels"] and name in d["name"] and sd.query_hostapis(d["hostapi"])["name"] == hostapi:
            return i, d
    return None, None
