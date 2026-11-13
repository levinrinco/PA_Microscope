# Changelog

本文件按日期记录用户可见的功能、行为和兼容性变化。历史日期快照（`1027`、`1111`）
已归档到仓库外，不再作为目录维护。

## [Unreleased]

## 2026-11-13

### Changed

- 仓库整理为共享源码 + `linux/` / `win/` 平台目录；Windows 适配并入共享 CMake，删除 `windows/include` 重复头文件。
- 增加 `CMakePresets.json`、GitHub Actions CI、MIT `LICENSE`、`.clang-format` / `.editorconfig` 和统一 `docs/`。
- 日期快照 `1027` / `1111` 不再作为目录维护；历史快照归档到仓库外，版本用 Git 标签 `snapshot-2026-11-11` / `v1.0.0` 管理。

## 2026-11-11

### Added

- 串口调试助手：顶栏独立串口终端，支持文本/十六进制收发、时间戳、循环发送、DTR/RTS，
  并与采集卡串口互斥；扫描空闲时可释放采集串口并在调试口关闭后自动恢复。
- `DisplayMailbox`：GUI 每约 30 ms 轮询最新显示状态；慢界面不再累计携带大数组的 Qt 消息。
- 光栅 C-scan 整帧快照刷新；轨迹强度/深度在同一帧内配对发布。
- `PA_BscanTests` 增加 C-scan 快照与深度方向检查；`PA_CoreTests` 增加分块写入与分层 PNG 测试；
  `PA_PipelineTests` 增加显示轮询、并发邮箱和存盘完整性测试。

### Changed

- DAT 按 65536 个采样点分块转换写入，临时缓冲最多 512 KiB。
- 仅保存强度/深度 PNG 时按深度层网格化并累计最大投影，不分配完整三维体。
- 保存计数改为轮询 `FileWriter::savedItemCount()`。

## 2026-10-27

### Added

- 深度编码：C-scan/MIP 按信号峰值深度上色，支持 Turbo、Viridis、Plasma、Magma、Inferno、Jet、
  红→青色标和独立 `depth_*.png` 输出。
- 轨迹 B-scan 起止区间（按采集顺序从 1 编号、包含两端、跨帧修改下一帧生效）。
- 光栅/轨迹共用 A-line、B-scan、C-scan 三视图。
- Windows x64 MSVC 构建脚本、Qt 部署流程和 Windows 构建文档。
- 离线测试覆盖深度编码、B-scan 数据和轨迹区间。

### Changed

- 光栅 B-scan 显示实时切片；轨迹 B-scan 直接显示采集顺序的原始波形，不经过网格插值。
- 停止采集时等待处理与存盘结束，并尝试排空 DMA 尾部。
