"""Acoustic end-to-end test: play test signals on the laptop speakers, record
the array (8-ch raw mode, WDM-KS), and compare the firmware's direction with
an independent offline GCC-PHAT estimate.

    python tools/acoustic_test.py [--out "Realtek"] [--vol 0.5] [--offsets]

Checks: every mic hears the signal (SNR, level balance), each mic channel
correlates with what was played (I2S bit alignment), and firmware DOA agrees
with the offline estimate. --offsets also tries I2S bit offsets 0..2.
"""
import argparse
import os
import sys
import threading
import time

import numpy as np
import sounddevice as sd

sys.path.insert(0, os.path.dirname(__file__))
import micarray  # noqa: E402
from record_check import FS, stream_args, to_float  # noqa: E402

R = 0.040
C = 343.0
OUT_DIR = os.path.join(os.path.dirname(__file__), "out")


def mic_xy():
    """Same frame as the firmware: x right, y towards mic 0, az clockwise from mic 0."""
    xy = [(R * np.sin(np.radians(60 * k)), R * np.cos(np.radians(60 * k))) for k in range(6)]
    return np.array(xy + [(0.0, 0.0)])


def make_signal(vol):
    rng = np.random.default_rng(1)
    sil = np.zeros(int(0.5 * FS))
    t = np.arange(int(2.0 * FS)) / FS
    f0, f1 = 200.0, 8000.0
    k = np.log(f1 / f0) / t[-1]
    sweep = np.sin(2 * np.pi * f0 * (np.exp(k * t) - 1) / k) * np.hanning(len(t)) ** 0.1
    noise = rng.standard_normal(int(3.0 * FS))
    spec = np.fft.rfft(noise)
    f = np.fft.rfftfreq(len(noise), 1 / FS)
    spec[(f < 300) | (f > 6000)] = 0
    noise = np.fft.irfft(spec, len(noise))
    noise *= 0.5 / np.abs(noise).max()
    sig = np.concatenate([sil, sweep, sil, noise, sil]) * vol
    marks = {"sweep": (len(sil), len(sil) + len(sweep)),
             "noise": (2 * len(sil) + len(sweep), 2 * len(sil) + len(sweep) + len(noise))}
    return sig.astype(np.float32), marks


def play_and_record(out_dev, in_dev, d, sig, console, channel="left"):
    n_rec = len(sig) + int(0.6 * FS)
    rec = np.zeros((n_rec, d["max_input_channels"]), np.int16)
    pos = {"in": 0, "out": 0}
    done = threading.Event()

    def in_cb(indata, frames, t, status):
        i = pos["in"]
        n = min(frames, n_rec - i)
        rec[i:i + n] = indata[:n]
        pos["in"] += n
        if pos["in"] >= n_rec:
            done.set()

    def out_cb(outdata, frames, t, status):
        i = pos["out"]
        chunk = sig[i:i + frames]
        outdata[:] = 0
        # One speaker only: the laptop's two speakers ~30 cm apart are two
        # sources, and which one dominates depends on frequency.
        if channel in ("left", "both"):
            outdata[:len(chunk), 0] = chunk
        if channel in ("right", "both"):
            outdata[:len(chunk), 1] = chunk
        pos["out"] += frames

    telem = []
    with sd.InputStream(device=in_dev, callback=in_cb, **stream_args(d)):
        t0 = time.time()  # host time of recording sample 0 (to within a block)
        with sd.OutputStream(device=out_dev, samplerate=FS, channels=2, dtype="float32", callback=out_cb):
            while not done.is_set():
                telem += console.pump(0.1)
    for m in telem:
        m["_rt"] = m["_ht"] - t0  # seconds into the recording
    return to_float(rec), telem


