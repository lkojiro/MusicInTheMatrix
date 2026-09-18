# Music in the Matrix

A terminal-native music visualizer for macOS. Reacts live to system audio
(level and detected beats) across six visual modes — FFT bar charts,
Matrix-style digital rain, checkerboard, oscilloscope, and VU-meter bands —
switchable on the fly, with support for spawning any number of synced
windows from a single audio pipeline.

## Installation

Requires CMake >= 3.16, a C++17 compiler, and PortAudio:

```sh
brew install portaudio
cmake -S . -B build
cmake --build build
```

System audio capture needs a virtual loopback device, since macOS has no
built-in "listen to what's playing" input:

```sh
brew install blackhole-2ch
```

Then in **Audio MIDI Setup**, create a Multi-Output Device combining your
normal output and "BlackHole 2ch", and select it in **System Settings >
Sound > Output**. Run `./build/mitm` — it looks for "BlackHole" by default,
or pass a device-name substring as the first argument to target something
else (e.g. a real microphone).

## Demo

https://github.com/user-attachments/assets/f2b53c06-9ebf-420b-8c52-5db6b4ed0a8b

## Usage

```sh
./build/mitm                       # starts in Bars Left mode
./build/mitm --visual=matrix       # or bands, bars-right, bars-middle, checkerboard, oscilloscope
./build/mitm --color=purple        # red/green/blue/yellow/cyan/magenta/white
```

Left/right arrow keys cycle modes live, up/down changes color, `n` spawns a
new synced window, `q` quits. A web control panel at
`http://127.0.0.1:7887` offers the same controls from a browser, plus
spawn/close/randomize for every open window.

## Technical highlights

- **Zero-dependency audio pipeline**: one process runs the only
  `AudioCapture` → `FftProcessor` → `BeatDetector` pipeline and fans it out
  over a Unix-socket IPC protocol to any number of visualizer windows —
  auto-electing itself host/child on launch, no manual orchestration step.
- **Hand-rolled beat detection**: spectral flux + adaptive threshold, kept
  license-clean (every mainstream real-time beat tracker is GPL/AGPL) and
  simple by design — it surfaces beat pulses, not a fragile BPM estimate.
- **Vendored, permissively-licensed DSP**: KissFFT (BSD-3) for the FFT
  instead of GPL-licensed FFTW or a hand-rolled transform.
- **Fully custom ncurses rendering**: five renderers sharing a generated
  xterm-256 brightness-ramp color system, log-spaced FFT bucketing with a
  frequency-dependent gain boost, and per-segment fade animation on the
  VU-meter bands mode.
- **From-scratch HTTP server**: a minimal, dependency-free HTTP/1.1 server
  (`ControlServer`) powers the web control panel — no framework, consistent
  with the project's hand-rolled IPC transport.
