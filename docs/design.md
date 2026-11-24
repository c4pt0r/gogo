# gogo 设计文档（草案）

> 项目名 gogo。日期 2026-11-23。状态：待用户确认。

## 1. 意图

在单片机和小型 Linux 设备（如带 Whisplay 小屏的 Raspberry Pi）上，用 **Go 语法** 写一组相互隔离的小 actor，来管理传感器、屏幕、按键等外设。借鉴 AtomVM 的核心思想（轻量进程、每进程独立堆、邮箱、按 reduction 抢占、link/monitor/监督树、"让它崩溃"），但 **不兼容 BEAM**，从语言到字节码到内存布局都自己设计。

同时这是一个研究项目：整套系统要小到一个人能完整读懂，设计干净优先于功能多。

## 2. 目标 / 非目标 / 反目标

**目标**
- 用 Go 语法写 actor 代码；编辑时 gopls 能补全和报错（通过 gogo 提供的 stub 包和 32 位类型设置）。程序 **只在 gogo VM 上运行**，不要求能用 `go run` 运行（用户决定）。
- actor 之间真正隔离：一个 actor 崩溃、死循环或耗尽自己的内存上限，不影响其他 actor 和 VM。
- 能跑在 RP2040（Raspberry Pi Pico，264KB RAM，2MB flash，Cortex-M0+）上并驱动真实外设。
- 同一个 VM 能在 macOS / Linux 上运行（开发调试用，也用于 Pi）。
- 代码量可读：VM 目标 ≤ 5k 行 C，编译器目标 ≤ 5k 行 Go，均不含测试。

**非目标（v1 不做）**
- 完整 Go 兼容；channel、泛型、反射、`defer`/`recover`、用户自定义接口的方法调用、浮点数、goroutine 间共享内存。
- 热更新代码、分布式、多核调度、JIT。
- 兼容 Go 标准库；只提供一个很小的自有标准库。

**反目标（做到了也算失败）**
- 变成一个"Go 编译器复刻"，为追兼容不断堆特性。
- 出现任何 actor 之间共享可变状态的途径。
- 任何单个 actor 的错误能让整个 VM 崩溃。
- 靠宏技巧、难读的代码换性能（对照 AtomVM 的 `opcodesswitch.h`）。

## 3. 约束

- **VM 用纯 C（C99）写，不用第三方库**（用户要求）。只依赖 C 标准库和 POSIX；唯一例外是 MCU 平台层使用芯片厂商 SDK（RP2040 用 Pico SDK），且只出现在 `platform/rp2040` 目录。VM 的测试框架自己写（一个很小的断言和用例运行器）。
- **电脑上的工具（编译器 `gogo`）用 Go 写，只用 Go 标准库**，同样不引入第三方模块（用户要求）。
- VM 同一份源码在 MCU 和 Unix 上编译。
- 最小目标硬件：RP2040。VM 固件（-Os）目标 ≤ 64KB flash；**每个 actor 的内存占用要尽可能小，不大于 AtomVM**：空闲 actor（阻塞在 `Receive`、没有待处理消息）目标 ≤ 160 字节（32 位），硬上限 256 字节；RP2040 上应能同时容纳 1000 个空闲 actor。对照基线：AtomVM 空闲进程 `process_info(memory)` 报 552 字节（64 位），实测 2 万个空闲进程平均约 1KB RSS（含 malloc 开销），32 位下估计约 300～500 字节。
- 编译器在电脑上运行；设备上不含编译器。
- 程序镜像存在 flash 中，原地执行（XIP），常量不拷进 RAM。

## 4. 方案选择

考虑过三种做法：

| 方案 | 说明 | 取舍 |
|---|---|---|
| **A. Go 子集 → 自有字节码 → C VM（已选）** | 编译器用 Go 写，前端直接用标准库 `go/parser` + `go/types` | Go 语法 + 真隔离 + 能上 MCU + 可完整读懂；代价是只支持子集、标准库自己写 |
| B. TinyGo，goroutine 当 actor | 写个监督树库即可 | 无隔离、协作式调度，与"带隔离"冲突 |
| C. TinyGo → wasm，每个 actor 一个 WAMR 实例 | 完整 Go、真隔离 | 每实例几十 KB，RP2040 放不下几个；VM 不是自己的，研究价值低 |

