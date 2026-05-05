# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This is an ESP-IDF v5.5 project using CMake with the IDF Component Manager. Two application variants exist:
- `application/basic_demo` — development/demo
- `application/edge_agent` — production provisioning

**Build commands** (run inside `espressif/idf:release-v5.5` Docker image or with ESP-IDF v5.5 sourced locally):

```bash
# Build basic_demo for esp32s3 DevKitC (default board)
idf.py -B build set-target esp32s3
idf.py -B build build

# Build with a specific board (Board Manager patches SDK config)
idf.py -B build -DBOARD=esp32_S3_DevKitC_1 set-target esp32s3
idf.py -B build build

# Build for M5Stack CoreS3
idf.py -B build -DBOARD=m5stack_cores3 set-target esp32s3
idf.py -B build build

# Build for ESP32-P4
idf.py -B build -DBOARD=esp32_p4_function_ev set-target esp32p4
idf.py -B build build

# Flash and monitor
idf.py -B build flash monitor

# Configure interactively
idf.py -B build menuconfig
```

Boards are defined in `application/basic_demo/boards/` and `application/edge_agent/boards/`. Available boards: `esp32_S3_DevKitC_1`, `esp32_S3_DevKitC_1_breadboard`, `m5stack_cores3`, `m5stack_sticks3`, `esp32_p4_function_ev`.

There is no test suite. CI builds multiple board/target combinations via `idf_build_apps` + `esp-bmgr-assist`.

## High-Level Architecture

ESP-Claw is an edge AI agent framework running on ESP32. It receives events from IM channels (QQ, Telegram, WeChat, Feishu) and other sources, routes them through an event router, optionally invokes an LLM agent loop to decide on actions, and executes capabilities (tools) on-device.

### Module Layer (`components/claw_modules/`)

Five core modules form the framework:

1. **`claw_core`** — The LLM agent loop. Takes `claw_core_request_t` (user text + session/channel metadata), sends system prompt + message history + tools to an LLM backend, iterates on tool calls, produces `claw_core_response_t`. Supports OpenAI, Anthropic, and Custom backends. Uses a FreeRTOS task with request/response queues. Context providers inject additional prompt sections (memory, skills, tools catalog). Completion observers are notified on each agent cycle finish.

2. **`claw_cap`** — Capability registry. Each capability is a `claw_cap_descriptor_t` with an `execute` callback. Capabilities are grouped into `claw_cap_group_t` and registered at init. Groups can be enabled/disabled at runtime, and marked LLM-visible (exposed as tools). Capabilities are callable from the LLM (via `claw_cap_call_from_core`), from the event router, or from the console.

3. **`claw_event_router`** — Event-driven rule engine. Incoming `claw_event_t` objects are matched against JSON rules (stored on FATFS). Rules define match conditions (event type, source cap, channel, content) and actions: call a cap, run the agent, run a Lua script, send a message, emit a new event, or drop. Handles outbound message delivery by binding channel names to send-message capabilities.

4. **`claw_memory`** — Structured memory with session history, long-term memory storage/recall/forget, and editable profiles. Operates on FATFS files. Has a lightweight mode (`CONFIG_APP_CLAW_MEMORY_MODE_FULL` disabled) that skips async LLM-based memory extraction. Injects memory context into the agent prompt via context providers.

5. **`claw_skill`** — Skill management. Skills are user-facing bundles (defined as files on FATFS under `/fatfs/skills/`) that describe behaviors and imply which capability groups to activate. The LLM uses `activate_skill`/`deactivate_skill` to manage which capabilities are available per session. Deactivate guards prevent unloading while jobs are active.

### Capability Layer (`components/claw_capabilities/`)

Each capability is an independent ESP-IDF component:
- `cap_im_qq`, `cap_im_tg`, `cap_im_wechat`, `cap_im_feishu` — IM channel integrations (WebSocket-based, with attachment support via `cap_im_attachment`)
- `cap_lua` — Lua runtime for on-device scripting
- `cap_mcp_client`, `cap_mcp_server` — MCP protocol support
- `cap_scheduler` — Cron-like scheduled event emission
- `cap_system` — System info/control (heap, restart, task stats)
- `cap_time` — NTP time sync
- `cap_files` — Filesystem operations
- `cap_web_search` — Web search via Brave/Tavily APIs
- `cap_llm_inspect` — Debug LLM requests/responses
- `cap_skill_mgr`, `cap_session_mgr`, `cap_router_mgr` — Management interfaces
- `cap_cli` — Console CLI
- `cap_boards` — Board manager integration

