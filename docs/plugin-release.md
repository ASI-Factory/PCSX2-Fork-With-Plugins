<!-- pcsx2f-runtime:begin -->
## Fork plugin support

These binaries are compiled from this fork's source, including its plugin support.
The Windows build jobs use the upstream build recipes in this repository; a
separate packaging workflow adds Plugin Injector after the fork builds succeed.
Upstream changes to compilers, dependencies, build jobs and tests therefore apply
without maintaining copies of those recipes in `_fork.yml` files.

PS2 guest plugins now use relocatable modules: the injector assigns a dynamic base,
private stack and heap, and initializes C++ constructors before the existing
plugin entry point. Rebuild old fixed-address plugins; they produce an update
warning. Existing game patches and PSP waits are preserved by the migration.

Enable **128 MB RAM**. Memory-card saves work normally; save/load states are blocked
while guest modules are active until module persistence is implemented. The fork
retains guest before-UI rendering for native rain plugins.

The hook runtime also provides optional EE floating-point ACC and VU0 register
preservation. Existing hooks keep their fast path; hooks requesting these flags
use a negotiated service and fail before installation on older hosts.
VU1, VIF and VU micro/data memory are outside this register-preservation service.

The compatible original PCSX2 download below is an optional alternative. It does
not replace the compiled fork binaries in this release.
<!-- pcsx2f-runtime:end -->
