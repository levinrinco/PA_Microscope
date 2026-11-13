# Windows Spectrum SDK 二进制

完整程序构建前，将匹配版本的 `spcm_win64_msvcpp.lib` 和 `spcm_win64.dll` 放到此目录：

```text
win/lib/spcm_win64_msvcpp.lib
win/lib/spcm_win64.dll
```

- 使用 x64 MSVC 导入库，不使用 32 位或 MinGW 库。
- CMake 在构建后把 DLL 复制到 exe 所在目录；Qt 依赖由 `win/build.ps1 -Deploy` 收集。
- 仅运行离线测试可使用 `-TestsOnly`，不需要上述二进制或采集卡。
- 目标电脑仍需安装完整 Spectrum Windows 驱动；只复制 DLL 不能代替驱动。
- SDK 二进制被 `.gitignore` 忽略，不会进入 Git 或源码包。

Windows 构建步骤见 [win/README.md](../README.md)；
头文件和校验说明见 [third_party/Spectrum](../../third_party/Spectrum/README.md)；
部署规则见 [docs/deployment-windows.md](../../docs/deployment-windows.md)。
