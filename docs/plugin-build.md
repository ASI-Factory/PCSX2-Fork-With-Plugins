# Maintaining fork builds

The fork is compiled from this repository, with its plugin changes, by the
unchanged upstream `windows_build_matrix.yml` / `windows_build_qt.yml` workflows.
`plugin_release.yml` runs after that Windows workflow succeeds, downloads the
artifacts from that exact run, verifies that their emulator exports the guest
module API, adds the injector, and updates the fork's `latest` release. Original
PCSX2 is an optional separate download in the release notes.

There are no copied `_fork.yml` build recipes to update. Compiler, dependency,
matrix and build/test changes apply when upstream is merged. Symbol artifacts
remain available on the original build run. Existing six release filenames are
preserved; additional Windows x64 build profiles receive names automatically.

The packaging workflow must exist on the default branch. It accepts only a
successful push build from this repository's current default-branch commit;
PR artifacts, other repositories/workflows, and stale commits cannot update the
release. A manual dispatch takes the completed build's run ID to retry packaging
without recompiling. It still validates that run's provenance. Publish the
migrated injector release first; old packages without the module profile fail.

Two small upstream interfaces remain: the workflow display name
`🖥️ Windows Builds` in `workflow_run.workflows`, and the
`PCSX2-windows-Qt-x64-` artifact prefix. If upstream renames either, update the
packaging trigger/prefix. Build steps are never mirrored. GitHub's existing fork
sync still merges the upstream source and workflow changes normally.

`docs/plugin-release.md` and `docs/plugin-upstream-download.md` provide marked
release sections. The composer replaces only those sections and preserves
manually written release notes. The upstream README remains untouched.

## Core integration

The runtime and host exports live in `pcsx2/PluginModuleRuntime.inc` and
`PluginHostRuntime.inc`; OSD code lives in `pcsx2/ImGui/PluginOverlays.inc`. They
are included by existing compiled translation units. CMake, Visual Studio project
files/filters and `VMManager.h` need no plugin-specific entries.

Small calls remain at actual integration boundaries: ELF lifecycle/initialization,
throttling, EI, syscall dispatch/heap size, save-state checks and OSD rendering.
The existing guest before-UI rendering integration remains intact. The syscall
helper preserves the BIOS dispatch for heap setup; only module returns, private
render requests and reserved-memory queries are handled directly.

Local checks:

```powershell
msbuild tests/guest-modules/RuntimeTests.vcxproj /p:Configuration=Release /p:Platform=x64
./build/guest-module-tests/RuntimeTests.exe
python -B -m unittest discover -s tests/plugin-release -v
```

The reduced integration rebuilt the full MSVC Release x64 application in the
actual fork `bin` directory. A fresh CMake/Clang configuration also compiled all
319 core objects without changes to build listings; the full CMake Qt application
was not built in this check. The production runtime passed 307 checks, and the
GTA VCS smoke check completed with existing fixes, CLEO, native rain and both C
probes. Seven packaging/release tests passed. A local package made from the rebuilt
fork executable, a real resource and the injector archive passed content and ZIP
CRC checks; an original PCSX2 executable was rejected. Workflow syntax checks
passed. Hosted packaging still requires its first run after the changes are
committed. Local build and smoke logs are under `build/plugin-maintenance/`.
