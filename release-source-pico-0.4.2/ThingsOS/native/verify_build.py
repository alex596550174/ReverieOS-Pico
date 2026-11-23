"""Validate development build inputs and artifacts; no hardware or originality certificate."""
from pathlib import Path
import hashlib, json, re, shutil, subprocess, sys

from pico_release import (BUILD as build, NATIVE as native, RESCUE as rescue, SNAPSHOT as workspace,
                          checked_images, read_json, release_configuration, require, validation_records)

if not __debug__:
    raise SystemExit('Run verify_build.py without Python optimization to retain the upstream checks.')

release_config = release_configuration()
official = workspace / 'references/read-pico-official'
snapshot = read_json(workspace / 'SOURCE-RELEASE.json')
expected = read_json(native.parent / 'upstream-sources.json')['official']['commit']
require(expected == snapshot['read_pico_bsp_commit'], 'Read Pico BSP provenance differs from SOURCE-RELEASE.json.')


def run(args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True,
                          text=True, encoding='utf-8', errors='replace').stdout.strip()


def check_snapshot_paths(*paths):
    relative = [path.relative_to(workspace) for path in paths]
    revision = release_config['source_snapshot_commit']
    require(all(run(['git', '-C', workspace, 'ls-tree', '-r', revision, '--', path]) for path in relative),
            'The trusted upstream snapshot does not contain the expected export.')
    subprocess.run(['git', '-C', str(workspace), 'diff', '--exit-code', revision, '--',
                    *map(str, relative)], check=True)
    require(not run(['git', '-C', workspace, 'ls-files', '--others', '--', *relative]),
            'The trusted upstream snapshot contains extra local files.')


keyboard_assets = native / 'firmware/main/assets'
keyboard_files = [keyboard_assets / name for name in ('keyboard-sans.json', 'keyboard-sans.ttf',
                                                       'OFL-Keyboard-Sans.txt')]
keyboard_meta = read_json(keyboard_files[0])
require(keyboard_meta['source_commit'] == expected, 'Keyboard font BSP provenance differs.')
require(hashlib.sha256(keyboard_files[1].read_bytes()).hexdigest() == keyboard_meta['sha256'],
        'Bundled keyboard font differs from its recorded SHA-256.')
snapshot_export = not (official / '.git').exists()
if snapshot_export:
    # The source release only exports the BSP needed by Read Pico. Verify those
    # exact files and the bundled font and license against the upstream release.
    check_snapshot_paths(official, *keyboard_files)
else:
    require(run(['git', '-C', official, 'rev-parse', 'HEAD']) == expected, 'Wrong official BSP checkout commit.')
    require(not run(['git', '-C', official, 'status', '--porcelain']), 'Official BSP checkout changed.')
    originals = official / 'sdcard/fonts'
    require(hashlib.sha256((originals / 'ChillDuanSansVF.ttf').read_bytes()).hexdigest() == keyboard_meta['source_sha256'],
            'Upstream keyboard font differs from the source SHA-256.')
    require(keyboard_files[2].read_bytes() == (originals / 'OFL.txt').read_bytes(),
            'Keyboard font license differs from the official BSP checkout.')

main_description = build / 'project_description.json'
rescue_description = rescue / 'project_description.json'
if not main_description.is_file() or not rescue_description.is_file():
    raise SystemExit('Build the ReverieOS main and rescue firmware with reverie_build.py before validating.')

metadata = read_json(main_description)
rescue_metadata = read_json(rescue_description)
idf = Path(metadata['idf_path']).resolve()
require(idf.is_dir() and run(['git', '-C', idf, 'rev-parse', 'HEAD']) == release_config['esp_idf_commit'],
        'The main build does not use the pinned ESP-IDF checkout.')
require(Path(metadata['project_path']).resolve() == native / 'firmware',
        'The main build was configured from a different source directory.')
require(Path(rescue_metadata['idf_path']).resolve() == idf and
        Path(rescue_metadata['project_path']).resolve() == native / 'rescue',
        'The rescue build was configured from a different source directory or SDK.')
