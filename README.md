# Visual Window App（C/SDL2 + 输入策略系统）

这是一个基于 C11 和 SDL2 的真实桌面窗口程序。程序加载背景图，并把原先硬编码快捷键重构为可审计、可远程更新、可回放的输入策略系统。

## 功能

- 受保护物理键：`Esc`、`F11`、方向键；
- 可配置恢复背景键：`R`、`B`、`Backspace`；
- 方向键/WASD 白名单别名，首动一次、长按按策略有界连续移动；
- F11 仅在按下边沿切换，系统 auto-repeat 不会来回全屏；
- 模态面板、输入框、展示画面三层分发；
- 中文 IME 组合态期间快捷键不截获输入；
- 远程策略代次 SHA-256 校验、保留键/冲突检查、staged→activated 抬键边界；
- SQLite 保存策略代次、设备状态和回执；
- JSON Lines 哈希链日志，只允许实验输入回放，不伪造业务确认；
- 失焦清键、全屏中显示器拔出、坏策略包和设置面板 Esc 均有可靠退出路径。

## 架构

```text
Browser 管理页
      │ HTTP JSON
Policy Service（Python 标准库 + SQLite）
      │ 版本化策略信封 + SHA-256 + 设备回执
C/SDL 平台适配层（scancode / focus / display / IME）
      │
纯 C 输入策略核心（无 SDL，可单测、可回放）
      │
OutputList 命令：移动 / 恢复背景 / 全屏 / 关面板 / 安全退出
```

详细设计见 [`docs/input-strategy.md`](docs/input-strategy.md)。

## 目录

```text
src/input/          纯 C 输入策略核心、JSON 策略解析、SHA-256、回放日志
src/main.c          SDL 事件适配和主循环
src/renderer.*      背景、移动标记、设置面板绘制
src/policy_client.* 最小 HTTP 策略传输（POSIX）
service/            SQLite 策略管理服务
web/                中文管理页面
tests/              C 核心测试和服务测试
docs/               设计说明
```

## 本地测试

```bash
make core-test
make service-test
# 或
make test
```

C 核心测试不依赖 SDL，可直接运行；服务测试只依赖 Python 标准库。

## 构建和运行窗口程序

```bash
make
./visual-window-app
```

可选地连接策略服务：

```bash
POLICY_URL=http://127.0.0.1:8090 DEVICE_ID=kiosk-01 ./visual-window-app
```

也可以用命令行：

```bash
./visual-window-app --policy-url http://127.0.0.1:8090
```

## 启动管理服务

```bash
python3 service/policy_server.py
# 打开 http://127.0.0.1:8090
```

SQLite 数据库默认在 `data/policies.sqlite3`；可用 `POLICY_DB` 和 `POLICY_PORT` 覆盖。

## noVNC

`docker compose up --build` 后访问 `http://localhost:6080`。noVNC 只承载真实 C 窗口画面；管理页面运行在独立策略服务中。

## 安全退出

- 展示画面：`Ctrl+Shift+Q`；
- 设置面板打开：先按 Esc 关面板，或直接 `Ctrl+Shift+Q`；
- IME 组合态：`Ctrl+Shift+Q` 仍作为安全兜底，窗口关闭按钮始终有效。
