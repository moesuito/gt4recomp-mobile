"""TASK-036: live gt4-gs.exe launcher wiring and pins (docs/GS-LIVE.md).

Pure checks run everywhere. Package and binary pin checks use the assets checkout (GT4_ASSETS_ROOT, default this
checkout) and GT4_NATIVE_BUILD (the slice193 build directory); they are skipped when those files are absent.
"""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import external_runtime_probe as probe  # noqa: E402
import play_performance as performance  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', ROOT))
MANIFEST = 'slice193-pcsx2.json'
EXECUTABLE = 'private/slice193-gs-live-build/build/gt4vulkan.exe'
PACKAGE = 'private/slice193-gt4gs-pkg'
GS_PROCESS = PACKAGE + '/gt4-gs.exe'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def make_package(directory):
    directory.mkdir(parents=True)
    (directory / 'gt4-gs.exe').write_bytes(b'MZ fake gs process')
    (directory / 'resources').mkdir()
    (directory / 'resources' / 'GameIndex.yaml').write_text('SCUS-97328: {}\n', encoding='utf-8')
    return probe.package_tree_digest(directory)


class ConfigTests(unittest.TestCase):
    def test_defaults_are_vulkan_3x_high_blending_automatic_deinterlace_16_9_vsync(self):
        text = probe.gs_config_text({})
        self.assertEqual(text.splitlines()[1:], [
            '[EmuCore/GS]', 'Renderer = 14', 'upscale_multiplier = 3',
            'deinterlace_mode = 0', 'AspectRatio = 16:9', 'VsyncEnable = true', 'DisableMailboxPresentation = true',
            'accurate_blending_unit = 3',  # TASK-186: the blending row sits after the effect toggles now
            'HWDownloadMode = 0', 'OsdShowFPS = false', 'OsdShowGPU = false', 'OsdShowResolution = false',
            'LoadTextureReplacements = true', 'LoadTextureReplacementsAsync = true', '', '[GT4GS]',
            'ProhibitDynamicCode = false'])  # slice194 (TASK-037): readback mode accurate by default; TASK-064: ACG off

    def test_scale_present_mode_and_aspect(self):
        text = probe.gs_config_text({'GT4_GS_SCALE': '1', 'GT4_PRESENT_MODE': 'immediate', 'GT4_GS_ASPECT': '16:9'})
        self.assertIn('upscale_multiplier = 1\n', text)
        self.assertIn('VsyncEnable = false\n', text)
        self.assertIn('AspectRatio = 16:9\n', text)
        # Owner, 2026-10-21: the aspect is fixed at 16:9 until the ultrawide update.
        with self.assertRaisesRegex(ValueError, 'GT4_GS_ASPECT must be one of 16:9$'):
            probe.gs_config_text({'GT4_GS_ASPECT': 'auto'})
        text = probe.gs_config_text({'GT4_PRESENT_MODE': 'mailbox'})
        self.assertIn('VsyncEnable = true\n', text)
        self.assertIn('DisableMailboxPresentation = false\n', text)

    def test_headless_and_cache_sections(self):
        text = probe.gs_config_text({}, Path('C:/run/gs-frames'), Path('C:/cache'))
        self.assertIn('[Folders]\nCache = C:', text)
        self.assertIn('[GT4GS]\nSurfaceless = true\nDumpDir = C:', text)
        self.assertIn('DumpEvery = 100\n', text)

    def test_bad_values_are_refused(self):
        for name, value in (('GT4_GS_SCALE', '0'), ('GT4_GS_SCALE', '13'), ('GT4_GS_SCALE', '1.5'),
                            ('GT4_GS_ASPECT', 'wide'), ('GT4_PRESENT_MODE', 'adaptive')):
            with self.assertRaises(ValueError, msg=f'{name}={value}'):
                probe.gs_config_text({name: value})