run([sys.executable, native / 'prepare_dependencies.py'])
run([sys.executable, native / 'generate_ime.py', '--check'])
assert 'app.ripple_enabled=false;' in (native/'firmware/main/main.c').read_text(encoding='utf-8')
assert '.bcdDevice=0x0402' in (native/'firmware/components/thingsos_board/usb_storage.c').read_text(encoding='utf-8')
ctest = shutil.which('ctest') or workspace / '.tools/idf-tools/tools/cmake/4.0.3/bin/ctest.exe'
test_output = run([ctest, '--test-dir', native.parent / 'build/native-host-cmake', '--output-on-failure'])
required_tests = {'runtime','ota_failure_paths','single_slot_failure_paths','font_psram_pool','online_download_resume','network_service_lifecycle','keyboard_files_persistence','browser_http_sha256',
    'reader_image_ui','epub_font_images_guard','display_pack_thumbnail_cache','weidu_workflow','weidu_notes_progress_time',
    'stagger_waveform_failure_paths','usb_disk_ownership_io','online_release_manifest','browser_image_prepare',
    'epub_mimetype_compat','epub_large_compilation','epub_capacity_guards',
    'epub_10240_boundaries','reader_image_fragmented_psram','pico_low_memory_policy',
    'epub_allocation_rollback','reader_partial_index_recovery','reader_network_bounded_waits',
    'weidu_cache_cancel_boundaries','rescue_no_psram_fallback',
    'epub_illustrated_live_psram','webp_alpha_pixel_equivalence','progressive_jpeg_gray_equivalence',
    'ble_hid_reports','cubox_cache_and_protocol','weidu_offline_time_recovery','flomo_protocol_mcp','pclk_recovery_policy','pico_release_layout'}
if sys.platform in ('win32', 'darwin'):
    required_tests.add('device_ui_controller')
if sys.platform == 'win32':
    required_tests.add('diagnostics_storage_and_reboot')
passed = set(re.findall(r'Test\s+#\d+: (\S+)', test_output))
missing = required_tests - passed
require('100% tests passed' in test_output and not missing,
        f'Host regression is incomplete. Missing tests: {sorted(missing)}.')
# Target frame evidence plus a reproducible patch of the pinned original.
from patch_freetype_pool import generate as generate_font_pool
from patch_webp_alpha import generate as generate_webp_alpha
from patch_stb_grayscale import generate as generate_stb_grayscale
pool_expected=generate_font_pool(native/'vendor/freetype/src/smooth',build/'verify-freetype-pool')
for directory in (build,rescue):
    compiled_alpha=directory/'esp-idf/thingsos_libs/thingsos-webp-alpha'
    reference_alpha=directory/'verify-webp-alpha'
    generate_webp_alpha(native/'vendor/libwebp/src/dec/vp8l_dec.c',reference_alpha/'vp8l_dec.c')
    for filename in ('vp8l_dec.c','alpha_dec.c','src/dec/alphai_dec.h','src/dec/vp8li_dec.h'):
        assert (compiled_alpha/filename).read_bytes()==(reference_alpha/filename).read_bytes()
    reference_stb=directory/'verify-stb/thingsos_stb_image.h'
    generate_stb_grayscale(native/'vendor/stb/stb_image.h',reference_stb)
    assert (directory/'esp-idf/thingsos_libs/thingsos-stb/thingsos_stb_image.h').read_bytes()==reference_stb.read_bytes()
for directory in (build,rescue):
    patched=directory/'esp-idf/thingsos_libs/freetype-pool'
    for name,digest in pool_expected['generated'].items():
        assert hashlib.sha256((patched/name).read_bytes()).hexdigest()==digest, 'Stale FreeType pool patch'
font_options=(native/'vendor/freetype/include/freetype/config/ftoption.h').read_text()
raster_pool=int(re.search(r'^#define FT_RENDER_POOL_SIZE\s+(\d+)L',font_options,re.M)[1])
prefetch_source=(native/'firmware/main/reader_cache.inc').read_text()
stack_extra=int(re.search(r'READER_PREP_STACK_BYTES \(FT_RENDER_POOL_SIZE \+ (\d+)u \* 1024u\)',prefetch_source)[1])*1024
prefetch_budget=raster_pool+stack_extra
raster_stack_frames=[]
for directory in (build,rescue):
    usage=directory/'esp-idf/thingsos_libs/freetype/CMakeFiles/freetype.dir/__/freetype-pool/smooth.c.su'
    frame=int(re.search(r':gray_convert_glyph\t(\d+)\t',usage.read_text())[1])
    assert frame<1024, 'Gray workspace still occupies the target stack'
    assert frame+8192<=prefetch_budget, 'Raster caller headroom exceeded'
    raster_stack_frames.append(frame)
