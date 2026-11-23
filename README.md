# ReverieOS

ReverieOS 是我为个人学习和练习维护的 Read Pico（RDP-G01-W）固件分支，源自 [ThingsOS](https://github.com/flowflic/ThingsOS) 的 Read Pico 0.4.2 源码。我用它来学习墨水屏阅读器的界面、EPUB 中文排版和固件构建，并尝试贴合自己阅读习惯的调整。

感谢 ThingsOS 作者和贡献者公开源码，也感谢 MindReset 和各第三方项目的贡献。ReverieOS 是个人 fork，与上游的官方发布各自独立。

## 学习内容

- 阅读首页、书库导航、菜单和状态栏。
- EPUB 章节和翻页行为，以及中文排版与分页。
- macOS 上的构建、测试和设备联调。

## 源码与上游资料

本分支关注 Read Pico（ESP32-S3）。Read Pico 源码位于 [0.4.2 发布快照](release-source-pico-0.4.2/)，构建入口在该快照的 `ThingsOS/native/`。仓库顶层 `native/` 对应上游的 Metalio E-Ink4-Plus（ESP32-S31）工程。

原项目说明、安装向导和固件发布请查看 [ThingsOS 上游仓库](https://github.com/flowflic/ThingsOS) 与 [上游发布网站](https://thingsos.scholarcn.com)。

## 开源许可

上游版权和许可声明随源码保留。本 fork 可独立授权的新增内容采用 Apache-2.0。第三方组件、移植代码和字体继续适用各自的条款。详见 [ReverieOS 许可说明](LICENSES.md)。
