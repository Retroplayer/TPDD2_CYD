# r0.1 testing guide

## Status and intended first test

This is an alpha for **Freenove FNK0104B / ESP32-S3 / 16 MB flash / 8 MB OPI
PSRAM / 2.8-inch capacitive-touch ILI9341 display**. It is not a binary for
other CYDs, original ESP32 Freenove boards, or the PanelLan.

The portable protocol core has native tests. The complete firmware is
cross-compiled, not executed on the cloud machine. Display/touch, SD hardware,
UART timing, actual RTS/CTS, and Model 100/client compatibility still require
physical testing. No REX# compatibility claim is made.

Use an expendable SD card or backed-up test folders. Normal writable TPDD
operations include append, delete, and rename. A client's overwrite command
sequence may delete the old file before creating its replacement. Files are
not journaled or transactionally replaced in this revision.

## Build or flash

Use the commands in the README. PlatformIO dependencies are pinned in
`platformio.ini`; board-specific settings are in
`boards/freenove_fnk0104b.json`. The build uses Arduino-ESP32 2.0.17,
TFT_eSPI 2.5.43, and LVGL 8.3.11.

For a source build, upload using PlatformIO:

```sh
.venv/bin/pio run -t upload --upload-port PORT
.venv/bin/pio device monitor --port PORT --baud 115200
```

Replace `PORT` with the board's USB serial port, for example `/dev/ttyACM0`
or `COM5`. If USB download is unavailable, use the board's BOOT/RESET
procedure to enter download mode; its port may change afterward. The firmware
uses native USB for diagnostics and UART1 exclusively for the TPDD cable.

For a packaged merged binary, use esptool 4.5.1 in a Python environment:

```sh
python -m pip install esptool==4.5.1
python -m esptool --chip esp32s3 --port PORT --baud 460800 write_flash 0x0 build/tpdd2-r0.1-fnk0104b.bin
```

The merged file includes bootloader, partition table, boot metadata, and app.
Its SHA256 is in the adjacent `.sha256` file. Flash at **0**, not `0x10000`.
Flashing the merged image replaces the existing application and clears saved
settings in the gaps it covers. It does not erase the removable SD card.
There is no need to run a whole-chip erase command.

If `PLATFORMIO_CORE_DIR` points somewhere other than the default, preserve
that variable when running both PlatformIO and `tools/package-firmware.py`.
Packaging can also take `--core-dir PATH`.

## Prepare storage

On a PC, format the test card FAT32 and prepare:

```text
/TPDD/BANK0/HELLO.DO
/TPDD/BANK1/SECOND.DO
```

Use small ASCII text files first. Later include binary files to detect data
corruption. The server never creates these directories or formats the card.

The first implementation has deliberately bounded behavior:

- Up to **40 supported files per bank**, each at most **65,534 bytes**.
- A virtual **80-sector bank**, 1,280 bytes per sector. Writes charge each
  file's rounded-up sector count; this is a virtual capacity policy, not a
  sector-accurate floppy image or a reflection of the SD card's free space.
- Names have 1-6 base characters and an optional 1-2 character extension.
  Supported characters are ASCII letters, digits, `_`, `-`, and `#`.
  Client names are uppercased and presented as padded 6.2 names. Existing
  lowercase SD names remain accessible. Automatic text/extension conversion
  is not performed.
- Unsupported names are hidden and counted in the mount result. Files over
  the size limit, too many supported files, or two names collapsing to one
  client name reject the mount. Subfolders are navigable on the touchscreen
  but are not listed as files to the Model 100.
- New-file open is exclusive: an existing file is never silently truncated.
  Append, delete, and rename are available when the bank is writable.
- Format requests always fail with write-protect status, including on a
  writable bank. Raw-sector and memory requests are unsupported.

## First boot and local UI

1. Start with the Model 100 disconnected. Check the RS-232 adapter and cable
   against [the hardware plan](hardware.md), especially voltage levels and
   the absence of an RTS/CTS loopback.
2. Power the board over USB with the test card inserted. Check the USB log
   for flash/PSRAM sizes. A landscape UI should appear. Confirm that touch
   targets line up with your finger across the screen before using writable mode.
3. Press **Folder** for Bank 0. Navigate to `/TPDD/BANK0` and choose
   **Use this folder**. Repeat with Bank 1 and `/TPDD/BANK1`.
4. Each selected folder starts **Read only**. Last-used folder paths are
   saved in flash only to reopen the picker conveniently. Banks always start
   unmounted after reboot; no saved path is silently applied to a different card.
5. Press **Start**. Requests are answered only while serving. Settings show
   counters, free memory, and the age of the last valid request. USB logs are
   printed every two seconds; they never enter the TPDD UART.

