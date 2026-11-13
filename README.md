# PA_Microscope

光声显微镜（PAM）实时数据采集、成像与存储软件。一个可执行程序同时支持规则光栅扫描和 CSV 自定义轨迹扫描，硬件层兼容 Spectrum M3i/M4i 系列采集卡及串口控制 FPGA。

项目共用一套 Qt 6 / C++17 源码，Linux 与 Windows x64 都从同一个 `src/` 构建；平台脚本、SDK 运行库和构建产物分别放在 `linux/`、`win/`。Windows 构建与离线测试已在 MSVC 19.42 + Qt 6.11.1 验证；实体采集卡/FPGA 联调需在目标电脑完成。详细说明见 [linux/README.md](linux/README.md)、[win/README.md](win/README.md) 和 [docs/development.md](docs/development.md)。

## 功能

- **共用三视图**：光栅和轨迹模式均显示 A-line、B-scan、C-scan。光栅 B-scan 显示实时切片；轨迹 B-scan 显示按采集顺序选取的一段 A-line，C-scan 仍显示双线性插值 MIP。
- **按格式存储管理**：每个模式独立勾选输出（光栅 DAT/PNG；轨迹 NPY/PCD/PNG），数据格式可选 float64/float32/float16（默认 float64）；未勾选任何格式时不创建目录
- **轨迹 PCD 点云**：每像素列峰值点云，支持 Otsu 自动/手动阈值背景分割（仅保留血管信号）
- **统一硬件控制**：两种模式共用 Spectrum FIFO DMA、采集状态机、FPGA `F1/F0` 串口协议和采集卡设置
- **串口调试助手**：顶栏独立串口终端，支持文本/十六进制收发、时间戳和循环发送；与采集卡串口互斥，扫描空闲时可释放后调试
- **模式独立参数**：光栅/轨迹各自完整保存扫描与 DAQ（时钟、触发、通道、DMA）参数，切换模式自动应用，控件变更即自动落盘（无需保存/读取按钮）
- **采集/处理解耦**：独立采集线程和处理线程通过 `moodycamel::ConcurrentQueue` 传递完整 segment
- **连续采集**：轨迹模式使用 MIP 三缓冲和有界原始帧缓冲，避免 UI/存盘直接阻塞采集

## 硬件要求

- Spectrum M3i.41xx 或 M4i.44xx 系列采集卡
- FPGA 触发源（115200 baud、8N1；启动 `0xF1 0x0A`，停止 `0xF0 0x0A`）
- 振镜或扫描台

当前数据管线按单通道 segment 解析，因此每次扫描只允许启用一个采集通道。

## 快速开始

Linux（依赖与细节见 [linux/README.md](linux/README.md)）：

```bash
./linux/build.sh test
./linux/build/PA_Microscope
```

Windows（细节见 [win/README.md](win/README.md)）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\win\build.ps1 `
    -QtRoot C:\Qt\6.11.1\msvc2022_64 -TestsOnly
powershell -NoProfile -ExecutionPolicy Bypass -File .\win\build.ps1 `
    -QtRoot C:\Qt\6.11.1\msvc2022_64 -Deploy
