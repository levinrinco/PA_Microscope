# 开发与仓库约定

## 1. 目标

- **一套共享源码**：`src/`、`tests/`、`third_party/` 只维护一份，Linux 和 Windows 都从这里构建。
- **平台目录只放平台内容**：`linux/` 和 `win/` 放构建脚本、SDK 运行库和构建/发布产物，不放源码副本。
- **不再使用日期快照目录**：版本由 Git 提交和标签管理，`1027`/`1111` 这类目录不再创建。
- **可重复验证**：本地脚本 + CMake Presets + GitHub Actions 覆盖两个平台的离线测试。

## 2. 仓库结构

```text
PA_Microscope/
  CMakeLists.txt          # 共享构建入口
  CMakePresets.json       # Linux/Windows 预设
  src/                    # 共享应用源码
  tests/                  # 共享离线测试
  third_party/            # 共享第三方依赖（含 Spectrum 头文件）
  cmake/                  # 共享 CMake 辅助模块
  linux/
    build.sh              # Linux 构建入口
    README.md             # Linux 构建说明
    lib/                  # libspcm_linux.so（忽略，不入库）
    build/                # Linux 构建产物（忽略）
    dist/                 # Linux 发布产物（忽略）
  win/
    build.ps1             # Windows 构建入口
    package-source.cmake  # 跨平台源码包
    README.md             # Windows 构建说明
    lib/                  # Spectrum MSVC 导入库/DLL（忽略，不入库）
    build/                # Windows 构建产物（忽略）
    build-tests/          # Windows 测试构建目录（忽略）
    dist/                 # Windows 运行发布产物（忽略）
    source-dist/          # 跨平台源码包（忽略）
  docs/                   # 跨平台开发、部署文档
  .github/workflows/      # CI
```

## 3. 跨平台代码规则

1. 平台差异优先放在 CMake 或少量条件编译中：
   - CMake：`if(WIN32)`、`if(MSVC)`、`if(NOT WIN32)`。
   - C++：`#ifdef _WIN32` / `#ifndef _WIN32`；Qt 代码也可用 `Q_OS_WIN`。
2. 不要在 `src/` 里维护 `src_windows/`、`src_linux/` 两份实现。
3. 平台差异必须同时满足两边的离线测试；不能只在一边编译通过。
4. 新增源文件时同步更新 `CMakeLists.txt` 的 `SOURCES`/`HEADERS`，新增测试时更新对应测试目标。
5. 平台路径不要写死：
   - Linux SDK：`linux/lib/libspcm_linux.so`，由 CMake 写 RPATH。
   - Windows SDK：`win/lib/spcm_win64_msvcpp.lib` + `spcm_win64.dll`，构建后复制 DLL。
6. 不要再引入 `windows/include` 之类的 SDK 头文件副本；统一用 `third_party/Spectrum/include/`。

## 4. 构建与测试

### Linux

```bash
./linux/build.sh test              # 有 SDK 时构建完整程序并测试
PA_BUILD_APP=OFF ./linux/build.sh test   # 只跑离线测试
```

输出：`linux/build/PA_Microscope`。

### Windows

```powershell
.\win\build.ps1 -QtRoot C:\Qt\6.11.1\msvc2022_64 -TestsOnly
.\win\build.ps1 -QtRoot C:\Qt\6.11.1\msvc2022_64 -Deploy
```

输出：`win/build/bin/Release/`，部署目录 `win/dist/windows-Release-<timestamp>/`。

### CMake Presets

```bash
cmake --preset linux-tests
cmake --build --preset linux-tests
ctest --preset linux-tests
```

```powershell
$env:QT_ROOT = 'C:\Qt\6.11.1\msvc2022_64'
cmake --preset windows-tests
cmake --build --preset windows-tests
ctest --preset windows-tests
```

`PA_BUILD_APP=OFF` 只构建离线测试，不要求 Spectrum SDK；`ON` 构建完整硬件程序。
完整程序的真机采集验证仍必须在装有采集卡和驱动的机器上完成。

## 5. Git 工作流

- 主分支：`main`。功能开发使用短分支，合并前跑 Linux/Windows 测试。
- 提交信息说明“为什么”，不要只写“update”。
- 版本标签：
  - 源码快照：`snapshot-YYYY-MM-DD`；
  - 对外发布：`vMAJOR.MINOR.PATCH`。
- **不要再创建 `1027/`、`1111/` 这类日期目录**。需要回退时用 `git checkout <tag>` 或
  `git worktree`；历史快照已归档在仓库外。
- `CHANGELOG.md` 记录日期、功能、行为和兼容性变化。
- `CMakeCache.txt`、`build/`、`dist/`、SDK 二进制、采集数据、`PA_Microscope.ini` 不入库。

## 6. CI

`.github/workflows/ci.yml` 在每次 push/PR 运行：

- Linux：Ubuntu + 系统 Qt 6，`PA_BUILD_APP=OFF`，构建并 `ctest`。
- Windows：Windows Server 2022 + Qt MSVC 套件，`PA_BUILD_APP=OFF`，构建并 `ctest`。

CI 不构建完整硬件程序，因为 Spectrum SDK 运行库不入库。发布前在本地用真实 SDK 执行
`-Deploy`，并按 [deployment-windows.md](deployment-windows.md) 在目标机验收。

## 7. 代码格式

- `.editorconfig`：UTF-8、LF、4 空格缩进；批处理/PowerShell 使用 CRLF。
- `.clang-format`：与现有代码风格一致（4 空格、100 列、函数/类大括号换行）。
- 只格式化改动过的文件，避免整文件重排造成无关 diff：

```bash
clang-format -i src/Foo.cpp src/Foo.h
```

## 8. SDK 与二进制

- Spectrum 头文件：`third_party/Spectrum/include/`，校验和见 `third_party/Spectrum/SHA256SUMS.txt`。
- Linux 运行库：`linux/lib/libspcm_linux.so`（忽略）。
- Windows 运行库：`win/lib/spcm_win64_msvcpp.lib`、`win/lib/spcm_win64.dll`（忽略）。
- 三种平台资源都来自官方 SDK/驱动；版本不匹配会导致设备打不开。
- 不把 Qt 或 Spectrum 二进制提交进仓库；源码包由 `win/package-source.cmake` 生成到
  `win/source-dist/PA_Microscope_Source.zip`。

## 9. 文档索引

| 文档 | 内容 |
|---|---|
| `README.md` | 功能、快速开始、数据格式、深度编码、架构 |
| `linux/README.md` | Linux 依赖、构建、运行、实时优先级 |
| `win/README.md` | Windows 构建、测试、打包、验收 |
| `docs/deployment-windows.md` | Windows 部署目录规则、目标机要求、常见问题 |
| `third_party/Spectrum/README.md` | SDK 头文件/运行库放置与校验 |
| `CHANGELOG.md` | 版本/日期变更记录 |

## 10. 新机器上手

1. `git clone` 仓库，切到 `main`。
2. 安装平台依赖（Qt 6、CMake、编译器、OpenGL/串口）。
3. 从官方 SDK 取得 Spectrum 运行库，放到 `linux/lib/` 或 `win/lib/`。
4. 跑离线测试：Linux `./linux/build.sh test`，Windows `.\win\build.ps1 -QtRoot ... -TestsOnly`。
5. 需要完整程序时带 SDK 构建，并按平台部署文档在目标机验收。

旧日期快照（`1027`、`1111`）和旧 Windows 工作树不作为开发入口；需要历史代码时从归档目录或
Git 标签获取。
