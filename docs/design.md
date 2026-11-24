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
4. **小标准库**（用 Go 子集写，导入路径都在 `gogo/` 下，不冒充 Go 标准库路径）：`gogo/supervisor`、`gogo/fmt`（只支持 `%d %s %v %x` 和基本类型、结构体的简单打印）、`gogo/strings`（`Index`/`IndexByte`/`Cut`/`HasPrefix`/`HasSuffix`/`TrimSpace`/`ToLower`/`EqualFold`/`Split`）、`gogo/strconv`（`Itoa`/`Atoi`/`FormatInt64`/`ParseInt64`）、`gogo/bytes`（`Bytes`、`Buffer`，内建）等。strings 与 strconv 是用 Go 子集写的库，按字节工作（无 rune 解码），导入即整体编进镜像（无死函数消除）。库可以导入存根包和其他库（无环，按依赖顺序编译）。M5：`gogo/dhcp`（`dhcp.Start(dhcp.Config{TimeoutMs})`：DHCP 客户端 actor，拥有 `netif`，T1 续租 / T2 重绑定 / 到期清地址；配置项 `net.ip`/`net.mask`/`net.gw`/`net.dns` 时用静态地址）、`gogo/dns`（`dns.Lookup(name, timeoutMs) (int, bool)`：注册名 `dns` 的解析 actor，A 记录，重试 2 次，4 条按 TTL 缓存）、`gogo/http`（`http.Get`/`http.Post`/`http.Do(method, url, contentType, body, maxBody, timeoutMs)`：只支持 `http://`，HTTP/1.0 + `Connection: close`，请求在临时 actor 里完成、调用者按 `Call` 等待，响应体默认上限 `MaxBody` 8 KB、超出截断并标 `Truncated`；`http.Serve(port, handler func(Request) Response) actor.PID`：监听 actor 与调用者 link，监听失败以退出码 100（`ListenFailed`）退出，每个连接 `Handoff` 给新 handler actor，请求头 ≤ 2 KB、体 ≤ 8 KB（按 `Content-Length`，不支持 chunked），畸形请求 400、体过大 413，回 `Content-Length` + `Connection: close` 后 `Shutdown` 退出；临时/handler actor 以 `SpawnOpt` 设内存上限 2×体上限 + 6 KB；响应体按全长记在调用者账上，16 KB 默认上限的调用者要么用更大的 `SpawnOpt`，要么用 `Do` 的小 `maxBody`；响应带 `Transfer-Encoding` 一律失败）、`gogo/config`（存根，`config.Get(key) (string, bool)` 是内建 48，`wifi.` 键对程序不可见；unix 存在 `$GOGO_CONFIG`（默认 `~/.gogo/config`，`key=value` 行），由 `gogo config set|get|list|rm` 编辑）。
5. **平台层**：`unix`（含模拟外设）和 `rp2040`；之后可加 ESP32 和 Linux 真实外设（Whisplay）。

### 5.2 Go 子集（v1）

支持：
- 类型：`bool`、`int`（固定 32 位；编译器以 `types.StdSizes{WordSize:4, MaxAlign:4}` 做类型检查，常量越界在编译期报错；运行时溢出按二进制补码回绕，和 Go 语义一致）、`int64`、`uint8/byte`、`uint32`、`string`（不可变字节串）、数组、切片、`map`（键限 int/string）、结构体、指针（仅在本 actor 内有意义）、`any`、函数值和函数字面量（闭包）、结构体方法（静态分派）。
- 位运算：`&`、`|`、`^`、`&^`、`<<`、`>>`、一元 `^`，及对应的复合赋值（`&=` `|=` `^=` `&^=` `<<=` `>>=`），作用于 `int`、`int64`、`byte`；移位语义同 Go：计数可以是任何整数类型，计数 ≥ 位宽时 `<<` 得 0、`>>` 按符号得 0 或 -1，负计数使 actor 以 `badshift` 退出；`>>` 是算术右移（没有无符号类型，`byte` 除外），`byte` 结果在每次运算后截断。
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

原因码常量：`Normal`、`Killed`、`OutOfMemory`、`MailboxOverflow`、`NoProc`、`DivZero`、`BadCode`、`Shutdown`、`BadShift`，用户码从 100 起。`error` 类型的退出原因留到语言里程碑（见 [M2 设计](superpowers/specs/2026-11-23-gogo-m2-design.md) §5.2、§7）。

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

