# PA_Microscope Windows 部署（win/dist）规则

本文说明 `win/dist/` 目录的用途、部署目录的生成方式、内容边界、保留与清理规则，以及到目标电脑上的运行与验收要求。适用于 Windows x64 + MSVC + Qt 6 的构建。

> 本文件位于 `docs/`，随仓库版本化。`win/dist/` 被 `.gitignore` 忽略，部署产物不进入 Git。

## 1. 基本约定

- `win/dist/` 根目录只存放 **Windows 可运行部署目录**，不存放源码、构建缓存、SDK 导入库或采集数据。
- 每个部署目录都是自包含的便携程序目录，不是安装包，不写注册表、不安装服务。
- 一次部署 = 一个新目录；**不覆盖、不复用、不手工往里补 DLL**。
- `win/dist/` 是生成物，可整体删除；删除后重新运行部署命令即可恢复。
- 唯一推荐的生成入口是 `win/build.ps1 -Deploy`，不要手工复制 `win/build/bin/Release` 下的文件来拼部署目录。

## 2. 生成方式

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\win\build.ps1 `
    -QtRoot C:\Qt\6.11.1\msvc2022_64 -Deploy
```

前置条件：

- Windows x64，Visual Studio 2022（MSVC x64 工具链）。
- Qt 6 MSVC x64 套件，且 `$QtRoot\lib\cmake\Qt6\Qt6Config.cmake` 存在。
- `cmake`、`ctest` 在 `PATH` 中。
- `win/lib/spcm_win64_msvcpp.lib` 与 `win/lib/spcm_win64.dll` 均存在（后者被 `.gitignore` 忽略，需自备）。
- `-Deploy` 不能与 `-TestsOnly` 同时使用。

脚本执行顺序：

1. 以 `PA_BUILD_APP=ON`、`BUILD_TESTING=ON` 配置 `win/build/`。
2. 编译 `PA_Microscope.exe`。
3. 运行 3 个离线测试（`PA_CoreTests`、`PA_PipelineTests`、`PA_BscanTests`）；任一失败即停止，不部署。
4. 创建新的 `win/dist/windows-<Configuration>-<时间戳>/`。
5. 复制 `PA_Microscope.exe` 和 `spcm_win64.dll`。
6. 运行 `windeployqt` 收集 Qt 运行库、平台插件和翻译。
7. 输出部署目录路径。

脚本失败时如果已经创建了目录，可能留下不完整部署。**规则：失败产生的半成品目录直接删除后重跑，不要修补使用。**

## 3. 命名与版本规则

部署目录名格式：

```text
win/dist/windows-<Configuration>-<yyyyMMdd-HHmmss-fff>/
```

示例：

```text
win/dist/windows-Release-20261113-102257-794/
```

规则：

- `Configuration` 为 `Release`、`Debug` 或 `RelWithDebInfo`，默认 `Release`。
- 时间戳取本机本地时间，精确到毫秒；一次部署一个目录，天然避免覆盖。
- **不要手工重命名**已部署目录；时间戳就是这次构建的版本标识，改名会丢失可追溯性。
- 同时存在多个目录时，**时间戳最新的才是当前版本**；旧目录可以保留用于回退，也可以删除。
- 发布给目标电脑时，优先使用 `Release`。`Debug` 需要 Debug 版 Qt 运行库，目录更大，且不应与 Release 混用；`RelWithDebInfo` 仅供开发排查。

## 4. 目录内容规则

一个干净的 Release 部署目录参考内容（Qt 版本不同，文件数会略有变化）：

```text
windows-Release-<时间戳>/
  PA_Microscope.exe          # 主程序（Windows GUI 子系统，无控制台）
  spcm_win64.dll             # Spectrum 运行期驱动 DLL，必须与 SDK/驱动版本匹配
  Qt6Core.dll
  Qt6Gui.dll
  Qt6Network.dll
  Qt6OpenGL.dll
  Qt6OpenGLWidgets.dll
  Qt6SerialPort.dll
  Qt6Svg.dll
  Qt6Widgets.dll
  opengl32sw.dll             # Qt 软件 OpenGL 回退
  D3Dcompiler_47.dll         # Qt 图形依赖
  platforms/qwindows.dll     # 必需：Windows 平台插件
  styles/
  imageformats/
  iconengines/
  generic/
  networkinformation/
  tls/
  translations/              # Qt 自带翻译
```