此前还讨论过兼容 BEAM（精简 AtomVM）和自创类 Erlang 语言，用户最终选择"不兼容 BEAM、用 Go 写"。

## 5. 设计

### 5.1 组成部分

1. **编译器 `gogo`（命令行工具，Go，只用标准库）**：`go/parser` 解析 → `go/types` 类型检查（32 位尺寸）→ 子集检查 → 生成字节码镜像 `.gogo`。
2. **VM**（C）：镜像加载、解释器、调度器、每 actor 堆与 GC、邮箱、定时器、端口（外设驱动）接口。
3. **`gogo/actor`、`gogo/port` 等内建包**：只有一份。Go 源码里是只含签名的 stub（供 `go/types` 和编辑器里的 gopls 使用，函数体为 `panic("gogo only")`）；编译器把对它们的调用识别为 VM 内建操作。
4. **小标准库**（用 Go 子集写，导入路径都在 `gogo/` 下，不冒充 Go 标准库路径）：`gogo/supervisor`、`gogo/fmt`（只支持 `%d %s %v %x` 和基本类型、结构体的简单打印）、`gogo/strings`、`gogo/bytes` 等。
5. **平台层**：`unix`（含模拟外设）和 `rp2040`；之后可加 ESP32 和 Linux 真实外设（Whisplay）。

### 5.2 Go 子集（v1）

支持：
- 类型：`bool`、`int`（固定 32 位；编译器以 `types.StdSizes{WordSize:4, MaxAlign:4}` 做类型检查，常量越界在编译期报错；运行时溢出按二进制补码回绕，和 Go 语义一致）、`int64`、`uint8/byte`、`uint32`、`string`（不可变字节串）、数组、切片、`map`（键限 int/string）、结构体、指针（仅在本 actor 内有意义）、`any`、函数值和函数字面量（闭包）、结构体方法（静态分派）。
- 控制流：`if`、`for`（含 `range`）、`switch`、类型 switch 与类型断言、`break/continue/return`、`const`/`iota`。
- 包：多文件、多包；`const` 不限。包级 `var` 的初始值必须是编译期可求值的常量数据（字面量、复合字面量），编进镜像放在 flash 中，**只读**。VM 的每条写内存指令都检查目标是否在当前 actor 自己的堆里，所以经由别名写入（`s := tbl; s[0] = 1`）在运行时报错退出，不需要编译器做别名分析。
- `panic(v)`：使当前 actor 以原因 `v` 退出。
- 内建 `error` 接口作为唯一的特例支持（`gogo/errors.New`、`.Error()`），其余用户接口的方法调用不支持。

不支持（编译时报清楚的错误）：channel 与 `select`、`go` 语句（改用 `actor.Spawn`）、泛型、`defer`/`recover`、反射、`unsafe`、浮点、用户自定义接口的方法调用、可变的包级变量。

### 5.3 actor API（`actor` 包）

M2 实际实现的 API（权威定义见 `actor/actor.go`）：

```go
type PID struct{ /* opaque */ }
type Ref struct{ /* opaque，可 == / != 比较 */ }
type Reason struct{ Code int; Func string }   // 退出原因，见 §5.8
type Options struct{ MaxMemory int }          // 字节；0 = 平台默认

func Spawn(f func()) PID
func SpawnLink(f func()) PID                    // 创建并原子地链接
func SpawnOpt(f func(), opts Options) PID       // 带内存上限
func Self() PID
func Send(to PID, msg any)
func Receive() any                              // FIFO 取下一条
func ReceiveTimeout(ms int) (msg any, ok bool)  // ms == 0 只查看不等待
func Call(to PID, req any, timeoutMs int) (reply any, ok bool) // 带 Ref 的请求-应答
func Reply(req Request, reply any)              // Call 的服务端
func Link(p PID); func Unlink(p PID)
func Monitor(p PID) Ref; func Demonitor(r Ref)
func TrapExit(on bool)
func Exit(code int)                             // 以原因码退出
func Kill(p PID, code int)                      // 向别的 actor 发 exit 信号
func MakeRef() Ref
func Register(name string, p PID) bool          // 名字被占用或 p 已退出时 false
func Whereis(name string) (PID, bool)
func Unregister(name string)
func SendAfter(ms int, to PID, msg any) Ref; func CancelTimer(r Ref) bool
func Sleep(ms int); func Now() int64            // VM 启动以来的毫秒数
func Info(p PID) ProcInfo

// 系统消息
type ExitMsg struct{ From PID; Reason Reason }
type DownMsg struct{ Ref Ref; From PID; Reason Reason }
type Request struct{ From PID; Ref Ref; Body any }
```

