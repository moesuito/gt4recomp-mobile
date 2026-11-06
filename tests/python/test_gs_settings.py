"""TASK-039: the GS settings table, gt4-gs.ini generation and allow-list (docs/GS-SETTINGS.md).

Pure checks: they read docs/launcher/gs-settings.toml and need no game build, assets or GPU.
"""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import external_runtime_probe as probe  # noqa: E402
import gs_settings  # noqa: E402
import play_performance as performance  # noqa: E402

# The TASK-036 default INI (Renderer Vulkan, 3x, blending High, deinterlace Automatic, 16:9, vsync), byte for byte
# as the pre-TASK-039 writer produced it. Written with the platform newline by the file writer.
# The default gt4-gs.ini of origin/main (TASK-036 settings plus HWDownloadMode = 0 from TASK-037), byte for byte as
# scripts/external_runtime_probe.py gs_config_text() produced it there. The table writer must reproduce it.
LEGACY_BASE = ('; gt4-gs.ini written by scripts/external_runtime_probe.py (TASK-036 minimal settings)\n'
               '[EmuCore/GS]\n'
               'Renderer = 14\n'
               'upscale_multiplier = 3\n'
               'deinterlace_mode = 0\n'
               'AspectRatio = 16:9\n'
               'VsyncEnable = true\n'
               'DisableMailboxPresentation = true\n'
               'accurate_blending_unit = 3\n'
               'HWDownloadMode = 0\n'
               # Owner, 2026-10-21: PCSX2's own OSD rows are forced off and always written (GT4Recomp has its own
               # performance overlay).
               'OsdShowFPS = false\n'
               'OsdShowGPU = false\n'
               'OsdShowResolution = false\n'
               # TASK-088 (gt4-gs 1.2.0): Load custom textures is on by default and always written.
               'LoadTextureReplacements = true\n'
               # TASK-118 (owner, 2026-10-26): textures load in the background, always on and always written.
               'LoadTextureReplacementsAsync = true\n')
# TASK-064: ProhibitDynamicCode is always written; its default is false (overlays work out of the box).
PROHIBIT_LINE = 'ProhibitDynamicCode = false\n'
LEGACY_DEFAULT = LEGACY_BASE + '\n[GT4GS]\n' + PROHIBIT_LINE
LEGACY_CACHE = LEGACY_BASE + '\n[Folders]\nCache = C:/cache\n\n[GT4GS]\n' + PROHIBIT_LINE
# A real owner run: cache folder and the [GT4GS] DataRoot (package data_root = true).
LEGACY_RUN = LEGACY_BASE + '\n[Folders]\nCache = C:/cache\n\n[GT4GS]\nDataRoot = C:/data\n' + PROHIBIT_LINE
LEGACY_HEADLESS = (LEGACY_BASE + '\n[GT4GS]\nSurfaceless = true\nDumpDir = C:/run/gs-frames\n'
                   'DumpEvery = 100\n' + PROHIBIT_LINE)
REQUIRED = ('id', 'label', 'help', 'page', 'order', 'control', 'section', 'key', 'type', 'default', 'written', 'status')


def rows():
    return gs_settings.load_table()


def by_id():
    return gs_settings.rows_by_id(rows())


class TableSchemaTests(unittest.TestCase):
    def test_table_loads_and_has_one_row_per_setting(self):
        table = rows()
        # 71 rows, plus textures_folder (TASK-088, launcher row).
        self.assertEqual(len(table), 74)
        self.assertEqual(len({row['id'] for row in table}), 74)

    def test_every_row_has_the_documented_columns(self):
        for row in rows():
            for field in REQUIRED:
                self.assertIn(field, row, row['id'])
            self.assertIn(row['page'], gs_settings.PAGES)
            self.assertIn(row['type'], gs_settings.TYPES)
            self.assertIn(row['control'], gs_settings.CONTROLS)
            self.assertIn(row['written'], gs_settings.WRITTEN)
            self.assertRegex(row['status'], gs_settings.STATUS_PATTERN.pattern)

    def test_settable_rows_name_their_ini_key(self):
        for row in rows():
            if row['status'] == 'ok' and row['written'] != 'never':
                self.assertTrue(row['section'] and row['key'], row['id'])

    def test_ini_keys_are_unique(self):
        pairs = [(row['section'], row['key']) for row in rows() if row['section'] and row['key']]
        self.assertEqual(len(pairs), len(set(pairs)))

    def test_readback_row_is_a_written_setting_again(self):
        # TASK-186: Readbacks is a player row again with the five values of the row before TASK-091, Accurate by default.
        row = by_id()['hw_download_mode']
        self.assertEqual((row['key'], row['written'], row['status'], row['control']),
                         ('HWDownloadMode', 'always', 'ok', 'dropdown'))
        self.assertEqual(row['default'], 'accurate')
        self.assertEqual({choice['name']: choice['status'] for choice in row['choices']},
                         {'accurate': 'ok', 'enabled-force-full': 'ok', 'no-readbacks': 'ok',
                          'unsynchronized': 'ok', 'disabled': 'ok'})
        self.assertNotIn('performance_mode', by_id())
        self.assertNotIn('gt4_readback', by_id())

    def test_the_effect_toggles_are_launcher_only_rows(self):
        for identifier, env in (('effect_flares', 'GT4_CORE2_PERF_FLARES'),
                                ('effect_road_sheen', 'GT4_CORE2_PERF_ROAD_SHEEN')):
            row = by_id()[identifier]
            self.assertEqual((row['type'], row['written'], row['default'], row['env']),
                             ('bool', 'never', 'true', env), identifier)
            self.assertFalse(row['section'] or row['key'], identifier)
            self.assertEqual(gs_settings.parse_value(row, 'false'), 'false', identifier)

    def test_validator_refuses_bad_tables(self):
        with gs_settings.TABLE_PATH.open('rb') as handle:
            good = tomllib.load(handle)
        self.assertEqual(gs_settings.table_errors(good), [])

        def row(data, identifier):
            return next(item for item in data['setting'] if item['id'] == identifier)

        def duplicate(data):
            data['setting'].append(copy.deepcopy(data['setting'][0]))

        cases = {
            'wrong schema': lambda data: data.update(schema_version=2),
            'no defaults profile': lambda data: data.update(profile=[]),
            'duplicate id': duplicate,
            'bad page': lambda data: row(data, 'graphics_api').update(page='Graphics'),
            'mods row on a page': lambda data: row(data, 'texture_load').update(page='advanced'),
            'bad type': lambda data: row(data, 'graphics_api').update(type='double'),
            'bad status': lambda data: row(data, 'graphics_api').update(status='maybe'),
            'bad order': lambda data: row(data, 'fxaa').update(order=0),
            'order used twice on a page': lambda data: row(data, 'fxaa').update(order=row(data, 'cas_mode')['order']),
            'bad control': lambda data: row(data, 'fxaa').update(control='knob'),
            'control does not fit the type': lambda data: row(data, 'fxaa').update(control='slider'),
            'launcher row with a control': lambda data: row(data, 'cache_folder').update(control='dropdown'),
            'settable row without key': lambda data: row(data, 'graphics_api').update(key=''),
            'duplicate INI key': lambda data: row(data, 'upscale').update(
                section=row(data, 'graphics_api')['section'], key=row(data, 'graphics_api')['key']),
            'int range inverted': lambda data: row(data, 'cas_sharpness').update(min=101, max=100),
            'choice offered flag not a bool': lambda data: row(data, 'upscale')['choices'][0].update(offered='no'),
            'preset height not a number': lambda data: row(data, 'upscale')['choices'][0].update(height='tall'),
            'accept_ini with duplicate ini values': lambda data: row(data, 'upscale')['choices'][1].update(ini='1'),
            'default outside range': lambda data: row(data, 'upscale').update(default='13'),
            'always row not ok': lambda data: row(data, 'adapter').update(written='always'),
            'path row set by user': lambda data: row(data, 'cache_folder').update(written='when-set'),
            'bool default': lambda data: row(data, 'fxaa').update(default='yes'),
            'launcher-only with an INI key': lambda data: row(data, 'gs_engine').update(
                section='EmuCore/GS', key='Renderer'),
        }
        for name, break_table in cases.items():
            data = copy.deepcopy(good)
            break_table(data)
            self.assertTrue(gs_settings.table_errors(data), name)


