# Building Felucca for the M-VAVE FM-1

This repository builds firmware for the FM-1 only. Host-side tests are development tools, not supported application platforms.

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `wavestorm.fwsc` | the installable package (app + loader) |

## Get the source

Two parts are git submodules: six engines (`firmware/engines/`, [Felucca-Engines](https://github.com/hugelton/Felucca-Engines),
MIT) and the hardware layer (`firmware/bsp/`, [felucca-bsp](https://github.com/hugelton/felucca-bsp), MPL-2.0), so clone
with `--recurse-submodules`:

```
git clone --recurse-submodules https://github.com/hugelton/Felucca.git
```

In a clone made without it, run `git submodule update --init` once. GitHub's "Download ZIP" and the source archives
it adds to a release leave `firmware/engines/` and `firmware/bsp/` empty; each release also has
`felucca-X.Y.Z-source.tar.gz`, the whole source with the submodules' files in it. The build stops with
"firmware/engines is empty" or "firmware/bsp is empty" when they are missing.

## Prerequisites (macOS)

- Python 3 with Pillow and fontTools: `pip3 install Pillow fonttools` (the UI font and icons are
  rasterised at build time)
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Node.js (optional, for the web tests).
- Emscripten (optional, `emcc`: the browser demo, `web/emu/`).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 1.8.1` makes a release build: the package identity becomes `FM-1_818` (WaveStorm's; a
development build is `FM-1_800`)
and the version string `v1.8.1`; the package is `build/wavestorm-1.8.1.fwsc`, and
`build/release-1.8.1/` holds what a release ships: the package, the app
(`wavestorm-1.8.1-app.bin`), `SHA256SUMS`, `LICENSE`, `LICENSING.md` and
`LICENSES/` (the package contains Apache-2.0 SDK files, so the licence texts travel with it).

Build options (environment, `0` or `1`; defaults in `firmware/src/felucca.c`, `core.h` and `icons.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_FLASH` | 1 | settings, user presets and the autosave in flash |
| `FELUCCA_OTA` | 1 | update entry (needs `FELUCCA_FLASH`) |
| `FELUCCA_CDC` | 1 | USB serial console |
| `FELUCCA_CDC_DEFAULT` | 1 | `0`: the console is built in but left out of USB from boot (as MENU > USB SERIAL OFF) |
| `FELUCCA_UAC` | 1 | USB audio input (the master output, stereo, 44.1 or 48 kHz) |
| `FELUCCA_UAC_48K` | 1 | `0`: the USB audio input at 44.1 kHz only, as up to 1.0.5.2 (`firmware/bsp/chip/usb.c`) |
| `FELUCCA_UAC_AS_FIRST` | 1 | the audio control header lists the audio streaming interface before the MIDI one, so the USB audio driver of older macOS (up to 15) finds it; `0`: 1.0 to 1.5's order, byte for byte (`firmware/bsp/chip/usb.c`) |
| `FELUCCA_CPU2` | 1 | the second core is built in (MENU > SYSTEM > 2ND CORE, OFF by default); `0`: none of it, 1.6's one-core code and RAM (`firmware/bsp/chip/fm1_cpu1.h`, `firmware/src/fx.c`) |
| `FELUCCA_UART` | 1 | TRS MIDI IN |
| `FELUCCA_ICONS` | 1 | parameter icons on the knob cards |

`FELUCCA_USB_LAYOUT` (`0` to `3`, default `0`) picks other USB descriptor layouts for testing; see
`firmware/bsp/chip/usb.c`. `FELUCCA_USB_PRODUCT` names the USB device (default `WaveStorm FM-1`; keep "FM-1" in it,
the installers look for it).

## Browser demo

```
./build.sh
web/emu/build.sh
python3 -m http.server --bind 127.0.0.1 -d build/emu 8790     # open http://localhost:8790/
```

`web/emu/felucca_web.c` builds the firmware's sources as the host tests do, against a simulated FM-1, and
Emscripten compiles it to WebAssembly (`build/emu/felucca.wasm`, with `index.html`, `worklet.js`, the font in `fonts/`
and the web editor as `editor.html`). It is a demo: the page keeps the flash's storage sectors (user presets, settings,
the autosave) in the browser; USB (audio, MIDI out, updates) and TRS MIDI are not emulated. The editor protocol is:
`editor.html` in another tab reaches it through the page (see WAVESTORM.md). Open it at `localhost` or `127.0.0.1`
(the audio needs a secure context).

## Tests

```
sh tests/ws_tests.sh
```

Runs every host test after `./build.sh` (it uses `build/gen` and `build/felucca.dis`): the STORM engine and the whole
firmware under ASan/UBSan, Felucca's tests of the platform WaveStorm keeps unchanged (keys, TRS and USB MIDI, USB
audio and descriptors, flash storage, the update entry, loader, package and installer, the text renderer), and the
target cost of the render loops in the pi32v2 disassembly against `tests/target_budget.txt` (after an intended change,
`BUDGET_UPDATE=1` rewrites it). The loader test needs `AC79_SDK` as for the build, the font spacing test a Python with
Pillow (`PYTHON=`); without them they are skipped. See [WAVESTORM.md](WAVESTORM.md).

## Install

Use the web installer in Chrome or Edge:
<https://hugelton.github.io/Felucca/webapp/installer/>. It installs the released package.

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/wavestorm.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Or, to install your own build from the web installer, make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
python3 web/make_site.py build/wavestorm.fwsc dev /tmp/wavestorm-site
cd /tmp/wavestorm-site && python3 -m http.server 8000
# open http://localhost:8000/webapp/installer/
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