```

- 只跑离线测试不需要 Spectrum SDK：Linux 使用 `PA_BUILD_APP=OFF ./linux/build.sh test`，Windows 加 `-TestsOnly`。
- 完整程序需要 Spectrum 运行库：Linux 放 `linux/lib/libspcm_linux.so`，Windows 放 `win/lib/spcm_win64_msvcpp.lib` 和 `win/lib/spcm_win64.dll`。二进制不入库，获取方式见 [third_party/Spectrum/README.md](third_party/Spectrum/README.md)。
- Linux 输出 `linux/build/PA_Microscope`；Windows 输出 `win/build/bin/Release/`，部署到 `win/dist/windows-Release-<时间戳>/`。
- 三个离线测试为 `PA_CoreTests`、`PA_PipelineTests`、`PA_BscanTests`，都不需要采集卡。
- CMake Presets：`cmake --preset linux-tests` 或 `windows-tests`（Windows 先设置 `$env:QT_ROOT`）；完整说明见 [docs/development.md](docs/development.md)。
- Linux 实时优先级默认不启用；需要时 `sudo setcap cap_sys_nice+ep linux/build/PA_Microscope`，再 `PA_ACQ_RT_PRIORITY=80 ./linux/build/PA_Microscope`。

## 使用

1. 启动程序并确认采集卡状态为“已连接”。
2. 选择 FPGA 串口。
3. 选择扫描模式：
   - **光栅扫描**：设置快轴点数、慢轴点数、深度和偏移。
   - **轨迹扫描**：选择 CSV，设置输出网格宽高；必要时启用 X/Y 交换。
4. 选择单帧或连续采集。
5. 如需保存，选择目录并勾选所需的输出格式（“保存选项”行：DAT/PNG 或 NPY/PCD/PNG）。
6. 点击“开始”。

深度与偏移必须满足 `depth + windowOffset <= segmentSize`。segment、触发、时钟、通道和 DMA 参数位于“采集卡设置”。

### 串口调试助手

顶栏“串口调试助手”打开独立的串口终端，可枚举或手输串口，设置波特率、数据位、校验、停止位、流控和 DTR/RTS，支持文本与十六进制收发、时间戳、自动滚动、循环发送及收发计数。`Ctrl+Enter` 发送。

采集卡正在使用 FPGA 串口时，调试助手拒绝打开同一个串口设备，避免两个读者分流应答。扫描空闲时可点“释放采集卡串口”：程序先发送 `F0` 停止，再断开采集串口，调试助手自动选中该端口；关闭调试串口或窗口后，程序自动重新打开采集卡串口。扫描进行中不能释放。

两种模式共用 A-line、B-scan、C-scan 三视图及深度尺、色标。轨迹模式在“轨迹 B-scan 点序号”中设置起点和终点：按 CSV 有效数据行的**采集顺序从 1 编号，包含两端**，不使用 CSV 的 index 列或 XY 坐标。例如一帧 33124 点，选择 1000–2000 就显示 1001 条 A-line。加载轨迹后区间自动限制到实际点数，设置随轨迹模式保存。

轨迹 B-scan 的横轴是所选 A-line 的采集顺序，纵轴是“偏移/深度”指定的信号窗口；直接截取这些 A-line，先去直流，显示时取幅值并使用与光栅 B-scan 相同的配色，**不经过双线性插值或笛卡尔网格重建**。每帧采齐所选区间后刷新一次，不完整的区间不刷新；连续采集中修改起止点从下一帧生效。停止后保留最后一段图像，修改区间需重新采集，不提供整帧历史波形回放。关闭所有保存选项仍可显示 B-scan，选取区间不改变已有的整帧存盘内容。

停止后会显示“正在结束采集并等待存盘…”，直到旧数据处理与保存结束，才允许重新开始、切换扫描模式或修改采集卡设置。

连续采集停止时，处理线程会等待采集线程结束提交，并继续处理内存队列中的全部完整帧；写盘较慢或帧缓冲耗尽不会使它提前退出。停止时还会尝试读取驱动报告的 DMA 尾部（包括不足通知量及跨环形边界的数据）。仅保存已启用的输出格式；不足一帧的数据不补成完整帧（光栅 DAT 仍按已完成的行保存），单帧模式仍只输出一帧。驱动读取或写文件失败会报错，不能视为全部保存成功。尚未传入主机的采集卡数据、断电和强制结束进程不在此保证内；停止后的 DMA 可读性仍需在实际采集卡上验证。

停止排空回归测试使用真实处理/写盘代码和模拟 DMA，覆盖 40 帧慢写盘积压、缓冲耗尽、生产者延迟提交、DMA 尾部/回绕，以及不完整帧；逐文件核对保存数量、样本值和顺序。

## 数据文件

每次扫描创建独立时间戳目录，前缀标明是单帧还是连续采集（`Single_` / `Continuous_`，如 `Continuous_2026-09-10_16-22-15`）；同一秒重复启动会自动添加编号，避免覆盖。若本次扫描**没有勾选任何输出格式**，则不创建目录、不写任何文件。

光栅模式，单帧（`Single_`）：

```text
Single_YYYY-MM-DD_HH-mm-ss/
  0.dat
  1.dat
  ...
  cscan_00000.png
  depth_00000.png
