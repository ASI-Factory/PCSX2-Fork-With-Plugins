"""Package this fork's build artifacts; never substitute an original PCSX2 binary."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile

PREFIX = 'PCSX2-windows-Qt-x64-'
LEGACY_NAMES = {
    'sse4-msvc': 'MSVC-SSE4', 'avx2-msvc': 'MSVC-AVX2', 'cmake-msvc': 'CMake-MSVC',
    'sse4-clang': 'Clang-SSE4', 'avx2-clang': 'Clang-AVX2', 'cmake-clang': 'CMake-Clang',
}
EXCLUDED = {'.bsc', '.exp', '.ilk', '.iobj', '.ipdb', '.pdb', '.lib', '.map', '.tmp'}


def validate_run(run, repository, branch, branch_sha):
    if (run.get('conclusion') != 'success' or run.get('event') not in ('push', 'workflow_dispatch') or
        run.get('head_repository', {}).get('full_name') != repository or
        run.get('head_branch') != branch or
        run.get('path') != '.github/workflows/fork_build.yml' or
        run.get('head_sha') != branch_sha or not re.fullmatch('[0-9a-f]{40}', branch_sha)):
        raise ValueError('Expected a successful Windows build of this fork at the current default-branch commit')
    return branch_sha


def package_name(artifact, source_sha):
    match = re.fullmatch(re.escape(PREFIX) + r'([a-z0-9-]+)-sha\[([0-9a-f]{7,40})\]', artifact)
    if not match or not source_sha.startswith(match[2]):
        raise ValueError(f'Unexpected artifact name/source commit: {artifact}')
    return 'PCSX2Fork-Windows-x64-' + LEGACY_NAMES.get(match[1], match[1]) + '.zip'


def verify_fork_executable(path):
    """Check the x64 PE export table for the compiled guest module host API."""
    data = path.read_bytes()

    def read(at, size):
        if at < 0 or size < 0 or at + size > len(data):
            raise ValueError(f'Truncated executable: {path}')
        return data[at:at + size]

    nt = struct.unpack('<I', read(0x3c, 4))[0]
    if read(0, 2) != b'MZ' or read(nt, 4) != b'PE\0\0' or read(nt + 4, 2) != b'\x64\x86':
        raise ValueError(f'Expected a Windows x64 executable: {path}')
    count = struct.unpack('<H', read(nt + 6, 2))[0]
    optional = nt + 24
    optional_size = struct.unpack('<H', read(nt + 20, 2))[0]
    if not 0 < count <= 96 or optional_size < 120 or read(optional, 2) != b'\x0b\x02':
        raise ValueError(f'Invalid x64 image headers: {path}')
    sections = []
    for index in range(count):
        section = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw = struct.unpack('<IIII', read(section + 8, 16))
        flags = struct.unpack('<I', read(section + 36, 4))[0]
        read(raw, raw_size)
        sections.append((rva, raw_size, raw, flags))

    def location(rva, size):
        found = [(raw + rva - start, raw + length, flags) for start, length, raw, flags in sections
                 if start <= rva and rva + size <= start + length]
        if len(found) != 1:
            raise ValueError(f'Invalid/ambiguous image address: {path}')
        return found[0]

    def at_rva(rva, size):
        at, _, _ = location(rva, size)
        return read(at, size)

    export_rva, export_size = struct.unpack('<II', read(optional + 112, 8))
    if not export_rva or export_size < 40:
        raise ValueError(f'Missing guest host exports; rebuild this fork: {path}')
    exports = at_rva(export_rva, 40)
    functions, names, addresses, pointers, ordinals = struct.unpack_from('<IIIII', exports, 20)
    if not 0 < names <= functions <= 65536:
        raise ValueError(f'Invalid export table: {path}')
    for index in range(names):
        name_rva = struct.unpack('<I', at_rva(pointers + index * 4, 4))[0]
        at, end, _ = location(name_rva, 1)
        terminator = data.find(b'\0', at, min(end, at + 4096))
        if terminator < 0:
            raise ValueError(f'Unterminated export name: {path}')
        if data[at:terminator] == b'GetGuestPluginHostApi':
            ordinal = struct.unpack('<H', at_rva(ordinals + index * 2, 2))[0]
            if ordinal >= functions:
                raise ValueError(f'Invalid guest host ordinal: {path}')
            address = struct.unpack('<I', at_rva(addresses + ordinal * 4, 4))[0]
            if export_rva <= address < export_rva + export_size or not location(address, 1)[2] & 0x20000000:
                raise ValueError(f'Guest host export is not compiled code: {path}')
            return
    raise ValueError(f'Guest module API missing; refusing an original/obsolete PCSX2 binary: {path}')


def package(artifacts, injector, output, source_sha):
    if not re.fullmatch('[0-9a-f]{40}', source_sha):
        raise ValueError('Invalid source SHA')
    with zipfile.ZipFile(injector) as archive:
        # The fork loads ASIs itself. It does not need version.dll or the old invoker.
        required = ('PCSX2PluginInjector.asi', 'PCSX2PluginInjector.stock.ini')
        if any(archive.namelist().count(name) != 1 for name in required):
            raise ValueError('Update the injector release to the relocatable module ABI first')
        additions = {name: archive.read(name) for name in required}
        if not additions[required[0]] or not additions[required[1]]:
            raise ValueError('Empty injector payload')
    plans = []
    for artifact in sorted(artifacts.iterdir()):
        if not artifact.name.startswith(PREFIX) or artifact.name.endswith('-symbols'):
            continue
        name = package_name(artifact.name, source_sha)
        if any(name == previous[0] for previous in plans):
            raise ValueError(f'Duplicate build variant: {name}')
        files = []
        for file in sorted(artifact.rglob('*')):
            if file.is_symlink():
                raise ValueError(f'Unexpected artifact symlink: {file}')
            if file.is_file():
                relative = file.relative_to(artifact).as_posix()
                if file.suffix.lower() not in EXCLUDED and relative.lower() != 'version.dll' and relative not in additions:
                    files.append((file, relative))
        executables = [file for file, relative in files if PurePosixPath(relative).parent == PurePosixPath('.')
                       and file.name.startswith('pcsx2-qt') and file.suffix == '.exe']
        if len(executables) != 1:
            raise ValueError(f'Expected exactly one compiled emulator in {artifact}')
        verify_fork_executable(executables[0])
        plans.append((name, files))
    if not plans:
        raise ValueError('No Windows x64 fork build artifacts found')
    output.mkdir(parents=True, exist_ok=True)
    for name, files in plans:
        temporary = output / (name + '.tmp')
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for file, relative in files:
                archive.write(file, relative)
            for relative, data in additions.items():
                archive.writestr(relative, data)
        temporary.replace(output / name)
    print(f'Packaged {len(plans)} variants compiled from this fork at {source_sha}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validate-run', type=Path)
    parser.add_argument('--repository')
    parser.add_argument('--branch')
    parser.add_argument('--branch-sha')
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--injector', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--source-sha')
    args = parser.parse_args()
    if args.validate_run:
        if not all((args.repository, args.branch, args.branch_sha)):
            parser.error('Run validation requires repository, branch and current branch SHA')
        print('sha=' + validate_run(json.loads(args.validate_run.read_text()), args.repository, args.branch, args.branch_sha))
    else:
        if not all((args.artifacts, args.injector, args.output, args.source_sha)):
            parser.error('Packaging requires artifacts, injector, output and source SHA')
        package(args.artifacts, args.injector, args.output, args.source_sha)


if __name__ == '__main__':
    main()