外设统一建模为"端口"：一个由 C 驱动实现的特殊 actor，有 PID，收命令消息、向所有者发事件消息。以下是 M3 的实际实现（详见 [M3 设计](superpowers/specs/2026-11-23-gogo-m3-design.md) §5.4 与 `port/port.go`）。

```go
led, ok := port.Open("gpio", port.GPIO{Pin: 25, Mode: port.Output})
actor.Send(led, port.Write{Value: 1})
btn, ok := port.Open("gpio", port.GPIO{Pin: 15, Mode: port.InputPullUp, Edge: port.Falling})
// 之后本 actor（所有者）会收到 port.Edge{Pin, Value} 消息
```

- `port.Open(driver string, cfg any) (actor.PID, bool)`：调用者成为所有者；驱动未知、配置无效、资源被占或内存不足时 `ok == false`，返回零 PID。`port.Close(p)` 关闭端口，陈旧或未知的 PID 被忽略。`port.Handoff(p, to) bool`（M5）：所有者把端口交给另一个活着的 actor，之后的事件发给 `to`，`to` 退出时端口关闭，端口的记账（驱动状态、端口代持的 `Bytes`）一起转过去；非所有者调用、`to` 不是活 actor、或 `to` 的配额放不下记账时返回 `false` 且什么都不变；交给自己只是恢复暂停的端口（如刚接受的 TCP 连接）。
- 端口用 `actor.PID` 表示；任何持有 PID 的 actor 都能 `Send` 命令（`port.Write{Value}`），事件（`port.Edge{Pin, Value}`，Value 为变化后的电平）只发给所有者，占所有者邮箱配额。所有者退出时端口自动关闭，VM 结束时关闭所有仍开着的端口；对端口 `Kill` 等同 `Close`。M3 不支持对端口 `Link`/`Monitor`，对端口 `SendAfter`/`Call` 被静默忽略。
- `port.GPIO{Pin, Mode, Edge, DebounceMs}`：Mode 为 `Input`/`InputPullUp`/`InputPullDown`/`Output`，Edge 为 `None`/`Rising`/`Falling`/`Both`，`DebounceMs <= 0` 取默认 20 ms。平台自己占用的引脚（ESP 上的屏幕、PM1、PSRAM、USB 等）`Open` 失败。
- 驱动接口是一张 C 函数表（open / command / close），新增外设不用改 VM 核心；端口表固定容量（`GOGO_MAX_PORTS`，默认 16）。驱动可选提供 `command_term`（M5），直接拿到原始命令项（嵌套结构、`Bytes` 字段，如 `net.SendTo`）。
- 网络（M5，`net/` 纯 C 协议栈，规格见 [M5 设计](superpowers/specs/2026-11-23-gogo-m5-network-design.md)）：端口 `netif`（`net.IfConfig{}`，全机一个所有者，命令 `net.SetAddr{IP, Mask, Gateway, DNS}`）和 `udp`（`net.UDP{Port}`，命令 `net.SendTo{Addr, Data}`，事件 `net.Datagram{From, Data}`，载荷是消息块里的堆内 `K_BYTES`，超配额丢弃并计数、不杀接收方）；`net.Subscribe()`（内建）之后收 `net.Up{IP, Gateway, DNS}` / `net.Down{}`；TCP：端口 `tcp`（`net.Dial{IP, Port, TimeoutMs}`，命令 `net.Write{Data}` / `net.Shutdown{}` / `net.Resume{}`，事件 `net.Connected{}` / `net.Data{Data}` / `net.Written{}` / `net.Closed{Reason}`）和 `tcp_listen`（`net.Listen{Port}`，事件 `net.Accepted{Conn}`，新连接归监听者所有且暂停到 `port.Handoff` 或 `Resume`）；窗口各 1 段、只收按序段、投递进邮箱成功才 ACK、RTO 1 s 倍增到 8 s / 6 次重传、TIME_WAIT 2 s；`Shutdown` 后关端口（或所有者退出）留下孤儿连接自行完成关闭，否则 RST；地址或链路丢失时所有连接以 `NoRoute` 结束（`net/tcp.c` 头注释）。`gogo/net` 的存根源码在 `netapi/`（`net/` 放 C 代码，Go 包目录不能含 C 文件），保留类型 9–26。
- 中断 / 输入事件走无锁单生产者环形缓冲区（`GOGO_EVQ_CAP`）；ISR 只写环，不加锁、不分配；调度器在每轮取出并转成消息。`platform_wait` 带"锁存唤醒"语义，生产者用 `platform_notify()`（ISR 安全）唤醒，不会丢唤醒。
- 平台实现：unix 的 `gpio` 从 stdin 读模拟事件（行 `pin value`，`sleep ms`，黄金测试用 `input` 文件；`gogo run --key c=pin ... <dir|file.gogo>` 的终端模式把按键映射成引脚（`--key` 转发给 gogovm；终端中 Ctrl-C / SIGTERM 会先恢复终端再退出；Ctrl-D 结束输入，之后没有定时器的程序即可退出，而 sticks3_demo 的监督者有周期定时器，需 Ctrl-C 结束）），输出打印到终端；ESP32-S3 的 `gpio` 用 GPIO 中断，KEY1=G11、KEY2=G12。单边沿引脚上的 ISR 释放回弹过滤：在未上报的反向变化后 `debounce_ms` 内到来的同向边沿被丢弃（已接受的取舍，见 [m3-followups.md](m3-followups.md)）。

