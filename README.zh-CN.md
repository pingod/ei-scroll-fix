# ei-scroll-fix

[![build](https://github.com/pingod/ei-scroll-fix/actions/workflows/build.yml/badge.svg)](https://github.com/pingod/ei-scroll-fix/actions/workflows/build.yml)

**修复 Synergy / Deskflow 作为 Wayland 服务端、macOS 作为客户端时滚轮"不顺畅"的问题。**

一个物理滚轮格本该滚动几行，在 Wayland 服务端上却可能变成 **16~17 个整格（约 50 行）**，
而且相邻格在 1 格和 17 格之间来回跳 —— 手感就是发涩、抽动、或者一动就冲出去。
本项目解释原因，并给出一个 10 KB 的 `LD_PRELOAD` 垫片，在服务端修好它，无需重新编译任何东西。

```
macOS 上每格行数：   3 行 → 51 → 48 → 51 → 48 → …      （故障）
修复后：            3 行 →  3 →  3 →  3 →  3 → …      （正常）
```

## 这是你的问题吗

以下条件全部成立：

- 服务端是 **Wayland** 会话（core 走 input-capture portal + libei）。
  检查：`echo $XDG_SESSION_TYPE` → `wayland`
- 服务端是 **deskflow** 或 **Synergy 3.x**（Synergy 3.8.4 内嵌 deskflow v1.26）
- 客户端是 **macOS**（`OSXComputer::fakeMouseWheel` 里有硬编码 3 倍加成）
- **鼠标**（不是触控板）滚动既偏快又不均匀 —— "一格 1 次、下一格 17 次"就是典型特征

不用装任何东西就能先确认，见 [验证诊断](#验证诊断)。

## 快速开始

```bash
git clone https://github.com/pingod/ei-scroll-fix
cd ei-scroll-fix
make test                                  # 14 项断言，不需要真实会话
./scripts/install.sh                       # 编译、安装、重启服务
```

然后把光标移到 macOS 屏幕上滚一下：

```bash
tail -3 ~/.local/state/Synergy/scroll-fix.log
```

```
1792485587.516 [107731] active smooth_div=1.5000 drop_discrete=1 wheel_only=1 disabled=0
1792485587.516 [107731] wheel   dy 15.000 -> 10.000
1792485587.516 [107731] discrete dy 15 -> 0
```

每一格都应该是 `dy ±15.000 -> ±10.000` 和 `discrete ±15 -> 0`。
撤销：`./scripts/uninstall.sh`。

## 到底哪里出了问题

同一条链路上叠了四个独立缺陷，只修其中任何一个都不够 —— 垫片必须掐掉服务端的两个。

```
物理滚轮一格              1 格 = 15°  且 value120 = 120
   │
   ├─ aquamarine  Session.cpp:615-651
   │      aqe.delta    = get_scroll_value()       → 15   （度）
   │      aqe.discrete = get_scroll_value_v120()  → 120  （v120）
   ▼
Hyprland  InputManager.cpp:994-999        CInputManager::onMouseWheel
   │   PROTO::inputCapture->axis(e.axis, e.delta);            // 15  ─┐
   │   if (e.source == 0)                                          │ ① 值错（应传
   │       PROTO::inputCapture->axisValue120(e.axis, e.delta); // 15 ─┘    deltaDiscrete=120）
   │                                                               ② 同一事件双发两条通道
   ▼
libei  ei_device_scroll_delta(15)  +  ei_device_scroll_discrete(15)
   ▼
deskflow / Synergy  EiScreen  （PR #10277 至今未合）
   │   onPointerScrollEvent:          acc += dx * 0.1;  trunc;     // 单位 = "格"（1.0 = 一格）
   │   onPointerScrollDiscreteEvent:  acc += dx;        / 120.0;   // 单位 = v120（120 = 一格）
   │   → ③ 两个 handler 共用 ei_device_get_user_data 的同一对余数
   ▼
协议  DMWM%2i%2i   （int16 整格 ×120，协议层没有小数变体）
   ▼
macOS 客户端  OSXComputer::fakeMouseWheel
       (int)(y * 3.0 / 120.0)   ← ④ 硬编码 3 倍加成，上游 PR #10257 才删除
       CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, y, x)
```

| # | 位置 | 缺陷 | 上游修了吗 |
|---|---|---|---|
| ① | Hyprland `InputManager.cpp:996` | `axisValue120(e.axis, e.delta)` 把**角度** 15 传进了要求 **v120**（120）的通道 | **没有** —— 也还没人报 issue |
| ② | Hyprland 同几行 | 同一格**双发** smooth 和 discrete；libei 头文件明确禁止 | **没有** |
| ③ | deskflow `EiComputer.cpp` | 两个滚动 handler 共用**同一个余数对，却用两种单位** —— discrete 的写入会污染分数累加器 | PR [#10277](https://github.com/deskflow/deskflow/pull/10277) —— **open** |
| ④ | deskflow `OSXComputer.mm` | macOS 客户端的 `3.0 *` 加成 | PR [#10257](https://github.com/deskflow/deskflow/pull/10257) —— **已合**（2026-10-13） |

③ 导致的算术后果：共享累加器持续漂移，截断结果在 1 和 17 格之间交替；
再乘上 ④ 的 3 倍，就是每格 3 / 51 / 48 / 51 / 48 行。
完整推导（含二进制里的真实表达式）见 [`docs/root-cause.md`](docs/root-cause.md)。

## 修复原理

`src/ei_scroll_fix.c` 拦截两个 libei 访问器 —— 坏值进入 deskflow 的唯一入口：

| 被拦截的符号 | 原本返回 | 垫片返回 | 理由 |
|---|---|---|---|
| `ei_event_scroll_get_discrete_dx/dy` | `15` | **`0`** | 掐掉缺陷 ①/②：这条通道的值小了 8 倍，整条丢弃 |
| `ei_event_scroll_get_dx/dy` | `15.0` | **`10.0`**（`÷1.5`） | 10 "pixels" × `0.1` = 正好 `1.0` 格 → 每格正好一个 120 单位事件 |

结果：每格恒定为 120 单位，也就是 macOS 上恒定的 3 行 —— 这正是缺陷 ④ 的加成原本想近似的效果。

### 为什么除数必须是 1.5

deskflow 的 smooth handler 是 `acc += dx * 0.1`，`trunc(acc) != 0` 时才发包，
所以 **10 单位 = 一格**。物理滚轮格在 EIS 路径上到达时是 `15.0`
（Hyprland 原样透传 libinput 的角度），因此除数 `15 / 10 = 1.5`。它是精确的：
`10 * 0.1 == 1.0` 在 IEEE-754 double 下成立，所以 1000 格产生恰好 1000 个事件、零漂移
（`make test` 和 `tools/simulate.py` 都验证了这一点）。

### 为什么不会弄坏触控板

滚轮格是 **15.0 的精确倍数**，触控板增量是任意浮点数。只有当取值与 15 的倍数相差在 `1e-6`
以内时才应用除数：

```
  15.000  滚轮   → ÷1.5 →  10.000      ⤷ 缩放
  30.000  滚轮   → ÷1.5 →  20.000      ⤷ 缩放（2 格合帧）
   7.300  触控板 → 原样 →   7.300
 -13.700  触控板 → 原样 → -13.700
  15.010  边界   → 原样 →  15.010      （不是精确倍数）
```

设 `EISFIX_WHEEL_ONLY=0` 可无条件缩放（**会让触控板变慢**）。

### 为什么不会弄坏 Linux 本机滚动

光标在 Linux 本机屏幕上时，deskflow 是**发送**滚动：走 `EiScreen::fakeMouseWheel` →
`ei_device_scroll_discrete()`。垫片只碰接收侧的四个 `ei_event_scroll_get_*` 访问器，
发送侧完全没动。往返已验证：物理 1 格 → 120 单位 → 本机正好 1 格。

## 验证诊断

装之前可以先证明你确实命中了这四个缺陷。

**1. 问服务端它准备发什么。** 相关日志是 `LOG_VERBOSE`（`%z5`），而 Synergy 服务在
生产构建里会把 `VERBOSE` 降级成 `DEBUG` —— 所以 GUI 看不到，`[log] level=DEBUG` 也看不到。
必须手动跑 core：

```bash
systemctl --user stop synergy-3.service
/opt/Synergy/synergy-core server --settings ~/.config/Synergy/synergy-core.conf
# 滚几格，然后看：
#   event: scroll (0.00, -15.00)          ← smooth 通道，15 度
#   event: scroll discrete (0, -15)       ← discrete 通道，是 15（！）不是 120
#   send mouse wheel to "mac…" +0,+120
#   send mouse wheel to "mac…" +0,+2040   ← 17 格
```

**2. 确认客户端乘了 3。** 在 Mac 上：

```bash
otool -tvV /Applications/Synergy.app/Contents/MacOS/synergy-core \
  | awk '/fakeMouseWheel/,/ret/' | grep -E 'fmul|fcvt'
```

**3. 排除其它可能** —— 见 [`docs/alternatives.md`](docs/alternatives.md)。简单说：
不是网络问题，不是 `input:scroll_factor`，不是 `emulate_discrete_scroll`，
不是 `scroll_sensitivity`（二进制里零引用的死字段），
不是 `com.apple.scrollwheel.scaling`（xref 为 0），也不是高分辨率滚轮硬件。

## 调参

| 变量 | 默认 | 说明 |
|---|---|---|
| `EISFIX_SMOOTH_DIV` | `1.5` | 滚轮格的除数。`1.5` = 一格正好一格。**往小调** = 更快（`1.2` ≈ 3.75 行，`1.0` = 不缩放 ≈ 4~5 行）；**往大调** = 发涩，不是变慢 |
| `EISFIX_DROP_DISCRETE` | `1` | `0` = 保留 Hyprland 的错误通道（会退回卡顿） |
| `EISFIX_WHEEL_ONLY` | `1` | `0` = 无条件除，**会连带放慢触控板** |
| `EISFIX_DISABLE` | — | `1` = 完全旁路，等于没装 |
| `EISFIX_DEBUG` | — | `1` = 往 stderr 打每次决策（只对手动跑的 core 有用） |
| `EISFIX_LOG` | — | 追加到这个文件；能绕过服务的 stderr 管道 |

> `EISFIX_SMOOTH_DIV` 大于 `1.5` 会让单格累积不满一格，观感是"发涩"而不是"变慢"。
> 想滚得更慢请调客户端的 scale（见下）。

修复生效后，客户端的 scale 变成干净的整数倍调节，不再是取整彩票：

| macOS `yScrollScale` | 每格行数 |
|---|---|
| 0.34 – 0.66 | 1 |
| 0.67 – 0.99 | 2 |
| 1.0（默认） | 3 |
| 1.34 – 1.66 | 4 |
| 2.0 | 6 |

## 依赖

- Linux 服务端，`x86_64` 或 `aarch64`，glibc。
- `gcc`/`clang` 和 `make`（仅编译时需要，产物就是一个 `.so`）。
- core 必须**不提权**启动 —— `sudo`/`pkexec` 会清掉 `LD_PRELOAD`。
  Synergy 的 `elevate=true` 在 Linux 上并不提权（已验证：它写的日志文件属主是用户而非 root）。
  若你的环境确实提权，请改用下面的手动方式。

## 手动 / 非 systemd 用法

```bash
make
LD_PRELOAD=$PWD/build/ei_scroll_fix.so EISFIX_DEBUG=1 \
  /opt/Synergy/synergy-core server --settings ~/.config/Synergy/synergy-core.conf
```

其它启动器同理：在最终 `exec` core 的那个环境里导出这些变量。

## 卸载

```bash
./scripts/uninstall.sh            # 恢复原本的滚动行为
rm -f ~/.local/lib/ei_scroll_fix.so
```

只改动 `~/.local/lib`、`~/.config/systemd/user/synergy-3.service.d` 和
`~/.local/state/Synergy/scroll-fix.log` 三处。`/opt/Synergy` 与任何
Synergy / Hyprland 配置文件都不动。

## 文档

- [`docs/root-cause.md`](docs/root-cause.md) —— 完整链路 + 反汇编与源码证据 + 3/51/48/51 的算术推导
- [`docs/alternatives.md`](docs/alternatives.md) —— 还能试什么、什么没用、已排除哪些
- [`docs/upstream.md`](docs/upstream.md) —— 上游 issue/PR 的准确状态，以及"真正的修法"
- [`docs/troubleshooting.md`](docs/troubleshooting.md) —— 装了没效果时查哪里
- [`CHANGELOG.md`](CHANGELOG.md)

## 许可证

MIT —— 见 [`LICENSE`](LICENSE)。本项目不含 deskflow / Synergy / Hyprland 的任何代码，
只在运行时拦截 libei 导出的符号。上游引用见 [`NOTICE`](NOTICE)。
