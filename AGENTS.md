# ReverieOS Read Pico 开发说明

- 目标设备为 Read Pico RDP-G01-W（ESP32-S3）。构建入口是 `release-source-pico-0.4.2/ThingsOS/native/`。保持快照内的 `ThingsOS/` 与 `references/` 同级。
- 上游来源、固定依赖和镜像校验见 [Read Pico 发布清单](release-source-pico-0.4.2/SOURCE-RELEASE.json)。保留原始许可、版权和第三方声明。
- 保持原文件行尾和现有阅读进度兼容。实机刷写须经明确授权，保护分区表、救援、出厂 VCOM 和现有用户数据。
- `upstream-mirror` 通过快进跟踪 `flowflic/ThingsOS:main`。在 `pico` 上只纳入相关的 Read Pico 变更，并在独立工作树中验证。