### Lua Modules (`components/lua_modules/`)

Hardware-abstraction Lua bindings: GPIO, ADC, I2C, UART, PWM (MCPWM), LED strip, Audio, Camera, Button, Display, LCD Touch, System, Storage, Delay, Event Publisher, Call Capability, Board Manager, ESP Heap, Dwin (custom serial LCD protocol).

### Application Orchestration (`components/common/app_claw/`)

`app_claw.c` wires everything together: initializes memory → skills → capabilities → event router → claw_core. `app_capabilities.c` defines which caps register as groups, which are LLM-visible by default, and handles the enable/visible config parsing. The system prompt is defined here.

### Startup Flow

1. NVS init → settings load → board manager init → FATFS mount (SPIFFS via wear-leveling) → WiFi manager start → HTTP config server (provisioning portal) → captive DNS
2. `app_claw_start()`: init session mgr → init event router → init scheduler → init memory → init skills → register capability groups (prepare → register) → set LLM-visible groups → start all caps → init claw_core (if LLM configured) → add context providers → start core → start event router → start scheduler → start time sync → start CLI

### Data Flow (IM message → response)

1. IM capability receives message via WebSocket → builds `claw_event_t` → publishes to event router
2. Event router matches rules → if `run_agent` action, builds session ID, submits `claw_core_request_t` to claw_core
3. Claw_core collects context from providers (profile, memory, session history, skills, tools) → sends to LLM backend
4. LLM response may include tool calls → core calls `claw_cap_call_from_core` → capability executes → result fed back to LLM
5. Final text response → core emits response → event router routes to outbound IM channel

## This Fork's Additions

Based on the upstream ESP-Claw repository, this fork adds:

- **ASR (speech recognition)**: `components/claw_tools/` provides `claw_tools_asr_transcribe()` — decodes QQ SILK AMR audio to WAV via silk decoder (from esp-opus), then uploads to an ASR HTTP server. `tools/asr-server/` is a Python FastAPI server using Vosk for offline Chinese speech recognition.
- **QQ voice hook**: `cap_im_qq` intercepts QQ voice attachment messages, routes them through ASR transcription, and injects the recognized text back into the text message pipeline.
- **Dwin serial screen**: `lua_module_dwin` handles a custom serial protocol for a DWIN LCD touch screen.
- **GBK/UFT8 conversion**: `application/basic_demo/tools/GBK.c` and `gbk_table_gen.py` provide character encoding conversion for Chinese font rendering.

## Key Config Patterns

- Kconfig options are prefixed `CONFIG_APP_CLAW_*` (edge_agent) or `CONFIG_BASIC_DEMO_*` (basic_demo)
- Runtime settings (WiFi, LLM API key/model/profile, IM credentials) are managed via the HTTP config server and stored in NVS
- Event router rules live at `/fatfs/router_rules/router_rules.json`
- Scheduler state at `/fatfs/scheduler/schedules.json`
- Skills at `/fatfs/skills/`, Lua scripts at `/fatfs/scripts/`
- Session/memory data at `/fatfs/sessions/` and `/fatfs/memory/`

## Common Patterns

- Components use `idf_component_register()` in CMakeLists.txt with `REQUIRES` for public deps, `PRIV_REQUIRES` for private
- Error handling uses `ESP_RETURN_ON_ERROR`, `ESP_GOTO_ON_ERROR` macros from `esp_check.h`
- Logging uses `ESP_LOGI`/`ESP_LOGE`/`ESP_LOGW` with a static `TAG`
- Memory allocations from PSRAM use `MALLOC_CAP_SPIRAM`; task stacks can be placed in PSRAM via `claw_task_create()`
- Capabilities follow a `prepare` → `register` → `start` lifecycle
- JSON parsing uses cJSON throughout