class PackageTests(unittest.TestCase):
    def evidence(self, digest, files):
        return {'gs_stream': {'renderer': 'pcsx2', 'gs_process': GS_PROCESS,
                              'gs_package': {'directory': PACKAGE, 'tree_sha256': digest, 'files': files,
                                             'source': 'test'}},
                'runtime_dependencies': [{'path': GS_PROCESS, 'sha256': 'ab' * 32}]}

    def test_owner_run_pins_the_process_and_writes_the_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            digest, files = make_package(workspace / PACKAGE)
            run = workspace / 'private/run'
            run.mkdir(parents=True)
            environment = {'GT4_GS_SCALE': '1', 'GT4_GS_PROCESS': 'C:/other.exe', 'GT4_RENDERER': 'donor'}
            recorded = probe.gs_stream_environment(environment, self.evidence(digest, files), workspace, True,
                                                   run_directory=run, headless=False)
            self.assertEqual(environment['GT4_RENDERER'], 'pcsx2')
            self.assertEqual(Path(environment['GT4_GS_PROCESS']), (workspace / GS_PROCESS).resolve())
            self.assertEqual(environment['GT4_GS_PROCESS_SHA256'], 'ab' * 32)
            config = Path(environment['GT4_GS_CONFIG'])
            self.assertEqual(config, run / 'gt4-gs.ini')
            text = config.read_text(encoding='utf-8')
            self.assertIn('upscale_multiplier = 1\n', text)  # --gs-scale survives the owner-run scrub
            self.assertNotIn('Surfaceless', text)  # windowed: no headless keys
            self.assertIn('[GT4GS]\nProhibitDynamicCode = false\n', text)  # TASK-064: always written
            self.assertEqual(recorded['gs_package']['tree_sha256'], digest)
            self.assertTrue((workspace / probe.GS_SHADER_CACHE).is_dir())

    def test_headless_run_is_surfaceless_with_frame_dumps(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            digest, files = make_package(workspace / PACKAGE)
            run = workspace / 'private/run'
            run.mkdir(parents=True)
            environment = {'GT4_RENDERER': 'pcsx2'}
            probe.gs_stream_environment(environment, self.evidence(digest, files), workspace, False,
                                        run_directory=run, headless=True)
            text = (run / 'gt4-gs.ini').read_text(encoding='utf-8')
            self.assertIn('Surfaceless = true\n', text)
            self.assertIn(f'DumpDir = {run / "gs-frames"}\n', text)

    def test_a_changed_package_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            digest, files = make_package(workspace / PACKAGE)
            (workspace / PACKAGE / 'extra.dll').write_bytes(b'x')
            with self.assertRaisesRegex(ValueError, 'GS package differs'):
                probe.gs_stream_environment({'GT4_RENDERER': 'pcsx2'}, self.evidence(digest, files), workspace, False,
                                            run_directory=workspace / 'private', headless=True)

    def test_tree_digest_covers_names_sizes_and_contents(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'pkg'
            digest, files = make_package(directory)
            self.assertEqual(files, 2)
            (directory / 'gt4-gs.exe').write_bytes(b'MZ fake gs procesS')
            self.assertNotEqual(probe.package_tree_digest(directory)[0], digest)

    def test_scale_is_refused_without_a_gs_package(self):
        evidence = {'gs_stream': {'renderer': 'pcsx2', 'gs_process': 'private/x/gs_stream_record.exe'},
                    'runtime_dependencies': [{'path': 'private/x/gs_stream_record.exe'}]}
        with self.assertRaisesRegex(ValueError, 'gt4-gs package'):
            probe.gs_stream_environment({'GT4_RENDERER': 'pcsx2', 'GT4_GS_SCALE': '2'}, evidence, Path('C:/a'), False)


class LauncherTests(unittest.TestCase):
    def test_mode_selects_the_slice193_pin(self):
        # Since slice194 (TASK-037/038) --mode pcsx2 points at slice194; slice193 stays selectable.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice193'], (MANIFEST, EXECUTABLE))
        self.assertIn(MANIFEST, probe.VU1_AOT_MANIFESTS)
        self.assertIn(MANIFEST, probe.VU0_AOT_MANIFESTS)
        self.assertIn('pcsx2-slice193', performance.MODES)

    def test_gs_scale_sets_the_environment_for_pcsx2_only(self):
        root = Path('C:/assets')
        with patch.object(performance, 'play') as play, patch.dict(os.environ, {}, clear=False):
            os.environ.pop('GT4_GS_SCALE', None)
            with patch.object(sys, 'argv', ['play_performance.py', '--assets-root', str(root), '--mode', 'pcsx2',
                                            '--gs-scale', '1']):
                self.assertEqual(performance.main(), 0)
            self.assertEqual(os.environ['GT4_GS_SCALE'], '1')
            play.assert_called_once()
        with patch.object(performance, 'play') as play:
            for arguments in (['--mode', 'main', '--gs-scale', '2'], ['--mode', 'pcsx2', '--gs-scale', '13']):
                with patch.object(sys, 'argv', ['play_performance.py', '--assets-root', str(root)] + arguments):
                    self.assertEqual(performance.main(), 1)
            play.assert_not_called()


class PinTests(unittest.TestCase):
    def manifest(self):
        return json.loads((ROOT / 'docs/inputs' / MANIFEST).read_text(encoding='utf-8'))

    def test_launcher_pin_describes_the_live_build(self):
        manifest = self.manifest()
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['slice'], 193)
        self.assertEqual(manifest['runtime']['path'], EXECUTABLE)
        stream = manifest['gs_stream']
        self.assertEqual(stream['renderer'], 'pcsx2')
        self.assertEqual(stream['gs_process'], GS_PROCESS)
        self.assertEqual(stream['gs_package']['directory'], PACKAGE)
        self.assertRegex(stream['gs_package']['tree_sha256'], '^[0-9a-f]{64}$')
        paths = [item['path'] for item in manifest['runtime_dependencies']]
        self.assertIn(GS_PROCESS, paths)
        self.assertEqual(len([path for path in paths if path.startswith('private/slice153-pacing-shaders/')]), 9)
        self.assertEqual(manifest['vu_scope_profile'], 'unit_split_run_fast_only')
        self.assertEqual(manifest['source_capsule'], 'private/slice191-gs-stream-runtime')

    def test_package_matches_its_pin_when_present(self):
        manifest = self.manifest()
        package = ASSETS / manifest['gs_stream']['gs_package']['directory']
        if not package.is_dir():
            self.skipTest('slice193 gt4-gs package not present')
        digest, files = probe.package_tree_digest(package)
        self.assertEqual((digest, files), (manifest['gs_stream']['gs_package']['tree_sha256'],
                                           manifest['gs_stream']['gs_package']['files']))

    def test_executable_pins_match_the_build_under_test(self):
        build = os.environ.get('GT4_NATIVE_BUILD')
        if not build or Path(build).resolve() != (ASSETS / 'private/slice193-gs-live-build/build').resolve():
            self.skipTest('GT4_NATIVE_BUILD is not the slice193 build')
        manifest = self.manifest()
        for entry in [manifest['runtime']] + manifest['runtime_dependencies']:
            path = ASSETS / entry['path']
            self.assertEqual((path.stat().st_size, sha256(path)), (entry['size_bytes'], entry['sha256']), entry['path'])


if __name__ == '__main__':
    unittest.main()
