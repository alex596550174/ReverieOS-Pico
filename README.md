# ReverieOS

ReverieOS 是我为个人学习和练习维护的 Read Pico（RDP-G01-W）固件分支，源自 [ThingsOS](https://github.com/flowflic/ThingsOS) 的 Read Pico 0.4.2 源码。我用它来学习墨水屏阅读器的界面、EPUB 中文排版和固件构建，并尝试贴合自己阅读习惯的调整。

感谢 ThingsOS 作者和贡献者公开源码，也感谢 MindReset 和各第三方项目的贡献。ReverieOS 是个人 fork，与上游的官方发布各自独立。

## 已发布版本

**`0.4.2-R-1.0.1`（2026-11-24）** · Read Pico RDP-G01-W · ESP32-S3 · 16 MiB Flash

本次更新调整首页、阅读页和控制中心，状态栏与控制中心以数字显示电量；同时统一固件版本配置和打包入口，补齐 macOS Host 测试。分区表和主应用、救援程序的内部标识沿用 R-1。

macOS CTest **53/53** 通过，主应用与独立救援构建成功。完整 USB 镜像和 esptool 独立合成结果逐字节一致。实机在兼容布局下写入主应用槽，回读一致，串口确认版本、SD 挂载和首屏刷新。跨系统整机写入、救援网页 OTA 上传及 Windows 构建待验证。

本次发布提供源码和本地打包步骤。以下摘要对应本次构建，重新构建的文件摘要可能因构建时间和路径而异。

| 镜像 | 大小 | SHA-256 |
| --- | ---: | --- |
| `ReverieOS-0.4.2-R-1.0.1-ota.bin` | 6,676,800 B | `0bd8aba66eaacce2995d64e9e718e9b58277a729e74d57db5b1c22f3b8c435ee` |
| `ReverieOS-0.4.2-R-1.0.1-usb.bin` | 16,610,104 B | `e3cb1fadee7d7236c31a97bf2dc0a31ada04403ba8f0d626654d229ecdb0e69b` |

[查看相对 ThingsOS 上游的逐文件差异](#逐文件差异)。

### 历史版本

**`0.4.2-R-1`** · Read Pico RDP-G01-W · ESP32-S3 · 16 MiB Flash

R-1 发布前的 macOS Host 回归为 **51/51**。该次实机使用现有兼容分区布局做了主应用单槽写入，回读摘要匹配，启动串口确认了 `0.4.2-R-1`。完整 USB 镜像通过离线合成和分区内容验证，跨系统整机刷写尚未实测。

R-1 的已验证主应用为 6,676,032 B，SHA-256 为 `6f9a2d5dbce06c55fbcbdb69d86a5208d70540fe9d3e1682e365649307f1d60f`。自行构建的时间戳和路径可能导致摘要不同，以本机生成文件的校验结果为准。

## 修改内容

- 重做首页、书库导航、阅读菜单、状态栏和未缓存封面角标。R-1 将「继续阅读」与「最近阅读」分开，最近列表最多展示另外三本书，并调整首页卡片与入口的对比度。
- 调整 EPUB 章节标题、上一页章内回退、中文标点折行与悬挂，以及 24 / 36 / 54 px 页边距。保留源字节偏移和相应的分页回归。
- 调整 USB MSC 重枚举、SD 多扇区传输和就绪检测，并补充 macOS Host 测试。直接挂载表现仍需按设备和 SD 卡复验。

### 逐文件差异

当前版本相对 [ThingsOS 上游 Read Pico 0.4.2 源码快照](https://github.com/flowflic/ThingsOS/tree/2723571f2e0e0ffc98e8e7ca6df8af82609d69d1/release-source-pico-0.4.2)共 修改 35 项、新增 14 项、删除 1 项。每次发布以该版本所用的 ThingsOS 上游源码提交为基线。以下路径以 `release-source-pico-0.4.2/` 为根，统计范围为 Read Pico 快照。

<details>
<summary>展开文件清单</summary>

| 状态 | 路径 |
| --- | --- |
| 修改 | `ThingsOS/native/CMakeLists.txt` |
| 修改 | `ThingsOS/native/firmware/components/thingsos_board/tusb_config.h` |
| 修改 | `ThingsOS/native/firmware/components/thingsos_board/usb_storage.c` |
| 修改 | `ThingsOS/native/firmware/components/thingsos_core/CMakeLists.txt` |
| 修改 | `ThingsOS/native/firmware/components/thingsos_net/network.c` |
| 修改 | `ThingsOS/native/firmware/components/thingsos_net/network.h` |
| 修改 | `ThingsOS/native/firmware/components/thingsos_net/web.html` |
| 修改 | `ThingsOS/native/firmware/main/features.inc` |
| 修改 | `ThingsOS/native/firmware/main/lock_pin.inc` |
| 修改 | `ThingsOS/native/firmware/main/main.c` |
| 修改 | `ThingsOS/native/firmware/main/pico_controls_ui.inc` |
| 修改 | `ThingsOS/native/firmware/main/reader_cache.inc` |
| 修改 | `ThingsOS/native/firmware/main/stats_ui.inc` |
| 删除 | `ThingsOS/native/firmware/main/status_power.inc` |
| 修改 | `ThingsOS/native/firmware/main/weidu_online_ui.inc` |
| 修改 | `ThingsOS/native/firmware/main/weidu_ui.inc` |
| 新增 | `ThingsOS/native/include/thingsos/home_ui.h` |
| 修改 | `ThingsOS/native/include/thingsos/pagination.h` |
| 修改 | `ThingsOS/native/include/thingsos/reader_ui.h` |
| 修改 | `ThingsOS/native/include/thingsos/ui.h` |
| 修改 | `ThingsOS/native/package_usb.py` |
| 新增 | `ThingsOS/native/pico_release.py` |
| 新增 | `ThingsOS/native/reverie-release.json` |
| 新增 | `ThingsOS/native/reverie_build.py` |
| 修改 | `ThingsOS/native/src/epub.c` |
| 修改 | `ThingsOS/native/src/graphics.c` |
| 新增 | `ThingsOS/native/src/home_ui.c` |
| 修改 | `ThingsOS/native/src/pagination.c` |
| 修改 | `ThingsOS/native/src/reader_ui.c` |
| 修改 | `ThingsOS/native/src/webp_decoder.c` |
| 新增 | `ThingsOS/native/tests/clreq_test.c` |
| 修改 | `ThingsOS/native/tests/content_test.c` |
| 新增 | `ThingsOS/native/tests/device_ui_macos.h` |
| 修改 | `ThingsOS/native/tests/device_ui_stubs.h` |
| 修改 | `ThingsOS/native/tests/device_ui_test.c` |
| 新增 | `ThingsOS/native/tests/epub_heading_prev_test.c` |
| 新增 | `ThingsOS/native/tests/home_ui_test.c` |
| 新增 | `ThingsOS/native/tests/host_files.h` |
| 新增 | `ThingsOS/native/tests/pico_release_test.py` |
| 修改 | `ThingsOS/native/tests/reader_http_test.c` |
| 新增 | `ThingsOS/native/tests/reader_ui_test.c` |
| 修改 | `ThingsOS/native/tests/stagger_test.c` |
| 修改 | `ThingsOS/native/tests/tilt_test.c` |
| 修改 | `ThingsOS/native/tests/weidu_cancel_test.c` |
| 修改 | `ThingsOS/native/tests/weidu_mp_test.c` |
| 修改 | `ThingsOS/native/tests/weidu_reading_test.c` |
| 新增 | `ThingsOS/native/tools/flash_pico_mac.sh` |
| 修改 | `ThingsOS/native/vendor/sources.json` |
| 修改 | `ThingsOS/native/verify_build.py` |
| 新增 | `fonts` |

</details>

在含有上游基线提交的本地 Git 仓库中，可查看具体改动。

```sh
git diff 2723571f2e0e0ffc98e8e7ca6df8af82609d69d1 HEAD -- release-source-pico-0.4.2/
```

## 从其他系统通过 USB 安装

适用于 Read Pico RDP-G01-W（ESP32-S3、16 MiB Flash），需要电脑和 USB 数据线。从原厂、其他系统或分区布局未知的版本迁入时，使用完整 USB 镜像，从地址 `0x0` 写入。完整安装会覆盖旧系统和内部设置，先备份 SD 卡数据以及同一台设备的 Flash。设备开启 Secure Boot、Flash Encryption，或芯片与容量不匹配时，停止并按原系统的恢复指引处理。

### 1. 准备宿主测试和固件构建

从仓库根目录执行以下 macOS 命令。宿主测试需要 CMake、C 编译器、Python 及 Pillow，并通过 Node.js 测试设备网页脚本。快照的 `fonts/方正屏显雅宋.TTF` 为默认字体路径，请按许可说明在本地使用字体和第三方材料。[许可说明](LICENSES.md)

```sh
cd release-source-pico-0.4.2
python3 ThingsOS/native/tests/generate_fixtures.py
python3 ThingsOS/native/tests/generate_state_fixtures.py
cmake -S ThingsOS/native -B ThingsOS/build/native-host-cmake \
  -DPython3_EXECUTABLE="$(command -v python3)"
cmake --build ThingsOS/build/native-host-cmake --parallel
ctest --test-dir ThingsOS/build/native-host-cmake --output-on-failure
```

安装并激活项目固定的 ESP-IDF 6.1，准备 `esp32s3` 工具链。在快照目录调用 ReverieOS 构建入口。主应用版本取自 `ThingsOS/native/reverie-release.json`，通过 `THINGSOS_BUILD_VERSION` 传给现有 CMake。救援版本取自上游快照清单。[固定的 ESP-IDF 提交](https://github.com/espressif/esp-idf/commit/fff9895c82d744c7237be8847347bdd1b07c6643)

```sh
python ThingsOS/native/reverie_build.py
python ThingsOS/native/verify_build.py
python ThingsOS/native/package_usb.py
version=$(python ThingsOS/native/reverie_build.py --print-version)
shasum -a 256 "ThingsOS/build/native-usb/ReverieOS-$version-usb.bin"
```

验证器检查主应用与救援的项目、版本和分区布局。打包脚本复核输入摘要，再生成完整 USB 镜像、OTA 应用镜像和清单。完整 USB 镜像包含 bootloader、分区表、OTA 启动选择、救援、主应用和系统字体。

### 2. 连接、备份并刷写

1. 在原系统中退出 USB 磁盘模式，关闭可能占用串口的程序，用数据线连接 Read Pico。如果自动连接失败，从原系统的「升级和恢复」进入其 BOOT / 下载模式，再重试。串口示例为 macOS 的 `/dev/cu.usbmodem…`、Linux 的 `/dev/ttyACM0`、Windows 的 `COM3`。参见 [MindReset Read Pico 刷机说明](https://dot.mindreset.tech/docs/read_0/firmware) 和 [连接问题](https://dot.mindreset.tech/docs/read_0/faq/firmware_install)。
2. 将下方 `PORT` 替换为实际串口，核对芯片、Flash 容量和安全状态。先读取完整 16 MiB Flash，确认备份读取成功、长度为 16,777,216 字节，并在本地妥善保存。另行备份 SD 卡的书籍及原系统专用数据。
3. 核对设备与本地镜像后启动写入，保持 USB 连接直至报告校验通过。设备重启后，在设置页核对版本，确认能进入书库并打开书籍。

```sh
PORT=/dev/cu.usbmodem101  # 示例，改为实际串口
version=$(python ThingsOS/native/reverie_build.py --print-version)
python -m esptool --chip esp32s3 --port "$PORT" flash-id
python -m esptool --chip esp32s3 --port "$PORT" get-security-info
backup="read-pico-before-reverie-$(date +%Y%m%d-%H%M%S).bin"
python -m esptool --chip esp32s3 --port "$PORT" read-flash 0x0 0x1000000 "$backup"
wc -c < "$backup"
shasum -a 256 "$backup"

python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 "ThingsOS/build/native-usb/ReverieOS-$version-usb.bin"
```

`ReverieOS-<version>-ota.bin` 为应用镜像，保留救援程序识别的 `ThingsOS_native` 项目标识。已装兼容 ThingsOS 或 ReverieOS 的设备可在其救援升级页面上传该文件。macOS 也可从仓库根目录运行 `release-source-pico-0.4.2/ThingsOS/native/tools/flash_pico_mac.sh --help` 查看备份和单槽恢复子命令。[Espressif esptool 5 命令](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html)

## 源码与上游资料

固件构建入口位于 [Read Pico 源码快照](release-source-pico-0.4.2/)的 `ThingsOS/native/`。`pico` 是本仓库的 Read Pico 分支。`upstream-mirror` 保留完整上游源码，供后续同步和比对。

原项目说明、安装向导和官方固件请查看 [ThingsOS 上游仓库](https://github.com/flowflic/ThingsOS) 与 [上游发布网站](https://thingsos.scholarcn.com)。

## 开源许可

上游版权和许可声明随源码保留。本 fork 可独立授权的新增内容采用 Apache-2.0。第三方组件、移植代码和字体继续适用各自的条款。详见 [ReverieOS 许可说明](LICENSES.md)。
