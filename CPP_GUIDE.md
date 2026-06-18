# C++ 实战教学：从一个光声显微镜采集程序说起

> 本文以 `PA_Spiral_PAM` 项目为真实案例，讲解 C 语言基础、C 与 C++ 的关键差异、以及现代 C++ 在实时数据采集系统中的核心概念。**零基础可读**——不需要你提前掌握任何前置知识。

---

## 目录

- [0. C 语言基础速通](#0-c-语言基础速通)
  - [0.1 变量与内存](#01-变量与内存)
  - [0.2 指针](#02-指针)
  - [0.3 数组与指针运算](#03-数组与指针运算)
  - [0.4 结构体](#04-结构体)
  - [0.5 栈与堆](#05-栈与堆)
  - [0.6 类型系统](#06-类型系统)
  - [0.7 位运算](#07-位运算)
  - [0.8 枚举](#08-枚举)
  - [0.9 预处理指令](#09-预处理指令)
  - [0.10 `const` 限定符](#010-const-限定符)
  - [0.11 条件判断与循环](#011-条件判断与循环)
  - [0.12 函数](#012-函数)
  - [0.13 `static` 关键字的三种用法](#013-static-关键字的三种用法)
  - [0.14 `void*` 通用指针](#014-void-通用指针)
  - [0.15 小结：一个完整的 C 程序](#015-小结一个完整的-c-程序)
- [1. C 与 C++ 的核心差异](#1-c-与-c-的核心差异)
- [2. 程序入口与应用生命周期](#2-程序入口与应用生命周期)
- [3. Qt 的元对象系统：信号与槽](#3-qt-的元对象系统信号与槽)
- [4. 多线程：工作线程模式](#4-多线程工作线程模式)
- [5. 同步原语：mutex、atomic、lock_guard](#5-同步原语mutexatomiclock_guard)
- [6. RAII 与资源管理](#6-raii-与资源管理)
- [7. 多缓冲：从双缓冲到三缓冲](#7-多缓冲从双缓冲到三缓冲)
- [8. DMA 与页对齐内存](#8-dma-与页对齐内存)
- [9. 设计模式实战](#9-设计模式实战)
- [10. 数据结构与内存布局](#10-数据结构与内存布局)
- [11. 实时系统的边界意识](#11-实时系统的边界意识)
- [附录：快速参考](#附录快速参考)

---

## 0. C 语言基础速通

这一章用**本项目真实代码**讲解 C 语言的所有核心概念。掌握了这些，读后面 C++ 的内容就无障碍。

### 0.1 变量与内存

变量 = 一块命名的内存空间。声明时指定**类型**，编译器就知道这块空间占多少字节、里面存的东西怎么解释。

```cpp
// src/Types.h —— 从项目中摘取的真实代码
int    depth   = 600;      // 4 字节有符号整数，值范围约 ±21 亿
int    segmentSize = 4096; // 同上
double scanRadiusM = 0.005; // 8 字节双精度浮点
float  w00, w01, w10, w11;  // 4 字节单精度浮点，每个 4 字节
bool   isOpen = false;      // 1 字节布尔值
```

**变量的本质**：当你写 `int depth = 600;`，编译器做三件事：
1. 在内存（栈或数据段）分配 4 字节
2. 把这 4 字节的二进制写成 `0x00000258`（600 的补码表示）
3. 在符号表里记录 `depth` 这个名字对应这块内存

**类型决定了"解释方式"**。同一块内存，换成不同类型指针去看，会得到完全不同的值。这是理解指针的基础。

### 0.2 指针

指针 = 一个存着**另一个变量的地址**的变量。

```cpp
// src/DataPipeline.cpp
int16_t *data = m_fifo->dataBufferAt(status.availUserPos);
//      ↑ data 不是 int16_t，而是"指向 int16_t 的指针"
//        data 本身的值是一个内存地址，比如 0x00007FF6A0001000
//        *data 才是那个地址上的 int16_t 值
```

**三个核心操作**：

| 操作 | 语法 | 含义 |
|------|------|------|
| 取地址 | `&x` | 获取变量 `x` 的内存地址 |
| 解引用 | `*p` | 读取指针 `p` 指向的那块内存的值 |
| 箭头 | `p->field` | 等价于 `(*p).field`，在结构体指针上访问成员 |

```cpp
// 用本项目代码演示
int16_t *ptr = &data[idx];   // ptr 指向 data 数组第 idx 个元素的地址
int16_t val = *ptr;          // 解引用：读出这个地址上存的值
ptr++;                       // 指针+1 = 前进 sizeof(int16_t) = 2 字节
```

**指针为什么有类型？** `int16_t *p` 和 `float *q` 的区别：
- `p++` 让地址前进 2 字节（`sizeof(int16_t)`）
- `q++` 让地址前进 4 字节（`sizeof(float)`）
- `*p` 读 2 字节按 int16_t 解释，`*q` 读 4 字节按 IEEE 754 浮点解释

指针本身不携带"它指向的东西有多大"的信息——这个信息来自**指针的类型**。

### 0.3 数组与指针运算

```cpp
// src/DataPipeline.cpp
const int16_t *alinePtr = &data[idx];
// data 是 int16_t 数组。data[idx] 等价于 *(data + idx)

int alineOff = m_pos * depth;
std::copy(alinePtr + windowOff,               // 起始地址 = alinePtr + windowOff 字节偏移
          alinePtr + windowOff + copyLen,     // 结束地址
          buf->rawAlines.begin() + alineOff); // 目标地址
```

**`数组名` 就是指向第一个元素的指针**：

```cpp
int arr[5] = {10, 20, 30, 40, 50};
int *p = arr;       // p 指向 arr[0]
// arr[2] 和 *(arr + 2) 和 *(p + 2) 完全等价，都是 30
```

**指针减法的含义**：两个同类型指针相减，结果不是字节差，而是**元素个数差**。

**`arr[负数]` 在 C 里完全合法**：
```cpp
int *p = &arr[2];   // p 指向 30
p[-1];              // = *(p - 1) = arr[1] = 20，完全不报错
```

### 0.4 结构体

结构体把多个变量打包成一个类型。在 C/C++ 里，结构体的每个字段（成员）按照声明顺序在内存中**紧凑排列**（可能有填充字节保证对齐）。

```cpp
// src/Types.h —— 轨迹点的结构体
struct TrajectoryPoint {
    float x_m = 0.0f;    // 偏移 0:  X 坐标 (米)
    float y_m = 0.0f;    // 偏移 4:  Y 坐标 (米)
    float r_m = 0.0f;    // 偏移 8:  径向距离 (米)
    float theta = 0.0f;  // 偏移 12: 角度 (弧度)
};                        // 总大小: 16 字节

// src/Types.h —— 双线性插值映射的查找表条目
struct MapInfo {
    int idx00, idx01, idx10, idx11;   // 偏移 0,4,8,12: 4 个 int (各 4 字节)
    float w00, w01, w10, w11;         // 偏移 16,20,24,28: 4 个 float (各 4 字节)
};                                     // 总大小: 32 字节
```

**一个真实的性能教训**：`MapInfo` 每个条目 32 字节。项目有 33124 条 A-line，这个查找表占 `33124 × 32 ≈ 1.06 MB`。全塞进 L3 缓存（典型 8-16MB）没问题，所以实时访问是纯缓存命中。

**从结构体指针访问成员**：

```cpp
// src/DataPipeline.cpp
const MapInfo &mi = (*m_mapInfo)[m_pos];  // mi 是 m_mapInfo 数组的第 m_pos 个元素
int pixelIdx = mi.idx00;                   // 点操作符 . 用于结构体本身
// 如果是指针：int pixelIdx = ptr->idx00;  // 箭头操作符 -> 是 (*ptr).idx00 的简写
```

**`.` 和 `->`**：
```cpp
TrajectoryPoint pt;         // 栈上的结构体
pt.x_m = 0.001f;            // . 访问成员

TrajectoryPoint *p = &pt;   // 指针
p->x_m = 0.002f;            // -> 等价于 (*p).x_m
```

### 0.5 栈与堆

这是 C/C++ 里最重要的内存概念之一。

**栈（Stack）**：
```cpp
void someFunction() {
    int depth = 600;           // 栈变量：函数返回时自动销毁
    float peak = 0.0f;         // 栈变量：分配和释放都零开销
    MapInfo mi;                // 32 字节结构体也在栈上
}  // depth, peak, mi 在此自动释放——编译器只需改一下栈指针寄存器
```

**堆（Heap）**：
```cpp
// src/SpectrumFifo.cpp —— DMA 缓冲必须在堆上
m_pData = static_cast<int16_t *>(VirtualAlloc(    // 向 OS 申请 64MB
    nullptr, (SIZE_T)bufferSizeBytes,
    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
// 这块内存在 freeBuffer() 调用 VirtualFree 之前一直存活
// 函数返回不影响它
```

| | 栈 | 堆 |
|---|---|---|
| 分配速度 | 1 条 CPU 指令（移栈指针） | 数百～数千条指令（OS 调用） |
| 大小上限 | 1-8 MB（OS 限制） | 几十 GB（物理内存限制） |
| 生命周期 | 作用域结束自动释放 | 手动释放，或程序结束时由 OS 回收 |
| 典型用途 | 局部变量、小数组 | 大缓冲区、跨函数存活的数据 |

**本项目的一个关键设计**：

```cpp
// src/DataPipeline.cpp —— 热路径上的局部变量全在栈上
float dcSum = 0.0;
float peak = 0.0f;
// 栈分配的"开销"只是一条 sub rsp, N 指令——比任何堆操作快几个数量级
```

### 0.6 类型系统

C/C++ 的类型决定了三样东西：**占多少字节、取值范围、运算行为**。

**整数类型**：

```cpp
// src/Types.h
int      imageWidth = 500;     // 至少 16 位，现代平台 32 位
int64_t  memSizeBytes;         // 精确 64 位有符号（出自 <cstdint>）
int32_t  fillSizePromille;     // 精确 32 位有符号
uint64_t channelMask;          // 精确 64 位无符号（出自 <cstdint>）
int16_t  rawSample;            // 精确 16 位有符号（ADC 采样值）
```

**固定宽度类型的必要性**：ADC 采样值是 16 位，如果写成 `int`，换一个平台可能变成 32 位——读取 DMA 数据时字节数就不对了。`int16_t` 保证永远是 2 字节。

**浮点类型**：

```cpp
float  peak = 0.0f;      // 32 位 IEEE 754，约 7 位有效数字
double dcMean = dcSum / win;  // 64 位 IEEE 754，约 15 位有效数字
```

**类型转换**：

```cpp
// 隐式转换（自动）
int a = 10;
double b = a;      // int → double，值不变，精度扩大

// 显式转换（C++ 风格——推荐）
double v = std::abs(static_cast<double>(alinePtr[i]) - dcMean);
//           ↑ 明确告诉读者"这里在转型"，比 C 风格 (double)alinePtr[i] 更安全

// 另一个真实例子
float peak = static_cast<float>(v);  // double → float，可能丢失精度
```

**类型转换的风险**：
```cpp
int64_t largeVal = 0x100000000;   // 超过 32 位
int32_t smallVal = static_cast<int32_t>(largeVal);  // 截断！只保留低 32 位
// 编译器不报错，因为 static_cast 表示"我知道自己在做什么"
```

### 0.7 位运算

```cpp
// src/MainWindow.cpp —— 解析采集卡状态寄存器
if (status & 0x00000001) parts << "PRETRG";  // 按位与：测试第 0 位
if (status & 0x00000002) parts << "TRG";     // 测试第 1 位
if (status & 0x00000100) parts << "BLOCK";   // 测试第 8 位

// src/MainWindow.cpp —— 构建通道使能掩码
uint64_t mask = 0;
mask |= (1ULL << ch);  // 把第 ch 位置 1。1ULL = unsigned long long 常量
// 例：ch=3 → 1ULL << 3 = 0b1000 = 0x8
```

| 运算 | 符号 | 示例 | 效果 |
|------|------|------|------|
| 按位与 | `&` | `x & 0x01` | 只保留最低位 |
| 按位或 | `\|` | `x \| 0x80` | 第 7 位置 1 |
| 左移 | `<<` | `1 << 5` | 得到 32 |
| 右移 | `>>` | `c >> 8` | 右移 8 位 |
| 取反 | `~` | `~x` | 所有位翻转 |

**逻辑运算 vs 位运算——容易混淆**：
```cpp
if (a && b)  // 逻辑与：整个表达式为真/假，短路求值
if (x & 0x1) // 位与：按位计算，结果是整数
// 这是两个完全不同的运算符！
```

### 0.8 枚举

枚举给整数值起名字，让代码可读。

```cpp
// src/Types.h —— C++11 的 enum class（推荐）
enum class ScanState {
    Idle,      // = 0
    Armed,     // = 1
    Scanning,  // = 2
    Done,      // = 3
    Error      // = 4
};

// 使用
if (m_state == ScanState::Idle)  // 必须带 ScanState:: 前缀——防命名冲突
    doArm();
```

**`enum class` vs C 风格的 `enum`**：

```cpp
// C 风格（不推荐）
enum ScanState { IDLE, ARMED, SCANNING, DONE, ERROR };
if (state == IDLE)  // IDLE 裸露在全局命名空间，容易和别的 IDLE 冲突

// C++11 enum class（推荐）
enum class ScanState { Idle, Armed, Scanning, Done, Error };
if (state == ScanState::Idle)  // 封闭在 ScanState 作用域内
```

### 0.9 预处理指令

预处理在**编译之前**执行，纯文本替换。

```cpp
// src/SpectrumFifo.cpp
#ifdef Q_OS_WIN                          // 如果定义了 Q_OS_WIN（Windows 编译）
    return VirtualAlloc(nullptr, ...);   // 用 Windows API
#else                                    // 否则（Linux 等）
    Q_UNUSED(size);                      // 参数未使用标记
    return nullptr;                      // 暂不支持
#endif

// 头文件保护
#pragma once  // 防止同一头文件被多次包含（所有现代编译器都支持）
```

**常见指令**：

| 指令 | 作用 |
|------|------|
| `#include <...>` | 插入系统头文件 |
| `#include "..."` | 插入项目头文件 |
| `#define X Y` | 定义宏 X 替换为 Y |
| `#ifdef / #ifndef` | 条件编译 |
| `#pragma once` | 防止重复包含 |

**`#include` 的本质**：编译器在预处理阶段把被包含文件的**全部文本**复制粘贴到 `#include` 那行。所以 `#pragma once`（或 C 传统的 `#ifndef` 守卫）是必要的——否则同一个结构体会被定义两次，编译报错。

### 0.10 `const` 限定符

`const` = "我承诺不修改这个值"。编译器会帮你检查。

```cpp
// 读法：从右往左读
const int maxVal = 100;           // maxVal 是常量 int
const char *str = "hello";        // str 指向常量 char（不能通过 str 修改，但 str 本身可变）
char *const ptr = &buf;           // ptr 是常量指针（ptr 本身不能变，但可以修改 *ptr）
const char *const c = "fixed";   // 两者都不能变

// 本项目中的真实用法
const int16_t *alinePtr = &data[idx];  // 承诺：不会通过 alinePtr 修改 data 的内容
const MapInfo &mi = (*m_mapInfo)[m_pos];  // 承诺：只读 mi，不修改
```

**`const` 的作用**：
1. 防止意外修改——把 bug 从运行时提前到编译时
2. 给读者信号——"这个值在后续代码中不会变"
3. 允许编译器做更多优化

### 0.11 条件判断与循环

```cpp
// if / else if / else
if (m_state != ScanState::Idle && m_state != ScanState::Done)
    return;  // 守卫：状态不对就提前返回

// 三元运算符（表达式层面的 if-else）
int64_t availSamples = (rawSamples < maxAvail) ? rawSamples : maxAvail;
//                      ↑ 条件                  ↑ 真           ↑ 假

// for 循环
for (int i = 0; i < alinesInBlock; ++i) {  // 初始化; 条件; 每次迭代后
    // 循环体
}
// ++i vs i++: 对 int 没区别。但 ++i 是"先加再用"，i++ 是"先用再加"
// 对复杂类型（迭代器），++i 更高效，所以习惯上偏好 ++i

// while 循环
while (!m_stopRequested.load(std::memory_order_acquire)) {
    // 当条件为真时重复执行
}

// continue 和 break
if (err == ERR_TIMEOUT) continue;  // 跳过本次迭代剩余部分，下次循环
if (err == ERR_ABORT) break;       // 退出整个循环
```

### 0.12 函数

```cpp
// 函数声明（放在 .h 文件）—— 告诉编译器有这个函数
int mapToIndex(double value, double maxVal, int mapping, bool clipping);

// 函数定义（放在 .cpp 文件）—— 实际的代码
int mapToIndex(double value, double maxVal, int mapping, bool clipping) {
    if (maxVal < 1e-9) return 0;    // 守卫：除零保护
    double norm = value / maxVal;
    if (mapping == 1) {             // 对数映射
        return static_cast<int>(log10(1.0 + norm * 99.0) / 2.0 * 255);
    } else {                        // 线性映射
        return static_cast<int>(norm * 255);
    }
}
```

**参数传递方式**：

```cpp
void byValue(int x);           // 传值：拷贝。x 的修改不影响调用者
void byPointer(int *x);        // 传指针：可以修改调用者的变量
void byReference(int &x);      // 传引用（C++ 独有）：像指针但语法更干净

// 本项目的例子——传 const 引用，避免拷贝大对象
ScanParams ConfigManager::loadScanParams() const {
    ScanParams p;
    p.depth = m_settings.value("Scan/depth", 600).toInt();
    return p;  // 现代编译器会用 RVO/NRVO 优化掉拷贝
}
```

### 0.13 `static` 关键字的三种用法

`static` 在 C/C++ 里含义随上下文变化——这是 C 语言最令人困惑的设计之一。

**用法 1：文件内全局变量（内部链接）**

```cpp
// src/Logger.cpp
static QFile s_logFile;           // 只有 Logger.cpp 内部能访问
static QMutex s_mutex;            // 同上
static bool s_inited = false;     // 同上
// 别的 .cpp 文件里即使写 extern QFile s_logFile; 也访问不到
```

**用法 2：函数内静态局部变量**

```cpp
// 等同于全局变量，但作用域限制在函数内，且首次执行到才初始化
static int callCount = 0;
callCount++;
```

**用法 3：静态成员函数（C++ 独有，后面章节会讲）**

### 0.14 `void*` 通用指针

`void*` = "我存着一个地址，但我不知道（或不关心）它指向什么类型"。必须显式转型才能用。

```cpp
// src/SpectrumFifo.cpp
void *SpectrumFifo::allocPageAligned(uint64_t size) {
    return VirtualAlloc(nullptr, (SIZE_T)size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    // VirtualAlloc 返回 void* —— Windows API 不知道你要这块内存存什么类型
}

// 使用时必须转型
m_pData = static_cast<int16_t *>(allocPageAligned(bufferSizeBytes));
//        ↑ 告诉编译器："我知道这块内存用来存 int16_t"
```

### 0.15 小结：一个完整的 C 程序

把上述概念串起来，写一个"用本项目代码片段手写的 C 程序"作为复习：

```c
#include <stdio.h>   // printf
#include <stdint.h>  // int16_t, uint32_t

// 结构体定义
typedef struct {
    int idx00, idx01, idx10, idx11;
    float w00, w01, w10, w11;
} MapInfo;

// 函数：计算 MIP 最大值（遍历所有像素）
float findMaxMip(const float *data, int count) {
    float maxVal = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (data[i] > maxVal)
            maxVal = data[i];
    }
    return maxVal;
}

int main(void) {
    // 栈上的数组
    float mipPixels[250000];  // 500×500
    int gridSize = 250000;

    // 堆上的查找表（用 malloc 分配）
    MapInfo *lut = (MapInfo *)malloc(33124 * sizeof(MapInfo));
    if (!lut) { printf("malloc failed\n"); return 1; }

    // 模拟 scatter（循环 + 指针运算 + 结构体访问）
    for (int i = 0; i < 33124; ++i) {
        float peak = (float)(i % 100) / 10.0f;
        int idx = lut[i].idx00;
        mipPixels[idx] += peak * lut[i].w00;
    }

    // 调用函数
    float maxVal = findMaxMip(mipPixels, gridSize);
    printf("Max MIP value: %f\n", maxVal);

    // 释放堆内存
    free(lut);
    return 0;
}
```

**这里有三个你看完所有章节后会意识到的问题**：
1. `malloc`/`free` 容易漏——C++ 用 RAII 自动管理
2. `MapInfo` 里的 `float` 数组如果能自动扩容就好了——C++ `std::vector` 解决
3. `findMaxMip` 只接受 `float*`，换个类型要重写——C++ 模板解决

---

## 1. C 与 C++ 的核心差异

**C++ 不是"带类的 C"**——它是一种用完全不同的哲学来组织代码的语言。

### 1.1 构造与析构：对象的出生和死亡

在 C 里，创建结构体就是分配内存，你得记得手动初始化：

```c
// C 风格
MapInfo mi;
mi.idx00 = 10;  // 手动初始化每个字段——漏一个就是 bug
mi.idx01 = 12;
// ...
```

C++ 里，**构造函数**在对象"出生"时自动执行，**析构函数**在对象"死亡"时自动执行：

```cpp
// src/DataPipeline.cpp
DataPipeline::DataPipeline(SpectrumFifo *fifo, QObject *parent)
    : QObject(parent)      // 先构造基类部分
    , m_fifo(fifo)         // 再按声明顺序初始化每个成员
{                          // 构造函数体——可选的额外逻辑
}
// 这里 m_mapInfo 自动初始化为 nullptr（在类定义里写的 = nullptr）
// 不需要像 C 那样手动给每个字段赋值
```

这是 C++ 最根本的设计哲学：**把资源生命周期的管理自动化**。第 6 章 RAII 会详细展开。

### 1.2 引用：安全的指针

```cpp
// C 里没有引用，只有指针
void swap_c(int *a, int *b) { int t = *a; *a = *b; *b = t; }
swap_c(&x, &y);  // 必须显式取地址，容易忘

// C++ 引用——语法糖，但很重要
void swap_cpp(int &a, int &b) { int t = a; a = b; b = t; }
swap_cpp(x, y);  // 直接传变量，不需要 &
```

引用的三条保证：
1. **不能为空**——没有"空引用"（对比：指针可以是 `nullptr`）
2. **不能改指向**——引用一旦绑定就不能重新绑到别的变量
3. **语法像值**——不需要解引用

```cpp
// src/DataPipeline.cpp —— 引用的真实用法
const MapInfo &mi = (*m_mapInfo)[m_pos];
// mi 直接当 MapInfo 用，不用 -> 。且 const 保证不会意外修改
float w = mi.w00;  // 干净，不是 mi->w00
```

### 1.3 命名空间：告别名字冲突

```cpp
// src/Logger.cpp
namespace Logger {           // Logger 命名空间——里面的东西不会和外面冲突
    void init(const QString &logDir);
}

// 调用时必须带前缀
Logger::init(logDir);
```

C 里没有命名空间，所有函数名在一个全局大池子里。项目大了之后，两个不同模块各有一个 `init()` 就会冲突。C 的传统解决方案是加前缀：`logger_init()`、`fifo_init()`——命名空间是这种字符串前缀的形式化。

### 1.4 函数重载：同名不同参

```cpp
// SpectrumFifo 里 readFifoStatus 只有一个版本，但 Qt 的 setValue 有几十个重载
m_settings.setValue("Scan/depth", 600);          // int 版本
m_settings.setValue("Scan/trajectory_csv", "");  // QString 版本
m_settings.setValue("Scan/channel_mask", 1ULL);  // unsigned long long 版本
```

C++ 编译器根据参数的类型和数量自动选择正确的函数版本。C 做不到——你得写 `setValueInt`、`setValueStr`、`setValueU64`。

### 1.5 模板：类型参数化

```cpp
// C 的做法：写死类型
float max_float(float a, float b) { return a > b ? a : b; }
int   max_int(int a, int b)     { return a > b ? a : b; }
// 每种类型一份——又臭又长

// C++ 模板：把类型变成参数
template<typename T>
T max(T a, T b) { return a > b ? a : b; }
// max(3, 5)       → 编译器生成 max<int>
// max(3.0f, 5.0f) → 编译器生成 max<float>
// 一份代码，编译器帮你生成所有需要的版本
```

本项目里模板用得不多但很关键——你用的 `std::vector<float>`、`std::atomic<int>` 都是模板实例化出来的具体类。

### 1.6 new/delete 替代 malloc/free

```cpp
// C
MapInfo *p = (MapInfo *)malloc(sizeof(MapInfo));
free(p);

// C++（对象）
MapInfo *p = new MapInfo;     // 分配内存 + 调用构造函数
delete p;                     // 调用析构函数 + 释放内存

// C++（数组）
int *arr = new int[100];      // 分配 100 个 int 的数组
delete[] arr;                 // 注意：数组用 delete[]
```

**`new` 比 `malloc` 多的关键步骤**：调用构造函数。`malloc` 只分配裸内存，不会初始化 C++ 对象（比如 `std::string` 不初始化就是一团乱码）。

**但在现代 C++ 里，裸 `new`/`delete` 应该尽量避免**——用 `std::vector`、`std::shared_ptr` 等容器和智能指针代替。本项目大多数动态内存都是用 `std::vector` 管理的。

### 1.7 类的访问控制：public / private / protected

```cpp
// src/DataPipeline.h
class DataPipeline : public QObject {
public:              // 任何人都能访问
    void setContinuous(bool on);
    std::vector<double> getLatestAline();

signals:             // = protected（Qt 宏展开后）
    void mipFrameCompleted(int slotIdx, int frameIdx);

private:             // 只有这个类自己的成员函数能访问
    int m_pos = 0;
    void handoffMipFrame(int totalAlines, int depth);
};
```

C 没有访问控制。任何代码都能读写结构体的任何字段。在大型项目中，这意味着你改一个字段的名字可能影响全项目几百个文件。C++ 的 private 把这种"实现细节"封装起来——外部代码只通过 public 接口交互。

### 1.8 总结对比表

| | C | C++ |
|---|---|---|
| 内存管理 | `malloc`/`free`（手动） | `new`/`delete` + 智能指针 + RAII |
| 参数传递 | 值 + 指针 | 值 + 指针 + **引用** |
| 类型安全 | 弱（`void*` 满天飞） | 强（`static_cast`，模板） |
| 代码组织 | 函数 + 全局变量 | 类 + 命名空间 + 访问控制 |
| 泛型编程 | 宏或 `void*` | **模板** |
| 资源管理 | 手动清理，容易漏 | **RAII** 自动清理 |
| 多态 | 函数指针 | 虚函数 + 继承 |
| 标准库 | 极小（`stdio`, `stdlib`, `string`） | 庞大（容器、算法、线程、智能指针……） |

---

## 2. 程序入口与应用生命周期

```cpp
// src/main.cpp
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // ...
    MainWindow w;
    w.show();
    return app.exec();  // ← 进入事件循环，阻塞直到窗口关闭
}
```

### 关键概念：事件循环

`app.exec()` 进入 **Qt 事件循环**。此后程序不再按行号顺序执行，而是：

```
while (程序未退出) {
    从队列取一个事件（鼠标点击、定时器到期、信号触发……）
    找到对应的槽函数，执行它
}
```

理解这一点是理解一切 Qt 程序的基础：**你的代码是被事件"驱动"的，不是你驱动事件。**

### QApplication 做了什么

| 责任 | 说明 |
|------|------|
| 事件分发 | 操作系统事件 → Qt 事件 → 槽函数调用 |
| 信号/槽路由 | 管理所有 QObject 之间的连接关系 |
| 资源初始化 | 字体、样式、剪贴板等 GUI 基础设施 |
| 生命周期 | 程序退出的最后一道守门人 |

---

## 3. Qt 的元对象系统：信号与槽

### 3.1 为什么不是回调函数指针？

在 C 里，你可能会写：

```c
typedef void (*callback_t)(int data);
void register_callback(callback_t cb);
```

问题：
- 函数指针不携带上下文 —— 你不知道"谁"注册的
- 线程安全要自己管 —— 回调在哪个线程执行？
- 断开连接麻烦 —— 需要维护链表

Qt 的信号/槽用 **观察者模式** 解决了这些问题。

### 3.2 MOC：Qt 的代码生成器

```cpp
// src/DataPipeline.h
class DataPipeline : public QObject {
    Q_OBJECT  // ← 这个宏是 MOC 的信号
public:
    // ...
signals:                 // signals 不是 C++ 关键字，是宏 → protected
    void mipFrameCompleted(int slotIdx, int frameIdx);
    void scanFinished();
    void errorOccurred(const QString &message);
};
```

`Q_OBJECT` 宏告诉 Qt 的 **Meta-Object Compiler (MOC)**：这个类需要特殊处理。MOC 会在编译前扫描头文件，为每个带 `Q_OBJECT` 的类生成额外的 C++ 代码（`moc_*.cpp`），包括：

- 类型信息（运行时反射）
- 信号/槽的调度表
- 动态属性系统

**关键理解**：`signals:` 下面的函数**你只声明不实现**。MOC 自动生成实现体，内部调用 Qt 的调度引擎。

### 3.3 连接的生命周期

```cpp
// src/MainWindow.cpp — spawnPipeline() 中
connect(m_pipeline, &DataPipeline::mipFrameCompleted,  // 发送者 + 信号
        this,                                          // 接收者
        [this](int slotIdx, int frameIdx) {            // 槽（lambda）
            // ... 异步处理 MIP 帧
        });
```

| 参数 | 含义 |
|------|------|
| `m_pipeline` | 信号发送者（QObject 子类） |
| `&DataPipeline::mipFrameCompleted` | 成员函数指针 —— 编译期类型安全 |
| `this` | 接收者上下文对象 |
| lambda | 实际执行的代码 |

**连接类型由 Qt 根据线程自动选择**：

| 发送者线程 == 接收者线程 | 连接类型 |
|---|---|
| 相同 | `DirectConnection` — 信号发射直接调用槽，同步执行 |
| 不同 | `QueuedConnection` — 信号参数拷贝到事件队列，异步执行 |

### 3.4 跨线程信号的参数传递

```cpp
// Pipeline 线程（工作线程）发射
emit mipFrameCompleted(completedSlot, m_frameIdx);
//    ↑ 信号参数是 int, int —— 小类型，值拷贝
```

**陷阱**：如果你发射的是大对象（比如 `std::vector<float>`），QueuedConnection 会**拷贝**整个对象到事件队列。对于大体积数据，应该传指针或 `shared_ptr`：

```cpp
// ❌ 不好：每帧拷贝 1MB+ 的数据
signals:
    void badSignal(std::vector<float> largeData);

// ✅ 好：只拷贝一个指针（16 字节）
signals:
    void goodSignal(std::shared_ptr<std::vector<float>> largeData);
```

---

## 4. 多线程：工作线程模式

### 4.1 为什么需要工作线程？

主线程（GUI 线程）跑事件循环。如果你在主线程里写一个 `while(1)` 等待硬件数据，整个 UI 会冻结 —— 按钮点不了、窗口拖不动。

解决：把阻塞 I/O 放到一个**专用工作线程**。

### 4.2 QThread 的正确用法：moveToThread

```cpp
// src/MainWindow.cpp — spawnPipeline()
m_workerThread = new QThread(this);
m_pipeline = new DataPipeline(m_fifo);

// 关键：移动 pipeline 到工作线程
m_pipeline->moveToThread(m_workerThread);

// 工作线程启动时，自动调用 pipeline->run()
connect(m_workerThread, &QThread::started, m_pipeline, &DataPipeline::run);
```

**线程亲和性（Thread Affinity）**：每个 QObject 属于一个特定线程。`moveToThread()` 改变这个归属关系：

```
创建时                         moveToThread 后
┌─────────────┐              ┌─────────────┐
│ 主线程       │              │ 主线程       │
│  ┌─────────┐│              │             │
│  │pipeline ││     →        ├─────────────┤
│  └─────────┘│              │ 工作线程     │
│             │              │  ┌─────────┐│
└─────────────┘              │  │pipeline ││
                             │  └─────────┘│
                             └─────────────┘
```

之后：
- pipeline 的**槽函数**在工作线程执行
- pipeline 发射的**信号**由 Qt 根据接收者线程自动选择连接类型
- pipeline 里操作的**数据**在工作线程的栈/堆上

### 4.3 run() 是工作线程的"main"

```cpp
// src/DataPipeline.cpp
void DataPipeline::run()
{
    // 1. 初始化（在工作线程的上下文中）
    // 2. while (!m_stopRequested) {
    //        waitForData();      // 阻塞等待硬件
    //        processBlock();     // 处理数据
    //    }
    // 3. emit scanFinished();
}
```

这个函数**在工作线程里执行**，不是主线程。它不需要像 `app.exec()` 那样的事件循环 —— 它是纯粹的计算循环 + 阻塞 I/O。

---

## 5. 同步原语：mutex、atomic、lock_guard

当两个线程访问同一块内存，必须有同步机制。本项目用了两种。

### 5.1 std::atomic —— 无锁的开关变量

```cpp
// src/DataPipeline.h
std::atomic<int> m_stopRequested{0};
std::atomic<bool> m_continuous{false};
std::atomic<int> m_liveWindowOffset{200};
```

**原理**：`std::atomic<T>` 保证对它的读写是一个**不可分割的 CPU 操作**（通常是一条 `MOV` 指令 + 内存屏障）。不会出现"读到一半被另一个线程写入覆盖"的情况。

```cpp
// 写入（工作线程或主线程都可以）
m_stopRequested.store(1, std::memory_order_release);

// 读取（工作线程）
if (m_stopRequested.load(std::memory_order_acquire))
    break;
```

`memory_order_release` / `acquire` 保证：在 store 之前的所有内存写入，对 load 之后的所有内存读取**可见**。这是比锁更轻量的同步。

**何时用 atomic？** 单一变量、简单操作（加载/存储/加减）、不需要保护一段代码块。

### 5.2 std::mutex + std::lock_guard —— 保护代码块

```cpp
// src/DataPipeline.cpp — handoffMipFrame()
{
    std::lock_guard<std::mutex> lk(m_mipPoolMutex);
    if (m_freeMipSlots.empty()) {
        // ... 紧急降级处理
    } else {
        nextSlot = m_freeMipSlots.back();
        m_freeMipSlots.pop_back();
    }
}  // ← lk 析构，自动解锁。哪怕中间 return 或抛异常也会解锁
```

**核心惯用法**：`lock_guard` 在构造时加锁，析构时解锁。**绝不要手动 `mutex.lock()` / `unlock()`** —— 当心早期 return 或异常导致死锁。

### 5.3 两种同步方式的选择

```
操作粒度         选 atomic        选 mutex
────────────────────────────────────────────
单变量读写        ✅               ❌（杀鸡用牛刀）
复杂数据结构      ❌（做不到）      ✅
临界区很短        ✅               看情况
需要保护多行代码   ❌               ✅
```

### 5.4 死锁是怎么发生的

```cpp
// ❌ 经典死锁场景
// 线程 A: lock(m1) → lock(m2)
// 线程 B: lock(m2) → lock(m1)
// → A 等 m2，B 等 m1，谁都不放手
```

**规避原则**：
1. 锁的获取顺序在所有线程中保持一致
2. 临界区尽量短 —— 不要在持有锁时做耗时操作
3. 能用 `atomic` 就不用 `mutex`

---

## 6. RAII 与资源管理

**RAII** = Resource Acquisition Is Initialization，"资源获取即初始化"。名字起得不好，理解为"**资源生命周期绑定到对象生命周期**"就对了。

### 6.1 硬件卡的生命周期

```cpp
// src/SpectrumFifo.cpp
class SpectrumFifo {
    drv_handle m_hCard = nullptr;  // 句柄，类似于文件描述符
public:
    bool openCard(int index) {
        m_hCard = spcm_hOpen(drvName);  // 获取资源
        return m_hCard != nullptr;
    }
    ~SpectrumFifo() {
        if (m_hCard) spcm_vClose(m_hCard);  // 释放资源
    }
};
```

**RAII 保证**：无论 SpectrumFifo 对象以何种方式销毁（正常退出、异常、提前 return），`~SpectrumFifo()` 一定会被调用。

### 6.2 lock_guard 本身就是 RAII

```cpp
// lock_guard 的简化原理
template<typename Mutex>
class lock_guard {
    Mutex &m;
public:
    explicit lock_guard(Mutex &mutex) : m(mutex) { m.lock(); }   // 构造=获取
    ~lock_guard()                               { m.unlock(); }  // 析构=释放
};
```

### 6.3 对比：不用 RAII 的 C 风格代码有多危险

```c
// ❌ C 风格 —— 容易出事的代码
void dangerous() {
    drv_handle h = open_card();
    if (!h) return;                    // 忘关？没有——还没打开

    void *buf = alloc_page_aligned(64*1024*1024);
    if (!buf) { close_card(h); return; }  // 手动关！容易忘

    int err = start_acquisition(h);
    if (err) {
        free(buf);                     // 每个 return 都要手动释放
        close_card(h);                 // 三个 return，三处清理代码
        return;
    }

    // ... 使用资源 ...

    free(buf);                         // 正常路径的清理
    close_card(h);
}
// 如果中间抛异常？C 没有异常，但 C++ 有——清理全跳过
```

```cpp
// ✅ C++ RAII 风格
void safe() {
    SpectrumFifo fifo;
    if (!fifo.openCard(0)) return;  // fifo 析构自动 close

    auto buf = std::unique_ptr<void, decltype(&freePageAligned)>(
        allocPageAligned(64*1024*1024), freePageAligned);
    if (!buf) return;

    if (!fifo.startAcquisition()) return;  // 不管哪里 return，都自动清理

    // ... 使用资源 ...
}  // fifo.~SpectrumFifo() 自动调用 closeCard
   // buf 的 deleter 自动调用 freePageAligned
```

---

## 7. 多缓冲：从双缓冲到三缓冲

### 7.1 为什么需要缓冲交换？

数据采集有两个并发的操作：

```
生产者（DAQ 卡）:  [A-line 0] [A-line 1] [A-line 2] ... → DMA 缓冲区
消费者（Pipeline）: 读取 → 处理 → 积累到帧缓冲区

帧边界:
  生产者：还在持续输出下一帧的 A-line
  消费者：刚完成一帧，需要"切换"到新缓冲区，同时处理旧缓冲区
```

如果只有一个缓冲区：

```
  采集 ──→ [buf] ──→ 处理（持有锁）
              ↑
  下一帧第一个 A-line：往哪写？buf 正在被处理！
```

### 7.2 双缓冲（原始数据帧）

```cpp
// src/DataPipeline.h
FrameBuf m_buf[2];    // 两个缓冲区
int m_activeBuf = 0;  // 当前写入的是哪个
```

帧边界交换：

```cpp
// 1. 取出旧缓冲区的数据（swap 是 O(1) 指针交换）
auto frameData = std::make_shared<FrameBuf>();
frameData->rawAlines.swap(activeBuf().rawAlines);
frameData->alinePeaks.swap(activeBuf().alinePeaks);

// 2. 切换到另一个缓冲区
m_activeBuf = 1 - m_activeBuf;

// 3. 旧缓冲区的内容现在在 frameData 里，可以安全发给存储线程
emit frameDataReady(frameData, ...);
```

**关键**：`std::vector::swap()` 只交换三个指针（data, size, capacity），不拷贝元素。O(1) 时间。

### 7.3 为什么需要三缓冲而不是双缓冲？

双缓冲不够的场景：

```
时间线:
  Buf0: [accumulating frame N]  →  完成
  Buf1: [accumulating frame N+1] → 正在写
  Buf0: [async finalize]         → 正在处理（1-2ms）

如果 Buf1 在 Buf0 处理完之前就也完成了：
  → 需要第三个缓冲区！

帧率 10Hz = 100ms/帧，处理只需 1-2ms
→ 双缓冲够用，三缓冲是安全裕量
```

```cpp
// src/DataPipeline.cpp — 三缓冲 MIP 累加器
MipAccum m_mipPool[3];           // 三个槽位
std::vector<int> m_freeMipSlots;  // 空闲列表：{1, 2}（初始时 0 在使用中）
std::mutex m_mipPoolMutex;       // 保护空闲列表的互斥锁
int m_activeMipSlot = 0;         // 当前写入的是哪个槽

// 帧边界：取一个空闲槽位，原子切换
void handoffMipFrame() {
    int completedSlot = m_activeMipSlot;
    int nextSlot = pop from m_freeMipSlots;  // 从空闲列表取
    m_activeMipSlot = nextSlot;              // 切换（仅一条 int 赋值）
    emit mipFrameCompleted(completedSlot);   // 发信号异步处理
}

// 主线程处理完：归还槽位
void releaseMipSlot(int slotIdx) {
    zero_fill(slotIdx);                 // 清零
    push slotIdx to m_freeMipSlots;     // 归还空闲列表
}
```

**槽位状态机**：

```
  Active ──(帧完成)──→ Pending ──(主线程处理完)──→ Free ──(下一帧需要)──→ Active
```

**与双缓冲的本质区别**：双缓冲只有"正在写"和"正在处理"两个状态，如果处理耗时接近帧间隔就会冲突。三缓冲引入第三个"空闲待命"状态，完全解耦生产和消费的速度波动。

---

## 8. DMA 与页对齐内存

### 8.1 为什么 DMA 需要页对齐？

```cpp
// src/SpectrumFifo.cpp
void *SpectrumFifo::allocPageAligned(uint64_t size)
{
#ifdef Q_OS_WIN
    return VirtualAlloc(nullptr, (SIZE_T)size,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
#endif
}
```

**DMA（Direct Memory Access）**允许 PCIe 设备绕过 CPU 直接把数据写入内存。但 DMA 引擎操作的是**物理页**（典型 4KB），它不经过 CPU 的 MMU 翻译。如果缓冲区跨越了页表边界，DMA 可能写到错误的物理地址。

`VirtualAlloc` 保证返回的地址对齐到 64KB 边界（Windows 的分配粒度），且对应的物理页是连续的。

### 8.2 DMA 缓冲区通知机制

```cpp
// src/SpectrumFifo.cpp — allocateBuffer()
spcm_dwDefTransfer_i64(
    m_hCard,
    SPCM_BUF_DATA,       // 传输数据
    SPCM_DIR_CARDTOPC,   // 方向：卡 → PC 内存
    m_notifySize,        // 每积累这么多字节通知一次
    m_pData,             // 目标缓冲区
    0,                   // 卡内存偏移
    m_bufferSize);       // 总缓冲大小
```

环形 DMA 缓冲区的结构：

```
[████████░░░░░░░░░░░░░░]  卡写到位置 A
          ↑ pos = 卡正在写的地址
[░░░░░░░░████████████░░]  一段时间后
                    ↑ pos = 写指针前移

availUserBytes = (pos - lastReleasedPos) % bufferSize
软件读取 availUserBytes 字节后，releaseData(N) 告诉卡"这 N 字节可以覆盖了"
```

### 8.3 为什么不直接用 malloc/new？

`malloc` 返回的地址可能不对齐页边界。更致命的是：`malloc` 分配的是**虚拟地址**空间，对应的物理页可能不连续。DMA 需要连续的物理地址。

---

## 9. 设计模式实战

### 9.1 状态机模式 —— ScanController

```cpp
// src/ScanController.h
enum class ScanState { Idle, Armed, Scanning, Done, Error };
```

```
                    startScan()
     ┌─────┐        ───────────→        ┌───────┐
     │ Idle │                            │ Armed │
     └─────┘        ←───────────         └───┬───┘
        ↑           stopScan()              │ 第一个数据到达
        │                                    │ onFirstDataReceived()
        │           ┌──────────┐             ▼
        ├───────────│  Error   │←──────  ┌──────────┐
        │           └──────────┘  错误    │ Scanning │
        │                                └────┬─────┘
        │                                     │ 所有 A-line 处理完
        │           ┌──────┐                  │ onPipelineScanComplete()
        └───────────│ Done │←─────────────────┘
                    └──────┘
```

实现细节：

```cpp
void ScanController::setState(ScanState s)
{
    if (m_state == s) return;  // 防御：重复设置同一状态是 no-op
    m_state = s;
    emit stateChanged(s);      // 通知所有观察者（UI 按钮状态等）
}

void ScanController::startScan()
{
    if (m_state != ScanState::Idle && m_state != ScanState::Done)
        return;                // 防御：只在正确状态接受命令
    doArm();
}
```

**设计要点**：
1. **状态转移有守卫**：非法的状态请求被静默忽略，不崩溃
2. **状态变更广播**：`stateChanged` 信号让 UI、日志等自动同步
3. **每个状态对应明确的操作权限**：Armed/Scanning 时禁止改参数，Done 时允许重新开始

### 9.2 策略模式 —— 颜色映射

```cpp
// src/ColorMap.cpp
int mapToIndex(double value, double maxVal, int mapping, bool clipping) {
    double norm = value / maxVal;
    if (mapping == 1) {
        // 对数映射：压制亮区，提升暗区
        return log10(1.0 + norm * 99.0) / 2.0 * 255;
    } else {
        // 线性映射
        return norm * 255;
    }
}
```

运行时切换映射策略，不改变上层调用代码。如果将来要加新的映射（比如平方根、直方图均衡），只需在 `mapToIndex` 里加一个 case。

### 9.3 观察者模式 —— 信号/槽

```cpp
// ScanController 不知道谁在监听它，只知道"我有状态变化"
emit stateChanged(newState);

// MainWindow 注册监听
connect(m_scanCtrl, &ScanController::stateChanged,
        this, &MainWindow::onScanStateChanged);
```

**关键设计特性**：
- 被观察者（Subject）不持有观察者（Observer）的引用 —— 解耦
- 一个信号可以连接多个槽，一对多广播
- 槽的添加/移除不影响 Subject 的代码
- 断开连接时自动清理，不会悬垂指针

---

## 10. 数据结构与内存布局

### 10.1 为什么 MapInfo 是预计算的查找表

```cpp
// src/Types.h
struct MapInfo {
    int idx00, idx01, idx10, idx11;   // 4 个像素的线性索引
    float w00, w01, w10, w11;         // 双线性插值权重
};
```

每条 A-line 都有一个对应的物理坐标 `(x_m, y_m)`。要把它的信号值贡献到输出图像的像素网格上，需要**双线性插值**：找到最近 4 个像素，按距离分配权重。

这个计算如果在实时采集时做（浮点除法 + floor + clamp + 4 个索引计算），每条 A-line 多 ~50 条指令。331,000 条/秒 × 50 = 1650 万条指令/秒 —— 不是省不了，而是**完全可以在加载 CSV 时一次性算好**。

```cpp
// src/TrajectoryLoader.cpp — 预处理，只执行一次
void buildMapInfo(/*...*/) {
    for (int i = 0; i < N; ++i) {
        // 1. [-R, +R] 米 → [0, W-1] 像素
        float fx = (x_m[i] / R + 1.0f) * 0.5f * (W - 1);
        float fy = (y_m[i] / R + 1.0f) * 0.5f * (H - 1);
        // 2. 双线性插值：4 个角像素 + 权重
        int x0 = floor(fx); int x1 = min(x0+1, W-1);
        int y0 = floor(fy); int y1 = min(y0+1, H-1);
        float dx = fx - x0; float dy = fy - y0;
        out[i].w00 = (1-dx)*(1-dy);
        // ... 预先算好，运行时只查表
    }
}
```

**设计原则**：把能离线算的都离线算好。实时热路径上只做数组索引和乘加。

### 10.2 Struct vs Class

```cpp
// Types.h — 纯数据结构
struct ScanParams { int depth; int segmentSize; /* ... */ };
struct MapInfo   { int idx00; float w00; /* ... */ };
struct MipAccum  { std::vector<float> sum; std::vector<float> weight; };
```

C++ 里 `struct` 和 `class` 几乎一样（唯一的区别是默认访问权限：struct 默认 public，class 默认 private）。本项目里的约定：
- `struct` → 数据容器，没有行为（POD-like）
- `class` → 有行为、有状态机、有信号/槽（QObject 子类）

### 10.3 为什么 alinePeaks 是独立的 vector

```cpp
struct FrameBuf {
    std::vector<int16_t> rawAlines;   // [totalAlines * depth] 原始 ADC 数据
    std::vector<float>   alinePeaks;  // [totalAlines] 每条 A-line 的峰值
};
```

`rawAlines` 是存储用的（写入 NPY 体积文件），`alinePeaks` 是显示用的（MIP 只需要峰值）。分开存储的好处：
- MIP 计算不需要遍历 depth 维度 —— 直接读 `alinePeaks[i]`
- 内存虽然多了 `totalAlines * 4 bytes`（~132KB for 33124），但避免了对每条 A-line 重复求 peak

---

## 11. 实时系统的边界意识

### 11.1 什么是"实时"？

这个项目面临的是**软实时**要求：数据以 331 kHz 速率到达，处理线程必须跟上。偶尔落后会被 DMA 缓冲区吸收（64MB 缓冲可以容纳约 8000 条 A-line 的数据），但持续落后最终会导致缓冲区溢出（FIFO overrun）。

**不是硬实时**：没有生命危险，没有硬件损坏风险。过载的结果是数据丢失，不是系统崩溃。

### 11.2 热路径上绝对不能做的事

数据采集的**热路径**（每 A-line 执行一次，每秒 33 万次）：

```cpp
// processBlock() 的 for 循环体内 —— 每秒执行 33 万次
for (int a = 0; a < alinesInBlock; ++a) {
    // ✅ 安全操作
    float peak = 0.0f;                    // 栈变量，零开销
    acc.sum[idx] += p * w;                // 数组索引 + 乘加
    std::abs(x);                          // CPU 指令，几个周期

    // ❌ 危险操作（如果在热路径上）
    (void)malloc(1024);                   // 堆分配：可能触发系统调用
    std::lock_guard<std::mutex> lk(m);   // 锁：如果竞争就阻塞
    emit someSignal();                    // 跨线程信号：拷贝参数到队列
    std::cout << "debug\n";              // I/O：阻塞 1-100ms
}
```

本项目的热路径只做：DC 累加、包络检波、scatter 4 像素的乘加运算。没有锁、没有 I/O、没有堆分配。

### 11.3 把重活推出去

```
热路径（每 A-line, 3μs）:
  └─ DC 去除 + 包络 + scatter to MIP

帧边界（每 100ms, <1μs）:
  └─ 三缓冲槽位切换 + 发射信号

主线程（每 100ms, 1-2ms）:
  └─ MIP 归一化 + QImage 生成 + UI 更新

线程池（每 100ms, 50-200ms）:
  └─ 3D gridding + NPY/PNG 写入磁盘
```

**原则**：每层只能做自己频率允许的事。让采集线程空出 CPU 时间等着 `waitForData()`，比让它算 MIP 重要得多。

### 11.4 背压与降级

当生产速度快于消费速度：

```cpp
// 紧急降级：池耗尽时丢弃 MIP 帧，不丢弃原始数据
if (m_freeMipSlots.empty()) {
    qWarning() << "MIP pool exhausted — frame dropped";
    zero_fill(completedSlot);
    return;  // 不发射 mipFrameCompleted，跳过 MIP 更新
}
```

**设计权衡**：宁可丢一帧显示，也不阻塞采集。原始数据（`frameDataReady`）仍然写入磁盘。这叫做**优雅降级**。

### 11.5 一个反直觉的真相

在实时采集程序里，**阻塞在 `waitForData()` 上等数据来** 是好设计。这比轮询省 CPU。

阻塞在计算上——那是坏的。三缓冲改造的意义就在这里：把唯一不该阻塞的地方的阻塞消除了。

---

## 附录：快速参考

### 本项目使用的 C++ 标准库组件

| 组件 | 头文件 | 用途 |
|------|--------|------|
| `std::atomic<T>` | `<atomic>` | 无锁线程间标志传递 |
| `std::mutex` | `<mutex>` | 保护共享数据结构 |
| `std::lock_guard` | `<mutex>` | RAII 自动解锁 |
| `std::shared_ptr<T>` | `<memory>` | 共享所有权指针 |
| `std::vector<T>` | `<vector>` | 动态数组（替代 C 数组） |
| `std::fill` | `<algorithm>` | 批量填充内存 |
| `std::copy` | `<algorithm>` | 批量拷贝内存 |
| `std::abs` | `<cmath>` | 浮点绝对值 |
| `std::thread` | `<thread>` | 并行 gridding |

### 本项目使用的 Qt 组件

| 组件 | 头文件 | 用途 |
|------|--------|------|
| `QObject` | `<QObject>` | 所有 Qt 对象的基类 |
| `QThread` | `<QThread>` | 工作线程 |
| `QTimer` | `<QTimer>` | 定时器（A-line 轮询等） |
| `QImage` | `<QImage>` | 像素图像（MIP 渲染） |
| `QSettings` | `<QSettings>` | 持久化配置 |
| `QSerialPort` | `<QSerialPort>` | FPGA 串口通信 |
| `QThreadPool` | `<QThreadPool>` | 全局线程池（文件保存） |
| `QMetaObject::invokeMethod` | — | 跨线程方法调用 |
| `Q_ARG` | — | 跨线程调用时的参数包装 |

### 信号/槽连接速查表

```cpp
// 标准格式
connect(sender, &SenderClass::signalName,
        receiver, &ReceiverClass::slotName);

// Lambda 槽
connect(sender, &SenderClass::signalName,
        receiver, [](int param) { /* ... */ });

// 跨线程（Qt 自动处理）
// sender 在工作线程，receiver 在主线程 → 自动 QueuedConnection

// 手动指定（通常不需要）
connect(sender, &SenderClass::signalName,
        receiver, &ReceiverClass::slotName,
        Qt::DirectConnection);  // 强制同步
```

### 调试技巧

1. **`qInfo() << ...`**：日志输出到文件 `logs/PA_Microscope_*.txt`
2. **检查 MOC 错误**：看 `build/PA_Spiral_PAM_autogen/` 下生成的 `moc_*.cpp` 是否最新
3. **跨线程信号没反应**：检查接收者是否在正确的线程（`QObject::thread()`）
4. **卡驱动错误**：`SpectrumFifo::lastErrorText()` 返回驱动层的错误描述