原因码常量：`Normal`、`Killed`、`OutOfMemory`、`MailboxOverflow`、`NoProc`、`DivZero`、`BadCode`、`Shutdown`，用户码从 100 起。`error` 类型的退出原因留到语言里程碑（见 [M2 设计](superpowers/specs/2026-11-23-gogo-m2-design.md) §5.2、§7）。

接收侧用类型 switch 代替 Erlang 的模式匹配：

```go
for {
    switch m := actor.Receive().(type) {
    case ButtonPressed:
        count++
        actor.Send(screen, ShowText{fmt.Sprint(count)})
    case actor.Request:
        actor.Reply(m, count)
    }
}
```

**选择性接收**只做一种：`Call` 等待带特定 Ref 的应答，其余消息留在邮箱里。不做任意条件的选择性接收（避免 O(n) 扫描和语义复杂度）。

**发送语义**：`Send` 对消息做深拷贝（含切片、map、指针指向的数据），接收方拿到的是独立副本。这与 Go 的别名语义不同，是隔离的代价，文档中明确写出。
- 拷贝 **保留内部共享和环**（两个字段指向同一对象，拷贝后仍指向同一副本）。实现上复用 GC 的复制算法：拷贝时在源对象上临时写转发标记，拷贝完再恢复，不需要额外的 visited 表。
- PID、Ref、端口、冻结的 `Bytes`、flash 中的常量原样传递，不拷贝。
- 闭包可以随消息发送（函数 + 捕获环境一起深拷贝）。

**Spawn 的闭包**：被捕获的变量在 spawn 时深拷贝进新 actor 的堆。

### 5.4 端口（外设）

外设统一建模为"端口"：一个由 C 驱动实现的特殊 actor，有 PID，收命令消息、向所有者发事件消息。

```go
led := port.Open("gpio", port.GPIO{Pin: 25, Mode: port.Output})
actor.Send(led, port.Write{Value: 1})
btn := port.Open("gpio", port.GPIO{Pin: 15, Mode: port.InputPullUp, Edge: port.Falling})
// 之后本 actor 会收到 port.Edge{...} 消息
```

v1 驱动：`gpio`、`uart`（含控制台日志）、`i2c`、`spi`、`display`（帧缓冲区整块刷新，帧缓冲用大二进制传递）。
驱动接口是一张 C 函数表（open / command / close / poll），新增外设不用改 VM 核心。
中断服务程序只往无锁环形缓冲区写事件；调度器在切换时取出并转成消息。ISR 中不加锁、不分配内存。

unix 平台的模拟外设：`gpio` 输入映射到键盘按键，输出打印到终端；`display` 输出到终端或定期写 PPM 文件。

### 5.5 值表示与内存

- **统一的 32 位标记字**（宿主和 MCU 相同）。小整数直接放在字里（约 30 位，`int` 和 `int64` 都是），超出范围的值自动装箱；`int64` 放进 `any` 时总是装箱（编译器按静态类型选 `*64` 操作码，并在转换处插入 `BOX64`），这样类型 switch 仍能区分 `int` 与 `int64`；其余复合值是指向 actor 堆的引用。
  - 选标记字而非"依赖静态类型的裸值"：GC 不需要编译器生成栈映射表，VM 更简单易读；代价是部分整数要装箱。
