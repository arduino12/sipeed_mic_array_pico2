# Sipeed 6+1 Mic Array on a Raspberry Pi Pico 2

The firmware turns the Sipeed R6+1 microphone array and a Pico 2 (RP2350) into a standard USB device with two functions:

- **USB microphone (UAC2), driverless.** Two audio modes, switchable over serial:
  - `stereo` (default): a normal 2-channel microphone, 48 kHz / 24-bit. Both channels carry the **filtered** audio. It can follow the talker, be aimed at a fixed direction, or pass only one sector (silence otherwise). See *Direction filter*.
  - `raw8`: 48 kHz / 16-bit. Channels 1–6 are the ring mics, channel 7 is the centre mic, channel 8 is the filtered output. This mode is for PC-side processing such as ODAS or your own Python code.
- **USB serial (CDC).** It streams the sound direction as JSON lines and accepts commands for the LEDs, beam steering and gating, mode, and calibration.

The 12-LED ring shows where the sound comes from.

## Wiring

Connect the module's J2/J3 headers (names as printed on the board) to the Pico 2:

| Module | Pico 2 | Pin |
|---|---|---|
| MIC_CK | GP10 | 14 |
| MIC_WS | GP11 | 15 |
| MIC_D0 | GP12 | 16 |
| MIC_D1 | GP13 | 17 |
| MIC_D2 | GP14 | 19 |
| MIC_D3 | GP15 | 20 |
| LED_CK | GP18 | 24 |
| LED_DA | GP19 | 25 |
| VIN | VBUS (5 V) | 40 |
| GND | GND | 38 |

### Waveshare RP2350-Zero (build env `zero`)

| Module | RP2350-Zero |
|---|---|
| MIC_CK | GP0 |
| MIC_WS | GP1 |
| MIC_D0–D3 | GP2–GP5 |
| LED_CK | GP6 |
| LED_DA | GP7 |
| VIN | 5V |
| GND | GND |

### Choosing other pins

Pins live in `src/config.h`, and `static_assert`s check these rules:
- MIC_WS must be the GPIO right after MIC_CK. The two clocks come from one PIO side-set pair.
- MIC_D0–D3 must be four consecutive GPIOs in that order. Each line has its own PIO state machine, so they don't need to sit next to the clocks. Scattered pins would need only a small pin table in the code.
- LED_CK and LED_DA must be SPI0 clock and data-out pins: LED_CK on GP2, 6, 18 or 22, and LED_DA on GP3, 7, 19 or 23.

Power and logic levels:
- VIN is 5 V. The board has its own 3.3 V regulator for the mics and level shifters on the LED lines, so the Pico's 3.3 V logic is fine.
- The LED brightness is capped (`led bright`, default 4/31). At full white the ring draws about 720 mA.

## Build and flash

PlatformIO, run from PowerShell:

```
pio run -e pico2              # or: pio run -e zero
python tools\flash.py pico2   # or: python tools\flash.py zero
```

- **First flash:** hold BOOTSEL while plugging in. `flash.py` copies the UF2 to the `RP2350` drive.
- **Later flashes:** `flash.py` sends the 1200 bps touch to reboot the board into BOOTSEL by itself.
- **Port and PIDs:** the board appears on a new COM port, with VID `1209` and PID `A6E1` (stereo) or `A6E3` (raw8).

## Angles

All azimuths are degrees **clockwise from mic 0, looking at the LED side**:

- **LEDs:** LED *i* sits at 30·*i*°. LED 0 is next to mic 0.
- **Ring mics:** mic *k* sits at 60·*k*°.
- **`offset`:** rotates every reported and commanded angle, so you can define your own "front". The GUI button "Set current sound as 0°" sets it for you.

## Serial protocol

Open the port at any baud **except 1200**, which reboots into BOOTSEL. Assert DTR, because telemetry is only sent while DTR is set.

Every command returns exactly one JSON line: `{"ok":"<cmd>",...}` or `{"err":"..."}`.

| Command | Effect |
|---|---|
| `info`, `stats`, `levels`, `map` | Status, CPU/USB counters, per-mic levels, SRP map (72 × 5°). |
| `mode stereo` / `mode raw8` | USB audio layout. Saves and re-enumerates. |
| `sector <from> <to>` / `sector off` | Pass only sound from that sector (see *Direction filter*), or go back to follow mode. |
| `mute full` / `mute <dB>` | Outside the sector: digital silence (default), or N dB quieter. |
| `width <deg>` | Stereo width: 0 = same audio on both channels, >0 = left/right beams. |
| `lookahead <ms>` | Output delay that lets the gate open before onsets (default 64). |
| `steer auto` / `steer <deg>` | The beam follows the DOA, or stays fixed at an angle. |
| `gate off` / `gate on` / `gate <halfwidth>` | Low-level form of `sector`: mute unless sound is within ±halfwidth of the steering direction. |
| `gain <dB>` | Digital gain on the USB audio (default 18). |
| `led doa` / `led off` / `led static r g b` / `led set i r g b` / `led bright 1..31` | LED ring. |
| `offset <deg>` | Rotate the angle frame. |
| `telem <hz>` / `telem off` / `telem json` / `telem odas` | Telemetry rate and format. |
| `src mics` / `src tone` / `src sim <deg>` | Self-test sources. `tone` puts 500·(k+1) Hz on mic k. `sim` injects a simulated plane wave from `<deg>`. |
| `tune margin <dB>` / `tune conf <x>` | DOA activity thresholds. |
| `save`, `defaults`, `reboot`, `bootsel` | Persist settings, reset to defaults, reboot, enter BOOTSEL. |

