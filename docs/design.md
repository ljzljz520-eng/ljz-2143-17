# 输入策略系统设计

## 1. 问题与目标

把原有 C/SDL 窗口里写死的快捷键（Esc / F11 / 方向键 / 恢复背景）升级为
**可由网页配置、可远程更新、可回放审计** 的输入策略系统，并满足：

- 网页管理映射；服务接口检查**保留键**与**冲突**；关系数据库记录**策略代次**
  与**设备回执**。
- 客户端按 **模态面板 / 输入框 / 展示画面** 三层分发。
- **中文输入组合态**不能被快捷键截走。
- 长按方向键允许**有界连续移动**；全屏切换**不能被重复事件来回触发**。
- 远程策略在**键仍按住时到达**，需要明确"旧键抬起 / 新映射启用"的边界，
  杜绝卡键。
- 失焦漏收抬键、全屏切换中拔屏、设置面板打开时按 Esc、映射包损坏等情况下
  必须有**可靠退出方式**。
- 网页重放**只能复现实验输入**，不得伪造现场人员已执行的业务确认。

## 2. 架构总览

```text
┌────────────────────────────── 网页配置台 (service/web) ─────────────────────────────┐
│  映射编辑(Esc/F11/方向键/恢复背景)  校验  发布  代次/回执  实验重放  现场确认          │
└───────────────┬───────────────────────────────────────────────────┬───────────────┘
                │ HTTP/JSON                                          │ 上传 .islog
        ┌───────▼────────┐                                    ┌───────▼────────┐
        │ 策略服务        │ validator.py 保留键/冲突/缺失校验    │ 参考模拟器      │
        │ server.py       │ simulator.py 与 C 核心同语义复算      │ journal_codec  │
        │ db.py (SQLite)  │ 代次/回执/确认/重放会话 严格分表      │ ISLOG 二进制    │
        └───────┬────────┘                                    └────────────────┘
                │ GET /policy（信封：payload + SHA-256 digest）
                │ POST /receipts, /confirmations
┌───────────────▼──────────────── C 客户端 (SDL2) ───────────────────────────────────┐
│ sdl_input.c：SDL_Scancode(物理位置) ──► InputEvent(归一化, origin, composition)     │
│                                                                                    │
│                            ┌────────────── Dispatcher ────────────────┐            │
│  IE_FOCUS_* / DISPLAY_* ──►│ 硬保留退出键(Ctrl+Q/Cmd+Q/Alt+F4)        │            │
│  IE_TEXT / COMPOSITION ───►│ 三层路由：模态面板 → 输入框(IME门控)      │            │
│  IE_KEY ──────────────────►│         → 展示画面(策略 lookup)          │            │
│                            │ RepeatController(有界连发)                │            │
│                            │ FullscreenFsm(边沿触发/超时/拔屏 reconcile)│           │
│                            │ Activation(旧键合成抬起→原子换策略)       │            │
│                            └───────────┬───────────────┬─────────────┘            │
│                                  统一 CommandQueue      安全闸(非human禁确认)        │
│                            sync.c 轮询/校验/回执   journal.c ISLOG 回放日志(CRC32)   │
└────────────────────────────────────────────────────────────────────────────────────┘
```

## 3. 架构抉择：本地归一化输入事件 **与** 统一命令队列

二者不是二选一，而是流水线的两段（本系统两者都实现）：

1. **平台边界做"本地归一化输入事件"**（`InputEvent`）。
   SDL2 / Web / 测试桩各自把原始事件翻译为统一结构，核心逻辑不再感知
   Windows VK、X11 keysym、macOS 键位差异。这是跨平台的前提。
2. **策略匹配后汇入"统一命令队列"**（`CommandQueue`）。
   所有 UI（真人、重放、参考模拟器）只消费命令；安全闸只有一个检查点；
   重放与真人走完全相同的路径。

若只用归一化事件而无命令层，业务动作与输入耦合、无法做统一安全审计；
若只用命令队列而无归一化层，则跨平台差异（修饰键顺序、自动重复、物理布局）
会泄漏到核心。分层后各司其职。

## 4. 跨平台差异及处理