后续驱动：`uart`、`i2c`、`spi`、`display`（帧缓冲区整块刷新，帧缓冲用大二进制传递）。M3 的屏幕只是 stdout/stderr 的调试字符界面镜像，不是 `display` 端口。

### 5.4.1 网络栈（M5 实际实现）

规格见 [M5 设计](superpowers/specs/2026-11-23-gogo-m5-network-design.md)，取舍与遗留见 [m5-followups.md](m5-followups.md)。

- **分层**：`net/`（纯 C：以太网 / ARP / IPv4 / ICMP echo / UDP / TCP，只依赖 `net/netif.h` 的平台钩子）→ 端口 `netif` / `udp` / `tcp` / `tcp_listen`（`vm/` 端口层）→ 用 Go 子集写的库 `gogo/dhcp`、`gogo/dns`、`gogo/http` 与内建 `config.Get`。`gogo/net` 的存根源码在 `netapi/`（Go 包目录不能含 C 文件）。**用 `dhcp.Start` 取代规格里的 `net.Start`**（`gogo/net` 是编译器存根包，放不下库代码）；`dhcp` / `dns` 带 Go 构建标签 `gogo`（宿主 Go 工具链解析不了 `gogo/net`）。
- **单槽接收**：平台（Wi-Fi 任务，CPU0）用 `net_rx_offer` 把一帧交给栈的唯一接收槽（拷贝或丢弃，立即释放驱动缓冲）；槽满时丢帧，TCP 一段窗口使之很少发生。栈本身不分配缓冲。
- **背压**：UDP 无 ACK，超出接收者配额的数据报被丢弃并计数（不杀接收者）；TCP 只有载荷成功投递进所有者邮箱才 ACK，邮箱满则不 ACK、对端重传；Data 给 Written / Closed 等控制事件预留余量，慢读者不会被控制事件挤死。
- **孤儿关闭**：`Shutdown` 后关端口、或所有者退出，连接作为孤儿自行完成关闭（FIN 握手 / 重传），否则 RST。地址或链路丢失（含 `SetAddr` 改地址）时所有连接以 `NoRoute` 结束；`netif` 所有者退出同样丢掉全部连接。
- **内存**（ESP32-S3 实测）：栈固定状态 `net_s` 3640 B（预算 4096 B）；TCB 56 B、UDP CB 12 B、Port 64 B（32 位）；net 代码约 11.3 KB（xtensa -Os）。Wi-Fi 驱动：堆约 30 KB（`esp_wifi_init` 25.1 KB + start 1.8 KB + 关联后约 2.4 KB，通信时再瞬时最多约 6.5 KB）、静态约 31.5 KB；lwIP / esp_netif 不链接，驻留为 0。启动时空闲堆 197 KB，运行 `examples/netdemo` 时约 173 KB。每个 HTTP 连接除 TCB 外有一个 handler actor（约 3.3 KB）。
- **HTTP actor 内存**：worker / handler 的 `MaxMemory` = 2×体上限 + 6 KB（默认 22 KB）：体在缓冲和冻结副本里各存一次，GC 拷贝堆；8 KB 体实测峰值 Get 19.7 KB、handler 17.9 KB。限额失败后 VM 再做一次"紧"GC（块大小 = 存活 + 需求，无扩容余量）。
- **语言与镜像**：函数值带参数和返回值（`CALLV`，镜像 v4），`http.Serve` 的 handler 用它；`port.Handoff`、`msg_try_enqueue`（TCP 的 Data 用不致命的入队）等见 `vm/port.h`。函数值不能放进 `any`、不能比较（包括与 `nil`）、不支持具名函数类型。
- **TLS 推迟**（规格 §5.7、第 9 项任务）：TLS 需要 mbedTLS 上下文（几十 KB，只能放 PSRAM，不可移植到 RP2040 级芯片）、同步握手会阻塞所有 actor 数百毫秒、证书包放 flash、没有宿主测试手段。M5 不含 HTTPS：`http.Get` 对 `https://` 立即失败。

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
| `byte`（`uint8`） | 小整数 0 | 0..255 的小整数立即数；`byte` 算术在每次运算后截断回绕 | 不能放进 `any`（运行时与 `int` 无法区分，编译期拒绝） | 不扫描 | 原样 |
| `rune`（`int32`） | 小整数 0 | 小整数立即数；仅作 `range` 字符串的循环值和字符字面量类型，其余用法拒绝 | 不能放进 `any` | 不扫描 | 原样 |
| `[]T`（切片） | `T_NILSLICE` = `0x52`（`s == nil` 为真） | `K_SLICE` 堆对象 `[hdr][off][len][cap][array]`，off/len/cap 为原始字；`array` 是 `K_ARRAY` `[hdr][n][elems]`（元素按 term 扫描）或 `K_BYTEARR` `[hdr][nbytes][bytes]`（`[]byte`，字节不扫描）的引用，容量为 0 时是 `T_NIL`；空而非 nil 的切片是 cap=0 的 `K_SLICE` | 原样 | 切片头与底层数组一起复制；`K_BYTEARR` 的字节不扫描 | 深拷贝，保留共享与环（所以别名在消息里仍然成立，但不跨 actor） |
| `bytes.Bytes` | `T_EMPTYBYTES` = `0x62` | 堆内 `K_BYTES` `[hdr][nbytes][bytes]`（不可变，字节不扫描，≤ 64 字节的 `Freeze` 结果与 `FromString`/`FromSlice` 的结果）；更大的 `Freeze` 结果是堆外句柄，标签 `1110`：`(index << 4) \| 0xE`，指向 VM 全局的引用计数表 | 原样（类型 switch 的 `case bytes.Bytes` 匹配 `TY_BYTES`） | 堆内字节不扫描；句柄在 GC 时重算持有关系，不可达则释放 | 堆内深拷贝；句柄只传引用，计数 +1，接收方按全部大小计费（排队消息计入邮箱） |
| `bytes.Buffer` | 不可用（零值调用方法会 `badcode`，必须 `NewBuffer`） | 引用类型 `K_BUF` `[hdr][len][array]`，`len` 为原始字，`array` 是 `K_BYTEARR`（即容量），容量为 0 时是 `T_NIL`；`Freeze` 复制一次并清空 | 原样 | 数组字节不扫描 | 深拷贝 |
| 结构体（含 `actor.*`、`port.*` 保留类型） | 各字段为其零值的 `K_STRUCT` | `K_STRUCT` 堆对象 `[hdr][type id][字段...]` | 原样（`TYPEIS` 比较 type id） | 字段逐个扫描 | 深拷贝，保留共享与环 |
| `any` | `T_NIL` | 所装动态值本身的 term | — | 按动态值 | 按动态值 |
| 函数值（任意签名） | `T_NIL`（`Spawn` 与调用都 `badcode`） | `K_CLOSURE` 堆对象 `[hdr][函数号][捕获...]`；`CALLV` 把捕获作为前导参数、实参随后 | 原样 | 捕获逐个扫描 | 深拷贝 |
| `actor.PID` | `T_ZEROPID` = `0x6`（槽 0、代 0：槽 0 从不使用代 0，端口槽 ≥ `0xFFF0`，所以不等于任何活着的 actor 或端口） | PID 立即数 `(代 << 20) \| (槽 << 4) \| 0x6` | 原样（`TYPEIS` → `PID`） | 不扫描 | 原样 |
| `actor.Ref` | `T_ZEROREF` = `0x42` 特殊常量，序号视为 0 | `K_REF` 堆对象 `[hdr][serial 低][高]`，序号从 1 开始 | 原样（`TYPEIS` → `Ref`，零值也是） | `K_REF` 被复制，内部不扫描；`T_ZEROREF` 不扫描 | `K_REF` 深拷贝（序号不变）；`T_ZEROREF` 原样 |

