# 失效模式与处置（题目四个必测场景）

| 场景 | 风险 | 系统行为 | 可靠退出方式 |
|---|---|---|---|
| **失焦时漏收抬键** | 方向键连发不停止 / F11 ARM 不解除 / 修饰键卡住 | `SDL_WINDOWEVENT_FOCUS_LOST` → `IE_FOCUS_LOST`：连发全部取消、全屏 ARM 清零、held 表清空、mods 清零；写 SYS_FOCUS_LOST 日志。重新聚焦从干净状态开始，不猜测物理键态 | Ctrl+Q / Cmd+Q / Alt+F4；WM 关闭 |
| **全屏切换中屏幕被拔出** | 切换回调永不返回、状态卡在 SWITCHING、重复请求来回切 | FSM：DISPLAY_DISCONNECTED → RECONCILING；期间新切换请求不排队（目标不翻转）；2s 超时后读取平台实际模式 reconcile 到 WINDOWED/FULLSCREEN；显示恢复后再以实际模式重对 | Ctrl+Q / Cmd+Q；切换中也响应硬退出；WM 关闭 |
| **设置面板打开时按 Esc** | 若 Esc 被策略改走则关不掉面板 | 模态 Esc 是**内置固定绑定**，`policy_modal_esc_is_fixed()`，服务不允许在模态层改键；Esc 必关闭面板。展示层快捷键在模态下全部不穿透 | Esc 关面板 → Ctrl+Q 退出；组合态 Esc 交还给 IME，但 Ctrl+Q 仍退出 |
| **映射包损坏**（截断/改值/坏 JSON/摘要不符/冲突/保留键） | 设备加载坏策略导致语义错乱或无法操作 | 严格 JSON 解析 + schema + 双向冲突 + 保留键 + SHA-256 摘要全部通过才 stage；任一失败拒绝、保留当前策略、回执 rejected+原因；服务端同样拒绝（409/校验报告）。首启/断网使用内置默认策略 | 默认策略中的 Ctrl+Q；硬退出键不参与策略，永不丢失 |

## 其他边界

- **远程策略在按住中到达**：见 design.md §9。合成抬起 + 清修饰 + 原子切换，
  切换后无按下态，杜绝卡键；回执上报 released_keys，日志记 R_ACTIVATION。
- **重放损坏日志**：CRC/截断立即中止，标记 corrupt 帧号，不半可信继续。
- **网页伪造确认**：非 human origin 的 confirm_business 在唯一安全闸被拦，
  数据库 CHECK 约束再兜底。
- **队列溢出**：命令队列满只丢最旧可丢动作并计数，退出命令不走队列。
