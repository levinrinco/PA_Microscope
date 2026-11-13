# Linux 构建与运行

Linux 和 Windows 共用根目录的 `src/`、`tests/` 和 CMake 配置。`linux/` 只放 Linux
构建脚本、SDK 运行库和构建产物；不需要复制第二份源码。

## 1. 依赖

Qt 6（Core、Gui、Widgets、OpenGL、OpenGLWidgets、SerialPort）、CMake 3.16+、
C++17 编译器和 OpenGL 开发包。Ubuntu/Debian 示例：

```bash
sudo apt install cmake ninja-build qt6-base-dev qt6-serialport-dev \
                 qt6-qpa-plugins libgl1-mesa-dev
```

## 2. 构建

在仓库根目录执行：

```bash
./linux/build.sh          # Release 构建到 linux/build/
./linux/build.sh test     # 构建并运行离线测试
./linux/build.sh clean    # 删除 linux/build/
./linux/build/PA_Microscope
```

脚本优先使用 Ninja，没有 Ninja 时回退到 Unix Makefiles。可用环境变量覆盖：

```bash
CMAKE_BUILD_TYPE=Debug ./linux/build.sh
BUILD_DIR=/tmp/pa-build ./linux/build.sh test
```

也可以直接使用 CMake：

```bash
cmake -S . -B linux/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build linux/build --parallel
ctest --test-dir linux/build --output-on-failure
```

或者使用 `CMakePresets.json`：

```bash
cmake --preset linux-tests        # 或 linux-release
cmake --build --preset linux-tests
ctest --preset linux-tests
```

## 3. Spectrum 运行库

完整程序需要将匹配版本的运行库放到：

```text
linux/lib/libspcm_linux.so
```

- 头文件统一使用 `third_party/Spectrum/include/`，Linux/Windows 共用一份。
- 构建时 CMake 会把 `linux/lib` 写入可执行文件 RPATH，因此从 `linux/build/` 直接运行即可，不需要 `LD_LIBRARY_PATH`。
- 运行前需加载 Spectrum 内核驱动，设备名为 `/dev/spcm0`。
- 该 `.so` 被 `.gitignore` 忽略，不进入 Git 或源码包。

只跑离线测试时不需要 `.so`：

```bash
PA_BUILD_APP=OFF ./linux/build.sh test
```

## 4. 采集线程实时优先级（可选）

Linux 上的 `SCHED_FIFO` 需要 `CAP_SYS_NICE`，且 FIFO 线程长时间不让出 CPU 会影响整机响应，
因此默认**不启用**，通过环境变量显式打开：

```bash
sudo setcap cap_sys_nice+ep linux/build/PA_Microscope   # 或直接用 root 运行
PA_ACQ_RT_PRIORITY=80 ./linux/build/PA_Microscope       # 取值范围 1..99
```

未设置该变量时采集线程保持默认 CFS 调度；设置了但权限不足时程序打印警告并继续以默认调度运行。
Windows 的优先级用法见 [win/README.md](../win/README.md)。

## 5. 离线测试

三个测试不需要采集卡：

- `PA_CoreTests`：轨迹解析、输出数据、配置、配色、分块 DAT 写入和分层 PNG。
- `PA_PipelineTests`：模拟 DMA、停止排空、深度图、轨迹 B-scan 区间、显示轮询与并发。
- `PA_BscanTests`：共用 B-scan/C-scan 绘图数据、深度切换；常规 CTest 使用 offscreen。

`ctest` 会为 `PA_BscanTests` 设置 `QT_QPA_PLATFORM=offscreen`，不依赖显卡上下文。
需要真实 OpenGL 验证时，参考 [win/README.md](../win/README.md) 的 `PA_TEST_OPENGL` 方式，
在 Linux 上设置相同的环境变量并运行 `linux/build/PA_BscanTests`。

## 6. 相关文档

- 跨平台开发约定、Git 流程和 CI：[docs/development.md](../docs/development.md)
- Windows 构建与部署：[win/README.md](../win/README.md)、[docs/deployment-windows.md](../docs/deployment-windows.md)
- Spectrum SDK 头文件与校验：[third_party/Spectrum/README.md](../third_party/Spectrum/README.md)