Telemetry, default 10 Hz:

```
{"t":12345,"az":357.3,"az_s":356.8,"conf":0.41,"db":-52.0,"floor":-66.1,"act":1,"steer":356.8,"gate":1}
```

- `az` is this frame's estimate and `az_s` is the tracked direction.
- `conf` is the SRP-PHAT peak, from 0 to 1.
- `db` / `floor` are the centre-mic level and the noise floor.
- `act` means the frame is above the floor and confident.

`telem odas` instead sends ODAS-SSL-style lines: `{"timeStamp":n,"src":[{"x","y","z","E"}]}`.

## Browser GUI

**Online:** https://arduino12.github.io/sipeed_mic_array_gui/ — open it in Chrome or Edge on any computer, press **Connect** and pick the mic array. Web Serial runs only in Chromium browsers.
The page is served by GitHub Pages from the GUI-only repo `arduino12/sipeed_mic_array_gui`. After changing `web/`, commit and republish it with `git subtree push --prefix web gui main`.

**Offline:** open `web/index.html` directly. If the browser blocks the `file://` page, serve it locally with `python -m http.server` in `web/` and open `localhost:8000`.

The GUI provides:
- A live compass showing the LEDs, the mics, the SRP map, the direction needle, and the beam/gate wedge. Clicking the compass steers the beam.
- Controls for LEDs, gain, gate, audio mode, calibration and the test sources.
- Stats and a command log.

## Windows notes

- **Stereo mode** works as an ordinary microphone ("Microphone (Sipeed Mic Array)") in every app.
- **Voice Clarity effect pack.** Windows 11 attaches it to new USB microphones, and it changes the raw signal:
  - It applies AGC and noise suppression.
  - In shared mode it narrows the **8-channel** endpoint to 2 channels.

  For `raw8` use, open *Settings → System → Sound → Microphone (Sipeed Mic Array 8ch) → Audio enhancements: Off*. The stereo device has its own, separate switch. Alternatively, use an app that opens the device through WASAPI exclusive mode or WDM-KS; the tools here use WDM-KS.
- **Linux** (e.g. a Raspberry Pi running ODAS) sees a standard 8-channel ALSA card in `raw8` mode.

## Direction filter (what the microphone sends)

The "beam" is ordinary audio: all 7 mics added with per-mic delays, so sound from the chosen direction adds up and sound from elsewhere partly cancels. Three ways to use it:

| Mode | Command | Output |
|---|---|---|
| Follow talker (default) | `sector off` | The beam follows whoever is talking; audio always flows. |
| Only a sector | `sector 20 50` | Audio only while the dominant sound arrives from 20°–50° (clockwise from `from` to `to`). Otherwise **digital silence**, or `mute 30` for 30 dB quieter. |
| Fixed beam | `steer 120` (and `gate off`) | The beam stays at 120°. Nothing is muted, but sound from there is louder. |

How the sector mode behaves:

- **When it opens.** The gate opens when the strongest sound *inside* the sector is (nearly) the strongest overall in the 300–4000 Hz direction band, for two consecutive 16 ms estimates. It stays open 400 ms after the last such estimate, to bridge pauses between words.
- **Look-ahead.** The output runs `lookahead` ms behind the mics (default 64) so the gate is already open when a sentence starts. The test measures onsets within a few ms. `lookahead 0` removes the extra latency at the cost of clipped first syllables.
- **Stereo width.** `width 60` turns the two channels into two beams, 30° to the left and right of the steering direction: a real stereo image, with about 8–9 dB of separation above 1 kHz. `width 0` (default) puts the same audio on both channels.
- **Limits.**
  - The array is 8 cm across, so below about 1 kHz it hardly separates directions (about 1–2 dB). When someone outside the sector talks at the same time as someone inside, the outside voice is reduced but not removed.
  - Use sectors at least about 30° wide; reflections move the estimate by a few degrees.

The browser GUI has the same controls: the "Direction filter" card, plus clicking the compass to move the sector. The LED ring shows the sector in blue while muted and green while passing audio.

## Which audio mode?

| | `stereo` (default) | `raw8` |
|---|---|---|
| Windows name | Microphone (Sipeed Mic Array) | Microphone (Sipeed Mic Array 8ch) |
| ch 1 / ch 2 | filtered audio (identical, or left/right beams with `width`) | mic 0 / mic 1 |
| ch 3–8 | – | mics 2–5, centre, filtered output |
| Format | 48 kHz / 24-bit | 48 kHz / 16-bit |
| For | Zoom, Teams, Sound Recorder, any app that wants "a microphone" | ODAS, Audacity multitrack, Python: anything that does its own array processing |