def gcc_phat_tdoa(a, b, interp=32, max_tau=None):
    """Delay of a relative to b in seconds (positive: a arrives later)."""
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    A, B = np.fft.rfft(a, n), np.fft.rfft(b, n)
    G = A * np.conj(B)
    f = np.fft.rfftfreq(n, 1 / FS)
    G[(f < 300) | (f > 6000)] = 0
    G /= np.abs(G) + 1e-12
    cc = np.fft.irfft(G, n * interp)
    max_shift = int(interp * FS * (max_tau or 2 * R / C)) + 2
    cc = np.concatenate([cc[-max_shift:], cc[:max_shift + 1]])
    return (np.argmax(cc) - max_shift) / (interp * FS)


def offline_doa(x):
    """SRP over pairwise GCC-PHAT TDOAs, 0.5 deg grid. Returns (az, residual_us)."""
    xy = mic_xy()
    pairs = [(i, j) for i in range(7) for j in range(i + 1, 7)]
    meas = np.array([gcc_phat_tdoa(x[:, i], x[:, j]) for i, j in pairs])
    best = None
    for az in np.arange(0, 360, 0.5):
        u = np.array([np.sin(np.radians(az)), np.cos(np.radians(az))])
        proj = xy @ u
        pred = np.array([(proj[j] - proj[i]) / C for i, j in pairs])  # t_i - t_j
        err = np.sqrt(np.mean((meas - pred) ** 2))
        if best is None or err < best[1]:
            best = (az, err)
    return best[0], best[1] * 1e6


def srp_phat_frames(x48, flo=312.5, fhi=4000.0, n=512, hop=256):
    """Same method as the firmware: 16 kHz, 512-sample Hann frames, PHAT
    cross-spectra of all pairs, SRP over azimuth. Returns the circular mean."""
    from scipy.signal import resample_poly

    x = resample_poly(x48, 1, 3, axis=0)
    fs = 16000
    xy = mic_xy()
    pairs = [(i, j) for i in range(7) for j in range(i + 1, 7)]
    f = np.fft.rfftfreq(n, 1 / fs)
    band = (f >= flo) & (f <= fhi)
    azs = np.arange(0, 360, 1.0)
    u = np.stack([np.sin(np.radians(azs)), np.cos(np.radians(azs))], 1)
    proj = u @ xy.T  # (az, mic)
    steer = {p: np.exp(1j * 2 * np.pi * f[band][None, :] * ((proj[:, p[1]] - proj[:, p[0]]) / C)[:, None])
             for p in pairs}
    win = np.hanning(n)[:, None]
    est = []
    for k in range(0, len(x) - n, hop):
        X = np.fft.rfft(x[k:k + n] * win, axis=0)[band]
        p_sum = np.zeros(len(azs))
        for i, j in pairs:
            G = X[:, i] * np.conj(X[:, j])
            G /= np.abs(G) + 1e-12
            p_sum += np.real(steer[(i, j)] @ G)
        est.append(azs[np.argmax(p_sum)])
    a = np.radians(est)
    return np.degrees(np.arctan2(np.mean(np.sin(a)), np.mean(np.cos(a)))) % 360


