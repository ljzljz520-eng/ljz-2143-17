# C 窗口输入策略系统设计

## 1. 边界与不变量

系统不把快捷键散落在 `SDL_KEYDOWN` 分支中，而分成三层：

1. **平台适配层**：SDL/X11、Windows、macOS、浏览器 VNC 只负责把原生事件归一化。
2. **输入策略核心**：纯 C、不依赖 SDL；执行策略代次、保留键、分层、IME 组合态、长按和热切换状态机。
3. **命令/展示层**：只消费核心输出的命令，不再解释物理键。

硬不变量：

- `Esc`、`F11`、`←/→/↑/↓` 是受保护物理键，只能承担固定角色，不能被重定向。
- `Ctrl+Shift+Q` 是保留安全退出，不允许出现在服务端或远端策略包中。
- 恢复背景键只能是 `R`、`B` 或 `Backspace`，且无修饰键。
- 一个物理键和修饰键组合只能映射一个命令；同一命令默认不能重复。方向角色只允许白名单别名（W/A/S/D）。
- 网页配置通过服务端二次校验；客户端解析策略时再次校验并校验 SHA-256。
- 回放只接受 `trust=experiment` 的哈希链日志；不产生现场业务确认，不补做业务命令。

## 2. 本地归一化输入事件与命令队列的取舍

当前实现选择 **本地归一化输入事件作为审计边界**：

```c
InputEvent { type,timestamp,key,phase,modifiers,scancode,layer,focused,composing,... }
```

原因：

- 日志必须能还原“当时按了什么物理键、当时处于哪个层/代次”，而不只是最终命令。
- 被 IME 吃掉的键、重复键、全屏状态确认、失焦清键本身都是关键证据；只记录命令会丢失这些负例。
- 纯 C 核心可以在无 SDL 的单元测试和回放工具中运行。

运行时仍有一个小的同步 `OutputList` 命令队列，由主循环处理全屏、退出、移动等副作用；这是执行队列，不是审计边界。跨进程异步总线容易在策略热切换和全屏状态确认中引入乱序，因此没有把它作为主路径。

## 3. 分层分发与中文 IME

优先级：

```text
安全退出（Ctrl+Shift+Q） > IME 组合态所有权 > 模态面板 > 输入框 > 展示画面
```

- IME 组合态来自 SDL `SDL_TEXTEDITING`；组合文本非空时，核心忽略所有快捷键，包括方向键和 Esc。组合结束后 Esc 才退出输入框/面板。
- 模态面板打开时，展示层的 F11、方向键和恢复背景键不能穿透；Esc 先关面板。
- 输入框层只允许 Esc 退出输入焦点；文本、退格和方向编辑交给输入框/IME。
- `Ctrl+Shift+Q` 是安全兜底：窗口管理器关闭事件也始终能退出程序。

## 4. 长按与全屏边沿

- 方向键第一次按下产生一格移动；持续按住后按策略的 `initial_ms` 和 `interval_ms` 由帧时钟产生有界移动。
- 操作系统的 auto-repeat `SDL_KEYDOWN` 被折叠，不直接产生移动；重复节奏由策略控制。
- 场景位置被钳制在边界内，防止连续移动后越界。
- F11 只在物理 `DOWN` 边沿产生一次 `OUT_FULLSCREEN_TOGGLE`。`REPEAT`、持续按住以及“已请求但未确认”的切换期间都不产生新命令。
- SDL/WM 的实际状态通过 `EV_FULLSCREEN_STATE` 回写；只有确认后才允许下一次边沿。

## 5. 远程策略到达时的抬键边界

核心为每个仍按住的物理键记录 `{key, physical scancode, policy generation, held}`。

