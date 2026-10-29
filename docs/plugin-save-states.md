# Guest plugins and save states

The fork stores relocatable guest plugins in the same EE RAM image as the game.
`PCSX2F Guest Modules.dat` stores the associated host runtime. Manual states,
quick slots, and resume states use the same archive path. Shutdown captures the
resume state before notifying guest plugins to discard their registrations.
This fork currently has no gameplay rewind facility.

## Compatibility

New plugin-bearing states are self-contained: the original ELF bytes are saved
alongside the host bookkeeping. Restoring parses those saved bytes at the saved
addresses to reconstruct registrations; the plugin code and globals execute
from the restored EE RAM. Updating, removing or adding installed plugins does
not prevent loading or replace the saved code. When the installed compatible
set differs by relative name or SHA-256, one short OSD note explains:
"Plugins from the save state are active. Restart the game to use the installed versions."
A matching set shows no note. Reset or restart initializes the installed set.
Configuration files are excluded from binary identity.

The earlier manifest version 1 did not store ELF bytes or export metadata.
Those older plugin states can be upgraded by loading with the original ELFs
available and saving again. A missing or changed original cannot be recovered
from that older format; the loader names the original ELF needed for upgrade.
Additional installed plugins do not prevent upgrading.

A state without the plugin section is treated as a state made without plugins.
Loading it stops existing guest-plugin consumers and clears module bookkeeping;
it does not install plugins into an already-running game. Reset or restart the
game to initialize the installed plugins. States require the same configured EE
RAM size as before; plugin states additionally require a complete EE RAM entry.

The injector must support `GuestPluginStateTransfer` and manifest version 2. An older injector produces
an update message instead of allowing a partial restore. Plugin-free states do
not require the new injector service when no guest modules are active.

## Section version 1

All scalar fields use the existing StateWrapper little-endian representation.
The section is bounded to 129 MiB; the injector manifest to 128 MiB, with at most
32 MiB per original ELF. Readers reject
unknown versions, truncation, invalid module counts/ranges, and trailing bytes.

| Field | Encoding |
| --- | --- |
| Section version | u32, currently 1 |
| Committed, running, thread ready | Three u8 booleans |
| Queue index, resume PC, thread return PC, heap return PC, memory return PC, saved SA | Six u32 |
| Captured runtime generation | u64; a restored layout receives a fresh host generation |
| Saved GPR and FPU structure sizes | Two u32; must match this build |
| Saved GPR, HI, LO, FPU | Fixed byte images using the same register layouts as the core state |
| Startup queue count | u32, at most 256 |
| Each queue entry | Entry PC, stack top, GP, context address; four u32 |
| Injector manifest byte count | u32 |
| Injector manifest | Bytes described below |

An uncommitted snapshot has no queue or manifest. A running snapshot's index
must refer to a queued module. After all initializers finish, the index equals
the queue length and the running flag is clear. Kernel startup return PCs and
saved registers allow a snapshot taken during startup to resume that phase.

The injector manifest starts with four u32 fields: version (2), disc CRC,
current ELF CRC, and module count. Each module stores a length-prefixed UTF-8
relative name, a length-prefixed 64-character lowercase SHA-256 digest, and
three u32 fields: base, image size, entry PC, then a u32 ELF byte count and the
original ELF bytes. Names are limited to 4096 bytes and must stay within PLUGINS.
The saved ELF hash, reparsed size/entry and startup queue must agree. Version 1
omits the ELF count/bytes and uses the original installed ELF for upgrade.
Host pointers, threads, graphics resources, and
filesystem handles are never serialized. The captured generation records the
original runtime instance; loading publishes a fresh host generation so native
consumers can invalidate guest-address caches even when loading repeatedly.

The injector service operations are snapshot (0), prepare/validate (1), suspend
old consumers (2), restore consumers with current host context (3), discard
staging (4), and validate the core startup queue (5). Operations execute on the
CPU thread. Prepare leaves running registrations untouched. The core clears
execution caches before restoring RAM; before-UI render measurements and cached
texture pointers are reset on the GS thread. Registration restoration does not
run guest constructors or plugin initialization again.
Restore (3) returns 1 when the installed set differs, otherwise 0; the core
shows the informational OSD only after successful restoration.

## Current configuration after loading

The injector refreshes `PluginData` from the current sidecar CFG (INI for older
plugins), settings-service capability words, and current window/display data.
Input, OSD, unthrottle and settings ownership registrations use the restored
module layout. Native guest-render callbacks are registered again when needed.
If the sidecar no longer exists, its saved PluginData and parsed guest settings
are retained and no settings-refresh generation is requested. A remaining CFG
still wins even if the corresponding installed ELF was removed or updated.
Plugins must ignore unknown keys in newer CFG files.

A plugin which caches parsed settings should export an aligned, writable
`uint32_t PCSX2FStateLoadGeneration = 0` with C linkage and a symbol size of four
bytes. When a current sidecar exists, after restoring RAM the injector
increments the restored word. At its next safe update, the plugin compares it
with a guest-owned last-seen value, reads current settings through the settings
bridge, and applies its live settings without reinstalling hooks. The plugin
updates its last-seen value after completing the refresh. This also handles a
state saved while a previous refresh was pending.

Plugins without this optional export can still restore their execution state.
Their initial configuration buffer is refreshed, but settings already cached
in globals or patched into game code require plugin-side refresh support;
the injector logs a warning rather than rerunning initialization over restored
memory. Restart-only options retain the plugin's own restart policy.

## Focused checks

Build and run `tests/guest-modules/SnapshotTests.vcxproj` in Release/x64 after
building the fork's Release common library. It exercises the production runtime
and snapshot serializer, including register round trips, truncation and legacy
teardown. The injector's `tests/GuestModuleStateTests.vcxproj` checks the manifest
codec and SHA-256 known vectors. Real-game acceptance remains necessary for
plugin hooks, native rendering, settings refresh, and startup timing.

The injector's `tests/GuestModuleStateServiceTests.vcxproj` exercises the built
injector with a synthetic ELF and a minimal host. It checks self-contained loads
with changed/missing/additional files, the installed-set note flag, missing CFG
preservation, corrupt ELF rejection before mutation, startup queue validation,
current sidecar contents, old-format upgrade and registration teardown/restoration.
It does not execute guest code or replace the real-game checks.
