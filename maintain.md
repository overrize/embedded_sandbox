# MDL 项目维护与协作手册

> **本文件是 MDL 项目唯一的协作约定文档**（agent 与同事共用）。
> 所有 agent 在 `mdl/` 目录工作时，先读本文件。
>
> 维护规则：
> - 改设计 → 更新对应章节，并同步 bump 章节版本标记；
> - 完成工作 → 在 **§9 工作日志** 追加条目（新条目在最上面）；
> - 认领任务 → 在 **§8 任务看板** 写上你的名字/agent id 和状态；
> - §10 是同事专属工作区，自由使用，他人勿动。

---

## 目录

1. [一句话目标](#1-一句话目标)
2. [核心设计决策](#2-核心设计决策已定勿违背)
3. [当前状态](#3-当前状态)
4. [MCP 接口定义](#4-mcp-接口定义草案-v01)
5. [编译期资源隔离](#5-编译期资源隔离规格)
6. [目录地图](#6-目录地图)
7. [关键契约文件](#7-关键契约文件)
8. [任务看板](#8-任务看板)
9. [工作日志](#9-工作日志)
10. [同事工作区](#10-同事工作区)

---

## 1. 一句话目标

让 AI agent 通过 **MCP（Model Context Protocol）** 对 AT32F435（Cortex-M4）设备
**无烧录、无整机复位**地完成设备功能的替换与移除。

模块代码在 PC 侧编译为 PIC 的 `.mdl` 包，经 USB CDC 下发，
以非特权 FreeRTOS-MPU 任务运行在 SRAM arena 中。

---

## 2. 核心设计决策（已定，勿违背）

### D1 — 单槽互斥（v1 铁律）

- 设备同一时刻**最多一个模块**在跑，`g_mdl_slot` 唯一。
- "换功能" = UNLOAD（删任务、整池回收内存）→ LOAD（搬代码、建任务）。
  旧模块任务必须已删除、槽位回到 `EMPTY` 后新模块才能加载。
- **运行时永远不存在两个模块并存，因此不存在运行时资源竞争。**
- 固件侧 LOAD 遇槽忙必须拒绝（`mdl/core/supervisor.c:46` 现状即如此，保持）。
- 多槽位是 v2 的事；若要做，须先回本节改写本决策并讨论资源仲裁。

### D2 — 资源竞争在编译期隔离和明确

- 资源（GPIO 引脚等）的合法性检查**全部前移到 PC 侧编译/pack 阶段**，
  设备运行时只做兜底校验（SVC handler 白名单检查保持现状）。
- 模块源码用宏声明资源（见 §5.2），编译进只读 manifest 符号；
  `tools/packer.py` 从 ELF 读出并与固件白名单比对——不合法**拒绝打包**，
  根本到不了设备。语义化资源冲突同样在 pack 期发现。
- 原则：**能静态确定的，绝不留到运行时**；运行时校验只是最后防线。

### D3 — 版本化 ABI，append-only

`host_api_t` 只增不改、版本号 bump；packer 在 pack 期比对模块与固件的
`HOST_API_ABI_VERSION`，不匹配拒绝。

---

## 3. 当前状态

| 里程碑 | 内容 | 状态 |
|:---:|---|---|
| M0 | MPU 区域编程自检 | 代码完成，未上真机 |
| M1 | LOAD + GOT 重定位 | 代码完成；**QEMU 已验证** |
| M2 | 非特权任务运行 | 代码完成，未上真机 |
| M3 | 故障恢复 + 软件看门狗 | 代码完成，未上真机 |
| M4 | USB CDC 传输 + 文本控制台 | **真机跑通**（枚举 COM5，控制台交互正常，288MHz 已实测） |
| MCP-A | MCP server（PC 侧） | **未开始 — 下一里程碑** |
| MCP-B | 资源 manifest + packer 编译期检查 | **未开始 — 随 MCP-A 一起做** |

**硬件已到位（2026-09-05）**：UYUP-RPI-A-2.4 底板（STM32F103VCT6 兼容 LQFP100），
实焊 **AT32F435VCT7**（256K flash / 384K SRAM / **24MHz 晶振**），外部 J-Link 接 H2。
板级事实、测试步骤、判定标准全部在 `mdl/tests/hil/ozone/README.md`，那里是唯一参考。

三个必须知道的板级前提：
1. **`DAP_CONF` 必须先短接到 GND 再上电**，否则板载 STM32F042 CMSIS-DAP 会和
   外部 J-Link 抢 SWDIO/SWCLK；
2. Ozone 只支持 SEGGER 探针，**用不了 CMSIS-DAP**，这条路不通；
3. **USB1 必须用 A-to-C 线**（2026-09-06 实测）。这块板的 USB-C 座作为 device
   缺 CC 上的 5.1k 下拉（Rd），Type-C 主机因此判定"没插设备"，
   **C-to-C 线下主机连"未知设备"都不显示**，任何固件都救不了。
   A 口没有 CC 协商，不受影响。排查 USB 先换线再怀疑代码。

原"板级配置不符"问题（芯片料号 define、Flash 长度、HEXT 晶振值）已于 2026-09-06 全部修正。

**M4 首跑 BusFault：已定位、修复并在真机验证（2026-09-06）。**

根因是链接脚本 `at32f435_m2/linker/AT32F435xC_MDL_MPU.ld` 的一个覆盖空洞：

```
.privileged_data   0x20000000..0x20008000  32KB   ← 既不拷贝也不清零
_sdata.._edata     0x20008000..0x20008100         ← 拷贝只覆盖 .data
_sbss.._ebss       0x20008100..0x20011020         ← 清零只覆盖 .bss
```

厂商启动代码只做两件事：`_sidata → [_sdata,_edata)` 拷贝、`[_sbss,_ebss)` 清零。
而 `_sidata` 原本写成 `LOADADDR(.data)`、`_sdata` 落在 `.data` 开头，
于是夹在中间的 `.privileged_data`（是**有 flash 初值的 loaded 段**）两头不靠。
FreeRTOS 全部 `PRIVILEGED_DATA` 对象——`pxCurrentTCB`、`pxReadyTasksLists[]`、
`uxCurrentNumberOfTasks`、`uxCriticalNesting`（初值 `0xaaaaaaaa`，不是 0）、
heap_4 的 `ucHeap`——上电即为 SRAM 随机值。

真机表现：`pxCurrentTCB` 垃圾非 NULL → `prvInitialiseTaskLists()` 从不执行 →
`pxReadyTasksLists[]` 是垃圾 → 首个 `xTaskCreate()` 在 `vListInsertEnd` 的
`str r1,[r2,#4]` 上写向非法地址 → 精确 BusFault（`CFSR=0x00008200`）。

**同一个根因极可能也是 2026-09-04 起悬而未决的 QEMU PendSV/MemManage 阻塞**：
QEMU 的 RAM 上电全零，`pxCurrentTCB==NULL` 侥幸正确，但 `uxCriticalNesting`
变成 0 而不是 `0xaaaaaaaa`，临界区嵌套计数从一开始就错——同因不同症。
修好后应重跑 QEMU 目标验证这个推断。

修法：`_sidata = LOADADDR(.privileged_data)`，`_sdata` 移到 `.privileged_data`
开头，`_edata` 仍在 `.data` 结尾；两段都是相邻的 `AT> FLASH`，LMA 连续
（已核对 `0x08016b2c + 0x8000 == 0x0801eb2c`），一次 memcpy 覆盖两段。
**不要把 `_sdata` 挪回 `.data`，也不要在这两段之间插入新的 loaded 段。**

排查方法本身值得记住：首次报的是 **IMPRECISERR**（`CFSR=0x00000400`，
`BFARVALID=0`），此时 PC 与真正出错的指令无关，顺着它查是查噪声。
`mdl/tests/hil/common/board_debug.c` 置 `ACTLR.DISDEFWBUF` 关掉写缓冲后
才变成 PRECISERR，一步定位。上真机排 bus fault 先开这个。

---

**M4 第二个真机 bug：宿主任务漏了 `portPRIVILEGE_BIT`（已修并验证）**

BusFault 修好后调度器起来了，随即在任务切换处停在 MemManage，
`CFSR = 0x00000008` = **MUNSTKERR**（异常返回出栈被 MPU 拒绝）。

根因：**全仓库从来没有用过 `portPRIVILEGE_BIT`**。FreeRTOS-MPU 里
`xTaskCreate()` 默认产生**非特权**任务，要特权必须显式
`uxPriority | portPRIVILEGE_BIT`。而 `supervisor_task` / `usb_task` /
`loader_task` / `watcher_task` 都是宿主基础设施：要写 `g_mdl_slot`（host .bss）、
要配 GPIO/CRM/OTG 寄存器、要驱动 USB 协议栈，全部需要特权。

而且它根本活不到访问那些资源：`xTaskCreate` 的任务栈从 FreeRTOS 堆分配，
heap_4 的 `ucHeap` 是 `PRIVILEGED_DATA`，`prvSetupMPU()` 把整个
`privileged_data` 段标为仅特权可访问 —— 非特权任务的 PSP 指进它够不着的内存，
**第一次异常返回到该任务时就 MUNSTKERR**。

已给 M2/M3/M4 及 QEMU 目标的宿主任务全部加上该位；
**唯一不能加的是模块任务**（`mdl/core/module_task.c`，非特权是它的全部意义，
且它的栈来自 arena 不是堆）。约定写在 `at32f435_m2/inc/FreeRTOSConfig.h`
`MDL_LOADER_TASK_PRIORITY` 旁边。

**这同时是 QEMU 那个"第二次任务切换 MemManage"阻塞的头号嫌疑**：
同样的 CFSR 值、同样的机制。QEMU 目标已一并加上该位，待复跑确认（H10）。
（`.privileged_data` 未初始化那条之前也被列为 QEMU 嫌疑——两条都真实存在，
QEMU 侧到底是哪一条、或两条都有，要跑过才知道。）

**构建纪律（2026-09-06 教训，重要）**：
"五个里程碑都编译通过"此前只在**各自落地当时**成立。首次五个一起编，发现
**M0/M1/M3 已坏了很久**——每个后续里程碑往*共享*文件加了早期裸机目标满足不了的依赖
（M3 把 `supervisor.h` 塞进 `fault_arm.c` 坏了 M0/M1；M2 把 `host/host_api.c` 改成依赖
FreeRTOS 坏了 M1；M4 把 `protocol.h` 塞进 `supervisor.c` 坏了 M3），而每次只重编了最新那个。
**没有 CI，所以提交前必须 `mdl/tests/hil/build.ps1 all -Clean` 五个全过。**

- 工具链：arm-none-eabi-gcc 12.2、Python 3.12；本机 QEMU 未装。
  `make` 是 **`mingw32-make.exe`**（WinLibs mingw64），且需要 Git 的 `usr/bin`
  提供 `mkdir -p` / `rm -rf`。别手拼，用 `mdl/tests/hil/build.ps1`。
- vendor 库路径为 `at32f435_lib/AT32F435_437_Firmware_Library_V2.2.6/`。

**已知技术债**（按优先级）：

1. 看门狗喂狗信号 = "调用了 host API"，CPU 密集模块超 3s 会被误杀；
2. ~~MDL 自身不显式置 `SCB->SHCSR.MEMFAULTENA`~~ → **已修**（`mpu_armv7m.c`
   `arch_setup_regions()` 末尾，与 MPU 使能同处）；
3. `.init_array` / C++ 静态构造不运行；
4. alloc 池双 free 不检出；
5. 无鉴权：能打开 CDC 端口即可 LOAD 代码；
6. 无 CI；测试为分散的 HIL/QEMU/mock 三套。
7. ~~M4 的 `main()` 从不调用 `sandbox_init()`，arena 边界为 0~~ → **已修**：
   拆出 `sandbox_bounds_init()`（只记边界、不碰 MPU），M3/M4 都调用它。
   真机 `arena` 已读回 text 0x20020000 / data 0x20024000 / heap 0x20026000 /
   guard 0x20028000，与链接脚本布局一致。

---

## 4. MCP 接口定义（草案 v0.1，冻结后实现）

- 形态：`mdl/tools/mcp_server/` 下的 Python MCP server，**stdio 传输**；
  通过 pyserial 操作 CDC 端口，server 内维护单连接，并发调用串行化。
- 设备返回的 UTF-8 错误串一律原样带回 tool result，不吞错。

### Tools

| Tool | 参数 | 返回 | 语义 |
|---|---|---|---|
| `describe_device` | — | `{abi_ver, arch, slot_state, gpio_whitelist, arena:{text,data,heap_stack}, watchdog_ms, transport}` | 设备能力发现 |
| `compile_check` | `source: string` | `{ok, diagnostics[], manifest{resources, sizes}}` | **只检不烧**：mock_host ASan 预检 → arm 交叉编译 → pack（含 manifest 校验）。agent 改代码的 inner loop 用它 |
| `deploy_feature` | `source: string`, `name: string` | `{ok, device_response, unloaded_previous: bool}` | 完整链：compile_check → 槽忙则 UNLOAD（原子替换）→ LOAD → 回读设备 OK/ERROR |
| `remove_feature` | — | `{ok, device_response}` | UNLOAD；槽已空返回 ok（幂等） |
| `device_status` | — | `{slot_state, fault_pc?, fault_text_offset?, uptime_ms}` | 槽位/故障查询；`fault_text_offset` 供 addr2line 定位模块内崩溃 |
| `read_logs` | `max_lines: int = 50` | `{lines[]}` | tail 设备日志（USB CDC） |

### 错误契约

- 所有 tool 返回 `{ok: bool, error?: string}`；`error` 直接面向 LLM，
  写明原因 + 下一步建议（沿用 `tools/packer.py` 现有错误文案风格）。
- `deploy_feature` 在 compile_check 失败时**不触碰设备**。
- 传输超时/断连：error 中报告端口状态，不自动重试（留给 agent 决策）。

---

## 5. 编译期资源隔离（规格）

### 5.1 规则

1. 模块资源声明齐全才可打包；声明与实际不符 = bug，pack 期能抓多少抓多少。
2. 声明的每个资源必须属于目标固件的 compile-time 白名单，否则拒绝。
3. v1 单槽下"两模块争同一资源"的语义 = **替换**：`deploy_feature` 返回
   `unloaded_previous: true`，agent 在 compile_check 输出里能看到旧 manifest。
4. 运行时 SVC 白名单校验保持现状，仅作兜底。

### 5.2 机制

模块文件作用域声明（与 `MDL_MODULE_ABI_DECLARE()` 同风格）：

```c
MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(GPIO(13), GPIO(14));  /* 宏生成只读 const 段 */
```

- packer 从 ELF 提取 `__mdl_resources` 符号 → 逐项对白名单 → 写进
  `.mdl` header 预留字段或 sidecar JSON，供 `compile_check` 与 STATUS 上报。
- 未来多槽位时，同一套 manifest 就是资源仲裁的输入，机制不变。

---

## 6. 目录地图

```
mdl/
├── tools/
│   ├── packer.py        ELF32 解析 + 校验 + 打包（MCP-B 的落点）
│   ├── watch.py         人工 inner loop（MCP server 将取代其角色）
│   ├── mock_host/       原生编译 + ASan 预检
│   └── mcp_server/      【待建】MCP server
├── transport/           USB CDC 厂商驱动 + MDLC 帧协议
├── core/                loader / supervisor / module_task / sandbox / registry
├── arch/arm_cm4/        MPU、故障恢复、GOT 重定位、r9 PIC 切换
├── host/                host_api_t vtable + SVC 门（唯一模块→宿主通道）
├── linker/              mdl_arena.ld（64K SRAM arena）
└── tests/               HIL m0–m4 / QEMU cmsdk_m4 / 7 个故障注入模块
```

## 7. 关键契约文件

| 文件 | 契约 |
|---|---|
| `mdl/core/mdl_format.h` | .mdl 线格式、重定位类型 |
| `mdl/host/host_api.h` | 模块 ABI（vtable + SVC 语义） |
| `mdl/transport/protocol.h` | MDLC 帧协议 |
| `mdl/linker/mdl_arena.ld` | 内存布局（arena 划分） |

---

## 8. 任务看板

### 8.1 优先级路线图（2026-09-07 定）

排序依据只有一条：**先补漏，再补课，最后才是拉开差距。**
把差异化功能建在一个正在漏的地基上，是这个项目最容易犯的错。

---

**P0 — 今天就在漏的洞。做完之前，别的都先放着。**

| # | 任务 | 量 | 为什么是 P0 |
|:---:|---|---|---|
| R1 | 卸载时把引脚恢复到安全态 | 半天 | `reclaim_module()` 现在**完全不碰 GPIO 配置**。MDL 把引脚配成什么样，卸载后就留成什么样，下一个 MDL 继承到一个状态未知的引脚。现在被掩盖着（MDL 只用 GPIO，而两个指示灯 host 会主动收回），换成 I2C/ADC 立刻暴露。**多外设仲裁的全部工作都压在这块地基上。** |
| R2 | 关掉 `BOARD_DEBUG_PRECISE_BUS_FAULTS`，或改成构建开关 | 一行 | 它设 `ACTLR.DISDEFWBUF`，**全系统每条 store 都变慢**。这是 bring-up 工具（H8/H12 都靠它定位），但留着它测出来的性能数字全是虚低的。**"几乎无性能损耗"是本项目的核心主张，拿错的数去对标 LuaTOS 会当场翻车。** |

---

**P1 — 没有这些就没资格谈对标 LuaTOS。**

| # | 任务 | 量 | 说明 |
|:---:|---|---|---|
| H2 | `watchdog_feed` 进 ABI，改掉"隐式喂狗" | 小 | **这是 F1 的前置，容易被漏。** 现在的喂狗信号是"调过任何 host API"，`sw3_blue` 靠 20ms 轮询恰好满足。F1 一落地，常驻 MDL 会**阻塞等事件**——几秒不调 API 是正常状态，会被当场误杀。先改语义，再做 F1。 |
| F1 | 中断 / 回调（引脚 EXTI、外设、定时器） | 大 | LuaTOS 有，我们没有——**纯补课项，不是差异化项**。架构和它一样是两段式（host ISR 记录 → 唤醒模块任务 → 非特权 handler），但我们的后半段是原生代码，没有解释器和 GC 停顿。要先定死一件事：**事件产生快于消费时的策略**（丢最旧 / 合并 / 只计数）。不明确定下来，将来必然变成难查的丢事件问题。 |
| R3 | 资源类型扩展：TIMER / I2C / UART / SPI / ADC，并按**引脚**判冲突 | 中 | 中断源本身就是资源（EXTI 线、定时器通道），F1 落地时必然要它。关键设计点：`MDL_RES_I2C(1)` 必须在 host 侧**展开成物理引脚**再比对——两个名字不同的声明可以撞在同一根线上（"I2C1_SDA" vs "GPIO 5"），只比名字必漏判。需要一张板级"功能↔引脚"映射表。 |

---

**P2 — 把已有的领先变成可证明、可自动化的护城河。**

| # | 任务 | 说明 |
|:---:|---|---|
| B2 | packer 提取 manifest + 与目标白名单比对 | 兑现 D2。**这才是"强制修改"的正确位置：让构建失败，而不是让加载失败。** 加载期拒绝是最后一道防线，不是第一道。 |
| P5 | 基准测试（对 LuaTOS / MicroPython） | 依赖 R2。要测的是**调度后 handler 的执行时间**和**事件到 handler 的延迟**，不是空转跑分。 |
| A1–A3 | MCP server | 让 agent 能直接编译/部署/查状态。 |

---

**P3 — 重要但不紧急。**

| # | 任务 | 说明 |
|:---:|---|---|
| F2 | flash 持久化 | MDL 现在在 SRAM，掉电即失。演示够用，产品不够。 |
| F3 | 原子热替换 | 现在是 unload → load 两步，中间有空窗。 |
| H10 | 复跑 QEMU 目标 | H12 大概率就是它的根因，跑一遍即可销账。本机未装 QEMU。 |

---

### 8.2 对标 LuaTOS 的定位（2026-09-07）

| 维度 | LuaTOS | 我们 | |
|---|---|---|---|
| 中断/回调 API | 有 | **无** | 落后，P1 补 |
| 回调执行 | 解释 + GC 停顿 | 原生，无 GC | 领先一个量级 |
| 用户逻辑定时精度 | ms 带抖动 | μs + 一次调度 | 领先 |
| 资源冲突检测 | 基本没有（后设置的静默覆盖） | 声明式 + 三道拦截 | **结构性领先** |
| 硬件隔离 | VM 内存安全（挡不住合法 API 改引脚复用） | MPU，且**无外设区域** | 领先 |
| 热替换 | 通常需重启 | 不重启不复位 | 领先 |
| **驱动/生态广度** | 数百个现成库 | 一个 GPIO | **差距巨大** |

> 结论要拆成两句话：**架构和确定性上超越它**——补完 F1 基本成立；
> **生态广度上超越它**——短期不可能，也不该作为目标。
>
> 真正该盯的是那些 LuaTOS **因为架构而做不到**的场景：μs 级确定性、
> 多方共用外设不静默打架、现场不断电改功能。

---

### 8.3 任务表

> 认领方式：在"负责"列写上名字/agent id，状态改为进行中。
> 约定状态：`待认领` → `进行中` → `已验证` → `完成`

| # | 任务 | 负责 | 状态 | 备注 |
|:---:|---|---|---|---|
| A1 | MCP server 骨架 + stdio 收发 | | 待认领 | 可先对 mock 设备开发 |
| A2 | `compile_check` 工具（接 mock_host + packer） | | 待认领 | |
| A3 | `deploy/remove/status/logs/describe` 工具 | | 待认领 | 依赖真机或仿真链路 |
| B1 | `MDL_MODULE_RESOURCES` 宏 + `__mdl_resources` 段 | claude/opus-5 | **已验证** | ABI v2；真机拒绝 PA9 认领，报错点名持有者 |
| B2 | P2 packer manifest 提取 + 白名单比对 | | 待认领 | 兑现 D2；"强制修改"的正确位置是构建期而非加载期 |
| R1 | **P0 卸载时恢复引脚到安全态** | | 待认领 | `reclaim_module()` 不碰 GPIO 配置，残留会传给下一个 MDL |
| R2 | **P0 关掉 DISDEFWBUF（写缓冲）** | | 待认领 | 一行；不改则所有性能数字虚低 |
| R3 | P1 资源类型扩展 + 引脚级冲突展开 | | 待认领 | TIMER/I2C/UART/SPI/ADC；需板级功能↔引脚表 |
| F1 | P1 中断 / 回调（两段式派发） | | 待认领 | ABI v3；先定事件队列溢出策略；依赖 H2 |
| F2 | P3 flash 持久化 | | 待认领 | 现在掉电即失 |
| F3 | P3 原子热替换 | | 待认领 | 现在 unload→load 有空窗 |
| P5 | P2 基准测试（对标 LuaTOS） | | 待认领 | 依赖 R2，否则测的是虚低的数 |
| H1 | 真机 HIL 配置修正（ozone README 板级问题） | claude/opus-5 | **完成** | 料号/Flash 长度/HEXT 全改；五目标重编通过 |
| H2 | **P1 看门狗误杀 → `watchdog_feed` 进 ABI** | | 待认领 | **F1 的前置**：常驻 MDL 阻塞等事件时会被隐式喂狗机制误杀 |
| H3 | 技术债 #2：显式置 MEMFAULTENA | claude/opus-5 | **完成** | `mpu_armv7m.c` `arch_setup_regions()` |
| H4 | 修复 M0/M1/M3 构建腐坏 + 加 `build.ps1` | claude/opus-5 | **完成** | 见 §3 构建纪律 |
| H5 | 24MHz PLL（`common/board_clock.c`）+ USB 48MHz | claude/opus-5 | **已验证** | 真机 `clk` 读回 288000000 |
| H6 | 非特权 trampoline 访问 host RAM（必炸） | claude/opus-5 | **完成** | 走 `freertos_system_calls` 提权；要等真的加载模块才算验证 |
| H7 | USB CDC 文本控制台（与 MDLC 帧共用一个口） | claude/opus-5 | **已验证** | 真机 COM5 交互正常；MDLC 帧共用尚未实测 |
| H8 | **M4 真机 BusFault 定位** | claude/opus-5 | **已验证** | 根因：链接脚本 `.privileged_data` 未被启动代码初始化，见 §3 |
| H10 | 复跑 QEMU 目标，验证 PendSV 阻塞是否同因 | | 待认领 | 本机未装 QEMU；H8+H11 两个修复都已带上 |
| H11 | **宿主任务漏 `portPRIVILEGE_BIT` → MUNSTKERR** | claude/opus-5 | **已验证** | M2/M3/M4/QEMU 全部补上；模块任务保持非特权 |
| H12 | **模块加载必炸设备（8B 空模块也炸）** | claude/opus-5 | **已验证** | 根因：`module_task.c` 把 `tskMPU_REGION_*` 抽象标志当 RASR 位传给 ARMv7-M 端口 → AP=000 + SIZE 被撑大；见 §9 |
| H13 | fault 记录跨复位存活 + `fault` 控制台命令 | claude/opus-5 | **已验证** | `.noinit` 段；fault 后主动复位而非锁死。H12 和 H15 两个根因都是靠它读出来的 |
| H15 | **`host->log()` 用半主机，无调试器时必炸** | claude/opus-5 | **已验证** | `bkpt 0xAB` 在 C_DEBUGEN=0 时升级为 HardFault(DEBUGEVT)；改为可替换 sink，M4 走 USB 控制台 |
| H14 | `build.ps1` 把编译失败报成 BUILD OK | claude/opus-5 | **完成** | 旧逻辑靠 `Test-Path firmware.elf`，陈旧 elf 即绿灯；改用 make 退出码 + error/warning 分离 |
| H9 | 技术债 #7：M4 arena 边界为 0 | claude/opus-5 | **已验证** | 拆出 `sandbox_bounds_init()`；M3 内联版还漏了 guard 边界 |

---

## 9. 工作日志

> 新条目加在最上面。格式：`### YYYY-MM-DD 名字/agent id` + 简短条目列表。

### 2026-09-07 claude/opus-5（下午：三个 MDL 真机验证 + 路线图）

**三个 MDL 真机验证通过，不重烧不复位切换**

| MDL | 行为 | 结果 |
|---|---|---|
| `sw3_blue` | 按住 SW3 → 蓝灯亮 | 通过 |
| `sw4_green` | 按住 SW4 → 绿灯亮 | 通过 |
| `sw3_blink` | 按住 **同一颗 SW3** → 绿灯闪 | 通过 |

第一个和第三个是全部意义所在：**同一颗物理按键做完全不同的事，差别只是
296 字节走了一次串口**。用户实测蓝灯 1Hz 存活闪烁频率**肉眼无变化**——模块任务
是最低优先级、只在 host 任务空隙里跑，没有挤动 `indicator_task` 的节拍。这是
"几乎无性能损耗"第一次以可观察的方式出现（还不是测量，见 P5/R2）。

资源仲裁也在真机上验了两个方向：`conflict.mdl` 认领 PA9 被拒且 `status` 仍是
`EMPTY`（一个字节都没进 arena）；`sw3_blue` 加载后蓝灯引脚连采 14 次全灭
（host 停止驱动），声明转移到别的 MDL 后闪烁自动恢复。

**本轮抓到的 bug**（全部已修）
- **回归（我引入的）**：把"必须声明才能碰引脚"的检查加在了 `host_gpio_set_impl()`，
  而 `host_gpio_direct_set()` 也走它 → 没加载 MDL 时 **host 拒绝了自己使用自己的
  指示灯**，两个灯全哑。检查放错了层：它约束 MDL，不是引脚的属性。
- `reclaim_module()` 不清 `gpio_claimed` → unload 后 host 永远不收回让出的引脚。
  其余声明字段之所以没出事，只因为 `help`/`pins` 恰好都先判了 `state != EMPTY`——
  **这不是可依赖的性质，是下一个 bug 在等人来写**。全部清干净，并给
  `host_gpio_yielded_to_module()` 补了状态判断。
- `pins` 所有权列把 host 项排在模块声明之前 → 报告 host 拥有一个它已让出的引脚。
  **和硬件矛盾的所有权列比没有这一列更糟。**

**`ver`：让设备自己回答"跑的是不是我刚构建的那版"**

`board_buildid.c` 每次 make 强制重编（`FORCE` 依赖），所以 `__DATE__ __TIME__` 是
镜像的构建时刻。起因是这个问题反复靠推断回答：**Ozone 在 `OnProjectLoad()` 里只读
一次 ELF，之后下载的是内存里那份**，重建在调试器侧是隐形的，一整轮测试可能花在上一
个镜像上。真机核对通过。

**术语定案：这些单元叫 MDL**

不是"固件"（那是烧进去、独占机器、要重启的东西），也不叫"模块"（嵌入式语境里已被
硬件模组占用）。`.mdl` 后缀、`MDLC` 帧魔数、`mdl_load()`、`mdl>` 提示符本来就全是
这个词，零成本。ABI 符号 `module_init`/`module_cmd` 不动——那是入口函数名，不是名词。

**工具**
- `console.py` 加本地命令：`/load <相对或绝对路径>` 在**同一个已打开的端口**上推 MDL，
  并解码返回的二进制帧。此前导入必须退出终端 → 跑 watch.py → 再开回来，就因为
  一个串口只能被打开一次。
- `watch.py` 接受直接推成品 `.mdl`：拿三个做好的 MDL 试用和开发一个 MDL 是两回事，
  前者不该要求装 ARM 工具链。

**代码已推送** `git@github.com:overrize/embedded_sandbox.git` `main` 分支。
本机原先没有任何 SSH 密钥，新生成 ed25519 一把。

**优先级路线图见 §8.1**，对标 LuaTOS 的逐项定位见 §8.2。一句话：
中断（F1）是**补课**不是差异化；资源仲裁和确定性才是护城河；生态广度短期追不上，
也不该当目标。

### 2026-09-07 claude/opus-5

**MDLC 帧路径：已验证可用**
- `CMD_STATUS`(13B) 真机回了合法 `RESP_STATUS` 帧；坏 magic 回
  `bad magic (not an MDL0 image)`，坏 CRC 回 `crc32 mismatch (corrupt transfer?)`。
  → magic 增量匹配、与控制台文本的分用、CRC、帧组装、supervisor 唤醒、
  `mdl_load()` 的全部校验，**这些都是好的**。H7 的"MDLC 帧共用尚未实测"可以销掉。
- 主机端 CRC 与设备端 `mdl_crc32_2()` 数值核对一致（`0xECB3F528`），
  两边都是标准 CRC-32（反射，`0xEDB88320`）。

**H12：模块一加载就炸，已二分到最小复现**
- 现象：写下 `CMD_LOAD` 帧后 USB 从总线掉线（`ClearCommError` /
  `PermissionError(13)`），随后设备重新枚举，`status` 显示 `slot: EMPTY`、
  `fault: none on record`——**证据随复位蒸发**。
- 三个模块依次推送，全炸：

  | 模块 | text | got | relocs | 内容 | 结果 |
  |---|---|---|---|---|---|
  | minimal | 8B | 3 | 0 | `return 7;` | 炸 |
  | nohost | 24B | 4 | 1 | 一个全局变量 | 炸 |
  | hello | 180B | 7 | 4 | 全局 + `host->log()` | 炸 |

  8 字节的 `movs r0,#7; bx lr` 都炸，**排除**重定位、GOT、host vtable、
  payload 大小、突发/流控（32B/60ms 限速同样炸）。
- 也逐一排除了：`init_off=0x1`（Thumb 位正确，非 INVSTATE）；
  arena 由 `mdl_arena.ld` 以 `(NOLOAD) ALIGN(65536)` 真实预留，不与 `.bss`/堆重叠；
  模块栈 `0x20027800` 对 2KB 自对齐，`xRegions[0..2]` 基址/长度均合法；
  四个 fault handler 都已链进 ELF。
- 剩下的嫌疑范围：`xTaskCreateRestricted` 到首次切入非特权模块任务这一段。
  这正是 M2 建立、但**至今在任何环境（含 QEMU）都没真正跑通过**的那条路，
  与 H10 记的 QEMU PendSV 阻塞很可能同因。

**里程碑达成：完整模块（含 host API 调用）真机跑通**

`hello.mdl`（288B，2 个全局、4 条重定位、3 次 `host->log()`、1 次 `host->gpio_get()`）：

```
-> RESP_OK
[module] hello module: module_init running     ← 模块日志从 USB 控制台出来
[module] hello module: counter ok
status : slot : LOADED (ran, idle)   fault : none on record
mem 0x20024000 12:
  0x20024000: 0x00001218 0x00000000 0x00000000 0x20020054
  0x20024010: 0x20020070 0x2002008C 0x2002401C 0x0000002A
                ↑ text-base 重定位（字符串字面量）  ↑ data-base   ↑ g_counter = 42
```

GOT 里三个槽指回模块 text（`0x2002xxxx`，字符串字面量），一个槽指向模块 data
（`0x2002401C` = `&g_counter`），而那个地址上就是 `0x2A`。`1 + 41 = 42` 是模块
自己算的、通过重定位后的指针写进去的。

至此 §1 的目标在真机上端到端成立：**不烧录、不重启，串口送进去的代码改变了 MCU 的行为。**

**里程碑：模块首次在真机上跑通（不重新烧录）**

推 `nohost.mdl`（96B，一个全局变量 + 一条 `R_ARM_RELATIVE`，不调 host API）：

```
-> RESP_OK
status : slot : LOADED (ran, idle)   entry : 0x20020001
mem 0x20024000 8:
  0x20024000: 0x00001164 0x00000000 0x00000000 0x20024010
  0x20024010: 0x0000002A                        ↑ GOT 槽已重定位到运行时数据基址
              = 42 = g_v 的 1+41
```

代码经 USB 送入 → 装进链接脚本预留的 SRAM arena → GOT 重定位 → MPU 下非特权执行
→ 通过重定位后的指针改写自身全局 → 干净返回。**§1 的核心主张到此为止是被证明的。**

**H15：`host->log()` 用半主机，真机上必炸（已修）**

`hello`（带 `host->log()`）仍然炸，但故障签名与 H12 完全不同：

```
hfsr  : 0x80000000   DEBUGEVT（不是 FORCED）
cfsr  : 0x00000000   一个 fault 状态位都没有
pc    : 0x08012D36   semihost_write0, host_api.c:168
lr    : 0x0801019F   host_log_impl, host_api.c:191
```

CFSR 全零 + HFSR.DEBUGEVT = 执行到了 `bkpt`，不是内存违例。`host_log_impl()` 走的是
ARM 半主机 `SYS_WRITE0`（`bkpt 0xAB`）。`bkpt` 在 `DHCSR.C_DEBUGEN == 0` 时**不是
静默失败**，它触发调试事件、在没有 debug monitor 的情况下直接升级成 HardFault。
所以任何调用 `log()` 的模块在脱机板子上都会打死设备。

源码里那句 `/* placeholder until M4's USB CDC */` 说明这本来就是临时方案，
只是 M4 的控制台做好之后没换掉。改为弱符号 sink `host_log_sink()`：
默认实现只在真的挂了调试器时才发半主机，`mdl/transport/console.c` 提供强实现
把模块日志打到 USB 控制台（前缀 `[module] `）。

**H12 根因（已修）：MPU 区域属性用错了编码体系**

`mdl_start_module_task()` 传的是 `task.h` 的抽象标志：

```c
tskMPU_REGION_READ_ONLY     = (1UL << 0)   /* 0x01 */
tskMPU_REGION_READ_WRITE    = (1UL << 1)   /* 0x02 */
tskMPU_REGION_EXECUTE_NEVER = (1UL << 2)   /* 0x04 */
```

这套标志是给 **ARMv8-M** 端口用的，那边 `vPortStoreTaskMPUSettings()` 会翻译。
**ARMv7-M 端口不翻译**，直接把 `ulParameters` OR 进 RASR，而 RASR 的位是
bit0=ENABLE、bits[5:1]=SIZE、bits[26:24]=AP。后果有两重：

1. **AP = 000 = 任何权限都不可访问**。不是"非特权被拒"——特权 handler 模式
   一样被拒。这就是为什么故障出现在 PendSV 里而不是模块代码里。
2. **SIZE 被多 OR 了 `0b011`**。8K 的 data 区编码 `0x18`，OR `0x06` 得 `0x1E`
   → SIZE=15 → 区域变成 **64K**；MPU 会忽略低于区域大小的基址位，于是它实际
   覆盖 `0x20020000..0x2002FFFF`，**把模块任务自己的栈吞了**。它是 region 6，
   栈是 region 4，重叠时高编号胜出，所以正确设好的栈区被它压掉。

净效果：加载**任何**模块（8 字节的 `return 7;` 也算），首次上下文切换进模块
任务时 `PendSV_Handler` 的 `ldmia r0!` 就吃 MemManage DACCVIOL。

真机取到的证据（`.noinit` 记录跨复位存活）：

```
which : MemManage in HOST code (pc outside module text)
pc    : 0x0800BEEC   = PendSV_Handler, port.c:549, ldmia.w r0!,{r3..lr}
r0    : 0x20027FB0   = 模块任务栈指针（在 0x20027800..0x20028000 内）
xpsr  : 0x2101000E   IPSR=14 = PendSV
cfsr  : 0x00000082   DACCVIOL + MMARVALID
MPU_CTRL = 0x00000005  PRIVDEFENA=1（所以背景区本该放行特权访问）
```

修复：改用端口自己的原始 RASR 位 `portMPU_REGION_READ_ONLY` /
`portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER`，并补上
`configTEX_S_C_B_SRAM`（端口只给它自建的栈区加，可配置区域不加；缺了就是
Strongly-Ordered 内存，非对齐访问会 fault）。

> 这条**极可能就是 H10 记的 QEMU PendSV 阻塞的同一个根因** —— 同一份端口、
> 同一段 `mdl_start_module_task()`、同样发生在"第二次任务切换"。

**仪器自身的两个缺陷（同时修掉）**
- `fault_arm.c` 在 `SCB->CFSR = cfsr`（write-1-to-clear）之后才让我读 MMFAR，
  那时它架构上已是 UNKNOWN。第一次真机读数里 `mmfar = 0xE000ED34` 就是这么
  来的假值——差点把我引去查 SCB。改为把清除前捕获的 `mmfar` 作为参数传入。
- MemManage 路径拿不到 EXC_RETURN，却仍按全 1 去解"来自 PSP 还是 MSP"，
  必然报错。改为未捕获时明说未捕获，并改从 xPSR 的 IPSR 报告异常号。

**H13：让 fault 活过复位（没有这个就没法继续查 H12）**
- 旧的 `board_fault_record()` 结尾是 `bkpt #0` + 死循环。没接调试器时
  `bkpt` 不是被忽略，而是在 fault handler 内再次 fault → **LOCKUP**：
  CPU 停摆、USB 不再应答、记录不可达。任何模块 fault 都长成
  "设备凭空消失"，这是 H12 一直查不动的直接原因。
- 改为：`.noinit` 段承载 `g_board_fault`（链接脚本新增，落在
  `_ebss` 之上；启动代码 `bcc` 是左闭右开，确认不会被清零），
  记录写完 `__DSB()` 后——有调试器则 `bkpt`，否则 `NVIC_SystemReset()`。
- 新增控制台 `fault` 命令，打印 pc/lr/xpsr/cfsr/hfsr/bfar/mmfar/excret，
  并把 IACCVIOL / DACCVIOL / MUNSTKERR / MSTKERR / IBUSERR / PRECISERR /
  IMPRECISERR / UNDEFINSTR / INVSTATE / INVPC / UNALIGNED 逐位译成人话，
  外加"来自 PSP 还是 MSP"。`board_debug_print_fault()` 收一个输出函数指针
  而不是直接调 `mdl_console_puts`——M0/M1 链接 `board_debug.c` 但没有控制台。

**H14：构建脚本在说谎**
- `build.ps1` 用 `Test-Path build/firmware.elf` 判定成功。编译报了 6 条
  `error:` 之后，因为上一轮的 elf 还在，它打印了 `built, with warnings` +
  `BUILD OK`。红灯被涂成绿灯，比不检查更糟。
- 改为以 `$LASTEXITCODE` 为准，并把 error 与 warning 拆成两个桶。

**工具**
- `watch.py --verify`：`RESP_OK` 后不关端口，在**同一个端口会话**里跑
  `status` / `mem 0x20024000 12` 并打印设备原文。板子只有一根线供电，
  断开重连=掉电=模块蒸发，所以验证必须留在同一会话内。
- `console.py` 去掉了开口时的 `

` 探测字节（它和 banner 抢时序）；
  `console.c` 把 `
` / `
` / `

` 统一算一次回车（CRLF 双提示符）。

### 2026-09-06 claude/opus-5

**硬件接入**
- 定位并解决 J-Link 连不上：Ozone 接口设成了 JTAG（日志里 `TotalIRLen = ?` /
  `IRPrint = 0x..0000` 是 JTAG IR 扫描的特征，板上只引出 SWD），且
  **`DAP_CONF` 未接地**导致板载 CMSIS-DAP 抢总线。
- 从原理图核对：USB1 走 PA11/PA12（OTGFS1，与 `usb_conf.h` 一致），
  **device-only 非 OTG**（VBUS/CC 都没接 MCU，且模板分配的 VBUS/ID 引脚
  PA9/PA10 被 DAP 的 UART 桥占用——绝不能给它们配 MUX）。
- Ozone 工程合并为唯一一个 `mdl/tests/hil/ozone/mdl.jdebug`。

**修复**
- H1 板级配置：`-DAT32F435VCT7`、Flash 256K、链接脚本重命名 xG→xC、
  `HEXT_VALUE` 8MHz→24MHz。
- H4 构建腐坏：M0/M1/M3 修好（弱符号 `mdl_supervisor_wake_from_isr` /
  `mdl_transport_write`，新增裸机 `at32f435_m1/host_api_m1.c`），
  新增 `build.ps1` 一键五目标。
- H3 `MEMFAULTENA`；H6 非特权 trampoline 提权。
- GPIO 白名单从 PA0/PA1 占位换成真实引脚（PD10/PE15 LED、PA3/PE2 按键），
  并补上从来没做过的引脚初始化（`host_api_init()` 原本是空函数）。

**新增**
- `common/board_clock.c`：**24MHz 下不能照抄厂商 PLL 参数**——厂商的
  `(ns=144, ms=1, fr=4)` 标注前提是 8MHz，24MHz 下算出 864MHz，且 `ms=1`
  时 PLL 参考输入 24MHz 已超出驱动文档 `2..16MHz` 上限。改用 **`ms=3`**：
  24/3=8MHz 参考 → VCO 1152MHz → 288MHz，落在与厂商完全相同的工作点。
  USB 48MHz 由 288/6 分频（原 HICK+ACC 无晶振方案作废，板上有真晶振）。
- `transport/console.{c,h}`：文本控制台与 MDLC 二进制帧**共用同一个 CDC 口**，
  靠逐字节匹配魔数前缀解复用（不用 4 字节滑窗，否则打字滞后 3 个字符）。
  命令在 supervisor 任务里执行，与故障恢复/看门狗单线程串起来。
- `common/board_debug.c`：精确故障捕获（H8 用）。

**H8：真机第一个 bug，已定位并修复**
- 症状先是 imprecise BusFault 升级成 HardFault，PC 报在 `vListInsertEnd` —— 是噪声。
  加 `ACTLR.DISDEFWBUF` 后变精确，定位到 `str r1,[r2,#4]`，即
  `pxIndex->pxPrevious->pxNext = ...` 写向非法地址。
- 根因：链接脚本把 32KB `.privileged_data` 留在启动拷贝/清零范围之外，
  FreeRTOS 全部 `PRIVILEGED_DATA` 上电即随机值。详见 §3。
- 已修 + 重编五目标 + 核对 map 里 LMA 连续性。**尚未复测真机。**

**H11：真机第二个 bug（H8 修好后浮出来的），已修**
- BusFault 消失，调度器启动，随即 MemManage `CFSR=0x00000008`（MUNSTKERR）。
- 根因：**全仓库从未使用 `portPRIVILEGE_BIT`**，所有宿主任务都是非特权的，
  而它们的栈来自 `PRIVILEGED_DATA` 里的 FreeRTOS 堆 → 异常返回出栈即被拒。
- M2/M3/M4/QEMU 四处全部补上；模块任务保持非特权不动。

**✅ 真机跑通了（2026-09-06）—— 项目第一次在真硅片上端到端工作**

重烧后 USB 正常枚举为 `USB\VID_2E3C&PID_5740` → **COM5**，控制台实测：

```
>>> clk
system_core_clock = 288000000 Hz
  (PLL from 24MHz HEXT: 24/3*144/4 -- at spec maximum)
>>> btn 2
1  (idle)
>>> status
slot : EMPTY / fault : none on record
```

同时证明：24MHz 晶振起振 + `ms=3` 的 PLL 锁到 288MHz、USB 48MHz 分频正确、
FreeRTOS-MPU 调度器稳定、CDC 收发、控制台回显与命令分发、真实 GPIO 读写。
**H12 之前怀疑的 0Ω 跳线是虚惊——板上焊的就是 R69/R70（PA11/PA12）。**

**H9（技术债 #7）：跑通当场就被 `arena` 命令抓出来了**
- `arena` 打印全零 → M4 从不调用 `sandbox_init()`，`g_mdl_slot` 的边界是 0，
  这时推一个 `.mdl` 进去会 memcpy 到地址 0。
- 拆出 `sandbox_bounds_init()`（只记边界、不碰 MPU），M4 调它；
  M2+ 的 MPU 区域本来就归 FreeRTOS 按任务管，`sandbox_init()` 写的
  region 4..7 在模块跑起来前必被覆盖，那半边对 RTOS 目标没有意义。
- M3 原先在 `supervisor_task` 里内联手写这段赋值，**而且漏了
  `guard_lo/guard_hi`**（`mdl_record_fault()` 的分类要用），一并换成调用。
- M3/M4 的 Makefile 之前都没有编 `sandbox.c`，补上。

**H12：USB 排查（已闭环，保留作参考）**
- H8+H11 修完后固件不再崩，但主机侧**连"未知设备"都看不到**
  （只有 J-Link 在线；`VID_2E3C/PID_5740` 全无）。
- 已核实软件侧完好：`OTGFS1_IRQHandler` 是自己的实现（非 `Default_Handler`）、
  `usb_cdc_init` 在 `main` 里被调用、`usbd_init`→`usbd_connect`→`usb_connect`
  链路完整（驱动内部就会释放 D+ soft-disconnect）。
- 为定位加了三样：
  1. `main.c` 的 `heartbeat_task`——1Hz 闪 LEDB，把"固件没跑"和"USB 没通"分开；
  2. `board_clock.c` 显式 `crm_usb_clock_source_select(PLL)`（原先靠
     `crm_reset()` 清 `misc1` 的副作用生效，能用但脆）；
  3. `usb_conf.h` 补上 **OTGFS2 分支**——原先只有 `#if (OTG_USB_ID == 1)`，
     改成 2 会编不出来。现在切到 PB14/PB15 就是改一个数字。
- 头号嫌疑仍是板上 0Ω 跳线选的是 R71/R73（PB14/PB15）而非 R69/R70（PA11/PA12），
  待万用表确认。

**未完成 / 交接**
- 复测 M4（H8 + H11 + H12）；按 H10 复跑 QEMU。
- **本次所有改动仍只做到编译与 map 验证，没有一行在真机上跑通过。**

### 2025-XX-XX 主线 agent

- 完成 `mdl/` 全量代码走读；确认 D1 单槽互斥语义与现状代码一致。
- 与用户对齐：资源竞争在编译期隔离（D2）；MCP 接口出草案（§4）。
- 建立本协作文档；待办：冻结 §4 → 认领 A1。

<!-- 同事从这里往上追加 -->

---

## 10. 同事工作区

> 本节预留给同事 agent：自由记录调研笔记、方案草稿、疑问。
> 建议每人一个四级标题，如 `#### 张三 / agent-2`，他人勿改动不属于自己的段落。

#### （示例）张三 / agent-2

- 疑问：watchdog_feed 进 ABI v2 后，旧模块（v1）在新固件上还能跑吗？
  → 能：append-only，v1 模块不调新函数即可；packer 按 abi_ver 精确匹配，
  固件升 v2 后旧 v1 包会被拒——这里要不要允许 "v1 模块跑在 v2 固件"？需讨论。

<!-- 同事工作区 -->
