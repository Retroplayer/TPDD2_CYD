# Hardware audit and proposed wiring

## Decision and validation status

Target the **Freenove FNK0104B**, the ESP32-S3 2.8-inch capacitive-touch
board, and the **Tandy Model 100**. The FNK0104A lacks touch; original
ESP32 Freenove boards require a different pin allocation.

The documentation audit supports this design without sacrificing the built-in
microSD, display, touch, or USB interfaces. This is a **proposed electrical
design, not a bench-validated assembly**. Actual board revision, connector
orientation, supply levels, firmware timing, and client compatibility must be
checked before declaring hardware readiness.

## Resource budget

| Resource | Manufacturer documentation | Design consequence |
| --- | --- | --- |
| CPU | Dual-core ESP32-S3, up to 240 MHz | Sufficient for protocol handling and a modest UI |
| Internal SRAM | 512 KB, shared with runtime needs | Keep UART and critical buffers here; measure free heap |
| PSRAM | 8 MB OPI | Available for UI buffers and bounded directory data |
| External flash | 16 MB | Firmware and settings; bulk storage lives on SD |
| Display | 240 x 320 ILI9341 | One RGB565 framebuffer is 153,600 bytes |
| Touch | Capacitive, I2C | Retain manufacturer wiring |
| Storage | Built-in microSD | Start with FAT32; stream sectors rather than whole images |
| Expansion | P3 exposes GPIO2, GPIO3, GPIO14, GPIO21 | Four signals for data and hardware flow control |

The principal emulated memory regions and sector buffer in the inspected dl2
source total about 7.4 KiB, excluding other state, filesystem allocations,
directory listings, task stacks, and UI. No full disk image needs to reside in
RAM. Verify the actual flash and PSRAM sizes during firmware bring-up.

## Freenove connector allocation

The audited schematic is titled **2.8-inch ESP32-S3 display module Schematic**,
dated **2025-06-11**, in the Freenove repository at commit
`90004f874ed567b3c09d534d021172cb716e61a1`.

| Connector pin | Schematic net | Proposed function | ESP32 direction |
| --- | --- | --- | --- |
| P3.1 | IO2 | UART RX | Input |
| P3.2 | IO3 | UART TX | Output |
| P3.3 | IO14 | UART RTS: emulator receive-ready | Output |
| P3.4 | IO21 | UART CTS: permission to send to Model 100 | Input |
| P4.1 | VCC3V3 | RS-232 adapter supply | Power |
| P4.2 | GND | Adapter and cable signal ground | Ground |

Use connector pin numbers from the schematic and verify continuity on the
actual board. Wire colors and apparent left/right position are not reliable
pin identifiers. Confirm connector pitch and housing before ordering mating
leads. P4.3 and P4.4 are I2C signals and remain unused by the adapter.

Use **UART1 through the GPIO matrix** with RX=2, TX=3, RTS=14, CTS=21.
These are proposed firmware settings, not implemented configuration.
Keep the P2 UART0 connector unused for the TPDD connection; UART0 may emit
boot diagnostics. Keep native USB on GPIO19/20 for programming and logging.

### Reset and GPIO3

GPIO3 is a strapping pin for JTAG source selection, not the flash/download
boot-mode selector. With default eFuses it is ignored for that selection;
when strap-based selection is enabled, high selects USB Serial/JTAG and low
selects the external JTAG pins. Do not change eFuses for this project.

Assigning GPIO3 to **TX** connects it to a transceiver input rather than a
receiver output that could drive the strap during reset. Fit a **10 kOhm
pull-up to 3.3 V** on GPIO3. It defines the strap and holds TX at UART idle
while the ESP32 output is high impedance. Inspect the actual board for any
additional load on IO3 before fitting the adapter.

Fit a second **10 kOhm pull-up to 3.3 V** on GPIO14. It holds UART RTS
deasserted during reset, so the RS-232 output tells the Model 100 to stop
sending until firmware is ready. Preserve this state during initialization;
enable reception and buffers before asserting receive-ready.

Cold-start, reset, and USB download must work with the cable attached and the
Model 100 both on and off. A transfer interrupted by reset still requires
client recovery; these pulls do not preserve an in-progress transaction.

## RS-232 adapter

Use a **TI MAX3232E**, powered at **3.3 V**, with both drivers and both
receivers accessible. A single TX/RX converter module is insufficient for
this design. MAX3232EID in SOIC-16 on a suitable breakout is one assembly
option; verify the exact package and pinout when buying a part or module.