```

光栅模式，连续（`Continuous_`）：原始行数据按帧分目录，行号在**每帧内部从 0 重新编号**，顶层只放单文件产物：

```text
Continuous_YYYY-MM-DD_HH-mm-ss/
  frame_00000/
    00000.dat
    00001.dat
    ...
  frame_00001/
    00000.dat
    ...
  cscan_00000.png
  cscan_00001.png
  depth_00000.png
  depth_00001.png
```

每个 `.dat` 是一行 B-scan，数据类型可选 little-endian float64 / float32 / float16（“采集卡设置→采集模式”，**默认 float64**），排列为连续的 `nFast × depth`，每条 A-line 已去 DC、未取绝对值。`cscan_XXXXX.png` 是整幅光栅完成后的 C-scan MIP 正面投影，颜色映射跟随前台显示设置。

光栅 PNG 与屏幕方向一致：宽度为 `nSlow`，高度为 `nFast`；慢轴从左到右，快轴从上到下。

轨迹模式（`Single_` / `Continuous_` 前缀同样适用，但轨迹的原始数据本就是每帧一个文件，因此不再套帧子目录）：

```text
YYYY-MM-DD_HH-mm-ss/
  mip_00000.png
  depth_00000.png
  volume_00000.npy
  cloud_00000.pcd
  frame_00000.dat
  mip_00001.png
  depth_00001.png
  volume_00001.npy
  cloud_00001.pcd
  frame_00001.dat
