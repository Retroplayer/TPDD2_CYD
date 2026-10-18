# Directory-first UI design

## Initial scope

The first firmware targets the Freenove FNK0104B in landscape orientation
(320 x 240), with a Model 100 / TPDD2 profile. Each TPDD2 bank serves the
ordinary files in one selected SD directory. Disk-image mounting is future
work. This document defines the intended behavior. Revision r0.1 implements
the basic folder/service flow; [the testing guide](testing.md) describes its
shipped features and limitations. Other sections remain design targets.

Directory mounting provides file-level drive emulation. It does not provide
raw-sector access or a coherent disk image for software that bypasses file
operations. Those capabilities must not be advertised in this version.

## Screens

### Drive (home)

Show the active computer/client profile and drive protocol, then its storage
and service state. In TPDD2 mode, show two large cards labeled **Bank 0**
and **Bank 1**. Each card shows its selected folder
or **Not mounted**, its read-only state, and a large **Choose folder** action.
Selecting a mounted bank opens its details, including **Change folder**,
**Read only**, and **Unmount**. Do not expose destructive file-management
actions on the home screen.

Follow the existing TPDD-server workflow: choose shared storage, start serving,
then operate files from the laptop's disk client. Offer **Start serving** and
**Stop serving**, with explicit states. Stopping while a file is open must be
rejected rather than silently discarding writes. A saved **Start automatically**
preference may start the service after the saved mounts validate. The UI does
not push selected files to the laptop during normal disk service.

Keep a status area visible for **Starting**, **Waiting for requests**,
**Reading <file>**, **Writing <file>**, **Finishing write**, and actionable
errors such as **SD card missing**. Show when the last valid request arrived;
an open UART is not proof that a computer is connected. Activity must come
from actual protocol/storage events, not decorative animations.

Use large touch targets, approximately 40 pixels high where practical.
Use text as well as color for bank and error states. Truncate long paths
on the home screen; show the full path in bank details.

### Choose folder

Open at the bank's current folder, or the SD root if none is selected.
Show a scrollable, paginated list of folders, **Up**, **Cancel**, and
**Use this folder**. Files do not appear as mount choices. Keep the current
path visible and provide a way to inspect it in full.

The picker changes only a draft selection. **Cancel** preserves the active
mount; **Use this folder** submits a mount request. Show pending state until
the storage owner confirms success. A failed selection preserves the old
mount and shows the reason. Missing folders are not created automatically.

Folder navigation here happens on the touchscreen. It does not imply that
the Model 100 can navigate SD subdirectories through TPDD2. DME navigation
is a separate TPDD1 extension and is outside the initial profile.

A later TS-DOS/DeskLink profile should instead expose a **Shared root** and
the client-selected **Current folder**. Client navigation must stay inside
that root. Show those paths separately so a request from the laptop never
looks like an unexplained local mount change. Do not imitate DME directories
inside strict TPDD2 mode.

### Settings

Initially expose the selected profile, serial settings, and storage status.
The Model 100 / TPDD2 profile defaults to 19,200 baud, 8N1, with the audited
RTS/CTS wiring. Keep unsupported profiles out of the selector. Changes to
serial settings require an idle protocol session and an explicit Apply action.

Diagnostics may later include error counters, buffer occupancy, and a bounded
serial trace. Keep protocol bytes and debug text on separate transports.

### Load disk software (planned follow-up)

Borrow mComm's separate **Inject DOS** workflow, labeled **Load disk software**:
select the computer and a supported loader, display the exact command to type
on that computer, wait for **Ready — send**, then show bytes sent and failures.
Return to disk service afterward. Sending a bootstrap requires an idle session
and temporarily owns the serial port; it cannot run alongside TPDD requests.
Serial completion means **Loader sent**, not proof that the laptop installed
or ran it. Keep this action separate from ordinary host-initiated file access.

Only offer loaders whose transfer behavior and distribution rights have been
verified. Do not bundle ROMs or client binaries merely because a reference
server distributes them. REX# update/test programs must not be run automatically.

## Existing-server and REX# references

The UI should borrow established workflows while retaining a layout suitable
for a small touchscreen. DeskLink2 remains the proposed protocol foundation;
studying another server's UI does not imply importing its implementation.

| Reference | Evidence inspected | Design implication |
| --- | --- | --- |
| mComm | Club100 manual: root folder, client-driven TPDD access, TS-DOS directory extension, DOS injection | Make sharing status and folder selection prominent; provide a separate guided loader action |
| LaddieAlpha | Documentation: DeskLink directories, configurable filename widths, client-specific extension handling | Put naming and directory behavior in client profiles rather than hard-coding Model 100 assumptions into widgets |
| REX# | Published server compatibility table and updater instructions | Treat REX Manager and its update loader as distinct compatibility targets |

The archived mComm manual includes an older v1.65 Windows screenshot and some
mixed-platform wording. It is workflow evidence, not a definitive description
of the latest interface or proof of interoperability with our firmware.

REX# documentation reports mComm Windows and LaddieAlpha working with specific
tested combinations, while real TPDD1 and TPDD2 drives failed in the listed
build-19/Model-100 tests. Therefore, basic TPDD2 emulation does not establish
REX# compatibility. Record client version, server profile, filenames, and
observed request/response behavior during validation. Do not label a profile
**REX# compatible** until tested.

