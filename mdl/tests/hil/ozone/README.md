# 上板测试用例 (无外设版)

板子: **UYUP-RPI-A-2.4**（深圳市优熠科技，STM32F103VCT6 兼容的 LQFP100 底板，
实际焊 AT32F435VCT7）。原理图: `UYUP-RPI-A-2.4.pdf`。

只需要 J-Link + 板子。不接串口、不接 USB CDC、不接任何 GPIO。
所有观测都通过调试器的内存/寄存器窗口完成。

工程只有一个: **`mdl/mdl.jdebug`**（仓库里 `mdl/` 目录下，不在本目录），
切换里程碑改里面 `File.Open` 那一行。它用 `$(ProjectDir)` 相对路径，
并绑定了本机 J-Link 的序列号 `600107328`；换探针要改 `Project.SetHostIF`。

---

## 怎么编译

工具链一个都不在 PATH 上，所以别手敲 `make`——用脚本：

```powershell
cd C:\self_staff\embedded_sandbx\mdl\tests\hil
.\build.ps1                # 编全部五个，打印 flash 占用表
.\build.ps1 m4             # 只编 M4
.\build.ps1 m1 -Clean      # M1 全量重编
.\build.ps1 all -Clean -Verbose
```

改了 Makefile 或链接脚本一定要加 `-Clean`（`-MMD` 只跟踪头文件依赖，不跟踪这两个）。

脚本干的事只是拼环境，等价的裸命令是：

```powershell
$env:Path = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin;' +
            'C:\Users\51771\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin;' +
            'C:\Program Files\Git\usr\bin;' + $env:Path
cd at32f435_m4
mingw32-make PYTHON="C:/Users/51771/AppData/Local/Programs/Python/Python312/python.exe"
```

三个目录缺一不可：ARM 交叉编译器；**`make` 在这台机器上叫 `mingw32-make.exe`**，
没有 `make.exe`；Makefile 用了 `mkdir -p` / `rm -rf`，要 Git 的 `usr\bin`。
`PYTHON=` 也必须显式传——PATH 上的 `python` 是 Microsoft Store 的桩，会直接报错。

产物固定是 `at32f435_<target>/build/firmware.elf`，也就是 `mdl/mdl.jdebug` 里
`File.Open` 指的那个路径。

**预期的无害告警**（不用管）：
- 链接器的 `LOAD segment with RWX permissions`——`.mdl_arena` 是 NOLOAD 但落在 RAM 里，每个目标都有
- M4 里一堆 `unused parameter`——全部来自厂商的 USB 中间件源码

---

## ⚠️ 前置 1: DAP_CONF 必须短接到 GND 再上电

板上 U3（STM32F042F6P6）是**板载 CMSIS-DAP 调试器**，它的
`JTAG_SWDIO`/`JTAG_SWCLK` 直连主芯片 SWDIO/SWCLK。不停用它，
它会和外部 J-Link 抢总线。

原理图注 1 原文：

> 当 DAP_CONF 短接到 GND 再上电时, 板载 SWD 停用, 这时使用外部调试器,
> 如 JLINK, ST-LINK 等（临时停用）

`DAP_CONF` 在 **H2**（`PZ254V-12-10P`，10 脚调试排针）上，J-Link 也接这里。
**必须先短接再上电**，上电后再短接无效。

顺带（原理图注 2）：DAP_CONF 悬空时，按住主芯片 RST 再上电，板载 DAP 会在
`[CMSIS-DAP 2.x / 1.x / 停用]` 三态间循环并保存。如果之前 DAP-Link 在
Ozone 里"没反应"，有可能它本来就被切到停用态了——但即使切回来，
**Ozone 也用不了 CMSIS-DAP**（Ozone 只支持 SEGGER 探针），这条路走不通。

## 前置 2: 型号与时钟（已修）

原理图 U1 是 100 脚 `STM32F103VCT6` 封装，AT32F435**V**CT7 与之引脚兼容
（V = LQFP100，C = 256KB flash）。以下三处已经改掉：

