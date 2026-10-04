# 输入策略系统（C/SDL2 客户端 + Python 服务 + 网页配置台）

在原有 C + SDL2 图片窗口之上，把写死的快捷键（Esc、F11、方向键、恢复背景）
升级为**可网页配置、可远程更新、可回放审计**的输入策略系统。

## 快速开始

```bash
# 单元/端到端测试（无需显示器，核心不依赖 SDL）
make test

# 仅启动策略服务 + 网页配置台（默认 :8080）
./service/run.sh
# 浏览器打开 http://localhost:8080

# 完整可视化链路（Xvfb + x11vnc + noVNC + C 程序 + 策略服务）
docker compose up --build
# noVNC: http://localhost:6080   配置台: http://localhost:8080

# 本地回放设备日志（不打开窗口、不产生真实副作用）
./replay_tool data/input-xxx.islog
```

设备端通过环境变量指定服务：

```bash
INPUT_STRATEGY_URL=http://127.0.0.1:8080 INPUT_STRATEGY_DEVICE=dev-001 \
  ./visual-window-app
```

## 组件

```text
src/core/      纯 C11 核心（不依赖 SDL，可独立单测）
  key/events      物理位置归一化按键 / 归一化输入事件
  command         统一命令 + 有界队列
  policy          策略 schema / 保留键 / 双向冲突 / 缺失 / 信封 SHA-256
  repeat          方向键有界连发（initial/repeat/max_hold + 视口钳制）
  fullscreen      F11 边沿触发 FSM（防来回切换、拔屏 reconcile、超时）
  activation      按住中换策略：旧键合成抬起 -> 原子启用新映射
  dispatcher      三层分发 + IME 组合态门控 + 唯一安全闸
  journal/replay  ISLOG 二进制日志(CRC32) + 强制 origin=replay 的重放
  json/sha/crc    严格 JSON（canonical 序列化）/ SHA-256 / CRC32
src/platform/  SDL2 适配、POSIX HTTP 轮询、策略同步
src/ui/        内置 5x7 字体 + 模态设置面板渲染
service/       Python 标准库 HTTP 服务 + SQLite + 同语义参考模拟器
service/web/   网页配置台（映射/校验/代次回执/实验重放/现场确认）
tests/         C 单元测试 + Python 跨语言与 HTTP E2E
docs/          design.md / failure-modes.md / replay-safety.md
```

## 关键语义（详见 docs/）

- **保留键**：`Ctrl+Q`（Linux/Win）、`Cmd+Q`（macOS）、`Alt+F4`
  在任何层、任何 IME 状态、任何策略代次下都可靠退出，不可映射；
  模态 `Esc` 固定关闭面板。
- **冲突**：同和弦两命令、同命令两和弦都被服务与客户端拒绝。
- **三层**：模态面板 > 输入框（IME 组合态快捷键全放行给输入法）> 展示画面。
- **有界连发**：方向键忽略 OS 自动重复，受 `max_hold_ms` 时间上界与视口
  空间上界双重约束。
- **全屏**：F11 只在首次非重复 keydown 触发，按住重复不会来回切换。
- **激活边界**：远程新策略在按住中到达时，旧键按旧映射合成抬起、修饰清零、
  再原子切换——切换后无按下态，杜绝卡键，released_keys 随回执上报。
- **重放安全**：重放只能复现实验输入；`confirm_business` 对 replay/remote
  一律拦截，真人确认表有 `CHECK(origin='human')` 兜底。

## 数据

SQLite（默认 `service/data/strategy.db`）：策略代次、设备回执、
真人业务确认、重放会话/拦截事件分表存储。设备日志为
`data/input-<device>.islog`（带 CRC32 帧，可上传网页做实验重放）。