1. 新策略通过 SHA-256、schema 和保留键冲突校验后只进入 `staged`。
2. 若存在旧策略下按下且尚未抬起的键，新策略不启用。
3. 旧物理键的 `UP` 始终由物理键身份消费，解除旧槽位；不会按新映射解释。
4. 当槽位全空时，在同一个安全边界原子切换到新代次，并输出 `activated` 回执。
5. 失焦、显示器拔出等“可能漏收 UP”的事件清空全部按键，然后允许新策略启用。

这样不存在“旧命令已开始但按新映射抬键”的卡键状态。

## 6. 失焦、显示器拔出和坏包恢复

- `SDL_WINDOWEVENT_FOCUS_LOST`：清空按键和重复节奏；策略可在此边界激活。
- 显示器在全屏中拔出：处理显示设备变化，退出全屏、把窗口移回可用显示器、清键，并发送幂等的窗口恢复命令。
- 策略包损坏、checksum 不符、代次回退或校验失败：保留现有活动策略，输出拒绝回执，不修改键状态。
- 设置面板打开时按 Esc：若 IME 正在组合，Esc 给 IME；否则先关面板。面板不拦截窗口关闭和安全退出。

## 7. 数据库、网页和设备回执

服务：`service/policy_server.py`（Python 标准库 + SQLite）。

- `policy_generations`：策略代次、规范化 payload、SHA-256、信封和状态。
- `devices`：设备最后见到的代次、平台、版本和心跳时间。
- `device_receipts`：`staged/applied/rejected`、原因、checksum 和时间。
- `replay_sessions`：实验日志哈希、信任级和拒绝原因；业务确认计数固定为 0。

接口：

- `GET /api/policy/current`
- `GET /api/devices/{id}/policy`
- `POST /api/policy/validate`
- `POST /api/policies`
- `POST /api/devices/{id}/receipts`
- `POST /api/devices/check-in`
- `POST /api/replay-sessions`

## 8. 可回放日志

日志为 JSON Lines，每行结构为：

```json
{"seq":1,"type":"key","t":10,"key":"F11","phase":1,...,"hash":"..."}
```

哈希链为：

```text
H(previous_hash || "|" || line_body_without_hash)
```

首行 manifest 声明 session、设备、版本和 `trust`。回放器：

1. 重算并验证每条哈希；
2. 拒绝 operational 日志；
3. 只把实验输入送入纯 C 核心；
4. 不发送设备回执，不执行业务确认；
5. 检测到业务确认字段直接拒绝。

因此网页重放只能复现实验输入，不能伪造“现场人员已确认”的业务事实。

## 9. 跨平台差异

- **Linux/X11/Wayland**：SDL 2 提供 scancode、auto-repeat 和显示变化；Xvfb/noVNC 中全屏为桌面全屏，显示热插拔事件可能由窗口管理器延迟。
- **Windows**：scancode 稳定；Win+全屏切换可能产生系统级显示变化，必须以实际窗口状态回写为准。传输层需替换为 WinSock 初始化版本。
- **macOS**：需在主 bundle/主线程处理窗口；全屏转换动画期间可能只收到最终状态。命令仍按边沿发出，但状态确认以系统回写为准。
- **浏览器/noVNC**：管理页面使用 `event.code` 表示物理键；真正的 C 窗口仍以 SDL scancode 为准。浏览器自身保留的快捷键可能无法被网页 preventDefault，不能把网页行为当作设备安全边界。

## 10. 测试覆盖

C 测试：`make core-test`

- F11 重复事件不能来回全屏；
- 方向键首动、初始延迟、间隔和有界移动；
- 模态、输入框、IME 组合态分层；
- 旧键按住时新策略 staged、UP 后 activated；
- 失焦清键和热激活；
- 全屏中显示器拔出恢复；
- 保留键/重复物理键冲突与坏 checksum；
- 哈希链日志回放和篡改检测。

服务测试：`make service-test`

- 默认代次、checksum 和设备 applied/staged/rejected 回执；
- 保护键、重复键、保留安全退出组合拒绝；
- 白名单移动别名和新代次发布；
- operational 回放及业务确认字段拒绝。
