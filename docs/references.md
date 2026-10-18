# Implementation references and dependencies

The r0.1 protocol core is new C++ code implementing published TPDD file-mode
packet formats. No mComm, LaddieAlpha, Freenove application code, ROM images,
client installers, or dl2 source files are bundled. Version/geometry bytes,
command numbers, board pin assignments, and register addresses are interface
facts; they are not claims of implementing every feature of a real TPDD2.

Consulted references:

- [TPDD Base Protocol](https://www.bitchin100.com/wiki/index.php?title=TPDD_Base_Protocol): packet/checksum format and file commands.
- [DeskLink2](https://github.com/bkw777/dl2): comparison of padded filenames, directory responses, TPDD2 version data, and command behavior. Its GPLv2 license must be respected if its code is imported later.
- [Freenove board schematic and specifications](https://github.com/Freenove/Freenove_ESP32_S3_Display): hardware facts, detailed in `hardware.md`.
- Freenove's touch-drawing example confirms the landscape coordinate transform (raw Y to screen X, inverted raw X to screen Y). This project reads the documented FT5x06 registers directly instead of importing the vendor's application code.
- mComm and LaddieAlpha documentation for service/folder/bootstrap workflow; see `ui.md` for evidence and compatibility limits.

Build dependencies are downloaded by PlatformIO, not vendored:

| Dependency | Version used | Upstream licensing |
| --- | --- | --- |
| PlatformIO Core | 6.1.18 | Apache-2.0 |
| Espressif32 platform | 6.10.0 | Apache-2.0 |
| Arduino-ESP32 | 2.0.17 | LGPL-2.1 and component-specific licenses |
| TFT_eSPI | 2.5.43 | FreeBSD/BSD-2-Clause; bundled assets have their own notices |
| LVGL | 8.3.11 | MIT; bundled fonts/assets retain their notices |
| pyserial (host tool only) | 3.5 | BSD-3-Clause |

Retain upstream notices and review component/source obligations before public
binary distribution. This testing revision does not establish a project-wide
license choice or grant rights to redistribute third-party client/ROM payloads.