| 差异 | 表现 | 处理 |
|---|---|---|
| 键盘物理布局 | QWERTY/AZERTY 下同位置不同字符 | 匹配一律用 `SDL_Scancode`/Web `event.code` 的**物理位置名**（F11、ArrowLeft、KeyQ…），不用 keycode |
| 修饰键 | macOS 用 Cmd 对应 Win/Ctrl | 归一化 `KM_CTRL/ALT/SHIFT/GUI`；保留 `Ctrl+Q`（Linux/Win）与 `Meta+Q`（Cmd+Q，macOS）双退出 |
| 窗口关闭 | X11/Win 为 Alt+F4 / WM_CLOSE，macOS 为 Cmd+Q | 三者均为保留键；同时处理 `SDL_QUIT`/`WM_CLOSE` 作为第二通道 |
| 自动重复 | 重复速率/是否带 repeat 标志因平台而异 | 核心**忽略 OS repeat**，方向键由单调时钟自管连发；F11 只认首次非重复 keydown |
| 修饰抬起时序 | 某些平台先收非修饰键抬起、后收修饰抬起 | 硬退出键用"事件快照 mods ∪ 跟踪态 mods"任一命中判定，避免偶发失效 |
| IME | Linux ibus / Windows TSF / macOS IME 事件形态不同 | 统一翻译为 `IE_COMPOSITION`/`IE_TEXT` + `composition` 标志 |
| 显示热插拔 | 全屏中可能丢失显示器 | `SDL_DISPLAYEVENT_DISCONNECTED` → FSM 进入 reconcile，以实际模式重对 |
| 失焦 | 可能漏收 keyup | `IE_FOCUS_LOST` 等价于"全部抬起"：停连发、解 ARM、清 held/mods |

## 5. 策略包与校验

```jsonc
// 信封
{ "generation": 7, "device_id": "dev-001", "issued_at": "...ISO...",
  "payload": { ...见下... },
  "digest": "<sha256 hex of canonical_json(payload)>" }
// payload（canonical = 键排序、无空白、UTF-8 直出）
{ "generation":7, "bounds_w":1280, "bounds_h":720,
  "move_initial_delay_ms":350, "move_repeat_ms":60, "move_max_hold_ms":3000,
  "bindings":[ {"chord":"F11","command":"toggle_fullscreen"}, ... ] }
```

- **保留键（任何层、任何命令不可映射）**：`Ctrl+Q`、`Meta+Q`、`Alt+F4`。
  另：模态面板的 `Esc` 固定为"关闭面板"，不接受网页改键。
- **冲突双向检测**：
  - 同一和弦映射到两个命令（DUP_CHORD，语义不确定）；
  - 同一命令绑定两个和弦（DUP_COMMAND，无法预期触发）。
  两者都拒绝并在响应中给出冲突对，供网页高亮两个输入框。
- **缺失检测**：open_settings / toggle_fullscreen / restore_background /
  四个方向移动 / confirm_business 必须齐备。
- **严格 schema**：只接受整数（拒绝 NaN/指数）、未知字段拒绝（防拼写错误
  被静默忽略）、重复 JSON 键拒绝、参数范围校验（如 10 ≤ repeat ≤ 500）。
- C（`policy.c`）与服务（`validator.py`）实现**同一套规则**，测试对拍。

摘要不匹配（篡改/截断）、schema 失败都返回结构化 `ValidationReport`。
客户端即使拿到绕过服务的坏包，也会独立再验一遍（纵深防御）。

## 6. 三层分发

优先级从高到低：

1. **模态面板**：设置面板打开时，展示层快捷键全部不穿透。
   `Esc` 固定关闭面板（内置、不可改键）。
2. **输入框**：面板内文本字段聚焦时，按键默认走文本编辑；
   `IE_TEXT` 负责提交文本（跨平台可打印字符一律走文本事件，不依赖 keycode）。
3. **展示画面**：策略映射唯一生效层。

## 7. IME 组合态门控

字段聚焦且 IME 组合中（`composition==true`）：
- Esc（取消组词）、方向键（组词内移动）、F11 等**全部**被吞，不触发任何
  策略快捷键；
- **唯一例外**：`Ctrl+Q` / `Cmd+Q` / `Alt+F4` 硬退出仍然生效——
  任何输入法状态下都必须有可靠退出。
组合结束（提交或取消）后快捷键恢复。

## 8. 有界连发与边沿触发

- 移动：首次 keydown 立即 1 步；`initial_delay` 后每 `repeat` ms 一步；
  总时长达到 `max_hold_ms` **自动停止**（时间上界）；视口回调返回"已钳制"
  立即停止（空间上界）。失焦/抬键/切层/激活边界都会取消。
- F11 / 恢复背景 / 业务确认 / 面板开合：**边沿命令**。
  OS 自动重复、未抬起的二次按下都被吞；必须真正 keyup 解除 ARM 后才能再次
  触发。全屏切换是异步 FSM：WINDOWED → SWITCHING → FULLSCREEN，
  SWITCHING/RECONCILING 期间不排队第二个切换（目标不翻转），杜绝来回跳。
  切换有 2s 超时，超时/拔屏后 reconcile 到**平台实际模式**。

