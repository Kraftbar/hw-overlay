# hw-overlay

Small always-on-top hardware monitor for Windows, in plain C.

Shows CPU/RAM usage, GPU load and temp, motherboard temps and fan speeds in the corner of the screen, plus a tray icon with the CPU %.

It can also show how many Claude Code sessions are busy/idle and how many subagents are running. For that, copy `claude-state.ps1` to `~/.claude/hw-overlay/` and call it from Claude Code hooks (`UserPromptSubmit` → `working`, `Stop` → `idle`, `SessionEnd` → `end`, `SubagentStart`/`SubagentStop` → `agent-start`/`agent-stop`).

Board sensors come from the ASUS BIOS WMI interface (`ASUSHW`), GPU stats from NVIDIA's `nvml.dll`. No drivers or extra programs. Made on an ASUS ROG STRIX B450-F, other boards/GPUs just get fewer readings.

Needs admin because of the ASUS sensors.

Build with `build.bat` (needs Visual Studio C++ tools).

Ctrl+Alt+O to hide/show, Ctrl+Alt+Q to quit.