- **堆内引用用"字偏移"而非原生指针**，所以 64 位宿主上的内存布局与 MCU 完全相同，镜像和调试工具可通用。
- 结构体在堆上带头部，含类型 ID（供类型 switch 和类型断言用）。
- **每个 actor 一块连续内存**，内含堆和栈帧。空间不够时做 Cheney 复制式 GC，同时按策略扩大或缩小；每个 actor 有可配置的内存上限，超过就以 `out_of_memory` 退出。
- **大块数据零拷贝只走一种类型**：`gogo/bytes.Buffer`（可写、属于当前 actor）调用 `Freeze()` 后变成不可变的 `bytes.Bytes`，所有权转移，原 Buffer 失效，不复制内容。`Bytes` 超过 64 字节时放在堆外、引用计数，发送时只传引用。帧缓冲就这样构建后发给 `display` 端口。普通 `[]byte` 和 `string` 发送时照常拷贝（小字符串常量在 flash 中不拷贝）。
- **消息**：发送时深拷贝到单独申请的消息块并挂到接收方邮箱；`Receive` 时再并入接收方的堆。VM 单线程，无需原子操作。
- **内存记账**：排队中的消息计入 **接收方** 的内存配额（堆 + 邮箱）。接收方超出上限时以 `MailboxOverflow` 退出（由监督者决定是否重启），发送方不受影响。
- **整机内存耗尽**：分配失败时，杀死正在尝试分配的那个 actor（原因 `OutOfMemory`）。VM 预留一小块应急内存，保证在这种情况下仍能完成退出处理、发出 exit/down 消息，VM 自身不崩。
- 镜像中的常量（字符串、常量表）直接引用 flash，不拷贝。
- **GC 根与死寄存器**：GC 扫描每个活动栈帧的全部寄存器，所以已经死掉但仍存着堆值的寄存器也会把对象留住。编译器在每条语句结束（释放的临时寄存器、被丢弃的调用结果，块或循环结束时还有其中的局部变量）和每次循环回跳之前插入 `CLEAR a n`（`ra..ra+n-1 = nil`），按区间合并，每个连续区间一条；跳转把"待清除"标记带到目标标签，`break`/`continue`/条件跳转离开的路径同样会被清掉。只存放从不为堆对象的值（bool、PID、常量）的寄存器不必清除。寄存器分配是词法作用域式的，低于当前分配点的寄存器都是外层仍然活着的局部变量，永远不会被清掉。

**值表示（按静态类型）**

| 静态类型 | 零值 | term 表示 | 转换进 `any` | GC 扫描 | 消息拷贝 |
|---|---|---|---|---|---|
| `int` | 小整数 0 | 31 位内为小整数立即数；超出为 `K_INT` 堆盒 | 原样（`TYPEIS` → `int`） | 立即数不扫描；`K_INT` 盒被复制，内部不扫描 | 立即数原样；盒深拷贝 |
| `int64` | 小整数 0 | 31 位内为立即数；超出为 `K_INT64` 盒（堆或常量区） | 总是 `BOX64` 成 `K_INT64` 盒（常量在编译期放进常量区），以便与 `int` 区分 | 同 `int` | 同 `int`；常量区盒原样 |
| `bool` | `T_FALSE` | 特殊常量 `T_FALSE` / `T_TRUE` | 原样 | 不扫描 | 原样 |
| `string` | `T_EMPTYSTR` | `K_STRING` 堆对象，或常量区引用 | 原样 | 堆串被复制（字节不扫描）；常量区引用不动 | 堆串深拷贝；常量区串原样，不拷贝 |
| 结构体（含 `actor.*`、`port.*` 保留类型） | 各字段为其零值的 `K_STRUCT` | `K_STRUCT` 堆对象 `[hdr][type id][字段...]` | 原样（`TYPEIS` 比较 type id） | 字段逐个扫描 | 深拷贝，保留共享与环 |
| `any` | `T_NIL` | 所装动态值本身的 term | — | 按动态值 | 按动态值 |
| `func()` | `T_NIL`（`Spawn` 拒绝：`badcode`） | `K_CLOSURE` 堆对象 `[hdr][函数号][捕获...]` | 原样 | 捕获逐个扫描 | 深拷贝 |
| `actor.PID` | `T_ZEROPID` = `0x6`（槽 0、代 0：槽 0 从不使用代 0，端口槽 ≥ `0xFFF0`，所以不等于任何活着的 actor 或端口） | PID 立即数 `(代 << 20) \| (槽 << 4) \| 0x6` | 原样（`TYPEIS` → `PID`） | 不扫描 | 原样 |
| `actor.Ref` | `T_ZEROREF` = `0x42` 特殊常量，序号视为 0 | `K_REF` 堆对象 `[hdr][serial 低][高]`，序号从 1 开始 | 原样（`TYPEIS` → `Ref`，零值也是） | `K_REF` 被复制，内部不扫描；`T_ZEROREF` 不扫描 | `K_REF` 深拷贝（序号不变）；`T_ZEROREF` 原样 |