Windows treats the two modes as two different microphones. Settings such as "Audio enhancements: Off" must be set on each one separately. In `raw8`, channel 8 runs `lookahead` ms behind channels 1–7.

## ODAS

ODAS (the open-source sound localisation and tracking library) is built inside WSL by `odas/build_odas_wsl.sh`. That script needs no root: it downloads the `.deb` packages and unpacks them into a private sysroot. It also patches ODAS's JSON output, which on current glibc prints only `}` per frame (`odas/patch_odas_sprintf.py`).

**ODAS Studio** is the ODAS GUI: a 3-D sphere plus azimuth/elevation plots of the tracked sources. Install it next to the project, then run it with the board in `raw8`:

```
wsl -e bash /mnt/c/<path>/sipeed_mic_array_pico2/odas/build_odas_wsl.sh
powershell -ExecutionPolicy Bypass -File odas\install_odas_studio.ps1
powershell -ExecutionPolicy Bypass -File odas\run_odas_studio.ps1          # Ctrl+C to stop
powershell -ExecutionPolicy Bypass -File odas\run_odas_studio.ps1 -Play    # with test noise on the left speaker
```

How it is wired:
- `tools/odas_bridge.py` captures the 8 channels (WDM-KS) and decimates them to 16 kHz.
- It feeds `odaslive` in WSL (`odas/sipeed_6p1.cfg`, the real mic geometry in the firmware's frame) through a FIFO.
- It forwards ODAS's tracked and potential sources to ODAS Studio on ports 9000/9001, the same TCP protocol `odaslive` uses itself.
- It also prints the tracks, and the firmware's own DOA when the COM port is free. Close the browser GUI for that.

The install script works around three problems:
- Studio pins Electron 5 and native `grpc`. The install skips native builds, so Google speech-to-text is unavailable, and it unzips Electron by hand.
- Shells launched from VS Code carry `ELECTRON_RUN_AS_NODE=1`, which `run_odas_studio.ps1` clears.
- On Linux, point the config's `raw.interface` at the ALSA card (`soundcard`) instead.

## Tests (`tools/`)

| Script | Checks |
|---|---|
| `record_check.py tone [s]` | Channel map (each mic channel at its own tone frequency), bit-exact continuity, no zero runs. |
| `record_check.py stress [n]` | Opens and closes the stream n times; the console must answer every time. This is the freeze that broke the old firmware. |
| `record_check.py long [s]` | One continuous stream, checked in 10 s chunks, plus underrun and overrun counters. |
| `acoustic_test.py [--beam] [--sector]` | Plays a sweep and noise on one laptop speaker (raising the volume only during playback). It checks per-mic SNR, balance and correlation, compares the firmware DOA with an offline GCC-PHAT estimate, measures beam gain toward and away from the source, and with `--sector` checks a sector on the speaker (passes, onset not cut) and one opposite it (digital silence). In stereo mode, run `--sector --az <deg>`. |
| `acoustic_test.py --no-console` | The same test, audio only, for when another program holds the COM port. |
| `odas_bridge.py [--studio] [--play]` | ODAS live from the USB stream, optionally feeding ODAS Studio. |

## Design notes

- **I2S capture.**
  - A PIO0 state machine generates BCK and WS at an exact /25 divider, which is why the sys clock is 153.6 MHz.
  - Four PIO1 follower state machines, one per data line, sample on each BCK rising edge and autopush ready 32-bit slots.
  - Endless DMA streams them into 8 KB rings, so no CPU work is needed.
- **I2S bit position.** The datasheet describes Philips I2S, but raw captures (`raw <slot>`) put the sign bit at BCK position 0. The firmware decodes with offset 0. The data has 18 significant bits followed by zero padding.
- **Core 1 (audio):**
  - DC block.
  - Delay-and-sum beamformer over all 7 mics, using 4-tap Lagrange fractional delays.
  - Gate.
  - 48→16 kHz decimation for the DOA.
  - Writes USB frames into a lock-free ring.
- **USB.** The USB ISR (`tud_audio_tx_done_isr`) moves the ring into TinyUSB's FIFO and never waits. TinyUSB's IN flow control sends 47, 48 or 49 frames per packet to absorb clock drift. The audio function has no controls the host can change, so no host request can reconfigure anything mid-stream.
- **Core 0 (DOA):**
  - SRP-PHAT across all 21 mic pairs at 300–4000 Hz.
  - The lag sums run on the M33's dual 16-bit MAC instructions (`SMLAD`/`SMLSD`) in Q12.
  - 62.5 estimates per second, at about 50 % of one core.
- **Load.** Core 1 runs at about 45 %.
- **Directivity.** The beam's directivity is limited by the 8 cm aperture: about 1–2 dB below 1.4 kHz, and 5–9 dB of rejection toward-versus-away above that.
