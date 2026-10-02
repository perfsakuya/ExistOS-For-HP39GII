# SkyOS / ExistOS 代码调研（2026-10-02）

基线：`perfsakuya/ExistOS-For-HP39GII` 的 `main`，提交 `af6233575451e3a6420320223734e35674069d54`。以下结论来自该提交的源码、CMake 和构建工作流；“可编译”不等于已在本机编译或实机验证。

## 当前实现

| 层 | 当前构建入口 | 已找到的实现 |
| --- | --- | --- |
| 启动器 `OSLoader` | `OSLoader/CMakeLists.txt`、`OSLoader/start.c` | ARM926EJ-S 启动、显示与键盘、FreeRTOS 任务、NAND/FTL、虚拟内存、USB CDC/MSC、固件维护与启动 System。开机位图由 `OSLoader/Include/logo.h` 提供，在 `OSLoader/start.c:167` 绘制。 |
| 系统 `System` | `System/CMakeLists.txt`、`System/core/main.c` | FreeRTOS 系统任务、图形 UI、FATFS 文件系统、配置文件、崩溃日志、KhiCAS 入口。UI 当前有应用、控制台、文件管理器和设置页（`System/graphics/UICore.cpp`）。 |
| 计算功能 | `System/applications/user/khicas` 和 `Libs/*.libcpp` | KhiCAS 包装代码参与构建；CAS、GUI、GMP 等核心代码通过仓库中的预编译库链接。修改这些库内功能需要另行重建相应库。 |
| 开发与发布 | `toolchain.cmake`、`.github/workflows/build.yml` | 使用 `arm-none-eabi-gcc` 交叉编译，生成 `OSLoader.sb` 与 `ExistOS.sys`，CI 在 `main` 推送时自动构建并发布。上游文档说明 GCC 10.3 已验证，其它版本可能导致 Loader 无法运行。 |

仓库中存在 LVGL、MicroPython、Game Boy、Emu48、littlefs 源码，但 `System/CMakeLists.txt` 对应目录或库的构建入口目前被注释掉。不能仅凭源码存在或 README 中的勾选，把它们算作当前固件已启用的功能。中文 README 的功能列表可作为项目历史目标，实际发布功能仍需以本次构建和实机测试核对。

SkyOS 的 50×25 衬线体开机位图及生成脚本已放入 `OSLoader/Include/logo.h` 和 `OSLoader/branding/`。用户已在实机看到开机画面；系统 UI 的 `System/graphics/ExistOSlogo.h`、控制台文案及设置页仍沿用 ExistOS 品牌。

## 优先改进项

### P0：刷写协议会把擦写失败报告为成功

`OSLoader/start.c:437-479` 的 `ERASEB`、`PROGP` 命令没有检查 `sscanf` 结果、页/块范围或 NAND 函数返回值，随后固定回复 `EROK` / `PGOK`。`OSLoader/HAL/mtd_up.c:364-445` 和 `:522-558` 明确返回操作结果，工作任务在 `:218-225` 把底层状态传回调用者。`PROGP` 还假定此前已执行 `RESETDBUF`，否则 `binBuf` 可为空。

建议先定义协议错误回复，校验命令格式和范围，遇到第一个失败页立即停止并返回具体页号/状态；主机端随后做读取校验。完成前，不应把 `PGOK` 当成写入成功的充分证据。

### P0：NAND 超时重试不能按预期退出

`OSLoader/HAL/mtd_up.c:75-76,144-161` 把 `retry_cnt` 设为 5，但超时分支的 `goto retry` 位于 `retry_cnt--` 前面，减计数永远执行不到。持续超时将反复重试，无法走到既有的错误返回分支。

建议把重试次数的更新放在跳转前，限制每次操作的总等待时间，并在耗尽次数时把明确的错误返回给调用者。用模拟底层超时的测试验证退出路径。

### P1：带元数据写页使用未初始化的标志

