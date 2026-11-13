# Linux Spectrum SDK 运行库

完整程序构建前，将匹配版本的 `libspcm_linux.so` 放在此目录：

```text
linux/lib/libspcm_linux.so
```

- 该文件来自 Spectrum 官方 Linux SDK，需与采集卡内核驱动版本匹配。
- 构建时 CMake 会把 `linux/lib` 写入可执行文件的 RPATH，因此从 `linux/build/` 直接运行即可，不需要 `LD_LIBRARY_PATH`。
- 运行前需加载 Spectrum 内核驱动，设备名为 `/dev/spcm0`。
- 文件被 `.gitignore` 忽略（`*.so`），不会进入 Git 或源码包。
- 只跑离线测试时不需要该文件：`PA_BUILD_APP=OFF ./linux/build.sh test`。

头文件和校验说明见 [third_party/Spectrum](../../third_party/Spectrum/README.md)，
构建步骤见 [linux/README.md](../README.md)。
