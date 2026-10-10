"""Automated audio checks for the mic array (run with the board plugged in).

    python tools/record_check.py tone [seconds]   internal test tones: channel map, rate, continuity
    python tools/record_check.py stress [cycles]  open/close the stream repeatedly; console must stay alive
    python tools/record_check.py speaker          play sweeps/noise on the laptop speakers, check capture

The `tone` source makes channel k (mic k, 0..6) a 500*(k+1) Hz sine, so every
USB channel can be identified by frequency.
"""
import os
import sys
import time

import numpy as np
import sounddevice as sd

sys.path.insert(0, os.path.dirname(__file__))
import micarray  # noqa: E402

FS = 48000
OUT_DIR = os.path.join(os.path.dirname(__file__), "out")


def expected_tones(mode):
    """{usb channel index: expected Hz} for the tone source."""
    if mode == "raw8":
        return {k: 500.0 * (k + 1) for k in range(7)}
    return {1: 3500.0}  # stereo: ch1 = beam (mix), ch2 = centre mic


def stream_args(dev_info):
    """The device's own samples in its native format: WDM-KS, or WASAPI
    exclusive as a fallback (both bypass the Windows audio engine)."""
    channels = dev_info["max_input_channels"]
    args = dict(samplerate=FS, channels=channels, dtype="int16" if channels == 8 else "int32")
    if dev_info.get("hostapi_name") == "Windows WASAPI":
        args["extra_settings"] = sd.WasapiSettings(exclusive=True)
    return args


def to_float(x):
    x = np.asarray(x)
    return x.astype(np.float32) / (32768.0 if x.dtype == np.int16 else 2147483648.0)


def record(dev, d, seconds):
    x = sd.rec(int(seconds * FS), device=dev, blocking=True, **stream_args(d))
    return to_float(x)


def dominant_freq(x):
    n = 1 << int(np.ceil(np.log2(len(x) * 4)))
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    k = int(np.argmax(spec[1:])) + 1
    a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
    off = 0.5 * (a - c) / (a - 2 * b + c)
    return (k + off) * FS / n


def tone_glitches(x, f, seg=480):
    """Least-squares sine fit per 10 ms segment; returns residual dB per segment."""
    t = np.arange(seg) / FS
    out = []
    for s in range(0, len(x) - seg, seg):
        y = x[s:s + seg]
        ph = 2 * np.pi * f * (t + s / FS)
        A = np.stack([np.cos(ph), np.sin(ph), np.ones(seg)], axis=1)
        coef, *_ = np.linalg.lstsq(A, y, rcond=None)
        res = y - A @ coef
        out.append(10 * np.log10((res ** 2).mean() / ((y ** 2).mean() + 1e-20) + 1e-20))
    return np.array(out)


def zero_runs(x, min_len=16):
    z = np.concatenate([[0], (x == 0).astype(np.int8), [0]])
    d = np.diff(z)
    starts, ends = np.where(d == 1)[0], np.where(d == -1)[0]
    return int(np.sum((ends - starts) >= min_len))


def check_tones(x, mode, verbose=True):
    ok = True
    x = x[int(0.3 * FS):]  # skip stream start-up
    for ch, f_exp in expected_tones(mode).items():
        y = x[:, ch]
        f = dominant_freq(y)
        ppm = (f / f_exp - 1) * 1e6
        res = tone_glitches(y, f)
        bad = int(np.sum(res > -30))
        zr = zero_runs(y)
        rms = 20 * np.log10(np.sqrt((y ** 2).mean()) + 1e-12)
        good = abs(ppm) < 1000 and bad == 0 and zr == 0 and rms > -40
        ok &= good
        if verbose:
            print(f"  ch{ch + 1}: {f:9.2f} Hz (exp {f_exp:.0f}, {ppm:+7.1f} ppm)  {rms:6.1f} dBFS  "
                  f"worst residual {res.max():6.1f} dB  glitch segs {bad}  zero runs {zr}  "
                  f"{'OK' if good else 'FAIL'}")
    return ok


def cmd_tone(seconds):
    with micarray.Console() as c:
        info = c.cmd("info")
        mode = info["mode"]
        c.cmd("src tone")
        dev, d = micarray.find_input()
        if dev is None:
            sys.exit("Sipeed Mic Array input not found")
        nch = d["max_input_channels"]
        print(f"mode {mode}, device '{d['name']}' via {d['hostapi_name']} ({nch} ch), recording {seconds} s ...")
        x = record(dev, d, seconds)
        st = c.cmd("stats")
        c.cmd("src mics")
    os.makedirs(OUT_DIR, exist_ok=True)
    np.save(os.path.join(OUT_DIR, f"tone_{mode}.npy"), x)
    ok = check_tones(x, mode)
    print(f"  stats: usb_underruns={st['usb_underruns']} usb_ring_full={st['usb_ring_full']} "
          f"dma_overruns={st['dma_overruns']} core1_load={st['core1_load']:.2f} doa_load={st['doa_load']:.2f}")
    print("TONE TEST", "PASS" if ok else "FAIL")
    return ok


def cmd_stress(cycles):
    with micarray.Console() as c:
        mode = c.cmd("info")["mode"]
        c.cmd("src tone")
        dev, d = micarray.find_input()
        before = c.cmd("stats")
        fails = 0
        for i in range(cycles):
            # callback stream: WDM-KS has no blocking read in PortAudio
            frames = sd.rec(int(0.4 * FS), device=dev, blocking=True, **stream_args(d))
            alive = np.abs(to_float(frames)[int(0.15 * FS):]).max() > 0.01
            st = c.cmd("stats", timeout=1.0)
            fails += not alive
            print(f"  cycle {i + 1:2d}: audio {'yes' if alive else 'NO'}  restarts={st['usb_restarts']}")
        x = record(dev, d, 3)
        ok = check_tones(x, mode) and fails == 0
        after = c.cmd("stats")
        c.cmd("src mics")
    print(f"  restarts {before['usb_restarts']} -> {after['usb_restarts']}, underruns {after['usb_underruns']}")
    print("STRESS TEST", "PASS" if ok else "FAIL")
    return ok


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "tone"
    arg = float(sys.argv[2]) if len(sys.argv) > 2 else None
    if what == "tone":
        ok = cmd_tone(arg or 5)
    elif what == "stress":
        ok = cmd_stress(int(arg or 20))
    else:
        sys.exit(__doc__)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