def analyse(x, sig, marks, telem, label=""):
    lat_ref = x[:, 6]
    s0, s1 = marks["sweep"]
    cc = np.correlate(lat_ref[: s1 + FS], sig[s0:s1], mode="valid")
    lat = int(np.argmax(np.abs(cc))) - s0
    print(f"{label}latency speaker->array {lat / FS * 1000:.1f} ms")

    n0, n1 = marks["noise"]
    noise_seg = x[n0 + lat + 2400: n1 + lat - 2400, :7]
    quiet = x[int(0.05 * FS): int(0.45 * FS), :7]
    lvl = 20 * np.log10(np.sqrt((noise_seg ** 2).mean(axis=0)) + 1e-12)
    floor = 20 * np.log10(np.sqrt((quiet ** 2).mean(axis=0)) + 1e-12)
    ref = sig[s0:s1]
    coh = []
    for k in range(7):
        y = x[s0 + lat: s1 + lat, k]
        c = np.correlate(y, ref, mode="full")
        coh.append(np.abs(c).max() / (np.linalg.norm(y) * np.linalg.norm(ref) + 1e-12))
    ok = True
    for k in range(7):
        name = f"mic{k}" if k < 6 else "centre"
        good = lvl[k] - floor[k] > 10 and abs(lvl[k] - np.median(lvl)) < 6 and coh[k] > 0.3
        ok &= good
        print(f"  {name:6s}: noise {lvl[k]:6.1f} dBFS  quiet {floor[k]:6.1f}  SNR {lvl[k] - floor[k]:5.1f} dB  "
              f"sweep corr {coh[k]:.2f}  {'OK' if good else 'FAIL'}")

    az_off, res_us = offline_doa(noise_seg)
    az_fwlike = srp_phat_frames(noise_seg)
    t_lo, t_hi = (n0 + lat) / FS, (n1 + lat) / FS
    # firmware estimates made while the noise was playing (+ DOA frame/serial lag)
    t_lo, t_hi = t_lo + 0.3, t_hi + 0.1
    act = [m["az"] for m in telem if m.get("act") and t_lo <= m.get("_rt", -1) <= t_hi]
    fw = None
    if act:
        a = np.radians(act)
        fw = np.degrees(np.arctan2(np.mean(np.sin(a)), np.mean(np.cos(a)))) % 360
    diff = None if fw is None else (fw - az_off + 180) % 360 - 180
    diff_fw = None if fw is None else (fw - az_fwlike + 180) % 360 - 180
    print(f"  offline wideband TDOA fit {az_off:.1f} deg (rms residual {res_us:.1f} us)")
    print(f"  offline firmware-method   {az_fwlike:.1f} deg")
    print(f"  firmware                  {'n/a' if fw is None else f'{fw:.1f}'} deg from {len(act)} active estimates"
          f"{'' if diff_fw is None else f'  (vs same method {diff_fw:+.1f}, vs wideband {diff:+.1f})'}")
    ok &= diff_fw is not None and abs(diff_fw) < 6 and abs(diff) < 20
    return ok, az_off, fw


def band_db(y, lo, hi):
    Y = np.fft.rfft(y * np.hanning(len(y)))
    f = np.fft.rfftfreq(len(y), 1 / FS)
    m = (f >= lo) & (f < hi)
    return 10 * np.log10(np.sum(np.abs(Y[m]) ** 2) + 1e-20)


def beam_test(c, out_dev, in_dev, d, sig, marks, toward, channel):
    """Steer the beam (raw8 ch8) at the speaker and away from it; compare the
    beam's level with the centre mic's in octave bands."""
    bands = [(350, 700), (700, 1400), (1400, 2800), (2800, 5600)]
    res = {}
    for name, az in (("toward", toward), ("away", (toward + 180) % 360)):
        c.cmd(f"steer {az:.1f}")
        c.pump(0.3)
        x, _ = play_and_record(out_dev, in_dev, d, sig, c, channel)
        n0, n1 = marks["noise"]
        lat = int(0.17 * FS)
        seg = x[n0 + lat + 4800: n1 + lat - 4800]
        res[name] = [band_db(seg[:, 7], lo, hi) - band_db(seg[:, 6], lo, hi) for lo, hi in bands]
    c.cmd("steer auto")
    print("  beam level re centre mic, dB      " + "  ".join(f"{lo}-{hi} Hz" for lo, hi in bands))
    for name in ("toward", "away"):
        print(f"    steered {name:6s}                " + "  ".join(f"{v:9.1f}" for v in res[name]))
    gain = [t - a for t, a in zip(res["toward"], res["away"])]
    print("    toward - away                    " + "  ".join(f"{v:9.1f}" for v in gain))
    ok = res["toward"][2] > -2 and gain[2] > 2 and gain[3] > 2
    print("  BEAM", "OK" if ok else "FAIL", "(expect ~0 dB toward, several dB rejection above 1.4 kHz)")
    return ok


