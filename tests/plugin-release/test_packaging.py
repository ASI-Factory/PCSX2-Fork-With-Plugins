"""Exercise provenance, fork-binary checks, packaging, and release preservation."""
import copy
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


packager = module('packager', 'package-plugin-artifacts.py')
notes = module('notes', 'plugin-release-notes.py')
SHA = '1234567890abcdef1234567890abcdef12345678'


def executable():
    image = bytearray(0x600)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', image, 0x84, 0x8664, 1)
    struct.pack_into('<H', image, 0x94, 240)
    struct.pack_into('<H', image, 0x98, 0x20b)
    struct.pack_into('<I', image, 0x98 + 108, 16)
    struct.pack_into('<II', image, 0x98 + 112, 0x1000, 0x100)
    section = 0x98 + 240
    struct.pack_into('<8sIIII', image, section, b'.text', 0x400, 0x1000, 0x400, 0x200)
    struct.pack_into('<I', image, section + 36, 0x60000020)
    struct.pack_into('<IIIII', image, 0x200 + 20, 1, 1, 0x1050, 0x1060, 0x1070)
    struct.pack_into('<I', image, 0x250, 0x1200)
    struct.pack_into('<I', image, 0x260, 0x1100)
    symbol = b'GetGuestPluginHostApi\0'
    image[0x300:0x300 + len(symbol)] = symbol
    return image


class Packaging(unittest.TestCase):
    def fixture(self, root):
        artifacts = root / 'artifacts'
        build = artifacts / ('PCSX2-windows-Qt-x64-sse4-msvc-sha[' + SHA[:9] + ']')
        build.mkdir(parents=True)
        (build / 'pcsx2-qtx64.exe').write_bytes(executable())
        (build / 'version.dll').write_bytes(b'old loader')
        (build / 'game.pdb').write_bytes(b'debug')
        (build / 'resources').mkdir()
        (build / 'resources/patches.zip').write_bytes(b'patches')
        (artifacts / (build.name + '-symbols')).mkdir()
        injector = root / 'injector.zip'
        with zipfile.ZipFile(injector, 'w') as archive:
            archive.writestr('PCSX2PluginInjector.asi', b'new injector')
            archive.writestr('PCSX2PluginInjector.stock.ini', b'profile')
            archive.writestr('version.dll', b'ASI loader')
        return artifacts, build, injector

    def test_packages_fork_with_resources_without_loader_or_debug_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts, build, injector = self.fixture(root)
            packager.package(artifacts, injector, root / 'out', SHA)
            with zipfile.ZipFile(root / 'out/PCSX2Fork-Windows-x64-MSVC-SSE4.zip') as archive:
                self.assertEqual(archive.read('pcsx2-qtx64.exe'), (build / 'pcsx2-qtx64.exe').read_bytes())
                self.assertEqual(set(archive.namelist()), {'pcsx2-qtx64.exe', 'resources/patches.zip',
                    'PCSX2PluginInjector.asi', 'PCSX2PluginInjector.stock.ini'})
                self.assertEqual(archive.read('PCSX2PluginInjector.asi'), b'new injector')

    def test_refuses_original_or_invalid_executable_before_writing_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts, build, injector = self.fixture(root)
            image = executable()
            image[0x300] = ord('X')
            (build / 'pcsx2-qtx64.exe').write_bytes(image)
            with self.assertRaisesRegex(ValueError, 'API missing'):
                packager.package(artifacts, injector, root / 'out', SHA)
            self.assertFalse((root / 'out').exists())

    def test_rejects_forwarded_or_nonexecutable_guest_api(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'pcsx2-qt.exe'
            for address in (0x1008, 0xffffffff):
                image = executable()
                struct.pack_into('<I', image, 0x250, address)
                path.write_bytes(image)
                with self.assertRaises(ValueError):
                    packager.verify_fork_executable(path)
            image = executable()
            struct.pack_into('<I', image, 0x98 + 240 + 36, 0x40000040)
            path.write_bytes(image)
            with self.assertRaises(ValueError):
                packager.verify_fork_executable(path)

    def test_rejects_old_injector(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts, _, injector = self.fixture(root)
            with zipfile.ZipFile(injector, 'w') as archive:
                archive.writestr('PCSX2PluginInjector.asi', b'legacy')
            with self.assertRaisesRegex(ValueError, 'Update the injector'):
                packager.package(artifacts, injector, root / 'out', SHA)

    def test_names_follow_source_and_keep_existing_download_names(self):
        for profile, expected in packager.LEGACY_NAMES.items():
            self.assertEqual(packager.package_name(packager.PREFIX + profile + '-sha[' + SHA[:9] + ']', SHA),
                             'PCSX2Fork-Windows-x64-' + expected + '.zip')
        self.assertEqual(packager.package_name(packager.PREFIX + 'new-build-profile-sha[' + SHA[:9] + ']', SHA),
                         'PCSX2Fork-Windows-x64-new-build-profile.zip')
        with self.assertRaises(ValueError):
            packager.package_name(packager.PREFIX + 'sse4-msvc-sha[abcdef123]', SHA)

    def test_run_provenance_blocks_prs_other_repos_paths_branches_and_stale_runs(self):
        run = dict(conclusion='success', event='push', head_repository={'full_name': 'example/fork'},
                   head_branch='master', path='.github/workflows/windows_build_matrix.yml', head_sha=SHA)
        self.assertEqual(packager.validate_run(run, 'example/fork', 'master', SHA), SHA)
        for field, value in [('conclusion', 'failure'), ('event', 'pull_request'), ('head_branch', 'other'),
                             ('head_sha', '0' * 40), ('head_repository', {'full_name': 'PCSX2/pcsx2'}),
                             ('path', '.github/workflows/windows_build_matrix_fork.yml')]:
            bad = copy.deepcopy(run)
            bad[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                packager.validate_run(bad, 'example/fork', 'master', SHA)

    def test_release_notes_preserve_human_text_and_update_each_section(self):
        runtime = (ROOT / 'docs/plugin-release.md').read_text()
        download = (ROOT / 'docs/plugin-upstream-download.md').read_text()
        body = 'Manual release notes with `code`, $(), and "quotes".\n'
        result = notes.compose(body, [runtime, download])
        self.assertIn(body.strip(), result)
        self.assertEqual(notes.compose(result, [runtime, download]), result)
        updated = runtime.replace('Fork plugin support', 'Updated fork plugin support')
        self.assertIn('Updated fork plugin support', notes.compose(result, [updated, download]))
        with self.assertRaises(ValueError):
            notes.compose(result + '<!-- plugin-upstream:begin -->', [download])


if __name__ == '__main__':
    unittest.main()