The table below uses the TI 16-pin package numbering and its DIN/DOUT/RIN/ROUT
names. Module labels such as TX and RX can use the opposite perspective.

| Connection | MAX3232E pin | Other end |
| --- | --- | --- |
| Supply | 16 VCC | Freenove P4.1, 3.3 V |
| Ground | 15 GND | Freenove P4.2 and Model 100 DB-25 pin 7 |
| Emulator TX | 11 DIN1 | Freenove P3.2, GPIO3 |
| Data to Model 100 | 14 DOUT1 | Model 100 DB-25 pin 3, RXD |
| Data from Model 100 | 13 RIN1 | Model 100 DB-25 pin 2, TXD |
| Emulator RX | 12 ROUT1 | Freenove P3.1, GPIO2 |
| Emulator receive-ready | 10 DIN2 | Freenove P3.3, GPIO14, UART RTS |
| Permission for Model 100 to send | 7 DOUT2 | Model 100 DB-25 pin 5, CTS |
| Model 100 RTS | 8 RIN2 | Model 100 DB-25 pin 4, RTS |
| Permission for emulator to send | 9 ROUT2 | Freenove P3.4, GPIO21, UART CTS |

For a bare IC at 3.3 V, follow the TI typical application:

- Four 100 nF charge-pump capacitors: pins 1-3, pins 4-5, pin 2-GND,
  and pin 6-GND. Use nonpolar ceramics rated at least 16 V and account for
  capacitance loss under DC bias.
- A 100 nF supply bypass and a 1 uF ceramic between pins 16 and 15, close
  to the IC. The latter is also required for TI's stated IEC ESD rating on
  the D, DB, and PW packages; the whole assembly still needs validation.
- The two 10 kOhm logic-side pull-ups specified above.

Do not connect the charge-pump V+ or V- nodes to the board supply. Power
the transceiver from 3.3 V so its receiver outputs remain ESP32-compatible.
RS-232 signals connect only to the transceiver's RS-232 side, never to GPIO.

## Model 100 cable

Make the emulator a **DCE-style peripheral**. Use a DB-25 male plug to mate
with the Model 100, either on the adapter or through a custom cable. The
signal table above already specifies the connection at the Model 100 end;
do not insert an additional null-modem crossover.

Add a removable **DB-25 pin 20 to pin 6 jumper** at the Model 100 end. This
returns the Model 100's DTR output to its DSR input for local ready status.
It is separate from flow control and does not report emulator power or
storage readiness. If real DSR status becomes necessary, add a driver and
remove the jumper; never connect two outputs to pin 6.

Do **not** loop pins 4 and 5: they carry the actual RTS/CTS connection.
The Model 100 RS-232 schematic shows no external DCD connection; leave pin
8 unconnected. Leave other unused DB-25 pins unconnected. Use pin 7 for
signal ground. Treat shield/shell bonding separately from signal return.

Use the connector's molded pin numbers: mating-face and solder-side views
are mirror images. Start with a short cable, preferably around one metre.
Power the Freenove over USB; do not derive power from serial control lines.

### Polarity and client behavior

At the ESP32, UART TX/RX use ordinary non-inverted TTL UART polarity.
MAX3232E performs the electrical inversion. UART RTS/CTS are active-low on
the ESP32 side: a low control signal becomes an asserted positive RS-232
voltage. Do not add firmware inversion on top of the transceiver.

The Model 100 technical reference shows that CTS/DSR are CPU-readable and
RTS/DTR are software-controlled. Wiring alone does not ensure that every
client uses these lines for pacing. Verify the behavior of the selected
Model 100 disk client; do not assume automatic receive-buffer flow control.
If a client does not provide useful RTS pacing, investigate its timing and
protocol expectations rather than tying emulator CTS permanently active.

Start at **19,200 baud, 8 data bits, no parity, 1 stop bit**. This is at most
1,920 data bytes/second. Keep diagnostics on USB and out of the protocol UART.
Use bounded RX/TX buffers and prioritize UART service over screen updates.
Size receive headroom for SD stalls and bytes already in flight; the hardware
FIFO threshold alone is not proof against application-buffer overruns.

## Parts to obtain