| 位置 | 原来 | 现在 |
|---|---|---|
| 五个 `Makefile` 的 `DEFINES` | `-DAT32F435RGT7`（64 脚） | `-DAT32F435VCT7` |
| `linker/AT32F435xC_MDL{,_MPU}.ld` | `FLASH = 1024K`（文件名还叫 xG） | `256K`（已重命名 xC） |
| `at32f435_m0/inc/at32f435_437_conf.h` | `HEXT_VALUE = 8000000` | `24000000` |

RAM 384K 和 `_estack = 0x20060000` 原本就是对的——和厂商
`AT32F435xC_FLASH.ld` 的 MEMORY 块一致。

### PLL：24MHz 晶振不能照抄厂商参数

新增 `mdl/tests/hil/common/board_clock.c`。厂商所有例程的
`crm_pll_config(HEXT, 144, 1, FR_4)` 旁边明写着 "hext(**8mhz**)"，在 24MHz 上会
变成 864MHz，而且 `pll_ms=1` 本身就违反驱动文档的
`2MHz ≤ hext/ms ≤ 16MHz`（24MHz 超上限）。

本工程用 **`ms=3`**：

```
参考频率 = 24MHz / 3   = 8MHz     (2..16MHz  ✓)
VCO      = 8MHz × 144  = 1152MHz  (500..1200MHz ✓)
sclk     = 1152MHz / 4 = 288MHz   (AT32F435 额定上限，不是超频)
USB      = 288MHz / 6  = 48MHz    (CRM_USB_DIV_6)
```

先除以 3 之后，PLL 落在和厂商 8MHz 方案**完全相同**的参考频率与 VCO 上——
不是一个新工作点，是同一个工作点换了个晶振走到。

### 各目标的 flash 占用（256K 上限）

| | text | data | flash | 占比 |
|---|---|---|---|---|
| M0 | 1 844 | 16 | 1 860 | 0.7% |
| M1 | 6 188 | 20 | 6 208 | 2.4% |
| M2 | 77 876 | 32 796 | 110 672 | 42.2% |
| M3 | 83 048 | 32 796 | 115 844 | 44.2% |
| M4 | 92 524 | 33 036 | 125 560 | 47.9% |

地址全部来自 `at32f435_m0/build/firmware.map` 和 packer 输出的真实值，不是估算：

```
.text        0x0800020C  0x4C4        (M0 总共约 1.8KB flash)
.mdl_arena   0x20010000  0x10000      (NOLOAD)
  text       0x20010000  16K
  data       0x20014000  8K
  heap+stack 0x20016000  8K
  guard      0x20018000  32B   <- MDL_PERM_NONE
```

---

---

## 测试 A — M0: MPU 隔离是否真的生效

`mdl/mdl.jdebug` 默认指向 M4（框架固件）。要跑 M0 就把 `File.Open`
改成 `at32f435_m0/build/firmware.elf`。下载，复位。

M0 的 `main()` 一共就三步：`registry_init()` → `sandbox_init()` → 读 `*(uint8_t*)0x20018000`。
那个地址是 guard 区，MPU 里配成 `AP=0b000`（任何特权级都禁止访问）。

### 断点

1. `at32f435_m0/main.c:46`  —— guard 访问那一行（越界访问发生前）
2. `at32f435_m0/main.c:50`  —— `for(;;) bkpt` 循环（**绝对不该到达**）

### 步骤

1. 停在断点 1。在 Register 窗口展开 MPU，或直接看内存：
   - `0xE000ED9C` (MPU_RBAR) / `0xE000EDA0` (MPU_RASR)，配合 `0xE000ED98` (MPU_RNR) 逐个切 region 4..7 读回
   - **region 7 应该是 guard**：RBAR = `0x20018000`，RASR 的 AP 字段(bit26:24) = `0`，SIZE 字段(bit5:1) = `4` (32B)，ENABLE(bit0) = 1
   - `0xE000ED94` (MPU_CTRL) 应该 = `0x7` (ENABLE | HFNMIENA | PRIVDEFENA)
2. 单步过第 46 行。

### 期望结果（B1 已修）

`arch_setup_regions()` 现在会置 `SCB->SHCSR.MEMFAULTENA`，所以越界应该走到
**`fault_arm.c` 的 `mdl_memmanage_handler_c`**，而不是升级成 HardFault。