Mount, unmount, read-only changes, service stop, and SD eject are rejected
while a client file is open. Close it from the client first. Folder scans run
in the worker and use bounded pages; large physical directories can still
make scans slow. Commands wait for a request and queued response to complete.

Use **Eject SD** before removing the card. It stops service, unmounts banks,
then ends the SD driver. Press **Retry SD** after inserting a card and select
folders again. Unexpected physical removal is not fully detected in this
revision: reported I/O failures stop a session, but removal can resemble EOF
or remain undetected by the driver. Do not hot-remove a mounted card.

## PC serial smoke test (optional before Model 100)

Use a real RS-232 adapter, not a TTL USB-UART lead, on the line side of the
MAX3232E. Verify the PC-to-emulator cable signal routing; do not add a null
modem merely because a different desktop TPDD server requires one.
The Model 100 cable in this project makes the emulator electrically DCE.

With the board serving Bank 0, run:

```sh
.venv/bin/python tools/serial-smoke.py --port PORT --bank 0
.venv/bin/python tools/serial-smoke.py --port PORT --bank 0 --read HELLO.DO --expected /path/to/original/HELLO.DO
```

Here `PORT` is the **PC's RS-232 adapter**, not the board's native USB console.
The default smoke test only queries status/version and lists the directory.
`--read` adds binary readback; `--expected` compares exact bytes.

To exercise writes, make the chosen bank writable on the touchscreen and run:

```sh
.venv/bin/python tools/serial-smoke.py --port PORT --bank 0 --roundtrip
```

This explicitly creates a uniquely named `Txxxxx.DO`, writes 1,024 bytes,
reads and compares all bytes, then deletes the test file after success.
An interrupted or failed run may leave a partial file for inspection. Check
and clean it up before repeating many times.

Both ends normally enable CTS. If wiring/handshake diagnosis requires it,
**stop serving**, set **CTS OFF** in Settings, then start again. This disables
only the board's CTS gating; its receive-side RTS remains active. The CLI's
`--no-cts` independently disables PC-side CTS gating. These are diagnostics,
not evidence that Model 100 handshaking works; restore CTS for acceptance tests.

## Model 100 test

Use an already-installed TPDD client, such as TEENY or TS-DOS; loader injection
is not included yet. With the service running and both banks read-only:

1. Connect the correctly wired RS-232 cable and request a directory.
2. Load `HELLO.DO` from Bank 0. If the client supports bank selection, load
   `SECOND.DO` from Bank 1. Check both contents.
3. On backed-up test folders only, enable writes for one bank. Save a new
   small file from the Model 100, close it, load it back, and compare contents.
4. Repeat with binary data and verify the SD copy on a PC after safe eject.
5. Exercise touch navigation during transfers; watch bad-frame and UART error
   counters. Confirm no byte loss with real handshaking before increasing load.

Failure with a particular client is a compatibility finding, not automatically
an electrical fault. Capture the client name/version, the operation, USB
counters, and relevant signal levels. A client requiring DME directories,
raw sectors, RAM access, special loaders, or other server extensions will need
additional implementation.

## Recover a stuck session

If the laptop disconnects with a file open, normal controls intentionally
refuse to swap its storage. Settings -> **Recover** offers a confirmed
**Stop + close** action. Stop the laptop's transfer first. Recovery closes the
file, stops serving, resets the UART driver and parser, and discards queued
serial traffic. Partial files are retained and may be incomplete. Inspect
or remove them before restarting; recovery does not roll back a write.

A UART overflow/framing/parity error also stops the file session. If CTS
leaves a response queued indefinitely, use the explicit recovery action,
then inspect wiring or select diagnostic CTS-off while stopped. Power loss
and physical SD removal can still lose data; recovery cannot fix those.

## Automated checks and current scope

`tools/test-native.sh` runs the actual portable core against real temporary
files under ASan/UBSan. It covers binary round trips, bank isolation,
read-only enforcement, format refusal, filename collisions, size/capacity and
entry bounds, failed mounts, recovery, invalid paths, checksum errors,
truncated packets, and malformed commands.

`tests/serial_smoke_test.py` builds a native server using the same core and
runs the actual CLI over a Linux pseudo-terminal: directory and read tests
for both banks, then a binary write/read/delete round trip. This validates
software framing and file behavior, not ESP32 UART or RS-232 electrical timing.

The GitHub Actions workflow repeats native tests and the firmware build and
packages the image when run. No CI run or physical test has been performed
merely by adding the workflow.

Future work includes DeskLink/DME folder navigation, verified REX# workflows,
bootstrap loading, disk images, raw sectors/memory operations, autostart,
additional computer profiles, and improved removable-media detection.