REX# support should cover serving files for ROM-image loading and RAM-image
save/restore, as well as a separately validated updater-file workflow. The
REX# updater documentation specifies case-sensitive six-character base names
and an automatically supplied `.BR` extension. Preserve those names and binary
contents; do not apply automatic text conversion. REX# support does not imply
REXCPM support, larger packets, faster baud rates, or Sardine sector access.

For the next compatibility milestone, investigate a Model 100 / TS-DOS / DeskLink
profile alongside strict TPDD2 banks. Its implementation and REX# tests are
additional work; the initial directory-backed TPDD2 scope remains unchanged.

## Mount and data safety

- Start with both banks unmounted unless a saved configuration validates.
  Suggest folders such as `/TPDD/BANK0` and `/TPDD/BANK1`; do not create or
  mount them silently. Save successful explicit selections.
- Store card identity when available as well as paths. On startup or card
  replacement, revalidate saved mounts. If card identity cannot be established,
  ask for mount confirmation after reinsertion rather than assuming an identical
  path identifies the same contents. A missing folder stays unmounted.
- Initially prevent both banks from mounting the same canonical folder.
  Reject paths outside the mounted SD filesystem.
- Changing or unmounting a bank is allowed only when it has no open client
  file and no request in progress. A quiet UART alone does not establish this.
  Reject a busy request with **Close the file on the computer and try again**;
  do not queue an unexpected folder swap for later.
- Complete and check buffered writes before acknowledging a close or unmount.
  Software unmount is distinct from physical card removal. Offer **Eject SD**
  in storage settings only when both banks can be closed safely; disable
  further card access before reporting that removal is safe.
- On unexpected card removal, stop using stale handles, fail affected operations
  with appropriate protocol errors, and show the fault. Never report an
  unsuccessful write as saved. Power-loss durability remains limited by the
  filesystem and card; successful close is not a guarantee against all power loss.
- Enforce read-only mode in the storage layer, including create, append,
  delete, rename, and format requests. Initial directory-backed operation
  rejects format without deleting SD contents. Never interpret a host format
  command as permission to format the SD card.
- Define filename mapping before implementing file operations. The initial
  profile should use unambiguous Model 100-compatible 6.2 names. Never silently
  select one file when multiple SD names map to the same client-visible name.
  The folder picker must surface naming conflicts and unsupported files.
- Directory-backed free-space and directory-limit responses need explicit
  protocol compatibility rules; SD capacity must not be blindly encoded into
  fixed-width TPDD fields. Resolve these in the protocol/storage implementation.

## Implementation boundaries

Use LVGL for widgets, navigation, and display of a state snapshot. A protocol
task handles serial requests. A storage owner serializes file operations and
mount changes; the UI must not open or close protocol-owned file handles.
Queues carry UI commands and operation results. Publish bounded activity and
error events; coalesce routine updates so UI backlog cannot delay serial I/O.

The shared model separates:

- **Protocol:** TPDD2 initially; other drive protocols later.
- **Computer/client profile:** compatibility behavior and serial defaults.
- **Mount:** stable identity, source type, path, read-only flag, and lifecycle.

Only `directory` sources are implemented initially. Keep file operations
behind an interface so image-backed storage can be added without putting
image assumptions into the folder UI. Future protocols can present mounts as
drives or volumes instead of banks. Screen labels and mount count come from
the active protocol's capabilities; there is no universal two-bank assumption.

## Acceptance checks for implementation

1. Select two distinct folders and verify the Model 100 sees the correct files
   in each bank. A successful write affects only the selected bank's folder.
2. Cancel browsing, reject invalid paths, and simulate a failed mount; the
   active folder stays unchanged. Handle empty and very large folder lists
   with bounded memory and responsive touch.
3. Attempt a swap, unmount, read-only change, or serial reconfiguration while
   a client file is open; reject it without disrupting the active operation.
4. Verify read-only enforcement, non-destructive format rejection, and name
   collision handling using actual protocol requests.
5. Remove/reinsert the card and reboot with missing or changed media; never
   silently serve a replacement folder or reuse an invalid handle.
6. Transfer binary files while browsing folders and refreshing activity;
   compare contents and verify serial errors and buffer bounds.

Follow-up compatibility checks: TS-DOS directory navigation within the shared
root; REX# ROM-image load and RAM-image round trips with byte comparison;
guided loader transfer; and serving the documented updater filenames without
case or content changes. Run destructive REX# test/update utilities only as
an explicit separate user operation with appropriate backups.

These checks are pending firmware and physical hardware. Their presence is
not evidence that the intended behavior has been implemented or verified.

## Reference material

- [Kurt McCullum's Club100 library](https://www.club100.org/memfiles/index.php?directory=Kurt%20McCullum): inspected `mComm Manual.zip`, especially `tpdd.html`, `dos.html`, and `images/mComm.png`.
- [LaddieCon / LaddieAlpha documentation](https://bitchin100.com/wiki/index.php?title=LaddieCon&oldid=4555).
- [REX# documentation and compatibility table](https://bitchin100.com/wiki/index.php?title=REXsharp&oldid=4463).
- [DeskLink2](https://github.com/bkw777/dl2).