> B1 是什么：全仓库原先 grep 不到 `SHCSR`。M2+ 靠 FreeRTOS `port.c:742` 顺带
> 打开了 MemManage，但 M0/M1 是裸机、没有 scheduler，MemManage 一直是关的，
> 所有越界都静默升级成 HardFault。现在这一位在 MPU 使能的同一个地方一起置了，
> 两种构型都覆盖。

在 `mdl_memmanage_handler_c` 断点上，Watch 窗口加 `g_mdl_last_fault`：

```
occurred    = true
pc          = main.c:46 那条 ldrb 的地址
mmfar       = 0x20018000
cfsr        bit1 DACCVIOL = 1, bit7 MMARVALID = 1
text_offset = 0xFFFFFFFF     <- 正确: slot 是 EMPTY，不是模块 fault
```

判定：

- ✅ **停在 MemManage handler 且上面几项对得上** → MPU 编程正确、隔离真实生效，
  MemManage 使能也正确。M0 完全通过。
- ⚠️ **停在 HardFault，`HFSR(0xE000ED2C)` bit30 `FORCED` = 1，且 CFSR/MMFAR 同上**
  → MPU 拦住了，但 MEMFAULTENA 没生效。隔离是好的，B1 的修复没起作用，回头查。
- ❌ **没有 fault，直接跑到断点 2 的 bkpt 循环** → MPU 没生效，真失败。先查
  `MPU_CTRL(0xE000ED94)` 是否 = `0x7`，再查 region 7 的 RASR。

之后它会走 `arch_system_reset()` 无限复位——这是**正确行为**（host 侧 fault
不可恢复），不是 bug。要看清楚就在 `fault_arm.c` 的 `arch_system_reset()` 调用上
打断点。

> M1 起还会先跑 `board_clock_init()`。**能走过那一行本身就证明 24MHz 晶振起振、
> PLL 锁定了**——它内部两个 `while` 在失败时会死等。M0 不调它，仍跑 48MHz HICK。

---

## 测试 B — M1: 加载 + GOT 重定位 + 模块执行

把 `mdl/mdl.jdebug` 里的 `File.Open` 改成 `at32f435_m1/build/firmware.elf`
（或在 Ozone 里 File → Open 直接换 ELF）。下载，复位。

`hello` 模块的 `.mdl` 镜像（288 字节）已经 objcopy 进固件了。
从 packer 头里解出来的真实数值：

```
text_size   180 B      data_size 4 B     bss_size 0 B
got_count   7          reloc_count 4
init_off    1          -> module_init 入口 = 0x20010001 (thumb)
```

加载后的运行时布局：

```
0x20010000  模块 .text (180B)
0x20014000  GOT, 7 slots = 28B
0x2001401C  模块 .data = g_counter (4B)   <- 唯一的观测点
```

> 注: `g_last_level` 在 `-O1` 下被判定为只写不读，已被 GCC 删除，所以 `bss_size = 0`。

### 断点

1. `at32f435_m1/main.c:36`  —— `mdl_load()` 刚返回
2. `at32f435_m1/main.c:45`  —— 最终的 `for(;;) bkpt`

### 期望结果

**停在断点 1 时：**

- Watch `st` == `MDL_LOAD_OK` (0)。如果不是，`mdl_load_status_str(st)` 会告诉你具体哪一步挂了
  （magic / crc32 / abi / text 太大 / reloc 越界）
- Memory 窗口看 `0x20010000`：应该有 180 字节非零的 Thumb 代码（不是全 0/全 FF）
- Memory 窗口看 `0x20014000`：28 字节 GOT，**每个 slot 都应该是 `0x2001xxxx`**
  （已经加过运行时基址了）。如果看到 `0x00000000`~`0x000000FF` 这种小值，说明重定位没跑
- Memory 窗口看 **`0x2001401C`**：应该是 `01 00 00 00`（g_counter 的初值 1，模块还没跑）

**停在断点 2 时（模块已执行完）：**

- **`0x2001401C` 变成 `2A 00 00 00`（= 42）** ← **这就是 M1 的判定标准**

  这一个值同时证明了四件事：模块代码被正确拷进 SRAM 并执行了；GOT 重定位算对了
  （否则 `g_counter += 41` 会写到别的地址去）；r9 单基址 PIC 约定成立；host vtable 调用没崩。

