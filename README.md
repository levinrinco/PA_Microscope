# PA_Microscope

光声显微镜（PAM）实时数据采集与可视化软件。支持 Spectrum M4i 系列采集卡，螺旋/矩形轨迹扫描，连续无丢帧采集。

## 特性

- **四线程采集架构** — 采集、处理、补充分配、存盘各自独立线程，lock-free queue 解耦
- **零丢帧连续采集** — DMA 缓冲自动对齐 segment 边界，环形回绕不丢数据
- **实时 MIP 成像** — 三缓冲异步渲染，帧边界无阻塞
- **3D 体积存储** — NPY 格式，多线程 gridding
- **AC/DC 耦合兼容** — 软件 DC 去除，适配不同前端

## 硬件要求

- Spectrum M4i.44xx 系列采集卡（PCIe）
- FPGA 触发源（串口控制，115200 baud）
- 振镜/扫描台

## 编译

### 依赖

- Qt 6.x（Core, Gui, Widgets, OpenGL, OpenGLWidgets, SerialPort）
- CMake 3.16+
- Visual Studio 2022（MSVC）
- Spectrum SDK（已包含在 `third_party/Spectrum/`）

### 构建

```bash
# 编辑 CMakeLists.txt 中的 Qt 路径，或通过命令行指定
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build build --config Release
```

生成 `build/Release/PA_Spiral_PAM.exe`。

### 打包

```bash
deploy.bat
```

自动调用 windeployqt 并复制 VC 运行时，输出到 `dist/`。

## 使用

1. 启动程序，等待采集卡初始化
2. 选择串口（FPGA 控制），点"打开"
3. 加载螺旋轨迹 CSV 文件（格式：`index, x_m, y_m`）
4. 设置图像尺寸、信号窗口深度、偏移
5. 勾选"连续采集"（多帧）或取消（单帧）
6. 设置存储路径，勾选"存储使能"
7. 点击"开始扫描"

## 文件存储

```
存储路径/
  2024-06-18_15-30-00/    ← 每次扫描一个 session 文件夹
    mip_00000.png          ← MIP 图像
    volume_00000.npy       ← 3D 体积 (float32, shape: depth×H×W)
    mip_00001.png
    volume_00001.npy
    ...
```

## 轨迹 CSV 格式

```csv
index,x_m,y_m
0,0.000004,-0.000002
1,0.000004,-0.000002
...
```

- 第一行可以是表头（自动检测跳过）
- 列顺序：index, x_m(米), y_m(米)
- 支持逗号或制表符分隔
- 自动检测扫描半径（max(|x|,|y|) × 1.05）

## 架构

```
采集线程 (TIME_CRITICAL)
  └─ waitForData → enqueue_bulk → releaseData

  ↓ lock-free queue (moodycamel::ConcurrentQueue)

处理线程
  └─ dequeue → DC去除 → 包络检波 → MIP scatter → 帧边界

  ├─→ MIP 三缓冲 → 主线程异步渲染
  ├─→ FrameBuf 三缓冲 → 补充分配线程
  └─→ 存盘线程 → NPY + PNG

主线程
  └─ MIP 最终化 + QImage + 显示
```

## 许可证

MIT