零值 PID / Ref 的语义：`Send` 到零 PID 被丢弃；`Link` / `Monitor` 零 PID 与对已死 PID 相同（`NoProc`）；`CancelTimer` / `Demonitor` 零 Ref 什么也不做（返回 `false`）；零值彼此相等，与任何活着的值都不等；`println` 打印为 `<pid 0>` / `<ref 0>`。注意崩溃报告和 `println` 只显示 PID 的槽位（`<pid N>`，不含代数）：重启后的 worker 可能复用旧槽位，零 PID 的 `<pid 0>` 也与主 actor（槽位 0）看起来相同。查不到名字的 `Whereis`、失败的 `port.Open` 返回零 PID。

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
- 镜像版本（`IMAGE_VERSION`，当前为 4）：操作码编号或语义、builtin 编号、保留类型、term 编码任何一项变化都要升版本；编译器与 VM 同步升级，加载器拒绝其他版本（`unsupported version`）。v1 为 M1/M2；v2 为 M3（通用算术只收 `int`、`*64` 操作码与 `BOX64`、`CLEAR`、有类型的零 PID/Ref）；v3 为 M4（多返回值 `RETN`、`CONV c=2` 转 byte）；v4 为 M5（调用函数值 `CALLV`）。

### 5.7 调度

- 单个调度器，就绪队列 FIFO。
- 每次函数调用和循环回跳消耗 1 个 reduction，配额用完就切换（初始配额暂定 1000，可调）。因此死循环也会被抢占。
- 无就绪 actor 时，计算最近的定时器截止时间，进入平台等待（MCU 上为 WFI，unix 上为 poll/sleep），被中断或事件唤醒。

