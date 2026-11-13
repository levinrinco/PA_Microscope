# Windows 构建与运行

Linux 和 Windows 共用根目录的 `src/`、`tests/` 和 CMake 配置。`win/` 只放 Windows
专用脚本、SDK 运行库和构建产物，不是独立项目；代码只维护一份。

当前状态：Windows 原生编译与离线测试已在 **MSVC 19.42 + Qt 6.11.1** 上验证；
实体采集卡/FPGA 联调仍需在目标电脑完成。源码包不包含 Qt 或 Spectrum 二进制。

## 1. 获取源码

推荐使用 Git：

```powershell
git clone https://github.com/levinrinco/PA_Microscope.git
cd PA_Microscope
```

也可以使用源码包 `win/source-dist/PA_Microscope_Source.zip`，它由 Linux/Windows 任一侧生成：

```powershell
cmake -P win\package-source.cmake
```

源码包包含 `src/`、`tests/`、`third_party/`、`linux/`、`win/`、文档和构建脚本，不包含
`.git`、构建缓存、Qt/Spectrum 二进制、采集数据和 INI 设置。ZIP 旁的 `.sha256`
校验整个压缩包，包内 `SOURCE_SHA256SUMS.txt` 校验每个源文件。

## 2. Windows 电脑需要准备

- Visual Studio 2022 或相应 Build Tools，安装“使用 C++ 的桌面开发”、MSVC x64 工具及 Windows SDK（包含 rc.exe）。
- 与编译器匹配的 **Qt 6 MSVC 64 位套件**，含 Qt Serial Port；不要选 MinGW 或 32 位套件。
- CMake 和 CTest，加入 PATH。
- 实际采集时安装 Spectrum Windows 驱动、FPGA/USB 串口驱动及显卡驱动。

下面的 Qt 路径仅为示例，改成目标电脑上实际安装的套件根目录：

```powershell
$QtKit = 'C:\Qt\6.11.1\msvc2022_64'
```

## 3. 先跑离线测试（不需要 Spectrum DLL/.lib 或采集卡）

在项目根目录的 PowerShell 中执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\win\build.ps1 -QtRoot $QtKit -TestsOnly
```

脚本依次配置、构建和运行三个测试；任一步失败就停止：

- PA_CoreTests：轨迹解析、输出数据、配置与配色；跨块 DAT 三种类型逐字节校验、分层 PNG 与完整体 PNG 逐像素比较。
- PA_PipelineTests：模拟 DMA、停止排空、深度图、轨迹 B-scan 的起止边界、跨帧更改及 33124 点区间；显示不刷新时只保留最新状态、仍完整存盘，以及并发读取和跨帧隔离。
- PA_BscanTests：共用 B-scan 绘图数据、符号到幅值转换与长区间，完整光栅 C-scan 快照与深度切换；常规 CTest 使用 offscreen，不依赖显卡上下文。下面的 OpenGL 模式还检查实际 B/C-scan 绘图方向及深度颜色。

构建目录为 `win/build-tests/`，输出位于 `win/build-tests/bin/Release/`。
如需单独验证真实 OpenGL 绘图，在 Qt 环境下运行：

```powershell
$env:PATH = "$QtKit\bin;$env:PATH"
$env:PA_TEST_OPENGL = '1'
.\win\build-tests\bin\Release\PA_BscanTests.exe
Remove-Item Env:PA_TEST_OPENGL
```

该步骤会短暂显示一个测试窗口。普通离线测试通过不等于实体采集卡已验证。

## 4. 构建完整程序

将从 Spectrum Windows SDK/驱动获得的匹配版本文件放到：

```text
win/lib/spcm_win64_msvcpp.lib
win/lib/spcm_win64.dll
```

本源码包不包含这两个文件。头文件统一使用 `third_party/Spectrum/include/`，Linux 和
Windows 共用同一份，构建不再依赖 `windows/include` 之类的副本。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\win\build.ps1 -QtRoot $QtKit -Deploy
```