零值 PID / Ref 的语义：`Send` 到零 PID 被丢弃；`Link` / `Monitor` 零 PID 与对已死 PID 相同（`NoProc`）；`CancelTimer` / `Demonitor` 零 Ref 什么也不做（返回 `false`）；零值彼此相等，与任何活着的值都不等；`println` 打印为 `<pid 0>` / `<ref 0>`。查不到名字的 `Whereis`、失败的 `port.Open` 返回零 PID。

### 5.5.1 每个 actor 的内存预算（核心约束）

空闲 actor 的内存由三部分组成，目标（32 位）：

| 部分 | 目标 | 做法 |
|---|---|---|
| actor 控制块 | ≤ 48 字节 | 只放：pc、栈帧指针、堆起止与分配指针、邮箱头尾、id、状态与标志、内存配额、指向"附加信息"的指针。不像 AtomVM 那样在控制块里内嵌 17 个 x 寄存器、异常字段、进程字典等 |
| 堆 + 栈 | 初始 ≤ 96 字节 | 寄存器放在栈帧里（属于该 actor 的内存块），不在控制块里。初始大小 = 入口函数的帧大小 + 闭包环境 + 一点余量，编译器在镜像里给出每个函数的帧大小，按需增长 |
| 附加信息 | 0（不用就不占） | link、monitor、注册名、定时器、`TrapExit` 等放在单独按需分配的小结构或全局表里，绝大多数 actor 一个字节都不付 |

配套规则：
- **空闲时收缩**：actor 阻塞在 `Receive` 且栈只剩入口帧时，若堆的空闲比例过高，下一次 GC 把内存块缩回最小尺寸（类似 Erlang 的 hibernate，但自动进行）。
- **无隐藏的每 actor 开销**：调度队列、邮箱都用控制块内的侵入式链表，不额外分配节点。消息块本身计入接收方配额。
- **内存分配器**：VM 自带一个面向小块的分配器（按尺寸分级的空闲链表），避免依赖平台 malloc 每块 8～16 字节的头部开销。
- **可观测**：`actor.Info(pid).Memory` 返回该 actor 的总字节数（控制块 + 内存块 + 附加信息 + 排队消息），与 AtomVM 的 `process_info(memory)` 口径可比。

### 5.5.2 内存布局（草案，32 位）

**值（term，32 位）** —— 低位标记：

```
xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxx1   小整数，31 位有符号（超出范围自动装箱）
oooooooo oooooooo oooooooo oooooo00   堆引用：本 actor 内存块内的字偏移（不是指针）
iiiiiiii iiiiiiii iiiiiiii iiii0010   特殊常量：nil / false / true / 空串 / 零 Ref（/ 空切片）
iiiiiiii iiiiiiii iiiiiiii iiii0110   PID：16 位槽号 + 12 位代数（槽 0 代 0 = 零 PID）
oooooooo oooooooo oooooooo oooo1010   flash 常量引用：镜像内偏移（字符串常量、只读包级变量）
iiiiiiii iiiiiiii iiiiiiii iiii1110   堆外大数据句柄：全局引用计数表的下标（冻结的 Bytes）
```

堆引用是相对内存块起点的偏移，所以内存块可以整体搬家（GC、扩容、收缩），消息块并入堆时只需给块内所有引用加同一个常数。

**actor 控制块（32 位目标上 48 字节，字段顺序以 `vm/actor.h` 为准）**