### 5.8 错误与崩溃

- 运行时错误（越界、nil map 写入、除零、类型断言失败、写只读内存、超出内存上限、`panic`）都只让当前 actor 退出。
- 退出原因（M2 实现）：`actor.Reason{Code int; Func string}`。`Code` 是 `actor.Normal`（从入口函数返回）、`Killed`、`OutOfMemory`、`MailboxOverflow`、`NoProc`、`DivZero`、`BadCode`、`Shutdown`、`BadShift` 或用户码（≥ 100，`actor.Exit(code)` / `actor.Kill(p, code)`）；`Func` 是运行时错误所在函数名，否则为空串。原设想的 `error` 类型原因（`RuntimeError{Kind, Msg, Func, Line}`、`panic(v)`）留到语言里程碑，见 [M2 设计](superpowers/specs/2026-11-23-gogo-m2-design.md) §5.2、§7。
- 退出时向链接的 actor 发 exit 信号（开启 `TrapExit` 则收到 `ExitMsg`，否则原因不是 `Normal` 时级联退出），向监视者发 `DownMsg`。这两种通知的消息块在建立 link/monitor 时就预留好，退出时不分配内存，送达有保证；预留的内存计入建立者自己的内存上限。`Kill(p, Killed)` 不可捕获。
- 崩溃报告打印到 stderr：`gogo: actor <pid N> crashed: <kind>[ in <func>]`，`<kind>` 为 `killed`、`oom`、`mailbox_overflow`、`noproc`、`divzero`、`badcode`、`badshift` 或 `exit(<code>)`；`Normal`、`Shutdown` 不报告。行号留到以后。
- 监督树由标准库 `gogo/supervisor` 包用 Go 子集实现：M2 是单子进程版（重启策略 Permanent/Transient、最大重启强度），多子进程 one_for_one 留到以后。

### 5.9 工具链与工作流

```
gogo build ./app -o app.gogo          # 编译
gogo run app.gogo                      # 在宿主 VM 上运行（模拟外设；stdin 为模拟 gpio 事件）
gogo flash app.gogo                    # 把镜像写入板子的 gogo 分区（需要 esptool）
gogo monitor -reset -t 30 -until "vm exited"   # 抓取串口输出
```

ESP32-S3（M5StickS3）上固件与镜像分开存放：固件用 ESP-IDF 构建、只需刷一次，换程序不必重刷固件：