- 如果 Ozone 的 Terminal 窗口开着 semihosting，还会看到三行：
  ```
  ok
  hello module: module_init running
  hello module: counter ok
  ```
  看不到不算失败——`host_api.c` 的 log 走的是 semihosting `SYS_WRITE0`（`bkpt 0xAB`）。
  如果 Ozone 没接管 semihosting，CPU 会停在那条 `bkpt` 上，此时按继续即可，
  内存判定标准不受影响。

### 失败排查

| 现象 | 大概率原因 |
|---|---|
| `st` = crc32 mismatch | objcopy 嵌入的 blob 和 packer 输出不同步，`make clean && make` |
| `st` = abi mismatch | 模块编译时的 `host_api.h` 和固件的不是同一版 |
| `0x20014000` 的 GOT 全是小值 | `arch_apply_relocs()` 没跑或算错基址 |
| `0x2001401C` 一直是 1 | 模块 text 拷贝了但没执行，查 `init_off` 的 thumb 位 |
| 跑飞/HardFault，PC 在 `0x2001xxxx` | 模块 text 区被 MPU 配成了不可执行，查 region 6 的 XN 位 |

---

---

## 测试 C — M4: USB CDC 终端（插 USB1，Windows 上开串口终端）

把 `mdl/mdl.jdebug` 的 `File.Open` 指到 `at32f435_m4/build/firmware.elf`，
下载，**让它跑**（不要停在断点上，USB 需要持续响应）。

### 接线

- **USB1**（靠 U2/SRV05-4A 那个口）→ PC。这是主芯片自己的 USB。
- USB2 是板载 DAP 的口，和这个测试无关。
- J-Link 仍接 H2，`DAP_CONF` 仍要接地。

### PC 侧

设备管理器里应该出现一个新的 **COM 口**（Artery CDC）。用 Windows Terminal、
PuTTY、或者 `mdl/tools/watch.py` 都行。**波特率随便填**——CDC 的波特率是虚拟的，
固件根本不看。

回车后应该看到：

```
=== mdl console (AT32F435VCT7 / UYUP-RPI-A-2.4) ===
type `help`. binary .mdl pushes share this same port.
mdl>
```

### 建议的验证顺序

| 命令 | 期望 | 说明 |
|---|---|---|
| `clk` | `system_core_clock = 288000000 Hz` + `(PLL from 24MHz HEXT ...)` | 时钟链全对。若显示 48000000 说明 `board_clock_init()` 没跑 |
| `arena` | text `0x20010000`、data `0x20014000`、guard `0x20018000` | 和 M0/M1 的地址一致 |
| `pins` | 列出 4 个引脚 | 模块能碰的白名单 |
| `led 0 0` | `ok`，**LEDB 亮** | LED 是低有效 |
| `led 0 1` | `ok`，LEDB 灭 | |
| `btn 2` | `1 (idle)`；按住 BTN0 再敲 → `0 (pressed)` | |
| `status` | `slot : EMPTY` / `fault : none on record` | |
| `mem 0x20014000 8` | 8 个字（此时应全 0） | 加载模块后再看就是 GOT |

然后用 `tools/watch.py` 推一个模块进去，再回终端敲 `status` / `mem 0x2001401c 1`
—— 应该看到 `0x0000002A`。**文本终端和二进制推送共用同一个口，互不干扰**。

### 这是怎么共用一个口的

`mdl_proto_rx_byte()` 做逐字节解复用：能接上 `MDLC` 魔数前缀的字节被暂存，
凑齐 4 个就进二进制帧解析；前缀一断，暂存的字节按序放给行编辑器。
魔数是大写，所有命令是小写，所以正常打字一个字节都不会被暂存。

### 排查

> ### ⚠️ 必须用 A-to-C 线（2026-09-06 实测确认）
>
> **C-to-C 线接 USB1 完全没有反应**——主机侧连"未知设备"都不出现。
> 换成 **A-to-C 线立刻枚举成功**。
>
> 原因是这块板的 USB-C 座作为 device 缺少 CC 上的 5.1k 下拉（Rd）：
> Type-C 主机靠 CC 上的 Rd 判断"对面接了个设备"才供 VBUS 并开始枚举，
> 没有 Rd 就当什么都没插。A 口没有 CC 这套协商机制，所以 A-to-C 不受影响。
>
> 这不是固件问题，任何固件都救不了——**排查 USB 时先换线，再怀疑代码。**

