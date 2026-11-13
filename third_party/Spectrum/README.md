# Spectrum SDK

`include/` 是 Linux 和 Windows 共用的 Spectrum SDK 头文件，仓库里只保留这一份。
不要在每个平台目录下再复制一套，否则 SDK 版本会悄悄漂移。

## 运行期库

运行期二进制不入库，由开发者在本地放入：

| 平台 | 文件 | 用途 |
|---|---|---|
| Linux | `linux/lib/libspcm_linux.so` | 用户态驱动，CMake 会把它写入构建 RPATH |
| Windows | `win/lib/spcm_win64_msvcpp.lib` | MSVC x64 导入库，链接时使用 |
| Windows | `win/lib/spcm_win64.dll` | 运行期驱动 DLL，构建后复制到 exe 旁 |

- 必须使用与采集卡驱动匹配的官方 SDK/驱动版本；混用旧 DLL 会导致设备打不开或行为异常。
- Linux 运行前需加载 Spectrum 内核驱动，设备名为 `/dev/spcm0`。
- Windows 设备名为 `spc0`，仍需安装完整 Spectrum 驱动；只复制 DLL 不足以连接采集卡。
- 这些文件被 `.gitignore` 忽略，不会进入 Git 或源码包。若 Spectrum 许可允许分发，
  可自行调整规则后显式提交。

## 头文件校验

`SHA256SUMS.txt` 记录 `include/` 下四个头文件的 SHA256。在本目录执行：

```bash
sha256sum -c SHA256SUMS.txt
```

Windows 上可逐个使用：

```powershell
certutil -hashfile include\dlltyp.h SHA256
```

校验失败通常说明换机器后 SDK 版本不一致；请使用同一版本的官方头文件和运行库。

## 许可

Spectrum SDK 的版权和分发条款归 Spectrum Instrumentation GmbH 所有。
仓库只包含可公开分发的头文件；运行期二进制由使用者自行从官方 SDK/驱动获取。