```
. ~/esp/esp-idf/export.sh
idf.py -C platform/esp32s3 build flash         # 固件（VM + 平台层 + 屏幕控制台）
gogo flash app.gogo                            # 镜像，写入 0x210000 的 gogo 分区（头部 + CRC）
gogo monitor [-port P] [-t 秒] [-until 文本] [-reset]
```

`gogo flash` 默认使用唯一的 `/dev/cu.usbmodem*`；`gogo monitor` 在自己的描述符上把串口设为 raw、CLOCAL、不 HUP，只有 `-reset` 才复位芯片（DTR/RTS 是复位与启动脚）。同一时间只有一个程序能打开串口。板子启动信息、分区表等见 `platform/esp32s3/README.md`。`examples/sticks3_demo` 是 M3 的演示程序，`examples/many_idle` 是"1000 个空闲 actor"检查；两者带 `expected.out`，由 `tests/golden_test.go` 一并在宿主 VM 上运行。

**设备配置 `gogo config`（M5）**：`gogo config set|get|rm|list <key> [value]`，另有 `scan`（板子看到的 2.4 GHz 网络）和 `stats`（网络计数、堆）；值写 `-` 表示从 stdin 读一行（密码用 `gogo config set wifi.password -`，不进 shell 历史）。设备存在固件的 NVS 里（命名空间 `gogo`，重刷固件或镜像都不会清掉），通过 USB 串口以 `\x02gogo-config ...` 帧通信；程序自己的输出里 `\x02` 被写成 `?`，程序伪造不了回复。`wifi.` 键对程序的 `config.Get` 不可见，`get wifi.password` 只回显 `(set)`。宿主侧存储选择：`-port` 指定板子；`-local` 指定本地文件 `$GOGO_CONFIG`（默认 `~/.gogo/config`）；都没给时，若设了 `$GOGO_CONFIG` 用本地文件，否则恰好连着一块板子就用板子（向 stderr 打一行提示），否则用本地文件——即板子优先于 `~/.gogo/config`。`examples/netdemo` 是 M5 的演示：DHCP、80 端口中英文页面（KEY1/KEY2 次数、最近抓取、运行时间）、每 10 秒 `http.Get(demo.url)`。

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
3. **M3 端口层 + M5StickS3 真机**（已完成）：端口层（`gpio` 驱动）、定时器最小堆、int64 立即数、镜像 v2（带类型的零 PID/Ref 与 `CLEAR`）、ESP32-S3 平台（固件、`gogo flash`/`gogo monitor`、屏幕调试控制台含中文），以及在 M5StickS3 上演示"按 KEY1 让 worker 崩溃、监督者重启它"（`examples/sticks3_demo`）。1000 个空闲 actor 在 160KB 内存上限内创建成功（`examples/many_idle`）。详见 [M3 设计](superpowers/specs/2026-11-23-gogo-m3-design.md)，遗留事项见 [m3-followups.md](m3-followups.md)。
4. **M4 字符串 / 字节 / 切片 / 多返回值**（已完成）：多返回值（`RETN`）、`byte`、镜像 v3；运行时字符串；切片与 `[]byte`；`bytes.Bytes` / `bytes.Buffer`（大块堆外、引用计数、按持有者计费）；`gogo/strings`、`gogo/strconv`；`examples/httpparse` 作为"写网络库是否顺手"的检验。详见 [M4 设计](superpowers/specs/2026-11-23-gogo-m4-strings-bytes-design.md)，遗留事项见 [m4-followups.md](m4-followups.md)。
5. **M5 网络**（已完成）：极小内存的纯 C 协议栈（ARP / IPv4 / ICMP echo / UDP / TCP）、`gogo/dhcp`、`gogo/dns`、`gogo/http`（客户端与服务器，HTTP/1.0）、函数值带参数调用（镜像 v4）、M5StickS3 上的 Wi-Fi 与 `gogo config`、`examples/netdemo`。TLS 推迟。详见 [M5 设计](superpowers/specs/2026-11-23-gogo-m5-network-design.md)，遗留事项与后续候选见 [m5-followups.md](m5-followups.md)。
6. **M6 Pi + Whisplay 小屏**：Linux 平台真实外设，在 Pi + Whisplay 上用 actor 驱动屏幕和按键（原"M4 小屏"顺延）。

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
