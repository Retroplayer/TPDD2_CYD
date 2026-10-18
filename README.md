# TPDD2_CYD

**r0.1 bench-test firmware** for a Tandy Portable Disk Drive file server on the
**Freenove FNK0104B** (ESP32-S3, 2.8-inch capacitive touch). The primary client
is the **Tandy Model 100**.

This revision provides:

- An LVGL touchscreen UI with two SD-folder banks, a paginated folder picker,
  read-only controls, service start/stop, safe SD eject/retry, and diagnostics.
- A bounded TPDD packet parser and directory, select, create, append, read,
  write, close, delete, rename, status, condition, and version operations.
- A separate serial/storage worker with hardware RTS/CTS and USB-only logging.
- Native protocol tests, a serial smoke-test tool, and a reproducible firmware
  build with a merged flash image.

**Not yet verified on physical hardware or with a Model 100.** This is a
file-mode subset, not full TPDD2 emulation. Disk images, sector/memory commands,
TS-DOS/DeskLink subdirectory extensions, bootstrap injection, and REX#
compatibility are not implemented. Test with expendable files first.

## Build and test

```sh
python3 -m venv .venv
.venv/bin/python -m pip install platformio==6.1.18 pyserial==3.5
./tools/test-native.sh
.venv/bin/python tests/serial_smoke_test.py
.venv/bin/pio run -j 4
.venv/bin/python tools/package-firmware.py
```

Native tests require a Linux host and a C++17 compiler with AddressSanitizer
and UndefinedBehaviorSanitizer. PlatformIO builds the firmware on supported
Linux, macOS, and Windows hosts. Adjust virtual-environment paths for Windows.

Upload with `.venv/bin/pio run -t upload --upload-port PORT`. The merged image
is `build/tpdd2-r0.1-fnk0104b.bin`, flashed at **offset 0**; see the testing guide
for detailed instructions and initial SD setup.

- [Flashing, first-use instructions, serial tests, and limitations](docs/testing.md)
- [Hardware audit, RS-232 adapter, and Model 100 cable](docs/hardware.md)
- [UI design and future compatibility work](docs/ui.md)
- [Implementation references and third-party dependencies](docs/references.md)

The initial portable protocol core is new code implementing the documented
file protocol. [DeskLink2](https://github.com/bkw777/dl2) remains a reference for
compatibility and future functionality; this revision does not import its
full desktop implementation. mComm and LaddieAlpha inform the UI workflow.