脚本会编译、跑离线测试，然后在新的 `win/dist/windows-Release-时间戳/` 中收集 exe、
Spectrum DLL、Qt DLL 和插件。从这个目录启动 `PA_Microscope.exe`。运行配置和日志也写在
可执行文件旁，建议放在可写的工作目录。

部署使用 Qt 官方的 [windeployqt](https://doc.qt.io/qt-6/windows-deployment.html)，并关闭
直接打包编译器运行库；目标电脑需安装官方 Visual C++ x64 Redistributable。完整 Spectrum
内核驱动仍须单独安装，仅复制 DLL 不足以连接采集卡。详细部署规则见
[deployment-windows.md](../docs/deployment-windows.md)。

脚本默认 Release；可用 `-Configuration Debug` 调试。若换 Qt 套件或编译器，使用新的构建
目录，勿复用旧 CMakeCache。脚本的 `-Generator` 支持对应的 Visual Studio 生成器名称。

## 5. 手动 CMake / Preset 入口

直接使用 CMake：

```powershell
cmake -S . -B win/build -G "Visual Studio 17 2022" -A x64 `
      "-DCMAKE_PREFIX_PATH=$QtKit" -DPA_BUILD_APP=ON -DBUILD_TESTING=ON
cmake --build win/build --config Release --parallel
$env:PATH = "$QtKit\bin;$env:PATH"
ctest --test-dir win/build -C Release --output-on-failure
```

或者使用仓库中的 `CMakePresets.json`：

```powershell
$env:QT_ROOT = $QtKit
cmake --preset windows-tests        # 或 windows-release
cmake --build --preset windows-tests
ctest --preset windows-tests
```

不构建硬件程序时使用 `-DPA_BUILD_APP=OFF`；SDK 文件在其他目录时可传
`-DSPECTRUM_WINDOWS_LIB_DIR=C:/path/to/sdk`。硬件程序编译后自动把 Spectrum DLL 复制到
exe 旁；Qt 依赖由构建脚本的 `-Deploy` 收集。

## 6. 平台适配

- CMake：Windows 使用 x64 MSVC 导入库，Linux 使用 .so/RPATH；MSVC 启用 /utf-8、/W4、NOMINMAX、WIN32_LEAN_AND_MEAN。
- SpectrumFifo：Windows 使用 [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)/VirtualFree；设备名采用 SDK 头文件记录的 `spc0`。Linux 保留 posix_memalign 和 /dev/spcm0。
- DataPipeline：Linux 保留可选 SCHED_FIFO；Windows 默认普通优先级，可通过 `PA_ACQ_THREAD_PRIORITY=highest` 或 `time-critical` 请求 [SetThreadPriority](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority)，失败仅记录告警。
- 测试：不链接 Spectrum SDK，`PA_BUILD_APP=OFF` 时可完全离线构建。
- Qt 串口：选择目标电脑实际的 COM 端口。CSV、保存目录在新电脑重新选择，不拷贝 Linux INI。

## 7. 到目标电脑后的验收

1. 三个离线测试通过，再运行可选 OpenGL 测试。
2. 装驱动，启动应用确认采集卡信息和 COM 端口。
3. 光栅单帧：A-line、实时 B-scan、C-scan、深度尺和色标正常。
4. 轨迹单帧：选同一个 CSV，设置 B-scan 起点/终点（从 1 开始且包含两端），确认所选信号顺序、深度方向与原始采集一致。
5. 轨迹连续：修改区间从下一帧生效；停止、重新开始、模式切换不残留旧帧。
6. 串口调试助手：能打开/关闭 COM 口，采集串口被占用时拒绝，扫描空闲时可释放并自动恢复。
7. 同参数比较 Linux/Windows 的 DAT 样本值、PNG 方向和深度配色；确认输出格式及停止后存盘完成。