class DefaultTests(unittest.TestCase):
    def test_default_ini_is_main_default(self):
        self.assertEqual(gs_settings.render_ini(rows()), LEGACY_DEFAULT)

    def test_probe_default_is_unchanged(self):
        self.assertEqual(probe.gs_config_text({}), LEGACY_DEFAULT)
        self.assertEqual(probe.gs_config_text({'GT4_GS_SCALE': '3', 'GT4_PRESENT_MODE': 'fifo',
                                               'GT4_GS_ASPECT': '16:9'}), LEGACY_DEFAULT)

    def test_cache_and_headless_sections_are_unchanged(self):
        self.assertEqual(probe.gs_config_text({}, None, 'C:/cache'), LEGACY_CACHE)
        self.assertEqual(probe.gs_config_text({}, Path('C:/run/gs-frames'), None),
                         LEGACY_BASE + '\n[GT4GS]\nSurfaceless = true\nDumpDir = C:\\run\\gs-frames\n'
                         'DumpEvery = 100\n' + PROHIBIT_LINE)
        self.assertEqual(gs_settings.render_ini(rows(), {}, {'cache_folder': 'C:/cache',
                                                             'headless_surfaceless': 'true',
                                                             'headless_dump_dir': 'C:/run/gs-frames',
                                                             'headless_dump_every': '100'}),
                         LEGACY_BASE + '\n[Folders]\nCache = C:/cache\n\n[GT4GS]\nSurfaceless = true\n'
                         'DumpDir = C:/run/gs-frames\nDumpEvery = 100\n' + PROHIBIT_LINE)

    def test_run_text_with_data_root_and_eager_switch(self):
        self.assertEqual(probe.gs_config_text({}, None, 'C:/cache', 'C:/data'), LEGACY_RUN)
        self.assertEqual(probe.gs_config_text({'GT4_GS_EAGER_ACQUIRE': '1'}, None, None, 'C:/data'),
                         LEGACY_BASE + '\n[GT4GS]\nDataRoot = C:/data\nDeferSwapChainAcquire = false\n'
                         + PROHIBIT_LINE)
        self.assertNotIn('DeferSwapChainAcquire', probe.gs_config_text({}, None, None, 'C:/data'))

    def test_default_file_bytes_match_the_main_writer(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'gt4-gs.ini'
            gs_settings.write_ini_atomic(target, probe.gs_config_text({}, None, 'C:/cache', 'C:/data'))
            # The main writer used Path.write_text, which applies the platform newline (CRLF on Windows).
            self.assertEqual(target.read_bytes(), (LEGACY_RUN.replace('\n', os.linesep)).encode('utf-8'))


class ValueTests(unittest.TestCase):
    def test_bool_accepts_only_true_or_false(self):
        self.assertEqual(gs_settings.parse_value(by_id()['fxaa'], 'true'), 'true')
        self.assertEqual(gs_settings.parse_value(by_id()['fxaa'], True), 'true')
        for value in ('yes', '1', 'True'):
            with self.assertRaises(gs_settings.SettingsError, msg=value):
                gs_settings.parse_value(by_id()['fxaa'], value)

    def test_int_range_is_checked(self):
        every = by_id()['headless_dump_every']
        self.assertEqual(gs_settings.parse_value(every, '1'), '1')
        self.assertEqual(gs_settings.parse_value(every, '08'), '8')
        self.assertEqual(gs_settings.parse_value(every, '100000'), '100000')
        for value in ('0', '100001', '1.5', 'x', ' 3', ''):
            with self.assertRaises(gs_settings.SettingsError, msg=value):
                gs_settings.parse_value(every, value)

    def test_upscale_presets_and_multipliers(self):
        upscale = by_id()['upscale']
        for text, name in (('1080p', '1080p'), ('3', '1080p'), ('1', '480p'), ('2', '720p'), ('4', '1440p'),
                           ('6', '4K'), ('12', '8K'), ('5', '5x'), ('11x', '11x')):
            self.assertEqual(gs_settings.parse_value(upscale, text), name, text)
        self.assertEqual(gs_settings.ini_value(upscale, '4K'), '6')
        for value in ('0', '13', '08', '1.5', '4k', ''):
            with self.assertRaisesRegex(gs_settings.SettingsError, 'upscale must be one of', msg=value):
                gs_settings.parse_value(upscale, value)

    def test_enum_accepts_only_listed_settable_names(self):
        deinterlace = by_id()['deinterlace']
        self.assertEqual(gs_settings.ini_value(deinterlace, gs_settings.parse_value(deinterlace, 'automatic')), '0')
        with self.assertRaises(gs_settings.SettingsError):
            gs_settings.parse_value(deinterlace, 'wide')

    def test_forced_rows_accept_only_their_fixed_value(self):
        # Owner, 2026-10-21: 16:9 until the ultrawide update; PCSX2's own OSD rows always off.
        aspect = by_id()['aspect']
        self.assertEqual(gs_settings.ini_value(aspect, gs_settings.parse_value(aspect, '16:9')), '16:9')
        for value in ('4:3', 'auto', 'wide'):
            with self.assertRaises(gs_settings.SettingsError, msg=value):
                gs_settings.parse_value(aspect, value)
        with self.assertRaisesRegex(gs_settings.SettingsError, 'aspect is fixed at 16:9'):
            gs_settings.settings_from_assignments(['aspect=4:3'], rows())
        self.assertEqual(gs_settings.offered_choices(aspect), ['16:9'])
        for identifier in ('osd_fps', 'osd_gpu', 'osd_resolution'):
            row = by_id()[identifier]
            self.assertEqual((row['page'], row['written'], row['default'], row['forced']),
                             ('hidden', 'always', 'false', True), identifier)
            self.assertEqual(gs_settings.parse_value(row, 'false'), 'false')
            with self.assertRaisesRegex(gs_settings.SettingsError, f'{identifier} is fixed at false'):
                gs_settings.parse_value(row, 'true')
        for identifier in ('osd_messages_pos', 'osd_performance_pos', 'osd_scale'):
            self.assertEqual(by_id()[identifier]['page'], 'hidden', identifier)
        offered = {row['id'] for row in gs_settings.offered(rows())}
        self.assertFalse({'aspect', 'osd_fps', 'osd_gpu', 'osd_resolution', 'osd_messages_pos',
                          'osd_performance_pos', 'osd_scale'} & offered)

    def test_forced_rows_must_be_hidden_always_and_ok(self):
        row = dict(by_id()['aspect'], page='basic')
        self.assertIn('aspect: forced rows must be page hidden, written always and status ok',
                      gs_settings._row_errors(row))

    def test_tbd_choices_and_rows_are_refused(self):
        renderer = by_id()['graphics_api']
        # TASK-088 (1.2.0): these values are enabled now (the binary accepts them).
        self.assertEqual(gs_settings.parse_value(renderer, 'd3d12'), 'd3d12')
        # TASK-103: Direct3D 11 and Vulkan are the other two; anything else (OpenGL, a software renderer) is refused.
        self.assertEqual(gs_settings.parse_value(renderer, 'd3d11'), 'd3d11')
        self.assertEqual(gs_settings.parse_value(renderer, 'vulkan'), 'vulkan')
        for refused in ('opengl', 'software', ''):
            with self.assertRaises(gs_settings.SettingsError, msg=refused):
                gs_settings.parse_value(renderer, refused)
        self.assertEqual(gs_settings.parse_value(by_id()['filter'], 'bilinear-ps2'), 'bilinear-ps2')
        self.assertEqual(gs_settings.parse_value(by_id()['hw_download_mode'], 'no-readbacks'), 'no-readbacks')
        self.assertEqual(gs_settings.parse_value(by_id()['blending'], 'minimum'), 'minimum')

    def test_path_values_are_single_line(self):
        self.assertEqual(gs_settings.parse_value(by_id()['cache_folder'], 'C:/cache'), 'C:/cache')
        with self.assertRaises(gs_settings.SettingsError):
            gs_settings.parse_value(by_id()['cache_folder'], 'C:/a\nCache = x')

    def test_unknown_ids_are_refused(self):
        with self.assertRaisesRegex(gs_settings.SettingsError, "unknown GS setting 'nope'"):
            gs_settings.settings_from_assignments(['nope=1'], rows())
        with self.assertRaisesRegex(gs_settings.SettingsError, 'unknown GS setting'):
            gs_settings.render_ini(rows(), {'nope': '1'})

    def test_launcher_rows_are_not_user_settable(self):
        for identifier in ('cache_folder', 'data_root', 'defer_swap_chain_acquire', 'headless_dump_dir'):
            with self.assertRaisesRegex(gs_settings.SettingsError, 'set by the launcher'):
                gs_settings.settings_from_assignments([f'{identifier}=accurate'], rows())
        with self.assertRaisesRegex(gs_settings.SettingsError, 'not a launcher row'):
            gs_settings.render_ini(rows(), {}, {'upscale': '2'})

    def test_assignment_syntax(self):
        with self.assertRaisesRegex(gs_settings.SettingsError, 'ID=VALUE'):
            gs_settings.settings_from_assignments(['upscale'], rows())
        with self.assertRaisesRegex(gs_settings.SettingsError, 'twice'):
            gs_settings.settings_from_assignments(['upscale=2', 'upscale=3'], rows())


class IniTests(unittest.TestCase):
    def test_user_settings_replace_defaults_and_add_when_set_rows(self):
        settings = gs_settings.settings_from_assignments(['upscale=6', 'fxaa=true', 'aspect=16:9'], rows())
        text = gs_settings.render_ini(rows(), settings)
        self.assertEqual(text.splitlines(), [
            gs_settings.INI_HEADER, '[EmuCore/GS]', 'Renderer = 14', 'upscale_multiplier = 6',
            'deinterlace_mode = 0', 'AspectRatio = 16:9', 'VsyncEnable = true',
            'DisableMailboxPresentation = true', 'accurate_blending_unit = 3', 'HWDownloadMode = 0', 'fxaa = true',
            'OsdShowFPS = false',
            'OsdShowGPU = false', 'OsdShowResolution = false', 'LoadTextureReplacements = true',
            'LoadTextureReplacementsAsync = true', '', '[GT4GS]',
            'ProhibitDynamicCode = false'])

    def test_sections_follow_the_fixed_order(self):
        settings = {'game_fixes': 'false', 'upscale': '2'}
        text = gs_settings.render_ini(rows(), settings, {'cache_folder': 'C:/cache'})
        headers = [line for line in text.splitlines() if line.startswith('[')]
        self.assertEqual(headers, ['[EmuCore/GS]', '[EmuCore]', '[Folders]', '[GT4GS]'])

    def test_json_settings_round_trip_and_are_checked(self):
        settings = gs_settings.settings_from_assignments(['upscale=2', 'fxaa=true'], rows())
        text = gs_settings.settings_to_json(settings)
        self.assertEqual(gs_settings.settings_from_json(text, rows()), settings)
        with self.assertRaisesRegex(gs_settings.SettingsError, 'not JSON'):
            gs_settings.settings_from_json('{', rows())
        with self.assertRaisesRegex(gs_settings.SettingsError, 'JSON object'):
            gs_settings.settings_from_json('[1]', rows())
        with self.assertRaisesRegex(gs_settings.SettingsError, 'upscale must be one of'):
            gs_settings.settings_from_json('{"upscale": 13}', rows())

    def test_json_settings_refuse_duplicate_ids(self):
        for text in ('{"upscale": 2, "upscale": 6}',
                     '{"upscale": 2, "upscale": 2}',
                     '{"upscale": 13, "upscale": 2}',
                     r'{"upscale": 2, "\u0075pscale": 6}'):
            with self.subTest(text=text), self.assertRaisesRegex(
                    gs_settings.SettingsError, 'upscale is given twice'):
                gs_settings.settings_from_json(text, rows())

    def test_duplicate_json_settings_do_not_reach_the_ini(self):
        with self.assertRaisesRegex(gs_settings.SettingsError, 'fxaa is given twice'):
            probe.gs_config_text({'GT4_GS_SETTINGS': '{"fxaa": false, "fxaa": true}'})


class AtomicWriteTests(unittest.TestCase):
    def test_write_replaces_the_file_and_leaves_no_temporary(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'session' / 'gt4-gs.ini'
            gs_settings.write_ini_atomic(target, 'first\n')
            gs_settings.write_ini_atomic(target, 'second\n')
            self.assertEqual(target.read_text(encoding='utf-8'), 'second\n')
            self.assertEqual([path.name for path in target.parent.iterdir()], ['gt4-gs.ini'])

    def test_failed_replace_keeps_the_old_file_and_removes_the_temporary(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'gt4-gs.ini'
            gs_settings.write_ini_atomic(target, 'old\n')
            with patch.object(gs_settings.os, 'replace', side_effect=OSError('locked')):
                with self.assertRaises(OSError):
                    gs_settings.write_ini_atomic(target, 'new\n')
            self.assertEqual(target.read_text(encoding='utf-8'), 'old\n')
            self.assertEqual([path.name for path in Path(temporary).iterdir()], ['gt4-gs.ini'])


class AllowListTests(unittest.TestCase):
    def test_allow_list_is_stable_and_committed(self):
        text = gs_settings.allowlist_text(rows())
        self.assertEqual(text, gs_settings.allowlist_text(rows()))
        self.assertEqual((ROOT / 'docs/launcher/gs-settings-allowlist.json').read_text(encoding='utf-8'), text)

    def test_allow_list_pairs_are_sorted_unique_and_named(self):
        entries = json.loads(gs_settings.allowlist_text(rows()))['entries']
        pairs = [(entry['section'], entry['key']) for entry in entries]
        self.assertEqual(pairs, sorted(pairs))
        self.assertEqual(len(pairs), len(set(pairs)))
        self.assertIn(('EmuCore/GS', 'upscale_multiplier'), pairs)
        self.assertIn(('GT4GS', 'DumpDir'), pairs)
        self.assertNotIn(('', ''), pairs)
        self.assertFalse(any(key == 'GT4_GS_READBACK' for _, key in pairs))


class OfferedTests(unittest.TestCase):
    def test_launcher_offers_only_settable_options(self):
        offered = {row['id'] for row in gs_settings.offered(rows())}
        self.assertTrue({'graphics_api', 'vsync', 'fxaa'} <= offered)
        # TASK-118 (owner, 2026-10-26): deinterlace and blending were cut from the launcher (hidden rows, defaults written).
        # TASK-118 (owner, 2026-10-26): deinterlace was cut from the launcher. TASK-186 brings blending back as a player
        # row (High or Minimum), so it is offered now.
        self.assertNotIn('deinterlace', offered)
        self.assertTrue({'blending', 'hw_download_mode', 'effect_flares', 'effect_road_sheen'} <= offered)
        self.assertNotIn('aspect', offered)
        # TASK-080 addendum: the internal resolution follows the launcher's Resolution and Render scale.
        self.assertNotIn('upscale', offered)
        self.assertIn('texture_load', offered)
        # TASK-118 (owner, 2026-10-26): the precache row was cut from the launcher; the file still accepts it.
        self.assertNotIn('texture_precache', offered)
        for identifier in ('adapter',
                           'cache_folder', 'headless_dump_dir', 'manual_hacks'):
            self.assertNotIn(identifier, offered, identifier)
        for row in gs_settings.offered(rows()):
            if row['type'] == 'enum':
                self.assertTrue(gs_settings.offered_choices(row), row['id'])
        self.assertEqual(gs_settings.offered_choices(by_id()['graphics_api']), ['auto', 'vulkan', 'd3d12', 'd3d11'])


class PlayPerformanceTests(unittest.TestCase):
    def run_main(self, arguments):
        with patch.object(performance, 'play') as play, \
                patch.object(sys, 'argv', ['play_performance.py', '--assets-root', 'C:/assets'] + arguments):
            code = performance.main()
        return code, play

    def test_no_options_leave_the_settings_variable_unset(self):
        with patch.dict(os.environ, {'GT4_GS_SETTINGS': '{"upscale": 5}'}):
            code, play = self.run_main(['--mode', 'pcsx2'])
            self.assertEqual(code, 0)
            play.assert_called_once()
            self.assertNotIn('GT4_GS_SETTINGS', os.environ)

    def test_gs_setting_is_checked_and_passed_as_json(self):
        with patch.dict(os.environ, {}, clear=False):
            code, play = self.run_main(['--mode', 'pcsx2', '--gs-setting', 'upscale=2',
                                        '--gs-setting', 'fxaa=true'])
            self.assertEqual(code, 0)
            play.assert_called_once()
            self.assertEqual(json.loads(os.environ['GT4_GS_SETTINGS']), {'fxaa': 'true', 'upscale': '720p'})

    def test_gs_setting_refusals(self):
        for arguments in (['--mode', 'main', '--gs-setting', 'upscale=2'],
                          ['--mode', 'pcsx2', '--gs-setting', 'nope=1'],
                          ['--mode', 'pcsx2', '--gs-setting', 'upscale=13'],
                          ['--mode', 'pcsx2', '--gs-setting', 'gt4_readback=accurate'],
                          ['--mode', 'pcsx2', '--gs-setting', 'hw_download_mode=accurate', '--gs-readback', 'no-readbacks'],
                          ['--mode', 'pcsx2', '--gs-setting', 'upscale=2', '--gs-scale', '3']):
            with patch.dict(os.environ, {}, clear=False):
                code, play = self.run_main(arguments)
            self.assertEqual(code, 1, arguments)
            play.assert_not_called()

    def test_gs_scale_is_the_upscale_row(self):
        with patch.dict(os.environ, {}, clear=False):
            code, play = self.run_main(['--mode', 'pcsx2', '--gs-scale', '1'])
            self.assertEqual(code, 0)
            self.assertEqual(os.environ['GT4_GS_SCALE'], '1')
            settings = probe.gs_settings_from_environment(os.environ, rows())
            self.assertEqual(settings['upscale'], '480p')

    def test_gs_resolution_is_a_preset_of_gs_scale(self):
        for preset, scale in (('480p', '1'), ('720p', '2'), ('1080p', '3'), ('1440p', '4'), ('4K', '6'), ('8K', '12')):
            with patch.dict(os.environ, {}, clear=False):
                code, play = self.run_main(['--mode', 'pcsx2', '--gs-resolution', preset])
                self.assertEqual(code, 0, preset)
                self.assertEqual(os.environ['GT4_GS_SCALE'], scale, preset)
                self.assertIn(f'upscale_multiplier = {scale}\n', probe.gs_config_text(os.environ))
        for arguments in (['--mode', 'main', '--gs-resolution', '720p'],
                          ['--mode', 'pcsx2', '--gs-resolution', '720p', '--gs-scale', '2'],
                          ['--mode', 'pcsx2', '--gs-resolution', '720p', '--gs-setting', 'upscale=4K']):
            with patch.dict(os.environ, {}, clear=False):
                code, play = self.run_main(arguments)
            self.assertEqual(code, 1, arguments)
            play.assert_not_called()
        with self.assertRaises(SystemExit):
            with patch('sys.stderr'):
                self.run_main(['--mode', 'pcsx2', '--gs-resolution', '2160p'])


class ProbeEnvironmentTests(unittest.TestCase):
    def test_settings_json_reaches_the_ini(self):
        text = probe.gs_config_text({'GT4_GS_SETTINGS': json.dumps({'upscale': 2, 'fxaa': True})})
        self.assertIn('upscale_multiplier = 2\n', text)
        self.assertIn('[EmuCore/GS]\n', text)
        self.assertIn('fxaa = true\n', text)

    def test_shortcut_and_json_conflicts_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'both set upscale'):
            probe.gs_config_text({'GT4_GS_SCALE': '2', 'GT4_GS_SETTINGS': '{"upscale": 3}'})
        with self.assertRaisesRegex(ValueError, 'GT4_PRESENT_MODE=mailbox'):
            probe.gs_config_text({'GT4_PRESENT_MODE': 'mailbox', 'GT4_GS_SETTINGS': '{"vsync": false}'})
        with self.assertRaisesRegex(ValueError, 'GT4_GS_ASPECT must be one of'):
            probe.gs_config_text({'GT4_GS_ASPECT': 'wide'})
        with self.assertRaisesRegex(ValueError, 'GT4_GS_SCALE must be an integer from 1 to 12'):
            probe.gs_config_text({'GT4_GS_SCALE': '13'})
        with self.assertRaisesRegex(ValueError, 'GT4_PRESENT_MODE must be one of'):
            probe.gs_config_text({'GT4_PRESENT_MODE': 'adaptive'})

    def test_readback_shortcut_and_json_conflict(self):
        self.assertIn('HWDownloadMode = 2\n', probe.gs_config_text({'GT4_GS_READBACK': 'no-readbacks'}))
        self.assertIn('HWDownloadMode = 0\n',
                      probe.gs_config_text({'GT4_GS_SETTINGS': '{"hw_download_mode": "accurate"}'}))
        with self.assertRaisesRegex(ValueError, 'both set hw_download_mode'):
            probe.gs_config_text({'GT4_GS_READBACK': 'accurate',
                                  'GT4_GS_SETTINGS': '{"hw_download_mode": "no-readbacks"}'})
        with self.assertRaisesRegex(ValueError, 'GT4_GS_READBACK must be one of'):
            probe.gs_config_text({'GT4_GS_READBACK': '3'})

    def test_present_mode_maps_to_vsync_and_mailbox(self):
        self.assertIn('VsyncEnable = false\n', probe.gs_config_text({'GT4_PRESENT_MODE': 'immediate'}))
        self.assertIn('DisableMailboxPresentation = false\n', probe.gs_config_text({'GT4_PRESENT_MODE': 'mailbox'}))


# Value checks for the rows resolved from the PCSX2 v2.8.2 sources (commit fd9d310c; citations in the table notes).
# Each entry: launcher id -> (INI key, {launcher name: INI value}, default launcher name).
ENUM_EXPECTED = {
    # TASK-108: auto is the default; it writes 14 only if never resolved (the launcher resolves it before every write).
    'graphics_api': ('Renderer', {'auto': '14', 'vulkan': '14', 'd3d12': '15', 'd3d11': '3'}, 'auto'),
    'filter': ('filter', {'nearest': '0', 'bilinear-forced': '1', 'bilinear-ps2': '2',
                          'bilinear-forced-sprite-excluded': '3'}, 'bilinear-ps2'),
    'trilinear': ('TriFilter', {'automatic': '-1', 'off': '0', 'ps2': '1', 'forced': '2'}, 'automatic'),
    'anisotropic': ('MaxAnisotropy', {'off': '0', '2x': '2', '4x': '4', '8x': '8', '16x': '16'}, 'off'),
    'blending': ('accurate_blending_unit', {'high': '3', 'minimum': '0'}, 'high'),
    'dithering': ('dithering_ps2', {'off': '0', 'scaled': '1', 'unscaled': '2', 'force-32bit': '3'}, 'unscaled'),
    'texture_preloading': ('texture_preloading', {'none': '0', 'partial': '1', 'full': '2'}, 'full'),
    'deinterlace': ('deinterlace_mode', {'automatic': '0', 'off': '1', 'weave-tff': '2', 'weave-bff': '3',
                                         'bob-tff': '4', 'bob-bff': '5', 'blend-tff': '6', 'blend-bff': '7',
                                         'adaptive-tff': '8', 'adaptive-bff': '9'}, 'automatic'),
    'fmv_aspect': ('FMVAspectRatioSwitch', {'off': 'Off', 'auto': 'Auto 4:3/3:2', '4:3': '4:3', '16:9': '16:9',
                                            '10:7': '10:7'}, 'off'),
    'linear_present': ('linear_present_mode', {'off': '0', 'smooth': '1', 'sharp': '2'}, 'smooth'),
    'cas_mode': ('CASMode', {'off': '0', 'sharpen-only': '1', 'sharpen-and-resize': '2'}, 'off'),
    'tv_shader': ('TVShader', {'none': '0', 'scanline': '1', 'diagonal': '2', 'triangular': '3', 'wave': '4',
                               'lottes-crt': '5', 'rgss-4x': '6', 'agss-nx': '7'}, 'none'),
    'hw_download_mode': ('HWDownloadMode', {'accurate': '0', 'enabled-force-full': '1', 'no-readbacks': '2',
                                            'unsynchronized': '3', 'disabled': '4'}, 'accurate'),
    'osd_messages_pos': ('OsdMessagesPos', {'none': '0', 'top-left': '1', 'top-center': '2', 'top-right': '3',
                                            'center-left': '4', 'center': '5', 'center-right': '6',
                                            'bottom-left': '7', 'bottom-center': '8', 'bottom-right': '9'},
                         'top-left'),
}

# Integer rows: id -> (INI key, min, max, default).
NUMBER_EXPECTED = {
    'cas_sharpness': ('CASSharpness', 0, 100, '50'),
    'osd_scale': ('OsdScale', 25, 500, '100'),
    'shade_boost_brightness': ('ShadeBoost_Brightness', 1, 100, '50'),
    'shade_boost_contrast': ('ShadeBoost_Contrast', 1, 100, '50'),
    'shade_boost_gamma': ('ShadeBoost_Gamma', 1, 100, '50'),
    'shade_boost_saturation': ('ShadeBoost_Saturation', 1, 100, '50'),
}

# Keys corrected in TASK-039 (the loader names, pcsx2/Pcsx2Config.cpp).
KEY_EXPECTED = {
    'fxaa': 'fxaa',
    'manual_hacks': 'UserHacks',
    'mipmap': 'hw_mipmap',
    'antiblur': 'pcrtc_antiblur',
    'offsets': 'pcrtc_offsets',
    'overscan': 'pcrtc_overscan',
    'shade_boost': 'ShadeBoost',
    'adapter': 'Adapter',
    'game_fixes': 'EnableGameFixes',
}


class ResolvedValueTests(unittest.TestCase):
    def test_no_row_is_left_unresolved_except_pending_tasks(self):
        statuses = {row['id']: row['status'] for row in rows()}
        self.assertNotIn('TBD-fork', statuses.values())
        self.assertEqual({identifier for identifier, status in statuses.items() if status.startswith('pending')},
                         {'texture_dump', 'texture_dump_mipmaps', 'texture_dump_fmv', 'user_hacks'})

    def test_enum_keys_names_and_ini_values(self):
        for identifier, (key, expected, default) in ENUM_EXPECTED.items():
            row = by_id()[identifier]
            self.assertEqual(row['key'], key, identifier)
            self.assertEqual({choice['name']: choice['ini'] for choice in row['choices']}, expected, identifier)
            self.assertEqual(row['default'], default, identifier)
            self.assertIn(default, expected, identifier)

    def test_number_rows_keys_and_ranges(self):
        for identifier, (key, minimum, maximum, default) in NUMBER_EXPECTED.items():
            row = by_id()[identifier]
            self.assertEqual((row['key'], row['min'], row['max'], row['default']), (key, minimum, maximum, default),
                             identifier)

    def test_shade_boost_switch_is_a_bool(self):
        row = by_id()['shade_boost']
        self.assertEqual((row['type'], row['key'], row['default']), ('bool', 'ShadeBoost', 'false'))

    def test_corrected_keys_match_the_loader(self):
        for identifier, key in KEY_EXPECTED.items():
            self.assertEqual(by_id()[identifier]['key'], key, identifier)
        self.assertEqual(by_id()['adapter']['default'], '')

    def test_hardware_readback_mapping(self):
        choices = {choice['name']: choice for choice in by_id()['hw_download_mode']['choices']}
        self.assertEqual(choices['accurate']['ini'], '0')
        self.assertEqual(choices['no-readbacks']['ini'], '2')
        self.assertEqual(choices['unsynchronized']['ini'], '3')
        self.assertEqual(choices['no-readbacks']['status'], 'ok')
        self.assertEqual(gs_settings.ini_value(by_id()['hw_download_mode'],
                                               gs_settings.parse_value(by_id()['hw_download_mode'],
                                                                       'no-readbacks')), '2')

    def test_resolved_rows_are_not_settable_or_offered(self):
        offered_ids = {row['id'] for row in gs_settings.offered(rows())}
        for row in rows():
            if row['status'] != 'resolved':
                continue
            self.assertNotIn(row['id'], offered_ids, row['id'])
            value = str(row['min']) if row['type'] == 'int' else row['default']
            with self.assertRaisesRegex(gs_settings.SettingsError, 'not settable yet', msg=row['id']):
                gs_settings.parse_value(row, value)
        # TASK-088 (1.2.0): every gt4-gs value the binary accepts is offered.
        self.assertEqual(gs_settings.offered_choices(by_id()['deinterlace'])[0], 'automatic')
        self.assertEqual(len(gs_settings.offered_choices(by_id()['deinterlace'])), 10)
        self.assertEqual(gs_settings.offered_choices(by_id()['blending']), ['high', 'minimum'])
        self.assertEqual(gs_settings.offered_choices(by_id()['graphics_api']), ['auto', 'vulkan', 'd3d12', 'd3d11'])

    def test_resolved_values_stay_out_of_the_default_ini(self):
        text = gs_settings.render_ini(rows())
        for key in ('filter', 'dithering_ps2', 'UserHacks', 'ShadeBoost', 'TVShader', 'Adapter'):
            self.assertNotIn(key + ' =', text)

    def test_allow_list_has_every_keyed_row(self):
        entries = {(entry['section'], entry['key']) for entry in
                   json.loads(gs_settings.allowlist_text(rows()))['entries']}
        for row in rows():
            if row['section'] and row['key']:
                self.assertIn((row['section'], row['key']), entries, row['id'])
        self.assertIn(('EmuCore/GS', 'dithering_ps2'), entries)
        self.assertNotIn(('EmuCore/GS', 'Dithering'), entries)
        self.assertNotIn(('EmuCore/GS', 'ManualUserHacks'), entries)
        self.assertNotIn(('EmuCore/GS', 'HWMipmap'), entries)
        self.assertNotIn(('EmuCore/GS', 'FXAA'), entries)


class PageLayoutTests(unittest.TestCase):
    def test_graphics_page_is_basic_only(self):
        basic = [row['id'] for row in rows() if row['page'] == 'basic']
        # TASK-186: the old Performance Mode row is gone; its items are rows of their own, the two GSdx-only settings last.
        self.assertEqual(basic, ['gs_engine', 'graphics_api', 'effect_flares', 'effect_road_sheen', 'blending',
                                 'hw_download_mode', 'fxaa', 'cas_mode', 'cas_sharpness', 'texture_load'])
        upscale = by_id()['upscale']
        self.assertEqual((upscale['control'], upscale['type'], upscale['default']), ('dropdown', 'enum', '1080p'))
        # TASK-080 addendum (owner, 2026-10-21): hidden, always written, set by the launcher from the display plan;
        # every multiplier 1 to 12 stays a settable choice, so the gt4-gs allow-list is unchanged.
        self.assertEqual((upscale['page'], upscale['written'], upscale['status']), ('hidden', 'always', 'ok'))
        self.assertEqual(sorted(int(choice['ini']) for choice in upscale['choices'] if choice['status'] == 'ok'),
                         list(range(1, 13)))
        # Owner decision 2026-10-16: ONLY these presets are offered.
        self.assertEqual(gs_settings.offered_choices(upscale), ['480p', '720p', '1080p', '1440p', '4K', '8K'])
        self.assertEqual({choice['label'] for choice in upscale['choices'] if choice.get('offered', True)},
                         {'480p (1x)', '720p (2x)', '1080p (3x)', '1440p (4x)', '4K (6x)', '8K (12x)'})

    def test_advanced_page_holds_the_rest_of_the_user_options(self):
        advanced = [row for row in rows() if row['page'] == 'advanced']
        # TASK-118 (owner, 2026-10-26): the Advanced page was cut down to these five rows; the other GS rows are
        # hidden (the file still accepts them) or basic.
        self.assertEqual(sorted((row['order'], row['id']) for row in advanced),
                         [(2, 'adapter'), (3, 'vsync'), (30, 'cursor_autohide'), (31, 'shader_compile_notice'),
                          (32, 'perf_overlay')])

    def test_orders_are_unique_per_page(self):
        seen = set()
        for row in rows():
            self.assertNotIn((row['page'], row['order']), seen, row['id'])
            seen.add((row['page'], row['order']))

    def test_mods_and_hidden_rows_are_on_neither_page(self):
        offered_ids = {row['id'] for row in gs_settings.offered(rows())}
        for row in rows():
            if row['page'] in ('mods', 'hidden'):
                self.assertNotIn(row['id'], offered_ids, row['id'])
        self.assertEqual(sorted(row['id'] for row in rows() if row['page'] == 'mods'),
                         sorted(['texture_dump', 'texture_dump_mipmaps', 'texture_dump_fmv']))

    def test_renderer_choice_is_gsdx_only(self):
        # TASK-096 (launcher 1.2.3): CoreGS 1.1 is no longer a launcher choice; GSdx is the only renderer.
        choices = [choice['name'] for choice in by_id()['gs_engine']['choices']]
        self.assertEqual(choices, ['pcsx2'])
        self.assertEqual(by_id()['gs_engine']['written'], 'never')
        self.assertIn('gs_engine', {row['id'] for row in gs_settings.offered(rows())})
        self.assertNotIn('gs_engine', gs_settings.render_ini(rows()))

    def test_no_fsr_row(self):
        self.assertFalse(any('fsr' in row['id'] for row in rows()))
        self.assertIn('cas_mode', by_id())


class ResetToDefaultsTests(unittest.TestCase):
    def test_reset_gives_the_exact_default_ini(self):
        settings = gs_settings.reset_to_defaults(rows())
        self.assertEqual(gs_settings.render_ini(rows(), settings), LEGACY_DEFAULT)

    def test_reset_holds_only_settable_user_rows(self):
        settings = gs_settings.reset_to_defaults(rows())
        self.assertEqual(settings['upscale'], '1080p')
        self.assertEqual(settings['graphics_api'], 'auto')
        for identifier in settings:
            row = by_id()[identifier]
            self.assertEqual(row['status'], 'ok', identifier)
            self.assertNotIn(row['written'], ('never', 'launcher'), identifier)
        for identifier in ('gs_engine', 'cache_folder', 'data_root'):
            self.assertNotIn(identifier, settings)

    def test_defaults_profile_is_in_the_table(self):
        with gs_settings.TABLE_PATH.open('rb') as handle:
            profiles = tomllib.load(handle)['profile']
        self.assertEqual([profile['name'] for profile in profiles], ['gt4-defaults'])

    def test_when_set_row_equal_to_its_default_is_not_written(self):
        text = gs_settings.render_ini(rows(), {'fxaa': 'false', 'antiblur': 'true', 'upscale': '3'})
        self.assertEqual(text, LEGACY_DEFAULT)

    def test_twelve_x_needs_no_extended_key(self):
        text = gs_settings.render_ini(rows(), gs_settings.settings_from_assignments(['upscale=12'], rows()))
        self.assertIn('upscale_multiplier = 12\n', text)
        self.assertNotIn('ExtendedUpscalingMultipliers', text)
        self.assertEqual(by_id()['extended_upscale']['page'], 'hidden')
        self.assertEqual(by_id()['extended_upscale']['written'], 'when-set')

    def test_mipmap_uses_the_hardware_key(self):
        self.assertEqual(by_id()['mipmap']['key'], 'hw_mipmap')



# TASK-060/061/062 rows and the launcher helpers for the resolution presets.
class OsdRowTests(unittest.TestCase):
    OVERLAY = {'perf_overlay': ('PerfOverlay', 'false'), 'perf_overlay_fps': ('PerfOverlayFPS', 'true'),
               'perf_overlay_frame_time': ('PerfOverlayFrameTime', 'true'), 'perf_overlay_cpu': ('PerfOverlayCPU', 'true'),
               'perf_overlay_gpu': ('PerfOverlayGPU', 'true'),
               'perf_overlay_resolution': ('PerfOverlayResolution', 'true'),
               'perf_overlay_breakdown': ('PerfOverlayBreakdown', 'true')}

    def test_overlay_rows_are_hidden_toggles_off_by_default(self):
        # TASK-118 (owner, 2026-10-26): the launcher keeps only the overlay's master switch (Advanced); the items are
        # hidden rows that the file still accepts.
        offered = {row['id'] for row in gs_settings.offered(rows())}
        self.assertEqual(by_id()['perf_overlay']['page'], 'advanced')
        self.assertIn('perf_overlay', offered)
        for identifier, (key, default) in self.OVERLAY.items():
            if identifier == 'perf_overlay':
                continue  # the master switch, checked above
            row = by_id()[identifier]
            self.assertEqual((row['page'], row['control'], row['section'], row['key'], row['default'], row['written'],
                              row['status']),
                             ('hidden', 'toggle', 'GT4GS', key, default, 'when-set', 'ok'), identifier)
            self.assertNotIn(identifier, offered)
        # Off by default: the default INI has no overlay line; the master switch alone turns every item on.
        self.assertNotIn('PerfOverlay', gs_settings.render_ini(rows()))
        text = gs_settings.render_ini(rows(), gs_settings.settings_from_assignments(['perf_overlay=true'], rows()))
        self.assertTrue(text.endswith('\n[GT4GS]\nPerfOverlay = true\n' + PROHIBIT_LINE), text)
        text = gs_settings.render_ini(rows(), gs_settings.settings_from_assignments(
            ['perf_overlay=true', 'perf_overlay_gpu=false'], rows()))
        self.assertTrue(text.endswith('[GT4GS]\nPerfOverlay = true\nPerfOverlayGPU = false\n' + PROHIBIT_LINE), text)


class ProhibitDynamicCodeTests(unittest.TestCase):
    """TASK-064: Arbitrary Code Guard is off by default so third-party overlays (RTSS) can hook gt4-gs."""

    def test_row_is_a_hidden_always_written_switch_defaulting_to_false(self):
        # TASK-118 (owner, 2026-10-26): no longer offered in the launcher; still always written, false.
        row = by_id()['prohibit_dynamic_code']
        self.assertEqual((row['page'], row['order'], row['control'], row['type'], row['section'], row['key'],
                          row['default'], row['written'], row['status']),
                         ('hidden', 55, 'toggle', 'bool', 'GT4GS', 'ProhibitDynamicCode', 'false', 'always', 'ok'))
        self.assertEqual(row['label'], 'Block third-party overlays (Arbitrary Code Guard)')
        self.assertNotIn('prohibit_dynamic_code', {offered_row['id'] for offered_row in gs_settings.offered(rows())})

    def test_default_ini_always_writes_false(self):
        self.assertTrue(gs_settings.render_ini(rows()).endswith('\n[GT4GS]\nProhibitDynamicCode = false\n'))
        self.assertIn('ProhibitDynamicCode = false\n', probe.gs_config_text({}))

    def test_true_is_accepted_and_written(self):
        settings = gs_settings.settings_from_assignments(['prohibit_dynamic_code=true'], rows())
        self.assertIn('ProhibitDynamicCode = true\n', gs_settings.render_ini(rows(), settings))
        text = probe.gs_config_text({'GT4_GS_SETTINGS': json.dumps({'prohibit_dynamic_code': True})})
        self.assertIn('[GT4GS]\nProhibitDynamicCode = true\n', text)
        self.assertNotIn('ProhibitDynamicCode = false', text)
        with self.assertRaisesRegex(gs_settings.SettingsError, 'must be true or false'):
            gs_settings.parse_value(by_id()['prohibit_dynamic_code'], 'maybe')

    def test_reset_to_defaults_restores_false(self):
        settings = gs_settings.settings_from_assignments(['prohibit_dynamic_code=true'], rows())
        self.assertEqual(gs_settings.reset_to_defaults(rows())['prohibit_dynamic_code'], 'false')
        self.assertIn('ProhibitDynamicCode = false\n', gs_settings.render_ini(rows(), gs_settings.reset_to_defaults(rows())))
        self.assertIn('ProhibitDynamicCode = true\n', gs_settings.render_ini(rows(), settings))

    def test_shader_notice_and_cursor_rows(self):
        notice = by_id()['shader_compile_notice']
        self.assertEqual((notice['page'], notice['section'], notice['key'], notice['default']),
                         ('advanced', 'GT4GS', 'ShaderCompileNotice', 'true'))
        self.assertNotIn('ShaderCompileNotice', gs_settings.render_ini(rows()))
        cursor = by_id()['cursor_autohide']
        self.assertEqual((cursor['page'], cursor['written'], cursor['env'], cursor['default'], cursor['key']),
                         ('advanced', 'never', 'GT4_CURSOR_AUTOHIDE', 'true', ''))
        self.assertIn('cursor_autohide', {row['id'] for row in gs_settings.offered(rows())})
        with self.assertRaisesRegex(gs_settings.SettingsError, 'set by the launcher'):
            gs_settings.settings_from_assignments(['cursor_autohide=false'], rows())

    def test_monitor_height_picks_the_smallest_preset_that_covers_it(self):
        pick = lambda height: gs_settings.upscale_preset_for_monitor(rows(), height)  # noqa: E731
        self.assertEqual([pick(h) for h in (None, 0, 480, 600, 720, 768, 1080, 1200, 1440, 1600, 2160, 2880, 4320, 8640)],
                         ['1080p', '1080p', '480p', '720p', '720p', '1080p', '1080p', '1440p', '1440p', '4K', '4K', '8K',
                          '8K', '8K'])

    def test_texture_limit_greys_out_large_presets(self):
        allowed = lambda size: gs_settings.upscale_presets_within_texture_limit(rows(), size)  # noqa: E731
        self.assertEqual(allowed(16384), ['480p', '720p', '1080p', '1440p', '4K', '8K'])
        self.assertEqual(allowed(8192), ['480p', '720p', '1080p', '1440p', '4K'])
        self.assertEqual(allowed(4096), ['480p', '720p', '1080p'])
        self.assertEqual(allowed(1024), ['480p'])

    def test_allow_list_carries_the_value_rules(self):
        entries = {(entry['section'], entry['key']): entry for entry in
                   json.loads(gs_settings.allowlist_text(rows()))['entries']}
        self.assertEqual(entries[('EmuCore/GS', 'upscale_multiplier')]['values'],
                         ['1', '2', '3', '4', '6', '12', '5', '7', '8', '9', '10', '11'])
        self.assertEqual(entries[('GT4GS', 'PerfOverlay')], {'section': 'GT4GS', 'key': 'PerfOverlay', 'type': 'bool',
                                                             'values': ['true', 'false']})
        self.assertEqual((entries[('GT4GS', 'SpinUs')]['min'], entries[('GT4GS', 'SpinUs')]['max']), (0, 2000))
        self.assertEqual(entries[('EmuCore/GS', 'Renderer')]['values'], ['14', '15', '3'])
        self.assertEqual(entries[('GT4GS', 'DataRoot')], {'section': 'GT4GS', 'key': 'DataRoot', 'type': 'path'})
        # Every key gt4-gs reads from [GT4GS] is listed (gt4-gs 1.0.0 ignores the others).
        for key in ('Surfaceless', 'SpinUs', 'ProhibitDynamicCode', 'DeferSwapChainAcquire', 'DebugStallWindowMs',
                    'DebugStallAfterMs', 'DataRoot', 'DumpDir', 'DumpEvery', 'ShaderCompileNotice', 'PerfOverlay'):
            self.assertIn(('GT4GS', key), entries, key)

    def test_cursor_switch_is_checked_and_recorded_by_the_probe(self):
        self.assertEqual(probe.CURSOR_AUTOHIDE_VALUES, ('0', '1'))


if __name__ == '__main__':
    unittest.main()