```

NPY 为 C-order，元素类型与 `.dat` 一致（float64/float32/float16），shape 明确为 `(depth, imageHeight, imageWidth)`；数值为去 DC 后的绝对幅值经双线性加权得到的体数据。`cloud_*.pcd` 为每像素列峰值点云（ASCII PCD v0.7，字段 `x y z intensity`），按“PCD 分割”设置（Otsu 自动/手动/关闭）过滤背景噪声，仅保留信号区。

## 深度编码

深度编码把 C-scan 从“信号有多强”改成“信号来自多深”：每个像素取该列 A-line 包络峰值所在的深度位置（相对信号窗口 `windowOffset` 的采样点序号，0 = 窗口顶部），并用配色表示。

- **显示**：勾选“显示设置 → 深度编码”后，C-scan 直接以深度上色。默认“深度色调 + 强度亮度”（色相 = 深度，明度 = 强度），也可切换为纯深度色。色标显示在 C-scan 下方，可设置量程上限以放大某个深度段；低于“背景阈值”的像素画成黑色，避免噪声被上成随机深度。
- **色标量程**：颜色按**采集窗口深度**标定（0 .. `depth-1` 采样点），不随数据自适应。因此同一深度在实时显示、存盘 PNG、色标三处颜色一致，连续采集的每一帧也可直接互相对比；超出量程的深度会被截断到端点色。若希望放大某个深度区间，在“显示设置 → 深度量程”里填上限即可（填 0 = 用整个窗口）。
- **配色**：可选 Turbo（默认）、Viridis、Plasma、Magma、Inferno、Jet、红→青。Viridis / Plasma / Magma / Inferno 是感知均匀色标——相同的深度差在量程各处看起来一样明显，Viridis 还色盲友好、灰度打印下仍可读，做定量对比建议换成 Viridis；Turbo / Jet 对比更强，但会夸大中间段的深度差异。色值取自各家公开的 256 级色表（CC0），以 33 个锚点线性插值复现，误差 ≤3/255。
- **保存**：勾选“PNG 深度”（光栅模式在 `cscan_*.png` 旁，轨迹模式在 `mip_*.png` 旁）才写出 `depth_*.png`；强度 PNG 与深度 PNG 相互独立，可只存其一或都存。勾选深度 PNG 会自动打开深度显示，避免存下一张屏幕上没看过的图。
- 不做数值导出：需要定量时用同时保存的原始 `.dat` / `.npy` 重建。
- 光栅模式的深度逐行随采集增量累积；轨迹模式按双线性插值把每条 A-line 的峰值深度以强度加权分摊到相邻像素，与 MIP 使用同一套权重。

## 连续扫描的显示与内存

- 界面每约 30 ms 读取一次最新状态，B-scan 只保留一个待显示区间；慢界面不再累计携带大数组的 Qt 消息。光栅 C-scan 保留当前帧所有已完成列，跨帧清空；轨迹强度和深度按同一完成帧一起更新。跳过的是中间显示，不是存盘数据。
- DAT 按 65536 个采样点分块转换写入，转换临时缓冲最多 512 KiB，不再复制整帧做 double/float/half 转换。原始顺序、数值精度、文件类型与命名不变。
- 轨迹仅保存强度 PNG / 深度 PNG（可同时保存 DAT）时，按深度切片网格化并累计最大投影，不分配完整三维体。保持“先网格化、再沿深度取最大值”的原算法；并行切片临时平面按约 32 MiB 预算选择线程数，超大网格至少需要两个平面。
- 勾选 NPY 或 PCD 时仍使用完整三维体，仍在本次扫描中生成，没有自动改成离线输出。
- **这些优化不是整个程序的内存硬上限。** 原始采集队列及光栅存盘队列尚未限额；若存盘长期慢于采集，仍可能积压。高水位安全停扫 / 硬件暂停恢复尚未实现。停止后还会保留 Pipeline 和 DMA 工作缓冲，并非立即归还全部内存。

## 轨迹 CSV

```csv
index,x_m,y_m
0,0.000004,-0.000002
1,0.000004,-0.000002
```

- 首行表头可省略。
- 支持逗号或制表符分隔。
- 坐标单位为米。
- 显示范围取 `max(abs(x), abs(y)) × 1.05`。

## 架构

```text
Spectrum DMA -> acquisitionLoop -> lock-free segment queue -> processingLoop
                                                        |-> A-line latest buffer
                   raster mode                          |-> latest B-scan + accumulated C-scan
                   trajectory mode                      |-> latest B-scan + paired MIP/depth
                                                        `-> dedicated save queue

Latest display mailbox -> GUI timer (~30 ms) -> shared OpenGL widgets

FPGA serial <-> ScanController state machine <-> MainWindow / OpenGL widgets
```

扫描索引到图像坐标的映射是唯一的模式差异：光栅模式按 `fast/slow` 计数，轨迹模式查预计算的 `MapInfo`。其余采集、校零、峰值、状态和错误处理均共用。

## 开发与发布

- 跨平台开发约定、Git 工作流、CMake Presets、CI 和代码格式：[docs/development.md](docs/development.md)
- Windows 部署规则：[docs/deployment-windows.md](docs/deployment-windows.md)
- 变更记录：[CHANGELOG.md](CHANGELOG.md)

## 目录结构

```text
CMakeLists.txt          共享构建定义（Qt 6 + Spectrum SDK）
CMakePresets.json       Linux/Windows 构建预设
src/                    共享应用源码
tests/                  共享离线测试（无需采集卡）
third_party/Spectrum/   Spectrum SDK 头文件（运行库在 linux/lib 或 win/lib）
third_party/concurrentqueue/  moodycamel 无锁队列
cmake/                  CMake 辅助模块
linux/                  Linux 构建脚本、SDK .so、build/、dist/
win/                    Windows 构建脚本、SDK lib/DLL、build/、dist/、source-dist/
docs/                   跨平台开发与部署文档
.github/workflows/      GitHub Actions CI
```

## 许可证

本项目使用 MIT 许可证，详见 [LICENSE](LICENSE)。
