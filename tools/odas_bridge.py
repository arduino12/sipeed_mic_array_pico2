"""Run ODAS on the mic array from Windows.

    python tools/odas_bridge.py [--seconds 20] [--play]

Captures the board's 8-channel raw stream (USB mode `raw8`, via WDM-KS),
decimates 48 -> 16 kHz, pipes interleaved int16 into `odaslive` running in
WSL (odas/sipeed_6p1.cfg), and prints ODAS's tracked sources next to the
firmware's own direction estimate. --play also plays band-limited noise on
the laptop's left speaker so there is something to track.

ODAS is built in WSL by odas/build_odas_wsl.sh (~/odas_build).
"""
import argparse
import json
import math
import os
import queue
import subprocess
import sys
import threading
import time

import numpy as np
import sounddevice as sd
from scipy.signal import firwin, lfilter

sys.path.insert(0, os.path.dirname(__file__))
import micarray  # noqa: E402
from record_check import FS, stream_args  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG_WIN = os.path.join(ROOT, "odas", "sipeed_6p1.cfg")


def wsl_path(p):
    drive, rest = os.path.splitdrive(os.path.abspath(p))
    return f"/mnt/{drive[0].lower()}" + rest.replace("\\", "/")


ODAS_CMD = ("L=$HOME/odas_build/sysroot/usr/lib/x86_64-linux-gnu; "
            "LD_LIBRARY_PATH=$L:$L/pulseaudio exec stdbuf -oL $HOME/odas_build/build/bin/odaslive -c '{cfg}'")


class Decimator:
    """Stateful 48 -> 16 kHz FIR decimator for interleaved blocks."""

    def __init__(self, ch):
        self.h = firwin(48, 7000, fs=FS)
        self.zi = np.zeros((len(self.h) - 1, ch))
        self.phase = 0

    def __call__(self, x):
        y, self.zi = lfilter(self.h, 1.0, x, axis=0, zi=self.zi)
        out = y[(3 - self.phase) % 3::3]
        self.phase = (self.phase + len(x)) % 3
        return out


def read_odas_json(stream, out_q):
    """ODAS's terminal sink prints one multi-line JSON object per frame."""
    buf, depth = [], 0
    for raw in iter(stream.readline, b""):
        line = raw.decode(errors="replace")
        depth += line.count("{") - line.count("}")
        buf.append(line)
        if depth == 0 and buf and "".join(buf).strip():
            try:
                out_q.put(json.loads("".join(buf)))
            except ValueError:
                pass
            buf = []


def noise_player(dev):
    rng = np.random.default_rng(3)
    spec = np.fft.rfft(rng.standard_normal(FS * 4))
    f = np.fft.rfftfreq(FS * 4, 1 / FS)
    spec[(f < 300) | (f > 6000)] = 0
    n = np.fft.irfft(spec).astype(np.float32)
    n *= 0.4 / np.abs(n).max()
    pos = [0]

    def cb(out, frames, t, status):
        idx = (np.arange(frames) + pos[0]) % len(n)
        out[:] = 0
        out[:, 0] = n[idx]
        pos[0] += frames

    return sd.OutputStream(device=dev, samplerate=FS, channels=2, dtype="float32", callback=cb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--play", action="store_true")
    a = ap.parse_args()

    con = micarray.Console()
    if con.cmd("info")["mode"] != "raw8":
        sys.exit("ODAS needs the 8-channel stream: send `mode raw8` first")
    con.cmd("telem 10")
    dev, d = micarray.find_input()
    odas = subprocess.Popen(["wsl", "-e", "bash", "-c", ODAS_CMD.format(cfg=wsl_path(CFG_WIN))],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    results = queue.Queue()
    threading.Thread(target=read_odas_json, args=(odas.stdout, results), daemon=True).start()

    audio_q = queue.Queue()
    dec = Decimator(8)
    player = None
    if a.play:
        out_dev, _ = micarray.find_output("Realtek")
        player = noise_player(out_dev)
    print(f"ODAS on '{d['name']}' via {d['hostapi_name']} for {a.seconds:.0f} s")
    print("   t   ODAS tracked sources (az deg / elev deg / activity)        firmware az (active)")
    last_print = 0
    t_end = time.time() + a.seconds
    latest_fw = None
    with sd.InputStream(device=dev, callback=lambda x, f, t, s: audio_q.put(x.copy()), **stream_args(d)):
        if player:
            player.start()
        t0 = time.time()
        try:
            while time.time() < t_end:
                try:
                    x = audio_q.get(timeout=0.5)
                except queue.Empty:
                    continue
                y = dec(x.astype(np.float64))
                odas.stdin.write(np.clip(np.round(y), -32768, 32767).astype("<i2").tobytes())
                for m in con.pump(0):
                    if "az" in m:
                        latest_fw = m
                while not results.empty():
                    r = results.get()
                    if time.time() - last_print < 0.5:
                        continue
                    last_print = time.time()
                    srcs = [s for s in r.get("src", []) if s.get("id", 0) and s.get("activity", 0) > 0.1]
                    txt = "  ".join(
                        f"{math.degrees(math.atan2(s['x'], s['y'])) % 360:5.1f}/"
                        f"{math.degrees(math.asin(max(-1, min(1, s['z'])))):4.0f}/{s['activity']:.2f}"
                        for s in srcs) or "-"
                    fw = (f"{latest_fw['az_s']:5.1f} ({'yes' if latest_fw['act'] else 'no'})"
                          if latest_fw else "-")
                    print(f"{time.time() - t0:5.1f}   {txt:58s} {fw}", flush=True)
        finally:
            if player:
                player.stop()
                player.close()
            odas.stdin.close()
            odas.terminate()
            con.close()


if __name__ == "__main__":
    main()