## 9. 激活边界（键仍按住时新策略到达）

```text
T0  收到信封：schema + SHA-256 + payload 校验通过 → 仅 stage（不改当前映射）
T1  commit（设备在输入处理间隙执行，原子）：
      a. RepeatController 停止全部连发；FullscreenFsm 解除所有边沿 ARM
      b. 枚举当前 held[] 中所有仍按住的物理键
      c. 对每个键按【旧策略】合成 KEYUP 做收尾（停止对应连发/ARM），
         但不下发任何业务命令（旧命令不会在新代次生效）
      d. 清空修饰键集合（带修饰的和弦必须重新按下）
      e. 原子替换 Policy（generation 前进）
      f. 写 R_ACTIVATION 日志帧 + POST applied 回执(released_keys=N)
T2  此后所有事件按新策略解释
```

不变式：**切换完成后没有任何键处于按下态**。物理键其实仍按着，但：
- 其后续 OS repeat 事件因 held 表已清空 + `repeat` 标志被丢弃；
- 若用户松开重按，则是一次干净的新 keydown（按新映射）。
因此不存在"新映射触发旧键"或"旧键永远不抬起"的卡键状态。

## 10. 可靠退出（所有失效模式下）

退出通道（互相独立，至少一个可用）：

1. 硬保留快捷键 `Ctrl+Q` / `Cmd+Q` / `Alt+F4`：不查策略、任何层、
   IME 组合中、切换中都生效；
2. 模态面板 `Esc`：固定关闭，策略改不掉；
3. 窗口管理器关闭 / `SDL_QUIT`：第二通道；
4. 映射包损坏：拒绝并保留上一良好策略（last-known-good），
   首次启动或服务不可达使用内置默认策略，退出键始终存在；
5. 失焦：所有键状态清零，重新聚焦从干净状态开始。

## 11. 可回放日志与重放安全

- 文件 `ISLOG`：116 字节显式序列化文件头（不依赖结构体内存对齐）+
  帧序列 `LEN|TYPE|BODY|CRC32`。输入、命令、系统事件、激活、拦截各有记录类型。
- 重放**只回放归一化输入帧**，命令由"选定策略 + 同一套核心"重新计算；
  所有输入的 origin **强制覆盖为 replay**（不信任日志里的原始 origin）。
- **安全闸**：`CMD_CONFIRM_BUSINESS` 只允许 `ORIGIN_HUMAN`。
  replay/remote 的确认在唯一的 `emit()` 检查点被拦截，计入
  `stat_blocked_business` 与 R_REPLAY_BLOCKED 审计帧，**不进入**
  `business_confirmations` 表（该表甚至有 CHECK(origin='human') 约束）。
- 重放走 headless：不产生真实全屏、不退出宿主进程（quit 仅记录）。
- CRC 错误/截断立即中止并报告损坏帧号。

数据库表：`devices / policy_generations / device_receipts /
business_confirmations / replay_sessions / replay_blocked_events`，
正式业务数据与实验重放严格分表。

## 12. HTTP 接口（摘要）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/validate | 仅校验（保留键/冲突/缺失/范围），不落库 |
| POST | /api/devices/{id}/publish | 校验通过则发布新代次（代次由 DB 分配） |
| GET  | /api/devices/{id}/policy | 设备轮询：返回信封（含 digest） |
| POST | /api/devices/{id}/receipts | applied/rejected + released_keys |
| POST | /api/devices/{id}/confirmations | **仅 origin=human**，否则 403 |
| GET  | /api/devices/{id}/generations,/receipts,/confirmations | 列表 |
| POST | /api/replay/events | 网页构造输入的实验重放 |
| POST | /api/replay/journal | 上传 ISLOG，跨语言复算命令序列 |
| GET  | /api/replay/sessions | 重放会话（实验数据） |

## 13. 测试

- `tests/test_core.c`：纯 C11、无 SDL 依赖，覆盖归一化、严格 JSON、
  SHA256/CRC32、保留键/冲突/缺失、信封篡改、三层与 IME 门控、
  F11 边沿 FSM（含拔屏/超时）、有界连发（时间+空间上界）、
  失焦清零、激活边界（合成抬起）、重放安全闸、损坏日志、可靠退出。
- `tests/test_service.py`：校验器、模拟器、C↔Python 一致性、HTTP E2E
  （校验/发布/轮询/回执/重放拦截/真人确认隔离/分表）。
- `journal_codec.py` 与 C `journal.c` 的二进制互通有专门的跨语言用例
  （C 写 → Python 解析；篡改字节 → CRC 失败）。