| Quantity | Item |
| --- | --- |
| 1 | Freenove FNK0104B, ESP32-S3 2.8-inch capacitive-touch board |
| 1 | microSD card formatted FAT32; a small SDHC card is sufficient |
| 1 | MAX3232E assembly with two drivers and two receivers exposed, operating at 3.3 V |
| 1 set | Charge-pump/bypass capacitors and two 10 kOhm pulls if absent from the assembly |
| 1 | DB-25 male connector, hood, and removable DTR/DSR jumper |
| 1 set | Correct mating leads for Freenove P3 and P4; cable and strain relief |
| 1 | Stable 5 V USB supply and suitable data cable |

A multimeter and a logic analyzer are useful for bring-up. An oscilloscope
or RS-232-rated analyzer can inspect the line side. Ordinary 3.3 V logic
analyzer inputs belong only on the TTL side.

## Bench acceptance checks (not yet executed)

1. **Unpowered continuity:** verify actual board identity, P3/P4 pin order,
   no supply shorts, every signal/power route through the transceiver,
   and the 20-6 jumper. Check that 4-5 are not shorted.
2. **Power and idle:** without the Model 100 attached, measure 3.3 V supply,
   GPIO3 high, GPIO14 high during reset, RS-232 data idle negative, and CTS
   to the Model 100 deasserted negative until firmware is ready. Verify the
   rail remains in specification with SD, display, and adapter active.
3. **Board coexistence:** identify flash/PSRAM sizes, operate display and
   touch, and read/write/verify a dedicated SD test file. Record available
   heap and maximum contiguous allocation with the intended UI active.
4. **Data loopback:** with the Model 100 disconnected, bridge DOUT1 to RIN1
   on the adapter. Exchange binary patterns at 19,200 8N1 through the actual
   transceiver, with byte-for-byte comparison. Remove the bridge afterward.
5. **Flow-control loopback:** still disconnected from the Model 100, bridge
   DOUT2 to RIN2. Exercise RTS and verify CTS follows with correct polarity;
   verify deasserted CTS pauses a queued transmission, allowing for a byte
   already in flight. Restore data loopback only if needed for this test.
6. **Model 100 signals:** remove all test bridges, attach the cable, and
   observe TXD, RXD, RTS, CTS, DTR, and DSR with the selected client. Confirm
   what the client actually does when CTS is deasserted and how it uses RTS.
7. **Protocol and stress:** with emulator firmware available, check drive
   detection, directory listing, file read/write, and both TPDD2 banks.
   Compare transferred binary data, repeat during SD writes and UI activity,
   and record UART errors and buffer high-water marks. Test injected pauses
   in both directions with enough buffer margin for the observed response.
8. **Reset/recovery:** repeat cold boot, reset, USB programming, cable
   disconnect/reconnect, and missing-card behavior. Confirm independent USB
   diagnostics and clean recovery after an interrupted transaction.

Passing the documentation audit does not substitute for these checks.

## Sources

- [Freenove model identification](https://github.com/Freenove/Freenove_ESP32_S3_Display/blob/90004f874ed567b3c09d534d021172cb716e61a1/README.md).
- [Freenove 2.8-inch S3 schematic](https://github.com/Freenove/Freenove_ESP32_S3_Display/blob/90004f874ed567b3c09d534d021172cb716e61a1/Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf), especially P3/P4 and MCU connections.
- [Manufacturer module specifications](https://github.com/Freenove/Freenove_ESP32_S3_Display/blob/90004f874ed567b3c09d534d021172cb716e61a1/Datasheet/ES3C28P_ES2N28P_Specification_V1.0.pdf), main-control parameters.
- [Espressif ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf), v2.2, sections 3.1 and 3.4: boot and JTAG strapping.
- [Espressif UART driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/uart.html): GPIO routing and hardware flow control.
- [TI MAX3232E datasheet](https://www.ti.com/lit/ds/symlink/max3232e.pdf), revision E, pin table, receiver behavior, and 3.3 V application circuit.
- [Model 100 Technical Reference](https://ftp.whtech.com/club100/doc/m100TechReference.pdf), printed pages 28-30 (PDF pages 36-38), especially figure 3-25: connector pins and signal directions.
- [TPDD base protocol](https://www.bitchin100.com/wiki/index.php?title=TPDD_Base_Protocol): 19,200 baud and request/response operation.
- [DeskLink2 source](https://github.com/bkw777/dl2): proposed protocol foundation; no code imported yet.