`OSLoader/HAL/mtd_up.c:522-539` 创建局部 `MTD_Operates newOpa`，仅在缓冲区未对齐时设置 `needToMoveData=true`；对齐时该字段未初始化。工作任务在 `:119-130` 根据该字段选择直接写或先复制到页缓冲区。建议初始化整个操作结构，并显式赋值该字段。

### P1：文件管理器的路径和文件名缓冲区有越界风险

`System/graphics/UICore.cpp:714-718` 为目录名分配 `strlen(name)+1` 字节，却又追加 `/`，缺少结束符空间。目录项缓冲区在 `:572-576`、`:1056-1062` 固定分配 255 字节；FATFS 配置允许 255 字符长文件名（`System/filesystem/fatfs/ffconf.h:116-117`），还需要一个结束符。路径拼接在 `:525,1067-1077` 使用无长度参数的 `strcat`。

建议统一为带容量的路径辅助函数，分配时包含分隔符和结束符，检查 FATFS 返回的名称长度及所有分配失败路径。先用最大长度名称与多层目录做边界测试。

### P2：配置文件在正常启动时被无谓重写

`System/core/SystemConfig.c:50-55` 在文件系统初始化前尝试读取配置；`System/graphics/UICore.cpp:1104-1111` 挂载文件系统后又读取一次。随后 `UI_SetLang()`（`:165-167`）即使语言未变也调用 `config_set_language()`，而该函数在 `System/core/SystemConfig.c:181-185` 每次都会保存配置。这会在启动时产生额外的 FATFS/NAND 写入。

建议挂载后只加载一次，值变化时才标记 dirty 并保存；再考虑断电时配置文件的安全更新方式。

### P2：开发分支缺少有效的 CI 产物

`.github/workflows/build.yml` 只在 `main` 推送时触发，非 `main` 分支的上传工件步骤因此不会运行。工作流还使用未固定版本的自动发布 Action。建议增加 PR/开发分支构建、保留构建工件，并固定关键依赖版本；`main` 发布与开发验证分离。

## 可扩展的功能入口

- 应用页已改为 `System/applications/AppRegistry.cpp` 中的注册表，统一绘制图标和启动入口。当前注册 KhiCAS 与 SkyOS Notes；后续应用可以在此扩展。
- 文件管理器进入普通文件的分支仍是占位注释（`System/graphics/UICore.cpp:723-733`）。可以先做文本/图片预览，再接入文件关联。
- 如果希望全系统使用 SkyOS 品牌，需另外修改 System UI 位图、控制台与设置页文案；当前实机验证只覆盖 Loader 的开机画面。

## 本机开发状态与下一步

- fork 已克隆到 `F:\workspace\39gii-reverse\ExistOS-For-HP39GII`，`origin` 使用 HTTPS。GitHub CLI 已登录 `perfsakuya`，并已成功推送 `skyos/boot-branding` 分支。原先的 SSH 公钥仍未获 GitHub 接受；当前开发使用已验证的 HTTPS 凭据。
- 按需检出已展开 `Libs`、`fonts`、`tools`、`OSLoader`、`System` 和 `Script`。官方 GNU Arm Embedded Toolchain 10.3-2021.10 安装在 `F:\workspace\39gii-reverse\.toolchains\gcc-arm-none-eabi-10.3-2021.10`，其 `bin` 已加入用户 PATH。下载的官方 ZIP 为 200,578,763 字节，SHA-256 为 `D287439B3090843F3F4E29C7C41F81D958A5323AECEFCF705C203BFD8AE3F2E7`；解压后的文件合计 729,974,068 字节。
- Windows 编译命令：`cmake -S . -B build -G Ninja`，然后 `cmake --build build --parallel 6`。2026-10-02 在本机成功生成 `build/OSLoader/OSLoader.sb`（93,264 字节，SHA-256 `8208C47FE7CDE6248EB44C592CE7F19011FD600528A5CF4B2DA2359FA872BCE4`）和 `build/System/ExistOS.sys`（5,274,964 字节，SHA-256 `E0133BE02B66159914488C3366F6D2463516B40963111EF72C67EA1411CF383C`）。两个 ELF 均为 ARM little-endian ELF32，Loader 的 `rom.bin` 中找到一份完整的 1,250 字节 SkyOS 开机位图。
- 首次 System 链接暴露了字体脚本相对路径错误。已在 `Script/sys_ld.script` 中按文件名 `INCLUDE`，并在 `System/CMakeLists.txt` 为链接器指定仓库 `fonts` 目录；这样构建目录位置不影响查找。重新配置与编译均成功。
- 上述阶段仅编译，未向设备写入新固件。后续的 Notes 功能验证见下节；刷写工具仍缺少 NAND 读回校验，不能仅凭 `PGOK` 判定实机功能正常。

