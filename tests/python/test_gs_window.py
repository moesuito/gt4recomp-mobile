"""TASK-037/038 (slice194, slice195 click fix): readback modes, gt4-gs folders, restart with the built-in renderer, and the slice194 pins.

Pure checks run everywhere. Package and binary pin checks use the assets checkout (GT4_ASSETS_ROOT, default this
checkout) and GT4_NATIVE_BUILD (the slice194 build directory); they are skipped when those files are absent.
See docs/GS-LIVE.md.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import external_runtime_probe as probe  # noqa: E402
import play_external_runtime as player  # noqa: E402
import play_performance as performance  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', ROOT))
MANIFEST = 'slice195-pcsx2.json'
EXECUTABLE = 'private/slice195-gs-window-build/build/gt4vulkan.exe'
PACKAGE = 'private/slice195-gt4gs-release-pkg'
GS_PROCESS = PACKAGE + '/gt4-gs.exe'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def make_package(directory):
    directory.mkdir(parents=True)
    (directory / 'gt4-gs.exe').write_bytes(b'MZ fake gs process')
    (directory / 'resources').mkdir()
    (directory / 'resources' / 'GameIndex.yaml').write_text('SCUS-97328: {}\n', encoding='utf-8')
    return probe.package_tree_digest(directory)


def evidence(digest, files, data_root=True):
    return {'gs_stream': {'renderer': 'pcsx2', 'gs_process': GS_PROCESS,
                          'gs_package': {'directory': PACKAGE, 'tree_sha256': digest, 'files': files,
                                         'source': 'test', 'data_root': data_root}},
            'runtime_dependencies': [{'path': GS_PROCESS, 'sha256': 'cd' * 32}]}


class ReadbackModeTests(unittest.TestCase):
    def test_modes_map_to_the_pcsx2_hardware_download_mode(self):
        self.assertEqual(probe.GS_READBACK_MODES, {'accurate': 0, 'enabled-force-full': 1, 'no-readbacks': 2,
                                                  'unsynchronized': 3, 'disabled': 4})
        self.assertIn('HWDownloadMode = 0\n', probe.gs_config_text({}))
        self.assertIn('HWDownloadMode = 0\n', probe.gs_config_text({'GT4_GS_READBACK': 'accurate'}))
        self.assertIn('HWDownloadMode = 2\n', probe.gs_config_text({'GT4_GS_READBACK': 'no-readbacks'}))

    def test_bad_readback_and_acquire_values_are_refused(self):
        for name, value in (('GT4_GS_READBACK', 'fast'), ('GT4_GS_READBACK', '3'), ('GT4_GS_EAGER_ACQUIRE', 'yes')):
            with self.assertRaises(ValueError, msg=f'{name}={value}'):
                probe.gs_config_text({name: value})

    def test_eager_acquire_switch_and_data_root(self):
        self.assertNotIn('DeferSwapChainAcquire', probe.gs_config_text({}))
        text = probe.gs_config_text({'GT4_GS_EAGER_ACQUIRE': '1'}, data_root=Path('C:/data'))
        self.assertIn('[GT4GS]\nDataRoot = C:', text)
        self.assertIn('DeferSwapChainAcquire = false\n', text)

    def test_owner_run_records_the_mode_and_writes_data_root_and_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            digest, files = make_package(workspace / PACKAGE)
            run = workspace / 'private/run'
            run.mkdir(parents=True)
            environment = {'GT4_GS_READBACK': 'no-readbacks', 'GT4_RENDERER': 'donor'}
            recorded = probe.gs_stream_environment(environment, evidence(digest, files), workspace, True,
                                                   run_directory=run, headless=False)
            self.assertEqual(recorded['gs_readback_mode'], 'no-readbacks')
            self.assertEqual(recorded['gs_stream_environment']['GT4_GS_READBACK'], 'no-readbacks')
            text = (run / 'gt4-gs.ini').read_text(encoding='utf-8')
            self.assertIn('HWDownloadMode = 2\n', text)
            self.assertIn(f'Cache = {(workspace / probe.GS_SHADER_CACHE).resolve()}\n', text)
            self.assertIn(f'DataRoot = {(workspace / probe.GS_DATA_ROOT).resolve()}\n', text)
            self.assertTrue((workspace / probe.GS_DATA_ROOT).is_dir())

    def test_default_mode_is_accurate_and_slice193_packages_get_no_data_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            digest, files = make_package(workspace / PACKAGE)
            run = workspace / 'private/run'
            run.mkdir(parents=True)
            recorded = probe.gs_stream_environment({}, evidence(digest, files, data_root=False), workspace, True,
                                                   run_directory=run, headless=False)
            self.assertEqual(recorded['gs_readback_mode'], 'accurate')
            self.assertNotIn('DataRoot', (run / 'gt4-gs.ini').read_text(encoding='utf-8'))

    def test_readback_mode_needs_a_gs_package(self):
        bare = {'gs_stream': {'renderer': 'pcsx2', 'gs_process': 'private/x/gs_stream_record.exe'},
                'runtime_dependencies': [{'path': 'private/x/gs_stream_record.exe'}]}
        with self.assertRaisesRegex(ValueError, 'gt4-gs package'):
            probe.gs_stream_environment({'GT4_RENDERER': 'pcsx2', 'GT4_GS_READBACK': 'accurate'}, bare, Path('C:/a'), False)

    def test_play_performance_gs_readback_is_pcsx2_only(self):
        root = Path('C:/assets')
        with patch.object(performance, 'play') as play, patch.dict(os.environ, {}, clear=False):
            os.environ.pop('GT4_GS_READBACK', None)
            with patch.object(sys, 'argv', ['play_performance.py', '--assets-root', str(root), '--mode', 'pcsx2',
                                            '--gs-readback', 'no-readbacks']):
                self.assertEqual(performance.main(), 0)
            self.assertEqual(os.environ['GT4_GS_READBACK'], 'no-readbacks')
            play.assert_called_once()
        with patch.object(performance, 'play') as play:
            with patch.object(sys, 'argv', ['play_performance.py', '--assets-root', str(root), '--mode', 'main',
                                            '--gs-readback', 'accurate']):
                self.assertEqual(performance.main(), 1)
            play.assert_not_called()


class RestartTests(unittest.TestCase):
    def test_marker_matches_the_game(self):
        source = (ROOT / 'native/gs_stream/gs_stream_client.cpp').read_text(encoding='utf-8')
        match = re.search(r'kRestartMarker = "([^"]+)"', source)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), probe.GS_RESTART_MARKER)

    def test_override_selects_the_built_in_renderer_and_drops_gs_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            digest, files = make_package(workspace / PACKAGE)
            environment = {'GT4_GS_SCALE': '6', 'GT4_GS_READBACK': 'no-readbacks'}
            recorded = probe.gs_stream_environment(environment, evidence(digest, files), workspace, True,
                                                   run_directory=workspace, headless=False, renderer_override='donor')
            self.assertEqual(environment['GT4_RENDERER'], 'donor')
            self.assertNotIn('GT4_GS_SCALE', environment)
            self.assertNotIn('GT4_GS_CONFIG', environment)
            self.assertNotIn('gs_package', recorded)
            with self.assertRaisesRegex(ValueError, 'built-in renderer'):
                probe.gs_stream_environment({}, evidence(digest, files), workspace, True, renderer_override='pcsx2')

    def test_player_restarts_once_with_the_built_in_renderer(self):
        calls = []

        def fake_run_probe(*arguments, **keywords):
            calls.append(keywords.get('renderer_override'))
            if len(calls) == 1:
                return {'gs_process_stopped': True, 'gs_restart_requested': 'donor', 'reason': 'GS process exited'}
            return {'manual_session_closed': True}

        with tempfile.TemporaryDirectory() as temporary, patch.object(player, 'run_probe', side_effect=fake_run_probe):
            assets = Path(temporary)
            (assets / 'private').mkdir()
            player.play(assets, ROOT / 'docs/inputs' / MANIFEST)
        self.assertEqual(calls, [None, 'donor'])

    def test_player_does_not_restart_without_the_choice(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(
                player, 'run_probe', return_value={'gs_process_stopped': True, 'reason': 'GS process exited'}):
            assets = Path(temporary)
            (assets / 'private').mkdir()
            with self.assertRaisesRegex(ValueError, 'Game stopped'):
                player.play(assets, ROOT / 'docs/inputs' / MANIFEST)


class PinTests(unittest.TestCase):
    def manifest(self):
        return json.loads((ROOT / 'docs/inputs' / MANIFEST).read_text(encoding='utf-8'))

    def test_mode_pcsx2_slice195_stays_selectable(self):
        # Since slice196 (TASK-047) --mode pcsx2 points at slice196; slice195 stays selectable.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice195'], (MANIFEST, EXECUTABLE))
        self.assertIn('pcsx2-slice195', performance.MODES)
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice194'],
                         ('slice194-pcsx2.json', 'private/slice194-gs-window-build/build/gt4vulkan.exe'))
        self.assertIn('slice194-pcsx2.json', probe.VU1_AOT_MANIFESTS)
        self.assertIn(MANIFEST, probe.VU1_AOT_MANIFESTS)
        self.assertIn(MANIFEST, probe.VU0_AOT_MANIFESTS)
        self.assertIn('pcsx2', performance.MODES)

    def test_launcher_pin_describes_the_slice195_build(self):
        manifest = self.manifest()
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['slice'], 195)
        self.assertEqual(manifest['runtime']['path'], EXECUTABLE)
        stream = manifest['gs_stream']
        self.assertEqual(stream['renderer'], 'pcsx2')
        self.assertEqual(stream['gs_process'], GS_PROCESS)
        self.assertEqual(stream['gs_package']['directory'], PACKAGE)
        self.assertIs(stream['gs_package']['data_root'], True)
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
            self.skipTest('slice195 gt4-gs package not present')
        digest, files = probe.package_tree_digest(package)
        self.assertEqual((digest, files), (manifest['gs_stream']['gs_package']['tree_sha256'],
                                           manifest['gs_stream']['gs_package']['files']))

    def test_executable_pins_match_the_build_under_test(self):
        build = os.environ.get('GT4_NATIVE_BUILD')
        if not build or Path(build).resolve() != (ASSETS / 'private/slice195-gs-window-build/build').resolve():
            self.skipTest('GT4_NATIVE_BUILD is not the slice195 build')
        manifest = self.manifest()
        for entry in [manifest['runtime']] + manifest['runtime_dependencies']:
            path = ASSETS / entry['path']
            self.assertEqual((path.stat().st_size, sha256(path)), (entry['size_bytes'], entry['sha256']), entry['path'])


if __name__ == '__main__':
    unittest.main()