| 现象 | 查什么 |
|---|---|
| Windows 完全没反应（连"未知设备"都没有） | **先换 A-to-C 线**（见上方警告，已实测确认）。排除线之后再查下面几项 |
| 出现"未知设备"/枚举失败 | 八成是 48MHz 不对。先在 Ozone 里断下来看 `system_core_clock` 是不是 288000000 |
| 枚举成功但敲什么都没回显 | 检查 D+/D- 到底焊在哪一组 0Ω 上（见下面附录）。也可能是终端把行结束符设成了别的 |
| 能回显但命令无响应 | supervisor 任务没跑起来，Ozone 里看任务列表 |
| 枚举成功但 LED 不亮 | 试 `led 0 1`——LED 极性是从原理图网表推的，没实测过 |

---

## 覆盖范围

| | M0 (A) | M1 (B) | M4 (C) |
|---|---|---|---|
| MPU 区域编程 + 越界拦截 | ✅ | ✅ | ✅ |
| MemManage 使能（B1） | ✅ | ✅ | ✅ |
| 24MHz 晶振 + PLL 288MHz | — | ✅ | ✅ |
| 加载 + GOT 重定位 | — | ✅ | ✅ |
| 模块执行（特权） | — | ✅ | — |
| 模块执行（**非**特权，B2） | — | — | ✅ |
| FreeRTOS 任务切换 | — | — | ✅ |
| USB 枚举 + CDC 收发 | — | — | ✅ |
| fault 恢复 / 看门狗 | — | — | ✅（要推 `fault_*` 模块） |

QEMU 上那个 PendSV/MemManage 问题在测试 C 里见分晓：M4 一旦能跑到打印 banner，
就说明调度器在真硅片上是好的，那个 bug 是 mps2-an386 的模型差异。

---

# 附: 从原理图核对出来的板级事实

## USB —— M4 的代码和板子对得上 ✅

板上有**两个** USB-C：

| 位置 | 接到哪 | 用途 |
|---|---|---|
| **USB1** | `USB_D+`/`USB_D-` → 主 MCU（经 U2 SRV05-4A ESD） | 主芯片自己的 USB，M4 的 CDC 走这里 |
| **USB2** | `JTAG_DP`/`JTAG_DN` → U3 STM32F042 | 板载 CMSIS-DAP 调试器 |

`USB_D+`/`USB_D-` 到主 MCU 的走线是**0Ω 跳线可选的两组**（板子要兼容多种 MCU）：

- `R69/R70` ↔ `P_70`/`P_71` = LQFP100 的 **70/71 脚 = PA11/PA12**
- `R71/R73` ↔ `P_53`/`P_54` = LQFP100 的 **53/54 脚 = PB14/PB15**

网络名 `PB14_PA11` / `PB15_PA12_USB_ON` 就是这个二选一的意思。

**`at32f435_m4/inc/usb_conf.h` 用的是 `OTG_USB_ID = 1` → OTGFS1 →
`OTG_PIN_GPIO = GPIOA`、`DP = PA12`、`DM = PA11`。**

AT32F435 的 OTGFS1 正好在 PA11/PA12，和 F103/F4 一致 —— **所以 M4 的配置是对的**，
条件是板上焊的是 `R69/R70` 那一组（PA11/PA12）而不是 `R71/R73`（PB14/PB15）。
**上板前用万用表通断档确认一下这两组 0Ω 哪组在位。** 如果焊的是 PB14/PB15，
就得把 `usb_conf.h` 改成 `OTG_USB_ID = 2`（AT32F435 的 OTGFS2 在 PB14/PB15）。

### USB1 是 device，不是 OTG

USB-C 连接器上只有 `D+`/`D-` 经 U2 走到 MCU。**VBUS 没有接到任何 MCU 引脚**，
没有给下游供电的 5V 开关，USB-C 本身也没有 ID 脚（角色靠 CC 协商，而 CC 也没接
到 MCU）。所以外设虽然是 OTGFS（硬件支持 OTG），**这块板上只能做 device**。