**必须由部署命令生成的内容：**

- `PA_Microscope.exe`。
- `spcm_win64.dll`（从 `win/lib/` 经构建目录复制而来）。
- Qt 运行库、`platforms/qwindows.dll` 等插件、`translations/`、`opengl32sw.dll`、`D3Dcompiler_47.dll`。

**禁止放入部署目录的内容：**

- `spcm_win64_msvcpp.lib`：只在链接期使用，运行时不需要。
- MSVC 运行库（`msvcp140*.dll`、`vcruntime140*.dll`、`concrt140.dll` 等）：脚本使用 `--no-compiler-runtime`，目标电脑统一安装官方 VC++ x64 运行库。
- Spectrum 内核驱动、FPGA/串口驱动、显卡驱动：必须单独安装，复制 DLL 不能代替驱动。
- `logs/`、`PA_Microscope.ini`：运行时生成，不属于干净发布包。
- 采集数据（`.csv/.dat/.npy/.pcd/.png` 等）和用户保存目录。
- 源码、`win/build/`、`win/build-tests/`、`.git`、Linux 的 `libspcm_linux.so`。
- 其他版本 Qt 的 DLL、手工放入的旧 `spcm_win64.dll`，或与当前 Qt 套件不匹配的插件。
- 任何手工改名的 DLL、替换的二进制文件。

**不要手工补 DLL。** 缺文件时重跑 `win/build.ps1 -Deploy`，让 `windeployqt` 按当前 Qt 套件重新收集；手工拼目录很容易造成 DLL 版本混用。

## 5. `spcm_win64.dll` 规则

- 只使用 **x64 MSVC** 版本，不使用 32 位或 MinGW 库。
- 部署目录里的 `spcm_win64.dll` 会优先于 `System32` 被加载，因此它必须与目标电脑上的 Spectrum 驱动/内核版本匹配；版本不一致时，宁可先从部署目录删掉本地 DLL、依赖系统安装，也不要放旧 DLL。
- 只复制 DLL 不足以连接采集卡；目标电脑仍须安装完整 Spectrum 驱动，并能打开 `spc0`。
- 该 DLL 与 `.lib` 都被 `.gitignore` 忽略，不随源码包分发。

## 6. 目标电脑前置条件

- Windows 10/11 x64。
- 官方 **Visual C++ 2015–2022 x64 Redistributable**（部署目录不含 MSVC 运行库）。
- 与构建时匹配的 **Spectrum Windows 驱动**（含内核驱动），采集卡可被系统识别。
- FPGA/USB 串口驱动，设备管理器中能看到对应 `COMx`。
- 显卡驱动；建议支持 OpenGL 2.1 以上。显卡/驱动异常时，可使用同目录的 `opengl32sw.dll` 进行软件 OpenGL 回退。
- 部署目录所在位置可写（程序会在 exe 同目录写 `logs/` 和 `PA_Microscope.ini`）。

## 7. 运行规则

- 从部署目录直接运行 `PA_Microscope.exe`，**不要只把 exe 拷走**；Qt DLL、平台插件和 `spcm_win64.dll` 都在同目录。
- 程序是 Windows GUI 子系统，没有控制台窗口；诊断信息写入 `logs/PA_Microscope_<时间戳>.txt`。
- 首次运行会在 exe 旁生成 `PA_Microscope.ini` 和 `logs/`。这两个是运行时文件：
  - 打包/归档前应删除，保持发布包干净；
  - 不要把 Linux 机器上的 `PA_Microscope.ini` 拷过来，COM 口、保存路径等需要在本机重新选择。
- 采集卡设备名使用 `spc0`。打开失败时先检查 Spectrum 驱动、DLL 版本和采集卡供电。
- 数据保存目录由用户在界面选择，不属于部署目录的一部分。

## 8. 保留、清理与打包规则

