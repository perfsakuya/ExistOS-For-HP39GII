# 社区 Doom 移植与 SkyOS Game 扩展

本次调研阅读了 next-hack 的 nRF52840 移植和后续 MG24 移植的实际代码，
并对本地 Freedoom 0.13.0 的全部 36 张地图和四类怪物素材做了离线统计。
结论是可以继续扩展 Game 的动画、敌人逻辑和关卡；最适合移植的是数据组织和
资源读取方法。社区固件的帧率不能直接用于估算 39gII。新增内容尚未接入或刷写。

## 源码版本与复现

调研日期为 2026 年 10 月 3 日，SkyOS 基线为 `7a4ad2b`。
两个社区仓库检出在 `F:/workspace/39gii-reverse/Community-Doom-research/`，
仅取引擎、平台源码及 WAD 工具，没有安装厂商 SDK。

| 仓库 | 固定提交 | 本次重点 |
| --- | --- | --- |
| [nRF52840Doom](https://github.com/next-hack/nRF52840Doom/tree/779cad0ba1fae35523139e9af1f64bb3b14a9aa1) | `779cad0ba1fae35523139e9af1f64bb3b14a9aa1` | 对象压缩、只读地图、预处理贴图、QSPI 列读取 |
| [MG24 Doom BLE](https://github.com/next-hack/MG24_Doom_BLE/tree/fc02a55340d1d5b1a5155b12ac049436ef5d4f7a) | `fc02a55340d1d5b1a5155b12ac049436ef5d4f7a` | 更紧凑的对象、sector 分离、非内存映射资源读取 |

审计源文件为 `Freedoom-research/extracted/freedoom-0.13.0/freedoom1.wad`，
28,795,076 字节，SHA256 为
`7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d`。
它采用标准 Doom 记录格式，区别于项目内转换后的 E1M1 WAD。

```powershell
python tools/doom/audit_expansion.py ../Freedoom-research/extracted/freedoom-0.13.0/freedoom1.wad --output docs/doom-expansion-audit.json
python -m unittest discover -s tools/doom -p test_audit_expansion.py -v
```

完整结果在 [审计 JSON](doom-expansion-audit.json)。程序只读取数据，验证目录、
地图记录和索引边界，计算难度筛选、资源大小和明确标注的预算。
通过了全部 36 张地图的数据检查和五项针对非法数据、难度位与 sector 边界的检查。
这些结果不证明地图可通关或新增功能的实机帧率。

## 社区如何节省 RAM

### 分开保存活动对象与静态对象

nRF 的动态 `mobj_t` 为 92 字节，静态 `static_mobj_t` 为 44 字节；
MG24 为 52 与 20 字节。两版均在 `p_mobj.c` 用编译期大小检查约束布局。
MG24 从状态编号推导 sprite 和 frame，并把固定物品的位置保存在只读表中。
尺寸是上游布局的定义及约束，本次没有用其厂商 SDK 重编译整个固件。
见 [nRF 对象](https://github.com/next-hack/nRF52840Doom/blob/779cad0ba1fae35523139e9af1f64bb3b14a9aa1/NRF52840Doom/Doom/include/p_mobj.h#L269)
与 [MG24 对象](https://github.com/next-hack/MG24_Doom_BLE/blob/fc02a55340d1d5b1a5155b12ac049436ef5d4f7a/DoomMG24BLE/Doom/include/p_mobj.h#L371)。

我们无需给每个装饰物分配完整对象。保留物品表与收集位图，为会移动、受伤和攻击的
怪物建立紧凑对象池即可。现有 Game 的 292 字节敌人血量数组只是固定位置规则；
升级需要位置、状态编号、计时器、目标、血量等字段，不能继续仅增加血量数组。

### 缩短引用并移除重复字段

nRF 的 `getShortPtr/getLongPtr` 用四字节对齐，将 16 位编号恢复为
`0x20000000` 起的 SRAM 地址；另外有三字节区域指针。
这依赖固定地址范围和链接布局。
见 [i_memory.h](https://github.com/next-hack/nRF52840Doom/blob/779cad0ba1fae35523139e9af1f64bb3b14a9aa1/NRF52840Doom/Doom/include/i_memory.h#L48)。

SkyOS 虚拟 RAM 在 `0x02000000`，因此应使用对象池索引和受检查的表偏移，
而不是照搬地址位运算。先采用自然对齐的明确整数类型；ARM926 上不能为了节省
几个字节而引入未对齐访存。health 要升级为有符号 16 位，才能表示较大敌人和伤害。

### 地图常量与变化状态分离

nRF 把 lines、sides、nodes 等关卡常量放到内部 Flash 缓存，只在 RAM 保存
会变化的 linedata、sector 等内容。MG24 又拆出 `ramSector_t`，将高度、灯光、
活动对象引用等变化字段与只读 sector 数据分开。
见 [nRF 地图加载](https://github.com/next-hack/nRF52840Doom/blob/779cad0ba1fae35523139e9af1f64bb3b14a9aa1/NRF52840Doom/Doom/source/p_setup.c#L269)
和 [MG24 sector](https://github.com/next-hack/MG24_Doom_BLE/blob/fc02a55340d1d5b1a5155b12ac049436ef5d4f7a/DoomMG24BLE/Doom/include/r_defs.h#L117)。

我们的只读生成表已经采用这类思路。ARM 10.3 编译器测得 `sizeof(DoomLiteGame)`
为 368 字节，现有 grid / things / special lines / doors 的只读数据分别为
13,860 / 2,920 / 840 / 240 字节。它们不等于常驻 SRAM 占用：只读代码与表会分页。

扩展应提供 `GameMap` 描述表，保存每张地图的数量、起点、网格、边界、特殊线与资源集。
一次只创建当前关卡的变化状态，关卡切换复用对象池。不要在切关时模仿 nRF 将地图
再次写入内部 Flash；我们可以在电脑上预处理并打包只读数据。

## 社区如何降低渲染和资源读取开销

nRF 的转换器先把多 patch 组成的墙纹理合成为单 patch；还将列数据长度放入
column offset 的高八位，偏移占低 24 位。运行时无需重新拼纹理或逐 post 扫描
才能得知读取长度。渲染器缓存列索引，并使用两个小列缓冲，在画上一列时通过
QSPI DMA 读取下一列。大面积使用相同亮度表时缓存该表，而非保留整套解码纹理。
见 [转换器](https://github.com/next-hack/nRF52840Doom/blob/779cad0ba1fae35523139e9af1f64bb3b14a9aa1/MCUDoomWadUtil/wadprocessor.c#L509)
和 [渲染读取路径](https://github.com/next-hack/nRF52840Doom/blob/779cad0ba1fae35523139e9af1f64bb3b14a9aa1/NRF52840Doom/Doom/source/r_fast_stuff.c#L1004)。

这也修正了“QSPI 可以直接访问，所以无需资源优化”的理解：该项目恰恰对随机小读取
做了专门优化。MG24 用 `extMemStartAsynchDataRead` 等接口支持非内存映射闪存，
绘制代码仍按列批量读取。其驱动把逐字节/短整数读取标记为未优化，不应在热路径使用。
见 [extMemory.h](https://github.com/next-hack/MG24_Doom_BLE/blob/fc02a55340d1d5b1a5155b12ac049436ef5d4f7a/DoomMG24BLE/src/extMemory.h)
和 [双列缓冲](https://github.com/next-hack/MG24_Doom_BLE/blob/fc02a55340d1d5b1a5155b12ac049436ef5d4f7a/DoomMG24BLE/Doom/source/r_fast_stuff.c#L1370)。

39gII 的 `OSLoader/Config/SystemConfig.h` 当前源码配置为 1 KiB 页、128 KiB VROM
缓存、40 KiB VRAM 缓存和 92 KiB ZRAM 池；整个物理 RAM 为 512 KiB。
VROM 缺页通过 `MTD_ReadPhyPage` 从 NAND 载入，VRAM 有压缩及可选交换。
这些资源由系统与应用共同使用。社区 256 KiB RAM 的独立固件不是本机游戏的可用预算，
虚拟 heap 剩余也不能当作可常驻的物理 RAM。

适合我们的做法是离线裁剪和灰度转换、连续存储当前帧/列、对齐资源块、小而有上限的
缓存，以及同一列放大后重复使用。优先减少分页；异步 NAND 预取需要新的 OSLoader
接口和同步验证，不能直接复制 QSPI 寄存器或 MG24 DMA 调用。
M4 上的汇编与 DSP 指令也需要为 ARM926 重写。

## 动画资源的实际大小

用本地 Freedoom 的 POSS、SPOS、TROO、SARG 四个家族统计。
正面加无方向素材共 77 个 patch，所有方向的唯一 patch 共 218 个，镜像共用素材不重复计算。
这里包含出现的普通及过度死亡帧，MVP 可以继续筛选。

| 编码范围 | 原始尺寸的 4 位像素数据 | 等比例限制到 32×48 的 4 位像素数据 |
| --- | --- | --- |
| 四类怪物的正面与无方向帧 | 82,825 B，约 80.9 KiB | 39,912 B，约 39.0 KiB |
| 四类怪物的全部方向素材 | 253,672 B，约 247.7 KiB | 132,432 B，约 129.3 KiB |

以上是静态资源的像素数据，不包含帧描述表，也不是新增 RAM 实测值。
透明列段的另一种候选编码，计入列偏移、段头及终止符后，缩小素材分别需要
37,672 B 和 111,818 B；部分帧的收益很小，不能假定压缩总能显著提速。
当前 4 位矩形读取路径已稳定，应先沿用它并测冷帧开销，再决定是否更换编码。

动画无需同时解码所有帧。采用只读 `state -> frame, tics, action, nextstate` 表，
每个怪物保存状态编号和剩余 tic，渲染器只读取该帧即可。
社区本身采用 `const state_t states[]`，行走、攻击、受伤和死亡都由状态表驱动。
见 [状态表](https://github.com/next-hack/MG24_Doom_BLE/blob/fc02a55340d1d5b1a5155b12ac049436ef5d4f7a/DoomMG24BLE/Doom/source/info.c#L109)。

建议先实现四类怪物的正面行走、攻击、受伤和死亡，保留尸体并防止重复击杀。
小型状态机需限制零 tic 状态连续跳转次数，保留负 tic 终止语义，并让伤害动作只执行一次。
以后增加侧面/背面素材、武器切换和投射物。渲染朝向选择不必改变世界模拟频率。

## 多关卡的实际障碍

第一章九关的全部原始地图 lumps 共 2,190,770 字节，不包括共用美术、音乐等。
按当前网格边界公式，九关的单字节网格合计 179,175 字节；未来若改为一位占用，
其像素位图为 22,398 字节。但精确边界、门和 sector 高度数据仍要另外保存，
不能把这个数当作完整地图包大小。

| 地图 | 中等难度单人怪物 | sector 数 | 当前编号是否够用 | 新增敌人种类 |
| --- | --- | --- | --- | --- |
| E1M1 | 29 | 182 | 是 | 无 |
| E1M2 | 93 | 380 | 否 | spectre |
| E1M3 | 97 | 330 | 否 | spectre |
| E1M4 | 124 | 274 | 否 | spectre |
| E1M5 | 119 | 214 | 是 | spectre |
| E1M6 | 194 | 395 | 否 | spectre、lost soul |
| E1M7 | 209 | 699 | 否 | spectre、lost soul |
| E1M8 | 3 | 97 | 是 | baron |
| E1M9 | 106 | 297 | 否 | spectre |

全部 36 关中，27 关超过当前 sector 编号范围，6 关超过当前网格八位尺寸范围。
这些是输入数据确认的代码边界。还需处理目前八位 tag 和门/边界引用的容量，
移除生成器中的固定数量、起点、钥匙位置、出口和 tag 映射。

E1M2 已含蓝、红、黄钥匙及多种 E1M1 以外特殊线；后续关卡还有平台、楼梯、
传送、伤害地面及 boss 触发。即使 E1M1 已有的 special 23/62/88，也没有在原生
MVP 中实现完整升降语义。因此资源能装下，不代表这些关卡可以按 Doom 规则通关。

一个试算：每怪物 32 B、每 sector 8 B、物品/已见线位图以及 64 个 16 B 投射物槽，
E1M2 的变化状态约 7,372 B；36 关中最大值为 E4M7 的 24,040 B。
这是未来简化模型的预算，不是完整 Doom 引擎占用，排除了系统、帧缓冲、栈、
对象池开销、导航及资源缓存；不保证 32 B 可以保留所有原版对象行为。

完整 IWAD 约 27.5 MiB，超过现有 14 MiB System VROM；36 关原始地图 lumps
就约 10.1 MiB。应按章节/关卡筛选、预处理、共享资源，而不是原样嵌入整个 IWAD。
第一章作为逐关扩展目标在存储规模上合理，是否达到 10 FPS 仍需逐关实测。

## 在现有项目中的实施顺序

| 阶段 | 涉及文件 | 验收内容 |
| --- | --- | --- |
| E1M1 怪物动画与移动 | `DoomLiteGame.c/h`、`DoomLite.c`、sprite 生成器 | 紧凑 actor 池；状态表；唤醒、追踪、墙体阻挡、攻击、受伤、死亡；不隔墙伤害，不重复计杀 |
| 通用地图接口和 E1M2 | grid/game/portal 生成器、GameMap 描述表、地图 HUD | 16 位编号；实际 player start；三色钥匙；关卡切换与状态重置；按真实 special/tag 选择触发对象 |
| sector 动态逻辑 | 精确边界与高度模型、`DoomLiteGame.c`、场景射线 | 门有开合过程；平台/楼梯；碰撞高度与可视高度一致；完成 E1M2 路线 |
| 第一章扩展 | 资源清单、关卡描述表、更多 actor/触发器 | 分别处理 spectre、lost soul、baron 及 boss/秘密出口；逐关通关并重复退出 |
| 更接近原版引擎 | `doom_port` 与新的读取后端 | 在独立试验目标验证小工作集 BSP/sector 模拟，再评估与原生渲染的整合 |

动画和追踪可以先在已经稳定的 E1M1 原生 Game 中加入，不需要先恢复旧完整引擎。
在 E1M2 以前完成通用编号和地图接口，避免继续堆 E1M1 特例。
更完整的楼梯、跨高度攻击和原版行为不能仅靠增加帧数，需要 sector 模型与规则移植。

## 耗时记录与性能验证

基线串口文件 `freedoom-game-classic-ui-hardware.log` 已记录三次运行，
平均 24.16 / 24.59 / 24.64 FPS，最大帧间隔 123 ms。
第一批 scene / weapon / HUD / LCD 平均为 14.908 / 0.100 / 6.043 / 12.972 ms。
Game 运行期间全系统 heap 样本为已分配 69,320 B、总 282,624 B；这是虚拟 heap
统计，不是游戏独占物理 RAM，也不是内存压力余量。以上沿用本地已保存实机记录，
本次调研没有新刷写或新增性能测量。

新增阶段必须保留现有微秒级 `DOOMG_PERF` 总耗时与最大值，并补充：

- actor AI、状态转换、投射物、视线/碰撞查询的 total/max 耗时与执行数量。
- 资源读取/解码的 total/max、请求字节、缓存命中/淘汰；区分冷帧和已加载帧。
- 当前 actor 数、可见 sprite 数、sector mover 数、对象池高水位与溢出次数。
- 地图转换/载入、切关、退出各阶段耗时；VROM/VRAM 缺页增量如底层接口可用。

延续当前每 32 帧聚合一次的日志方式，避免逐帧打印干扰调度和串口带宽。
底层缺页次数即使记录到，也只能说明分页活动，不能代替读取耗时。
性能验证应覆盖怪物密集场景、冷资源、连续转向、移动、门、地图与射击退出；
报告平均 FPS、帧间隔分布/峰值及 dropped ticks。
当前结果为新增功能提供了平均帧率空间，但尚不能承诺每帧都小于 100 ms。

采用社区代码时保留相应 GPL 来源与修改记录；美术继续使用已有 Freedoom 素材及其
COPYING/CREDITS。本次新增审计工具没有复制社区引擎实现。
