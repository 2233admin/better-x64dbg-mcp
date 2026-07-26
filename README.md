# better-x64dbg-mcp

[中文](README_CN.md) | English

An improved x64dbg MCP (Model Context Protocol) plugin, rebuilt from [x64dbg_mcp](https://github.com/bromoket/x64dbg_mcp) by [@bromoket](https://github.com/bromoket).

The original project uses a two-layer architecture — a TypeScript MCP server communicating with a C++ x64dbg plugin via HTTP. This project **replaces both layers with a single C++ DLL** that implements the [MCP Streamable HTTP transport](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports#streamable-http) directly inside x64dbg.

## Key Improvements

| | Original | This Project |
|---|---|---|
| **Layers** | 2 (TypeScript + C++) | 1 (C++ only) |
| **Dependencies** | Node.js + npm + DLL | DLL only |
| **Transport** | stdio (JSON-RPC over stdin/stdout) | Streamable HTTP |
| **Event Push** | Not possible | SSE (breakpoint, exception, pause, step, process, DLL events) |
| **Endpoints** | 148 REST routes | 1 (`/mcp`) |
| **Client Config** | `npx -y x64dbg-mcp-server` | `http://127.0.0.1:27042/mcp` |

## Features

- **MCP Streamable HTTP** (2025-03-26 spec) — single `/mcp` endpoint, `POST`/`GET`/`DELETE`
- **22 mega-tools** covering debug control, registers, memory, breakpoints, disassembly, symbols, stack, threads, modules, search, tracing, dumping, exceptions, control flow, patches, annotations, memory map, process info, handles, analysis, log capture, and raw commands
- **Real-time event push** via SSE — 9 x64dbg callbacks (breakpoint hit, pause, exception, step complete, debug stop, process create/exit, DLL load/unload)
- **Zero deployment friction** — drop the `.dp64` into x64dbg's `plugins/` folder

## Quick Start

1. Build the project in Visual Studio (x64 Debug/Release) or:
   ```
   msbuild better-x64dbg-mcp.vcxproj /p:Configuration=Release /p:Platform=x64
   ```
2. Copy the output `.dp64` to your x64dbg `plugins/` directory
3. Start x64dbg — the MCP server starts automatically on `127.0.0.1:27042`
4. Connect your MCP client:
   ```bash
   # Claude Code
   claude mcp add --transport http x64dbg http://127.0.0.1:27042/mcp

   # Or in .mcp.json
   {
     "mcpServers": {
       "x64dbg": { "type": "http", "url": "http://127.0.0.1:27042/mcp" }
     }
   }
   ```

## Debug State & Pause Reasons

The `x64dbg_debug` tool's `state` action reports **why** the debuggee is paused through a `reason` field (present only while paused). `reason` is one of:

| `reason` | Meaning | Detail object |
|---|---|---|
| `breakpoint` | Paused at a registered software/hardware/memory breakpoint | `breakpoint`: `{ type, address, name, module, hit_count, log? }` |
| `step` | Paused after a single step (step into/over/out) | — |
| `exception` | Paused on an exception | `exception`: `{ code, name, address, first_chance }` |
| `unknown` | Cause not positively identified — manual pause, a DLL/DebugString/loadlib event pause, or a breakpoint that did not pass conservative validation | — |

When the reason carries extra context, a sub-object keyed by the `reason` value is included (`breakpoint` / `exception`). `step` and `unknown` have no detail object. In the breakpoint detail, `type` is the breakpoint kind (`software` / `hardware` / `memory` / `dll` / `exception`), and `log` (or `log_raw` if formatting failed) appears only when the breakpoint defines log text.

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

`reason` is attributed conservatively: it is reported as `breakpoint` only when a `CB_BREAKPOINT` fired, the breakpoint has no `commandText`, and CIP is unchanged since that callback. Pauses that cannot be positively attributed fall through to `unknown` rather than being mislabeled.

## Reading the Log Window

The x64dbg plugin SDK has no log callback, so the `x64dbg_log` tool captures the GUI log window on demand (`GuiFlushLog` + `GuiLogSave` to a temp file, diffed incrementally) and keeps the last 10,000 lines in a plugin-side ring buffer. This finally makes **logging breakpoints** usable from MCP.

| Action | Purpose |
|---|---|
| `read` | Incremental read: pass the previous `next_cursor` (or 0 for oldest retained); page until `truncated` is `false` |
| `tail` | Fresh capture, then the last N complete lines |
| `write` | Emit a marker line into the log; the returned `[cursor_before, next_cursor)` range brackets it |
| `clear` | Clear the GUI log and the retained buffer (cursors are not reset) |
| `status` | Buffer stats, `log_enabled`, `doc_bytes` headroom |
| `read_file` | Byte-offset paged read of a file — pair with the breakpoint `log_file` field below |

Semantics worth knowing:

- Cursors are monotonic line sequence numbers. `capture_epoch` changes when the plugin reloads — treat old cursors as stale when it does.
- The GUI log document is purged wholesale by x64dbg when it exceeds ~1 MiB (checked every ~50 s). The plugin's ring buffer outlives the purge; a purge or manual clear surfaces as `log_cleared: true`, and lines never captured before a purge are gone (`dropped`/`dropped_count` report cursor gaps).
- `partial` is the trailing line not yet terminated by a newline; it has no cursor and reappears as a normal line once completed.
- High-frequency logging breakpoints should log to a file instead of the window: set `log_file` (absolute path) via `x64dbg_breakpoints` `configure`, then page it with `read_file` — this bypasses the log window and its 1 MiB purge entirely, and works while the debuggee is running.

```json
{ "action": "configure", "address": "0x7FFC09EBE1A0",
  "log_text": "hit rcx={rcx}", "log_file": "C:\\temp\\bp.log",
  "silent": true, "fast_resume": true }
```

## Build Requirements

- Visual Studio 2022+ with MSVC v145 toolset
- Windows SDK 10.0
- C++23 (x64 and Win32)

All dependencies are vendored: x64dbg Plugin SDK and nlohmann/json.

## Acknowledgments

This project is built upon [x64dbg_mcp](https://github.com/bromoket/x64dbg_mcp) by [@bromoket](https://github.com/bromoket). The handler logic, bridge executor, and UI dialogs are adapted from the original C++ plugin. The MCP protocol layer, event system, and Streamable HTTP transport are new.

## License

MIT