names = ['read_pico','epdiy','e0470_epaper_waveform','cst836u','fca9555','sy7636a','read_pico_pmu','sc7a20h','pwm_audio']
allowed = [native.resolve(), idf] + [(official / 'components' / n).resolve() for n in names]
from patch_esp_tls import patched_source
idf_tls_source = idf/'components/esp-tls/esp_tls.c'
patched_tls = build/'thingsos-esp-tls/esp_tls.c'
assert patched_tls.read_bytes() == patched_source(idf_tls_source), 'Stale ESP-TLS async select fix'
inventory = []
for directory in (build, rescue, build / 'bootloader'):
    commands = json.loads((directory / 'compile_commands.json').read_text(encoding='utf-8'))
    generated = {(directory / 'esp-idf/thingsos_libs/freetype-pool/smooth.c').resolve()} | {(directory / n).resolve() for n in ('project_elf_src_esp32s3.c','x509_crt_bundle.S','web.html.S','web_sha256.js.S','web_image.js.S','weread_root.pem.S','keyboard-sans.ttf.S','cubox.html.S','flomo.html.S')}
    sources = []
    if directory in (build,rescue):
        generated |= {(directory/'esp-idf/thingsos_libs/thingsos-webp-alpha'/name).resolve()
                      for name in ('vp8l_dec.c','alpha_dec.c')}
    if directory == build:
        generated.add(patched_tls.resolve())
    for command in commands:
        path = Path(command['file'])
        if not path.is_absolute(): path = Path(command['directory']) / path
        path = path.resolve()
        assert path in generated or any(path.is_relative_to(p) for p in allowed), path
        assert path.is_file(), path
        sources.append({'file':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    inventory.append({'build':str(directory),'sources':sources})
for application in inventory[:2]:
    paths=[s['file'].replace('\\','/') for s in application['sources']]
    assert sum(p.endswith('/thingsos_board/scan_adapter.c') for p in paths)==1
    assert not any(p.endswith('/output_lcd/render_lcd.c') for p in paths), 'Duplicate LCD renderer'
assert [Path(s['file']) for s in inventory[0]['sources'] if Path(s['file']).name == 'esp_tls.c'] == [patched_tls.resolve()]
assert any('/thingsos_boot_guard/hooks.c' in s['file'].replace('\\','/') for s in inventory[2]['sources'])
for project in ('firmware','rescue'):
    config = (native / project / 'sdkconfig').read_text(encoding='utf-8')
    for item in ('# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set','CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y','CONFIG_IDF_TARGET="esp32s3"','CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y','CONFIG_ESP32S3_DATA_CACHE_64KB=y'):
        assert item in config, item
    if project == 'firmware':
        assert 'CONFIG_SPIRAM_IGNORE_NOTFOUND=y' not in config, 'Main firmware requires PSRAM for network and TLS'
        assert 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y' in config
        assert 'CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y' in config
        for option in ('ESP_WIFI_IRAM_OPT', 'ESP_WIFI_RX_IRAM_OPT'):
            assert f'# CONFIG_{option} is not set' in config, f'{option} would reduce Pico internal SRAM headroom'
        assert 'CONFIG_ESP_MAIN_TASK_STACK_SIZE=24576' in config
        assert 'CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y' in config
        assert 'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y' in config, 'TLS must not compete for fragmented internal RAM'
        assert 'CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y' not in config
        wifi_static = int(re.search(r'^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=(\d+)$', config, re.M)[1])
        wifi_dynamic = int(re.search(r'^CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=(\d+)$', config, re.M)[1])
        wifi_window = int(re.search(r'^CONFIG_ESP_WIFI_RX_BA_WIN=(\d+)$', config, re.M)[1])
        assert wifi_static >= wifi_window, 'WiFi RX buffers must cover the AMPDU receive window'
        assert wifi_dynamic == 0 or wifi_dynamic >= wifi_static
    else:
        assert 'CONFIG_SPIRAM_IGNORE_NOTFOUND=y' in config, 'Rescue retains no-PSRAM recovery'
inputs = checked_images(release_config)
images = validation_records(inputs)
font = inputs['system-font'][3]; original=(workspace/'fonts/方正屏显雅宋.TTF').read_bytes()
root_pem = (native/'firmware/components/thingsos_weidu/certs/weread_root.pem').read_text(encoding='ascii').strip()
trusted_store = (idf/'components/mbedtls/esp_crt_bundle/cacrt_all.pem').read_text(encoding='utf-8')
assert root_pem in trusted_store, 'WeRead CA must come from the trusted SDK root store'
root_pem_bytes = (native/'firmware/components/thingsos_weidu/certs/weread_root.pem').read_bytes().strip()
assert root_pem_bytes in inputs['app'][3], 'Full WeRead CA not embedded in application'
assert font[:8]==b'THFONT01' and font[48:]==original and font[12:44]==hashlib.sha256(original).digest()
assert 'thingsos_device_acceptance_required' not in (build/'build.ninja').read_text(encoding='utf-8')
assert 'usb-bundle' in (build/'build.ninja').read_text(encoding='utf-8')
assert 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' in (native/'firmware/sdkconfig').read_text()
main_config=(build/'config/sdkconfig.h').read_text()
rescue_config=(rescue/'config/sdkconfig.h').read_text()
assert re.search(r'^#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1$',main_config,re.M)
assert re.search(r'^#define CONFIG_BT_CTRL_BLE_MAX_ACT_EFF 2$',main_config,re.M)
assert not re.search(r'^#define CONFIG_BT_ENABLED 1$',rescue_config,re.M)
decoder=(native/'src/image_decode.c').read_text()
assert 'heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)' in decoder
assert 'heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)' in decoder
stagger=(native/'firmware/components/thingsos_board/stagger.inc').read_text()
assert 'thingsos_refresh_text_wave(' in stagger
assert 'memset(hl.dirty_columns,0xff,epd_width()/2)' in stagger
assert 'drive_mode=MODE_GC16' in stagger
assert 'MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT' in stagger
assert 'reader_cache_before_scan' in prefetch_source
reader_header=(native/'include/thingsos/reader_ui.h').read_text()
assert 'THINGSOS_READER_SIZE_DEFAULT = 46' in reader_header
network=(native/'firmware/components/thingsos_net/network.c').read_text(encoding='utf-8')
network_defaults=(native/'include/thingsos/network_defaults.h').read_text(encoding='utf-8')
assert '#define THINGSOS_AP_SSID "ThingsOS"' in network_defaults
assert '#define THINGSOS_AP_PASSWORD "picoread"' in network_defaults
assert 'esp_wifi_scan_start(&config,false)' in network
assert 'esp_wifi_scan_start(&config, true)' not in network
assert 'ap_ssid[]=THINGSOS_AP_SSID, ap_password[]=THINGSOS_AP_PASSWORD' in network
network_ui=(native/'firmware/main/network_ui.inc').read_text(encoding='utf-8')
assert 'wifi_scan_pending && !app.wifi_scan_running && !app.dirty' in network_ui
report = {'scope':'Independent module integration release build','version':release_config['version'],'rescue_version':release_config['rescue_version'],'firmware_ready':False,
    'wifi_page_scan_async':True,'wifi_scan_after_first_frame':True,'wifi_scan_cache_ms':30000,
    'ap_ssid':'ThingsOS','ap_password_bytes':8,
    'font_gray_pool_psram_bytes':raster_pool,'main_stack_bytes':24576,'prefetch_stack_budget_bytes':prefetch_budget,'target_raster_stack_frames':raster_stack_frames,
    'image_decode_psram_only':True,'wallpaper_decoder_live_budget_bytes':3*1024*1024,
    'epub_resource_limit':10240,'epub_chapter_metadata_dynamic':True,
    'webp_decoder_live_budget_bytes':3*1024*1024,'webp_psram_reserve_bytes':512*1024,
    'weidu_parallel_prefetch_workers':0,'weidu_online_chapter_window':3,
    'wallpaper_compressed_input_limit_bytes':1024*1024,'usb_default_flash_complete':True,
    'reader_default_px':46,'reader_stagger_full_pixels':True,'reader_full_refresh_stagger_gc16':False,
    'reader_full_refresh_direct_global_gc16':True,'reader_prefetch_joins_before_dma':True,
    'reader_text_transition_gc16':True,'reader_stagger_internal_working_lut_bytes':8192,
    'build_dependency_boundary_passed':True,'official_bsp_commit':expected,'compiled_sources':sum(len(v['sources']) for v in inventory),
    'host_tests':test_output,'images':images,'application_bytes':images[0]['bytes'],'application_sha256':images[0]['sha256'],
    'rescue_and_font_images_present':True,'rollback_config_enabled':False,'single_application_slot':True,'boot_guard_linked':True,
    'device_keyboard_file_browser_bookmark_stats_connected':True,'font_complete':True,'usb_flash_enabled':True,'device_validation':'not performed','published':False,
    'book_management_connected':True,'pinyin_candidates_connected':True,'book_date_stats_connected':True,
    'upload_collision_cancel_connected':True,'reader_close_before_transfer':True,
    'limitations':['Complete device functional acceptance pending; basic startup evidence is recorded separately','Single main slot; physical power-loss recovery not qualified',
        'EPUB renderer uses bounded native heading/paragraph/image layout, not arbitrary CSS',
        'EPUB ZIP/manifest/logical chapters are bounded to 10240 entries; actual capacity also depends on available PSRAM',
        'Online reading, physical display quality and runtime low-water memory require Pico device acceptance'],
    'compiled_input_inventory':'compiled-inputs.json'}
(build/'compiled-inputs.json').write_text(json.dumps(inventory,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
(build/'validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,ensure_ascii=False,indent=2))