## SkyOS Notes 开发记录（2026-10-02）

- 在本地分支 `skyos/notes-app` 增加应用注册表与短便笺应用。应用支持键盘输入、光标移动、保存、退出后重新读取，使用内部 FAT 存储中的 `SKYNOTE.TXT` 与 `SKYNOTE.BAK`。超长或非 ASCII 文件只读显示，避免覆盖外部编辑的数据。
- GNU Arm Embedded Toolchain 10.3-2021.10 构建通过。当时的第二候选系统镜像为 5,281,332 字节，SHA-256 为 `86923137FAC77F00D2B09F783D58A5D2D7E25BAE3DCBA27053F09607F25B4E0B`，未刷入。OSLoader 没有改动。
- 曾将第一候选系统镜像（5,281,292 字节，SHA-256 `9CB072C4DA7D4550110A64C6376A162F1D6DBF2F15DB946ADE355B8766C1D6D2`）通过 EDB 写入系统起始页 1984；工具返回 0，设备重新枚举为 ExistOS USB 复合设备。实机可进入 Notes，但用户报告输入文字并保存后，退出时出现 System Panic。

## Notes 退出崩溃排查（2026-10-02）

- 用户照片显示 Loader 的 `System Panic!`，`FAR=0000000c`，寄存器画面中 `R15=004b30c4`。该地址落在本次构建的 newlib `_free_r` 内；Loader 在 `OSLoader/start.c` 显示的是 `pSysTask` 保存的寄存器，未必是发生访问异常的任务现场。因此目前能确认有非法内存访问及释放路径活动，不能仅凭照片断定具体是哪一次 `free` 损坏了堆。
- Notes 原来在启动时释放主 UI 的 32,512 字节缓冲区，另行分配自己的缓冲区；退出时释放自己的缓冲区，再恢复主 UI 的缓冲区。Loader 的显示调用会异步排队读取传入的缓冲区，这样的生命周期存在悬空引用风险。修复候选改为暂停主 UI 任务并借用其现有缓冲区，退出后直接恢复 UI 任务，同时把 Notes 任务栈从 1,024 个 word 增至 2,048 个 word。
- 新候选 `build/System/ExistOS.sys` 为 5,281,316 字节，SHA-256 `3040BB49FB2F2D815C35360709E1AE89437AFF109FBB4D4B655402D1096DFFC6`。GNU Arm 10.3 + Ninja 本机构建成功；EDB 对系统起始页 1984 的写入返回 0，USB 恢复接口在刷写前在线。仍需用户在实机重复输入、F2 保存、F6 退出、再进入 Notes 的流程。EDB 尚无 NAND 读回校验。
- 另发现 Loader `OSLoader/drivers/display/display_up.c` 的 `DisplayFlushArea(..., false)` 原来仍把指向局部 `fin` 的指针放入异步队列，显示任务稍后写回该指针。源码已改为仅阻塞调用传入完成指针，且 Loader 单独构建成功；**此 Loader 修复尚未刷入设备**，本次实机测试仍使用原 Loader。
