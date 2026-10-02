# HP 39gII 实机验证（2026-10-02）

测试固件为启用串口计时日志的 SkyOS System 构建，`ExistOS.sys` 长度 5,285,548 字节，SHA-256 为 `E519D0FCDC080210910CAFF491DA84A50EA74C7AF3C589F6360F55B9743A7DD8`。使用仓库自带 Windows EDB 将**主系统**写入第 1984 页；未改写 OSLoader。EDB 对传输块报告匹配的校验值并以退出码 0 结束，设备重新枚举为 `VID_CAFE:PID_4003`。EDB 未执行 NAND 逐页读回校验。

用户在本机打开 `Vapor Test`，确认上方向键使屏幕显示 `Count: 1`、Enter 使其恢复 `Count: 0`，F6 正常返回应用列表。串口记录显示按键触发的脏行掩码为十进制 `40`（第 3、5 行）。

串口取证使用 USB CDC `COM6`、9600 波特率（System 日志通道）；保留的原始片段见 [HARDWARE_LOG.txt](HARDWARE_LOG.txt)。第一轮已捕获：

| 指标 | 真机结果 | 解释 |
| --- | ---: | --- |
| System 内存分配，进入前 | 52 / 276 KiB | 系统日志以 KiB 截断显示 |
| System 内存分配，应用运行时 | 62 / 276 KiB | 应用自身日志给出 64,328 字节 |
| System 内存分配，退出后 | 52 / 276 KiB | 任务释放后回到原显示值 |
| `VaporDemo` 栈最低剩余 | 1,863 个 32 位字 | 任务栈总共 2,048 字；观测到约 740 字节被使用 |
| 一次按键的状态更新与像素绘制 | 171–173 µs | 两个 256×8 像素行，共 4,096 像素 |
| 两个显示请求入队 | 1,572–1,578 µs | 包含底层 API 调用；**不等于 LCD 完成显示的时间** |
| System 空闲任务 CPU 占比 | 97%（应用打开时一次状态快照） | 不能推导持续动画帧率 |

Loader 另报告 CPU 频率约 392 MHz、HCLK 约 196 MHz。该频率高于 [NXP 对 STMP3770 标称的 360/180 MHz](https://www.nxp.com/docs/en/supporting-information/STMPFAMCOMPTBL.pdf)，是现有固件运行状态的读数，本次未修改时钟配置。Loader 的“Free PhyMem 7,436 Bytes”属于其物理页池，不是 System 应用堆；System 日志报告的已分配内存见上表。

本次串口监听开始时，应用已打开；后续监听也没有捕获应用重启。因此未取得 `SKYVAPOR_TASK` / `SKYVAPOR_BOOT` 行，启动耗时和自动 64 次状态更新基准仍待复测。没有测 LCD 实际完成刷新时间或长时间运行稳定性。上游 Todo 仅通过交叉编译，尚未在此硬件上运行。

当前 8 KiB 任务栈的最低剩余为 1,863 字，观察到的使用量约 185 字。改成 1,024 字任务栈有节省 4 KiB 的潜力，但需要另一次刷入和相同工作负载的栈余量复测，尚未实施。276 KiB 是 System 报告的总可分配量；不能把它与已分配量相减后当作已验证的单块空闲内存。

恢复用的已知原始 System 镜像保存在 `F:\workspace\39gii-reverse\hp-39-gii\outputs\skyos-build125\ExistOS.sys`，SHA-256 为 `688B36C936957915B48C6368D3C8D9D7D60FBD53D2B336FDB382176EED53CBFC`；当前测试镜像保存在同一 `outputs` 下的 `pocket-vapor-device-test-2026-10-02` 目录。
