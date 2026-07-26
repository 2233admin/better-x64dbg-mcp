# better-x64dbg-mcp

中文 | [English](README.md)

改进版 x64dbg MCP (Model Context Protocol) 插件，基于 [@bromoket](https://github.com/bromoket) 的 [x64dbg_mcp](https://github.com/bromoket/x64dbg_mcp) 重构。

原项目采用两层架构——TypeScript MCP 服务器通过 HTTP 与 C++ x64dbg 插件通信。本项目**将两层合并为单个 C++ DLL**，直接在 x64dbg 内实现 [MCP Streamable HTTP 传输协议](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports#streamable-http)。

## 核心改进

| | 原项目 | 本项目 |
|---|---|---|
| **层数** | 2 层 (TypeScript + C++) | 1 层 (纯 C++) |
| **部署依赖** | Node.js + npm + DLL | 仅 DLL |
| **传输协议** | stdio (JSON-RPC over stdin/stdout) | Streamable HTTP |
| **事件推送** | 不支持 | SSE (断点、异常、暂停、单步、进程、DLL 事件) |
| **端点数量** | 148 条 REST 路由 | 1 个 (`/mcp`) |
| **客户端配置** | `npx -y x64dbg-mcp-server` | `http://127.0.0.1:27042/mcp` |

## 功能

- **MCP Streamable HTTP** (2025-03-26 规范) — 单一 `/mcp` 端点，`POST`/`GET`/`DELETE`
- **22 个聚合工具** 覆盖调试控制、寄存器、内存、断点、反汇编、符号、栈、线程、模块、搜索、追踪、转储、异常、控制流、补丁、标注、内存映射、进程信息、句柄、分析、日志捕获、原始命令
- **实时事件推送** 通过 SSE — 9 个 x64dbg 回调 (断点命中、暂停、异常、单步完成、调试停止、进程创建/退出、DLL 加载/卸载)
- **零部署成本** — 将 `.dp64` 放入 x64dbg 的 `plugins/` 目录即可

## 快速开始

1. 在 Visual Studio 中构建 (x64 Debug/Release)，或命令行：
   ```
   msbuild better-x64dbg-mcp.vcxproj /p:Configuration=Release /p:Platform=x64
   ```
2. 将输出的 `.dp64` 复制到 x64dbg 的 `plugins/` 目录
3. 启动 x64dbg — MCP 服务器自动在 `127.0.0.1:27042` 上运行
4. 连接 MCP 客户端：
   ```bash
   # Claude Code
   claude mcp add --transport http x64dbg http://127.0.0.1:27042/mcp

   # 或在 .mcp.json 中配置
   {
     "mcpServers": {
       "x64dbg": { "type": "http", "url": "http://127.0.0.1:27042/mcp" }
     }
   }
   ```

## 调试状态与暂停原因

`x64dbg_debug` 工具的 `state` 动作通过 `reason` 字段报告被调试进程**为何**暂停（仅在暂停时出现）。`reason` 取值之一：

| `reason` | 含义 | 细节对象 |
|---|---|---|
| `breakpoint` | 命中已注册的软件/硬件/内存断点 | `breakpoint`：`{ type, address, name, module, hit_count, log? }` |
| `step` | 单步（步入/步过/步出）后暂停 | — |
| `exception` | 因异常暂停 | `exception`：`{ code, name, address, first_chance }` |
| `unknown` | 无法明确归因——手动暂停、DLL/DebugString/loadlib 事件暂停，或未通过保守校验的断点 | — |

当原因带有附加上下文时，会包含一个以 `reason` 取值为键的子对象（`breakpoint` / `exception`）。`step` 和 `unknown` 没有细节对象。断点细节中的 `type` 表示断点类型（`software` / `hardware` / `memory` / `dll` / `exception`）；`log`（格式化失败时为 `log_raw`）仅在断点设置了日志文本时出现。

```json
{
  "state": "paused",
  "cip": "0x00007FFC09EBE1A0",
  "module": "neteasemusic",
  "reason": "breakpoint",
  "breakpoint": {
    "type": "software",
    "address": "0x00007FFC09EBE1A0",
    "name": "",
    "module": "neteasemusic",
    "hit_count": 1
  }
}
```

`reason` 采用保守归因：只有当 `CB_BREAKPOINT` 已触发、断点无 `commandText`、且自该回调以来 CIP 未变化时，才会报告为 `breakpoint`。无法明确归因的暂停会退为 `unknown`，而不是被错误标注。

## 读取日志窗口

x64dbg 插件 SDK 没有日志回调，因此 `x64dbg_log` 工具按需捕获 GUI 日志窗口（`GuiFlushLog` + `GuiLogSave` 落到临时文件后增量 diff），并在插件侧维护最近 10,000 行的环形缓冲。**日志断点**从此可以在 MCP 工作流中使用。

| 动作 | 用途 |
|---|---|
| `read` | 增量读取：传入上次的 `next_cursor`（0 = 最旧保留行）；`truncated` 为 `false` 前持续翻页 |
| `tail` | 先捕获一次，再返回最后 N 个完整行 |
| `write` | 向日志窗口写入一条标记行；返回的 `[cursor_before, next_cursor)` 区间恰好包住它 |
| `clear` | 清空 GUI 日志与插件缓冲（cursor 不重置） |
| `status` | 缓冲统计、`log_enabled`、`doc_bytes` 余量 |
| `read_file` | 按字节偏移分页读文件 — 与下方断点 `log_file` 字段配合使用 |

需要了解的语义：

- Cursor 是单调递增的行序号。插件重载后 `capture_epoch` 会变化——此时旧 cursor 视为失效。
- GUI 日志 document 超过约 1 MiB 时会被 x64dbg **整体清空**（约每 50 秒检查一次）。插件环形缓冲不受影响；purge 或手动清空表现为 `log_cleared: true`，purge 前从未捕获过的行将永久丢失（cursor 缺口由 `dropped`/`dropped_count` 报告）。
- `partial` 是尚未以换行结束的末尾行；它没有 cursor，补完后会以正常行再次出现。
- 高频日志断点建议写文件而不是写窗口：通过 `x64dbg_breakpoints` `configure` 设置 `log_file`（绝对路径），再用 `read_file` 分页读取——完全绕开日志窗口及其 1 MiB 清空机制，且被调试进程运行中即可读取。

```json
{ "action": "configure", "address": "0x7FFC09EBE1A0",
  "log_text": "hit rcx={rcx}", "log_file": "C:\\temp\\bp.log",
  "silent": true, "fast_resume": true }
```

## 构建要求

- Visual Studio 2022+，MSVC v145 工具集
- Windows SDK 10.0
- C++23 (x64 与 Win32)

所有依赖已内置：x64dbg Plugin SDK 和 nlohmann/json。

## 致谢

本项目基于 [@bromoket](https://github.com/bromoket) 的 [x64dbg_mcp](https://github.com/bromoket/x64dbg_mcp) 改造。Handler 逻辑、Bridge 执行器和 UI 对话框适配自原项目的 C++ 插件。MCP 协议层、事件系统和 Streamable HTTP 传输为全新实现。

## 许可证

MIT