```
偏移  大小  字段
 0    4    next         调度队列 / 空闲链表（侵入式）
 4    4    mem          内存块起始地址
 8    2    mem_words    内存块大小（字），单 actor 上限 64K 字 = 256KB
10    2    hp           堆顶（字偏移，向上长）
12    2    sp           栈顶（字偏移，向下长）
14    2    fp           当前栈帧（字偏移）
16    4    pc           指向 flash 中的字节码
20    4    mbox_head    消息链表头
24    4    mbox_tail    消息链表尾
28    4    extra        附加信息指针，绝大多数为 NULL
32    4    id           PID（槽号 + 代数）
36    1    state        RUNNABLE / WAITING / EXITED
37    1    flags        trap_exit、has_extra、...
38    2    reductions   本时间片剩余配额
40    2    mbox_words   排队消息总大小（计入配额）
42    2    max_words    内存配额（0 = 默认）
44    4    reserved     未用（定时器在 Vm.timers 里）；保持 32 位上 48 字节
```

另有全局 actor 表：每个槽 4 字节（指向控制块），PID 的槽号索引它，代数防止旧 PID 误投。

**内存块（堆 + 栈共用一块）**

```
mem[0]                                                        mem[mem_words]
├──────────────────────────┬──────────────┬─────────────────────────────┤
│ 堆对象 ...           → hp │    空闲区     │ sp ← ... 栈帧 n ... 栈帧 0   │
└──────────────────────────┴──────────────┴─────────────────────────────┘
           向上长                                    向下长
hp 与 sp 相遇 → 触发 GC（Cheney 复制到新块，同时按策略扩容或收缩）
```

**栈帧**（寄存器就在这里，不在控制块里）

```
高地址  ┌──────────────┐
        │ ret_pc       │  以小整数形式存（镜像内偏移）
        │ saved_fp     │  以小整数形式存
        │ r0           │
        │ r1           │  寄存器个数由编译器给出（镜像函数表里的帧大小）
        │ ...          │
低地址  │ r(n-1)       │ ← fp
        └──────────────┘
```

栈上每个字都是合法的 term（返回地址和帧指针也编码成小整数），GC 扫栈时不需要栈映射表，统一处理。

**堆对象**：首字是头部 `[ 大小:26 位 | 种类:6 位 ]`，后跟内容。

```
STRUCT   [hdr][type_id][field0][field1]...           结构体（type_id 供类型 switch）
ARRAY    [hdr][e0][e1]...                             数组 / 切片底层存储（元素是 term）
BYTES    [hdr][len][b0 b1 b2 b3][...]                 字节数据，GC 不扫描内容
SLICE    [hdr][backing][off][len][cap]                切片头（backing 指向 ARRAY 或 BYTES）
STRING   [hdr][len][bytes...]                         运行时生成的字符串
MAP      [hdr][count][k0][v0][k1][v1]...              小 map：有序数组，二分查找
CLOSURE  [hdr][func_idx][cap0][cap1]...               闭包
CELL     [hdr][value]                                 被取地址或被闭包捕获的变量
INT      [hdr][raw32] / [hdr][lo][hi]                 装箱的 int / int64
REF      [hdr][serial_lo][serial_hi]                  actor.Ref
FORWARD  （GC 时临时写入）                            转发标记
```

子集限制（与布局相关）：`&` 只能用于变量和复合字面量，不支持取字段或元素地址（`&s.f`、`&a[i]`）。结构体是值类型，赋值时由编译器生成拷贝。

**消息块**（堆外，由 VM 的小块分配器分配）

```
┌──────┬────────┬───────┬──────┬────────────────────────┐
│ next │ words  │ flags │ root │ 拷贝出来的对象 ...       │
└──────┴────────┴───────┴──────┴────────────────────────┘
  4B      2B       2B      4B     引用是"相对块内"的偏移
```

`Receive` 时把块整体拷到接收方 `hp` 处，块内每个引用加上 `hp`，然后释放消息块。小整数、PID 这类消息（`root` 本身就是值）`words = 0`，没有对象部分。

