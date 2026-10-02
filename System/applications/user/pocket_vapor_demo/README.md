# Pocket Vapor / SkyOS 可行性验证

此目录是一个可编译的概念验证。它把受限的 Vue 风格 TypeScript/TSX 在电脑上提前编译为 C，再作为 `Vapor Test` 应用链接进 SkyOS。计算器上没有 JavaScript 引擎。上游 [Pocket Vapor](https://github.com/pocket-nexus/pocket-vapor) 是独立的早期实验，并非 PocketJS 主线或任意 TypeScript 应用的兼容运行时。本次验证固定在上游提交 `99e8e324192a285be6cc4443042dbb9cbd9426de`。

## 已做的验证

在主机上，`host_smoke.c` 使用**实际生成的 C 和屏幕适配层**，检查按键使 `ref` 改变、`computed` 更新、只重绘两个受影响的行，以及重复复位时不重绘。生成的 256×127 灰度画面已检查。使用 GNU Arm Embedded Toolchain 10.3-2021.10 编译完整 `ExistOS.sys` 成功。尚未将此构建刷入计算器；真机画面、按键、退出流程与响应时间仍待测。

本例占用（ARM 对象文件的 `size`，单位为字节）：

| 对象 | text | data | bss |
| --- | ---: | ---: | ---: |
| `gen_app.c` | 1,376 | 0 | 16 |
| `VaporCore.c` | 1,136 | 0 | 38 |
| `VaporDisplay.c` | 676 | 0 | 970 |
| `VaporDemo.c` | 514 | 0 | 0 |
| **合计** | **3,702** | **0** | **1,024** |

`ExistOS.sys` 为 5,285,044 字节，相比加入本例前的 5,281,316 字节增加 3,728 字节。`sys_ld.script` 给 System 分配 200 KiB RAM 和 6 MiB ROM；当前镜像距 6 MiB 上限还有 1,006,412 字节。本应用另开 FreeRTOS 任务，栈深 2,048 个 32 位字，即 8,192 字节，加上任务控制块等少量动态分配；显示时借用 System UI 已存在的 32,512 字节帧缓冲，不再新分配一份。**新增运行内存约 9 KiB 起**，不是整机剩余内存或实测峰值。

GBA 的 288 KiB 是 [Nintendo 所列的 32 KiB 内部工作内存加 256 KiB 外部 WRAM](https://www.nintendo.com/de-at/Hardware/Unternehmensgeschichte/Game-Boy-Advance/Game-Boy-Advance-627139.html)，并非 Pocket Vapor 的最低占用；另有 96 KiB 显存。SkyOS 的 200 KiB System 链接区域小于 GBA 工作内存总量，但本例实际新增内存远小于该区域。是否有足够**空闲**堆仍须以真机测量为准。

上游 Todo 示例用相同的 32×15 目标编译后，编译器规划的状态为 940 字节，覆盖列表、筛选、光标、编辑文本和派生视图；生成的 ARM 对象是 `text=4,163, bss=1,048` 字节。此数据仅证明编译与静态规模，Todo 尚未集成到 SkyOS 或在真机运行。

## 复现

将上游仓库克隆到此仓库的同级目录 `pocket-vapor`，检出上述提交并执行 `bun install --frozen-lockfile`。安装 Bun、GNU Arm Embedded Toolchain 10.3-2021.10 和项目原有的 CMake/Ninja 依赖。PowerShell 示例：

```powershell
git clone --branch experiment/standalone https://github.com/pocket-nexus/pocket-vapor.git ..\pocket-vapor
git -C ..\pocket-vapor checkout 99e8e324192a285be6cc4443042dbb9cbd9426de
Push-Location ..\pocket-vapor; bun install --frozen-lockfile; Pop-Location
bun tools/generate_pocket_vapor_demo.ts
cmake --build build --target ExistOS.sys -j 6
```

`gen_app.c`、`profile.h`、`memory_plan.txt` 是生成产物；普通 SkyOS 构建只需要仓库中提交的这些文件，不需要 Bun 或上游源码。`runtime/vapor.h` 和 `runtime/vapor_core.c` 来自上游，许可证见 `runtime/LICENSE`。编译器当前复用上游 ESP32 的 1 位字形及 RGB565 样式输出，并改为 32×15 个 8×8 字符格；`VaporDisplay.c` 将样式转换到计算器的 8 位灰度帧缓冲。

若有本机 C 编译器，可独立检查生成代码与灰度适配层：

```powershell
gcc -std=c11 -O2 -I System/applications/user/pocket_vapor_demo/runtime -I System/applications/user/pocket_vapor_demo System/applications/user/pocket_vapor_demo/gen_app.c System/applications/user/pocket_vapor_demo/VaporCore.c System/applications/user/pocket_vapor_demo/VaporDisplay.c System/applications/user/pocket_vapor_demo/host_smoke.c -o build/vapor_host_smoke.exe
./build/vapor_host_smoke.exe build/vapor-demo.pgm
```

## 可保留的功能边界

已通过本例验证：静态 TSX 字符行布局、少量样式到灰度的映射、`ref`/`computed`、实体按键事件、依赖驱动的局部重绘。上游 Todo 的列表、过滤、固定容量字符串池和编辑逻辑**通过编译与内存规划**，仍需移植按键映射与真机验证。文件持久化、图片、中文字体、连续动画需要分别接入 SkyOS 原生服务并测量资源使用。

这个 AOT 路线不提供运行时加载任意 JS/TS、DOM、浏览器 API 或 npm 包的能力。现有适配层仅支持 ASCII 字符格和一个页面，且没有真机帧率数据。按键扫描间隔是 20 ms；改变两个字符行会写入 4,096 个帧缓冲像素并发出两次整行显示更新，实际显示延迟取决于驱动和设备。