- `win/dist/` 可以整体删除；程序运行中不要删除正在使用的部署目录（`logs/` 被占用）。
- 只保留最新版本时，关闭程序后直接删除旧的时间戳目录即可。
- 部署目录内出现 `logs/`、`PA_Microscope.ini` 后，如需重新打包，先关闭程序，再删除这两项。
- 自己打 ZIP 时：
  1. 关闭程序；
  2. 删除 `logs/` 和 `PA_Microscope.ini`；
  3. 压缩整个 `windows-Release-<时间戳>/` 目录（压缩包内保留这一层目录），或压缩目录内容（解压到指定目录）；
  4. 记录 ZIP 的 SHA256；
  5. 解压到新目录试运行一次，确认不依赖原路径。
- ZIP 不是安装包。目标电脑仍须先装 VC++ 运行库和 Spectrum/串口/显卡驱动。
- `win/dist/` 已被 `.gitignore` 忽略，不要提交部署产物；源码迁移包也不包含 `win/dist/`。

## 9. 部署后验收清单

- [ ] 部署命令完整跑完，3 个离线测试全部通过。
- [ ] 目录内有 `PA_Microscope.exe`、`spcm_win64.dll`、`platforms/qwindows.dll`。
- [ ] 干净目录内没有 `logs/`、`PA_Microscope.ini`、`*.lib`、MSVC 运行库。
- [ ] 目标电脑已安装 VC++ x64 运行库、Spectrum 驱动、串口驱动、显卡驱动。
- [ ] 双击启动无缺 DLL 报错，界面正常显示 A-line / B-scan / C-scan、深度尺和色标。
- [ ] 能选择 `COMx`，采集卡状态正常（`spc0`）。
- [ ] 光栅单帧：A-line、实时 B-scan、C-scan、深度编码/PNG 输出正常。
- [ ] 轨迹单帧：加载 CSV，设置 B-scan 起止点，深度图与 MIP 正常。
- [ ] 连续采集：停止后等待存盘结束，文件数量和样本值正确。
- [ ] 串口调试助手：能打开/关闭 `COMx`；采集串口被占用时拒绝打开；扫描空闲时可释放采集卡串口，关闭调试串口后采集串口自动恢复。
- [ ] 与 Linux 同参数样本对拍：`.dat` 值、PNG 方向、深度配色一致。
- [ ] 没有控制台窗口；异常时能在 `logs/` 找到日志。

## 10. 常见问题

| 现象 | 原因/处理 |
|---|---|
| 启动报 `0xc0000135` 或缺少 `Qt6Core.dll` | 没有从部署目录运行，或部署目录不完整；不要单独移动 exe，重跑 `-Deploy`。 |
| 缺少 `VCRUNTIME140.dll` / `MSVCP140.dll` | 目标电脑未安装 VC++ 2015–2022 x64 运行库；安装后重试。 |
| 日志出现 `Failed to open card 0` | Spectrum 驱动/内核未安装、采集卡未供电，或 `spcm_win64.dll` 与驱动不匹配。 |
| 找不到 COM 口 | FPGA/USB 串口驱动或线缆问题；设备管理器确认端口号后重选。 |
| 界面黑屏/OpenGL 报错 | 更新显卡驱动；确认 `opengl32sw.dll` 存在，必要时用软件 OpenGL 回退。 |
| 双击后没有黑窗口输出 | 正常，程序是 GUI 子系统；看 exe 同目录 `logs/`。 |
| `windeployqt` 报错或部署目录不完整 | 检查 Qt 套件路径/位数；删除半成品目录后重跑，不要手工补 DLL。 |
| 部署目录里 DLL 版本混乱 | 不要复用旧目录或手工复制 DLL；每次 `-Deploy` 新建目录。 |

## 11. 目录边界

| 目录 | 用途 | 是否入库 |
|---|---|---|
| `src/`、`tests/`、`third_party/`、`cmake/`、`docs/`、`linux/`、`win/` | 源码、测试、构建脚本与文档 | 是 |
| `win/build/`、`win/build-tests/` | CMake/MSVC 构建缓存与中间产物 | 否（`win/build*/` 已忽略） |
| `win/lib/` | Spectrum 导入库与运行期 DLL | 否（`*.lib`/`*.dll` 已忽略） |
| `win/dist/` | Windows 部署目录 | 否（`win/dist/` 已忽略） |

一句话规则：**`win/dist/` 只放 `win/build.ps1 -Deploy` 生成的时间戳部署目录；每次新建、保持干净、不手工拼装、不提交、不把运行时文件打进发布包。**