**附加信息**（只有用到时才分配）

以 `vm/signals.c` 中的 `struct ActorExtra` 为准：待处理退出的原因码与函数名、links、monitors（监视本 actor 的）、watching（本 actor 监视的）、`charged_words`（本 actor 创建的待触发 SendAfter、它的 monitor（两端条目 + 预留 DownMsg）、它那一侧 link（条目 + 预留 ExitMsg）占的字数，与 `mbox_words` 一起经 `actor_charged_words` 计入内存上限）。注册名在 `vm/names.c` 的全局表里，定时器在 `Vm.timers` 里。

**空闲 actor 的账（典型 `for { switch m := actor.Receive().(type) { ... } }` 循环）**

| 项目 | 字节 |
|---|---|
| 控制块 | 48 |
| 全局 actor 表槽 | 4 |
| 内存块：入口帧（2 + 4 个寄存器）+ 闭包环境（约 4 字）+ 余量 | 约 96 |
| 附加信息 | 0 |
| **合计** | **约 148**（目标 ≤ 160） |

### 5.6 字节码与解释器

- 寄存器式字节码，定长 32 位指令字（8 位操作码 + 3 个 8 位操作数），需要时跟扩展字。每个函数有固定数量的寄存器（栈帧大小由编译器算出）。
- 解释器是一个普通的 `switch` 循环，每个操作码一个 case，不用宏生成代码。
- 镜像格式：头部（魔数、版本）、常量池、类型描述表（结构体布局、类型名）、函数表、代码段、可选的行号表。镜像与位置无关，可原地执行。
- 镜像版本（`IMAGE_VERSION`，当前为 2）：操作码编号或语义、builtin 编号、保留类型、term 编码任何一项变化都要升版本；编译器与 VM 同步升级，加载器拒绝其他版本（`unsupported version`）。v1 为 M1/M2；v2 为 M3（通用算术只收 `int`、`*64` 操作码与 `BOX64`、`CLEAR`、有类型的零 PID/Ref）。

### 5.7 调度

- 单个调度器，就绪队列 FIFO。
- 每次函数调用和循环回跳消耗 1 个 reduction，配额用完就切换（初始配额暂定 1000，可调）。因此死循环也会被抢占。
- 无就绪 actor 时，计算最近的定时器截止时间，进入平台等待（MCU 上为 WFI，unix 上为 poll/sleep），被中断或事件唤醒。

### 5.8 错误与崩溃

- 运行时错误（越界、nil map 写入、除零、类型断言失败、写只读内存、超出内存上限、`panic`）都只让当前 actor 退出。
- 退出原因（M2 实现）：`actor.Reason{Code int; Func string}`。`Code` 是 `actor.Normal`（从入口函数返回）、`Killed`、`OutOfMemory`、`MailboxOverflow`、`NoProc`、`DivZero`、`BadCode`、`Shutdown` 或用户码（≥ 100，`actor.Exit(code)` / `actor.Kill(p, code)`）；`Func` 是运行时错误所在函数名，否则为空串。原设想的 `error` 类型原因（`RuntimeError{Kind, Msg, Func, Line}`、`panic(v)`）留到语言里程碑，见 [M2 设计](superpowers/specs/2026-11-23-gogo-m2-design.md) §5.2、§7。
- 退出时向链接的 actor 发 exit 信号（开启 `TrapExit` 则收到 `ExitMsg`，否则原因不是 `Normal` 时级联退出），向监视者发 `DownMsg`。这两种通知的消息块在建立 link/monitor 时就预留好，退出时不分配内存，送达有保证；预留的内存计入建立者自己的内存上限。`Kill(p, Killed)` 不可捕获。
- 崩溃报告打印到 stderr：`gogo: actor <pid N> crashed: <kind>[ in <func>]`，`<kind>` 为 `killed`、`oom`、`mailbox_overflow`、`noproc`、`divzero`、`badcode` 或 `exit(<code>)`；`Normal`、`Shutdown` 不报告。行号留到以后。
- 监督树由标准库 `gogo/supervisor` 包用 Go 子集实现：M2 是单子进程版（重启策略 Permanent/Transient、最大重启强度），多子进程 one_for_one 留到以后。

