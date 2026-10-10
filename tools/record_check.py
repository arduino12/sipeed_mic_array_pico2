"""Automated audio checks for the mic array (run with the board plugged in).

    python tools/record_check.py tone [seconds]   internal test tones: channel map, rate, continuity
    python tools/record_check.py stress [cycles]  open/close the stream repeatedly; console must stay alive
    python tools/record_check.py speaker          play sweeps/noise on the laptop speakers, check capture

The `tone` source makes mic k (0..6) a 500*(k+1) Hz sine, so every raw USB
channel can be identified by frequency. The processed channels (stereo ch1/ch2,
raw8 ch8) carry the beam: a fixed mix of all seven tones.
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


ALL_TONES = tuple(500.0 * (k + 1) for k in range(7))


def expected_tones(mode):
    """{usb channel index: expected Hz, or ALL_TONES for a beam channel}."""
    if mode == "raw8":
        out = {k: 500.0 * (k + 1) for k in range(7)}
        out[7] = ALL_TONES
        return out
    return {0: ALL_TONES, 1: ALL_TONES}


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
    """Least-squares fit of sines at f (one frequency or a tuple) per 10 ms
    segment; returns the residual in dB re the segment, per segment."""
    freqs = f if isinstance(f, tuple) else (f,)
    t = np.arange(seg) / FS
    out = []
    for s in range(0, len(x) - seg, seg):
        y = x[s:s + seg]
        cols = [np.ones(seg)]
        for fk in freqs:
            ph = 2 * np.pi * fk * (t + s / FS)
            cols += [np.cos(ph), np.sin(ph)]
        A = np.stack(cols, axis=1)
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
        if isinstance(f_exp, tuple):  # beam channel: fixed mix of all tones
            f, ppm = f_exp, 0.0
        else:
            f = dominant_freq(y)
            ppm = (f / f_exp - 1) * 1e6
        res = tone_glitches(y, f)
        bad = int(np.sum(res > -30))
        zr = zero_runs(y)
        rms = 20 * np.log10(np.sqrt((y ** 2).mean()) + 1e-12)
        good = abs(ppm) < 1000 and bad == 0 and zr == 0 and rms > -40
        ok &= good
        if verbose:
            what = "beam (7-tone mix)" if isinstance(f, tuple) else f"{f:9.2f} Hz (exp {f_exp:.0f}, {ppm:+7.1f} ppm)"
            print(f"  ch{ch + 1}: {what:34s}  {rms:6.1f} dBFS  "
                  f"worst residual {res.max():6.1f} dB  glitch segs {bad}  zero runs {zr}  "
                  f"{'OK' if good else 'FAIL'}")
    if mode == "stereo":
        same = np.allclose(x[:, 0], x[:, 1])
        ok &= same
        if verbose:
            print(f"  ch1 == ch2 (stereo width 0): {'yes' if same else 'NO'}")
    return ok


class FixedBeam:
    """Beam fixed at 0 deg with no gate while a synthetic source plays (the
    DOA of test tones is meaningless); restores the user's settings after."""

    def __init__(self, console):
        self.c = console

    def __enter__(self):
        self.info = self.c.cmd("info")
        self.c.cmd("steer 0")
        self.c.cmd("gate off")
        return self

    def __exit__(self, *exc):
        i = self.info
        self.c.cmd("src mics")
        self.c.cmd("steer auto" if i["steer"] == "auto" else f"steer {i['steer_az']}")
        self.c.cmd(f"gate {i['gate_width']}" if i["gate"] else "gate off")


def cmd_tone(seconds):
    with micarray.Console() as c, FixedBeam(c) as fb:
        mode = fb.info["mode"]
        c.cmd("src tone")
        dev, d = micarray.find_input()
        if dev is None:
            sys.exit("Sipeed Mic Array input not found")
        nch = d["max_input_channels"]
        print(f"mode {mode}, device '{d['name']}' via {d['hostapi_name']} ({nch} ch), recording {seconds} s ...")
        x = record(dev, d, seconds)
        st = c.cmd("stats")
    os.makedirs(OUT_DIR, exist_ok=True)
    np.save(os.path.join(OUT_DIR, f"tone_{mode}.npy"), x)
    ok = check_tones(x, mode)
    print(f"  stats: usb_underruns={st['usb_underruns']} usb_ring_full={st['usb_ring_full']} "
          f"dma_overruns={st['dma_overruns']} core1_load={st['core1_load']:.2f} doa_load={st['doa_load']:.2f}")
    print("TONE TEST", "PASS" if ok else "FAIL")
    return ok


def cmd_stress(cycles):
    with micarray.Console() as c, FixedBeam(c) as fb:
        mode = fb.info["mode"]
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
    print(f"  restarts {before['usb_restarts']} -> {after['usb_restarts']}, underruns {after['usb_underruns']}")
    print("STRESS TEST", "PASS" if ok else "FAIL")
    return ok


def cmd_long(seconds, chunk_s=10.0):
    """One continuous stream for `seconds`, tone-checked in 10 s chunks."""
    import queue

    with micarray.Console() as c, FixedBeam(c) as fb:
        mode = fb.info["mode"]
        c.cmd("src tone")
        dev, d = micarray.find_input()
        q = queue.Queue()
        st0 = c.cmd("stats")
        chunk = int(chunk_s * FS)
        buf, n_chunks, bad = [], 0, 0
        with sd.InputStream(device=dev, callback=lambda x, f, t, s: q.put(x.copy()), **stream_args(d)):
            t_end = time.time() + seconds
            while time.time() < t_end or q.qsize():
                buf.append(q.get())
                if sum(len(b) for b in buf) >= chunk:
                    x = to_float(np.concatenate(buf))
                    buf = []
                    good = check_tones(np.vstack([x[:int(0.3 * FS)], x]), mode, verbose=False) if n_chunks else True
                    n_chunks += 1
                    bad += not good
                    st = c.cmd("stats")
                    print(f"  {n_chunks * chunk_s:5.0f} s: {'OK' if good else 'GLITCH'}  underruns={st['usb_underruns']} "
                          f"ring_full={st['usb_ring_full']} dma_overruns={st['dma_overruns']} "
                          f"core1={st['core1_load']:.2f} doa={st['doa_load']:.2f}", flush=True)
        st1 = c.cmd("stats")
    ok = bad == 0 and st1["usb_underruns"] == st0["usb_underruns"] and st1["dma_overruns"] == st0["dma_overruns"]
    print("LONG TEST", "PASS" if ok else "FAIL", f"({n_chunks} chunks, {bad} with glitches)")
    return ok


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "tone"
    arg = float(sys.argv[2]) if len(sys.argv) > 2 else None
    if what == "tone":
        ok = cmd_tone(arg or 5)
    elif what == "stress":
        ok = cmd_stress(int(arg or 20))
    elif what == "long":
        ok = cmd_long(arg or 600)
    else:
        sys.exit(__doc__)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