更要命的是：厂商模板给 OTG 分的 VBUS/ID 是 **PA9/PA10**，而这块板的 PA9/PA10 是
`PA9_U1_TX`/`PA10_U1_RX`，接去板载 DAP 的 USB 转串口了。**给它们配 MUX 就等于
把调试串口废掉。** 这条已经写进 `usb_conf.h` 的注释里，`USB_VBUS_IGNORE` 是永久
正确设置，不是占位。

### 一个待实测项

`PB15_PA12_USB_ON` 网络上挂着 `U9 (SS8550 PNP)` + `R42 1.5kΩ`。这是给
**STM32F103 补 D+ 上拉**用的（F103 没有内部上拉）。AT32F435 的 OTGFS **有内部
上拉**，这个外部 1.5k 可能多余甚至干扰枚举。M4 枚举不出来先怀疑这里。

## 时钟（已处理）

- **X2 = 24MHz**（C11/C12 = 15pF）接 `OSC_IN`/`OSC_OUT`，是主 MCU 的 HEXT
- **X1 = 32.768kHz**（C7/C8 = 6pF，R80/R81 1MΩ 标了 NC）接 `OSC32_IN`/`OSC32_OUT`
- `HEXT_VALUE` 已改成 `24000000`
- `board_clock_init()`（`mdl/tests/hil/common/board_clock.c`）做 24MHz → 288MHz，
  M1/M4 在 `main()` 第一行调用；M0 不调，保持最小依赖跑 48MHz HICK
- USB 48MHz 从 288MHz PLL 分频（`CRM_USB_DIV_6`），**不再**用原来的 HICK+ACC
  无晶振方案——那是在还不知道板子有没有晶振时写的，现在知道有真晶振，PLL 派生
  比拿 SOF 去 trim 内部 RC 精确

## 另一条串口：板载 DAP 的 UART 桥

板载 DAP（U3 STM32F042）除了调试，还桥接了 `PA9_U1_TX` / `PA10_U1_RX`
到 USB2 —— 插 USB2 就有一个 USB 转串口，接在主芯片的 USART1 上。

现在 M4 的终端走的是主芯片自己的 USB CDC（USB1），所以这条不是必需的。
但它是一条**独立于 USB1 的**后备通道：如果 USB1 枚举一直调不通，
`transport/protocol.c` 本来就写了"transport-medium-agnostic，未来可以喂给
debug UART"，把 `mdl_transport_write()` / RX 换成 USART1 就能整套搬过去，
console 和帧协议都不用改。

> 待实测：`DAP_CONF` 接地停用板载 SWD 时，这个串口桥是否还工作。
> 如果不工作，J-Link 调试和这条串口不能同时用，那时改用 RTT（TODO E2）。
> —— 注意这只影响这条后备通道，**USB1 上的 M4 终端不受影响**。

## 板上其它可用外设（给 F1 回调模型和 P5 benchmark 备用）

| 外设 | 引脚 |
|---|---|
| LED | `PD10`(LEDB)、`PE15`(LEDG) —— **GPIO 白名单的首选测试目标** |
| 按键 | `PA3`(BTN0)、`PE2`(BTN1) —— 中断回调（F1）的天然测试源 |
| WS2812 RGB | `PB1` (LED5/LED6 串联) |
| SPI Flash | W25Q32，`PE3`=NCS，SPI3 = `PB3`/`PB4`/`PB5` —— **F2 模块持久化的落点** |
| EEPROM | AT24C64，I2C1 = `PB8`/`PB9` |
| TF 卡 | SDIO：`PC8-PC12`/`PD2`，`PE14`=检测 |
| CAN | TJA1042，`PD0`(RX)/`PD1`(TX) |
| RS485 | SP3485，USART3 = `PD8`(TX)/`PD9`(RX)，`PA0`=DE |
| BOOT0 | H1 跳线（`PZ2.0-1*2`），刷砖了从这里救 |
| 扩展口 | U5/U6 两个 `PZ2.54-2*20`（树莓派 40pin 形态） |