### 5.9 工具链与工作流

```
gogo build ./app -o app.gogo          # 编译
gogo run app.gogo                      # 在宿主 VM 上运行（模拟外设）
gogo flash app.gogo                    # 生成 UF2 并刷入 Pico（VM 与镜像分区存放）
```

### 5.10 测试

- **黄金输出测试**：一批示例程序在 `gogo run` 下的输出与提交在仓库里的期望输出比对。示例程序的输出只依赖消息因果顺序（例如统一交给一个打印 actor），不依赖调度细节和 map 遍历顺序。这是验证编译器和 VM 语义的主要手段。
- 编译器：子集检查的正例和反例测试。
- VM：GC 压力测试、调度公平性测试、崩溃隔离测试（死循环、内存耗尽、panic）。
- 基准：fib、1 万 actor ping-pong，与 AtomVM 对比。
- **内存占用测试（每次提交都跑）**：创建 N 个空闲 actor，检查 `actor.Info().Memory` 和平台实际内存增量，超过 160 字节目标给警告，超过 256 字节硬上限则测试失败。

### 5.11 里程碑

1. **M1 宿主可跑**：编译器 + VM，跑通 fib 和两个 actor 的 ping-pong；内存占用测试从 M1 起就纳入。
2. **M2 actor 语义完整**（已完成）：link/monitor/定时器/Call/监督树，黄金输出测试通过。
   - 交付：link/monitor/`Kill`/`TrapExit`、退出原因与崩溃报告、定时器与 `ReceiveTimeout`、`Call`/`Reply`、注册名、`int64`、函数值与多包构建、每 actor 内存上限与邮箱配额，以及用 Go 子集写的单子进程 `gogo/supervisor`（两层监督树黄金测试）。详见 [M2 设计](superpowers/specs/2026-11-23-gogo-m2-design.md)，遗留事项见 [m2-followups.md](m2-followups.md)。
3. **M3 Pico**：按键 + LED + 串口日志，在真机上演示"崩溃后被监督者重启"。
4. **M4 小屏**：Linux 平台真实外设，在 Pi + Whisplay 上用 actor 驱动屏幕和按键。

## 6. 交给实现者决定的

- 具体操作码列表与编码细节、镜像格式字段。
- 编译器内部 IR 的形式、寄存器分配算法。
- GC 的扩容/缩容策略具体参数、reduction 配额默认值。
- 源码目录结构。构建：Unix 上用一个普通 Makefile；RP2040 移植因 Pico SDK 要求而用 CMake，只限该平台目录。
- 标准库首批函数的具体范围（以示例程序需要为准）。

## 7. 本轮由 Claude 拍板的决定（请用户过目）

1. 内建包和标准库都放在 `gogo/` 导入路径下，不冒充 `fmt`、`strings` 等标准库路径；`gogo/fmt` 只支持少量格式。
2. `int` 固定 32 位，按 32 位做编译期类型检查，运行时溢出回绕。
3. 包级变量只能用常量数据初始化，放在 flash 中只读，由 VM 写指令检查强制。
4. 大块数据零拷贝只通过 `bytes.Buffer.Freeze()` → `bytes.Bytes`（所有权转移）。
5. 深拷贝保留共享和环；PID/Ref/端口/冻结 Bytes 原样传；闭包可发送。
6. 排队消息计入接收方配额，超限接收方退出；整机内存耗尽时杀死正在分配的 actor，VM 有应急内存。
7. 退出原因为 `gogo/actor` 中的一组具体类型，均实现 `error`；`error` 是唯一支持方法调用的接口。
8. 测试以黄金输出为主，示例程序输出只依赖消息因果顺序。
9. 每 actor 内存预算：控制块 ≤ 48B、初始堆栈 ≤ 96B、附加信息按需分配；空闲目标 ≤ 160B、硬上限 256B（32 位），并作为每次提交都跑的测试。
10. 语言与依赖：VM 纯 C、只依赖 libc/POSIX（MCU 平台层允许芯片 SDK），Unix 用 Makefile，测试框架自写；编译器用 Go，只用标准库。
