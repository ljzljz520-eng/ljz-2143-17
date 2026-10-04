# 重放安全：为什么不能伪造现场业务确认

## 威胁模型

网页实验台希望"把一段输入序列重放一遍看看会触发什么命令"。但
`confirm_business`（现场人员对业务的确认，例如确认提交/放行）代表真实世界
责任。若重放能直接产出该确认，实验脚本就可以冒充现场人员完成业务动作。

## 控制点（只有一个）

所有命令（真人、本地重放、网页复算）都经过 `dispatcher.c` 的 `emit()`：

```c
if (cmd_is_business_confirm(c.id) && c.origin != ORIGIN_HUMAN) {
    stat_blocked_business++;
    on_command(...);          // 写 R_REPLAY_BLOCKED 审计帧
    return;                   // 不下发、不入可执行队列
}
```

- origin 在重放入口被**强制覆盖**为 `ORIGIN_REPLAY`，日志里自带的 origin
  不被信任（防止有人篡改日志标记为 human）。
- 服务端 `business_confirmations` 表对 origin 有
  `CHECK (origin = 'human')` 约束；HTTP 层对非 human 的确认请求直接 403。
- 重放会话与确认记录分表：`replay_sessions` / `replay_blocked_events`
  与 `business_confirmations` 物理隔离。

## 重放到底能做什么

- 复现**实验输入**：F11 是否只切一次、方向键连发多少步/何时被 max_hold 截断、
  IME 组合态快捷键是否被门控、激活边界合成抬起了几个键、视口最终位置等。
- 在 headless 模拟器里展示完整命令序列与"此处确认会被拦截"的标记，
  用于教学、回归与策略评审。
- 不能：触发真实全屏/退出（C 重放是 headless）、不能写业务确认、
  不能影响任何在线设备（没有"远程注入按键"通道，控制面是单向轮询）。