class NullConsole:
    def pump(self, seconds):
        time.sleep(seconds)
        return []


class SystemVolume:
    """Temporarily set the default speaker's master volume (restored on exit)."""

    def __init__(self, level):
        self.level = level
        self.ep = None

    def __enter__(self):
        if self.level is None:
            return self
        try:
            from pycaw.pycaw import AudioUtilities

            self.ep = AudioUtilities.GetSpeakers().EndpointVolume
            self.prev = (self.ep.GetMasterVolumeLevelScalar(), self.ep.GetMute())
            self.ep.SetMute(0, None)
            self.ep.SetMasterVolumeLevelScalar(self.level, None)
            print(f"speaker volume {self.prev[0] * 100:.0f}% -> {self.level * 100:.0f}% for the test")
        except Exception as e:  # pycaw missing or no default speaker: play at current volume
            print(f"(could not set speaker volume: {e})")
            self.ep = None
        return self

    def __exit__(self, *exc):
        if self.ep is not None:
            self.ep.SetMasterVolumeLevelScalar(self.prev[0], None)
            self.ep.SetMute(self.prev[1], None)
            print(f"speaker volume restored to {self.prev[0] * 100:.0f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="Realtek")
    ap.add_argument("--vol", type=float, default=0.8, help="digital level of the test signal")
    ap.add_argument("--sysvol", type=float, default=0.6, help="speaker master volume during the test, <0 = leave")
    ap.add_argument("--offsets", action="store_true")
    ap.add_argument("--channel", default="left", choices=["left", "right", "both"])
    ap.add_argument("--beam", action="store_true", help="also run the steer toward/away beam test")
    ap.add_argument("--no-console", action="store_true",
                    help="audio only (e.g. while the browser GUI holds the COM port): offline DOA, no firmware check")
    a = ap.parse_args()

    out_dev, od = micarray.find_output(a.out)
    in_dev, d = micarray.find_input()
    if out_dev is None or in_dev is None:
        sys.exit("speaker or mic array not found")
    sig, marks = make_signal(a.vol)
    print(f"play on '{od['name']}', record '{d['name']}' via {d['hostapi_name']}")
    os.makedirs(OUT_DIR, exist_ok=True)
    if a.no_console:
        with SystemVolume(a.sysvol if a.sysvol >= 0 else None):
            x, _ = play_and_record(out_dev, in_dev, d, sig, NullConsole(), a.channel)
        np.save(os.path.join(OUT_DIR, "acoustic.npy"), x)
        analyse(x, sig, marks, [])
        print("(audio-only run: firmware DOA not checked)")
        return
    with micarray.Console() as c:
        if c.cmd("info")["mode"] != "raw8":
            sys.exit("needs raw8 mode: send `mode raw8` first")
        c.cmd("src mics")
        c.cmd("telem 20")
        c.cmd("telem json")
        offsets = [0, 1, 2] if a.offsets else [None]
        ok = True
        for off in offsets:
            if off is not None:
                c.cmd(f"i2soff {off}")
            with SystemVolume(a.sysvol if a.sysvol >= 0 else None):
                x, telem = play_and_record(out_dev, in_dev, d, sig, c, a.channel)
            np.save(os.path.join(OUT_DIR, f"acoustic{'' if off is None else f'_off{off}'}.npy"), x)
            r, az, fw = analyse(x, sig, marks, telem, label="" if off is None else f"[i2s offset {off}] ")
            ok &= r
        if a.offsets:
            c.cmd("i2soff 0")
        if a.beam:
            with SystemVolume(a.sysvol if a.sysvol >= 0 else None):
                ok &= beam_test(c, out_dev, in_dev, d, sig, marks, fw if fw is not None else az, a.channel)
        c.cmd("telem 10")
    print("ACOUSTIC TEST", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
