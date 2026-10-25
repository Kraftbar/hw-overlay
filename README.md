# hw-overlay

A tiny always-on-top hardware monitor for Windows, written in plain C (one file, no dependencies).

- **Overlay** in the top-left corner: CPU load, RAM, GPU load/temp/fan, motherboard temperatures and fan RPM.
  It is click-through and never steals focus.
- **Tray icon** showing the CPU temperature as a number. Hover for all stats, left-click to show/hide the overlay,
  right-click for a menu.

## Where the numbers come from

| Data | Source |
| --- | --- |
| CPU load, RAM | `GetSystemTimes`, `GlobalMemoryStatusEx` |
| Temperatures, fan RPM | ASUS BIOS WMI interface (`root\WMI`, class `ASUSHW`) |
| GPU load, temp, fan % | NVIDIA NVML (`nvml.dll`, installed with the driver) |

No kernel driver or helper program is used. Sensors that are not available are simply left out, so the program still
runs on non-ASUS boards (without board sensors) and on non-NVIDIA GPUs (without the GPU line).

Tested on an ASUS ROG STRIX B450-F GAMING with a Ryzen 7 3700X and an RTX 3060 Ti. The `ASUSHW` interface is the same
one the Linux `asus_wmi_sensors` driver uses, so other ASUS boards from that era should work too.

## Requirements

- Windows 10/11
- Administrator rights (Windows only exposes the ASUS sensor interface to admin processes). The exe asks for elevation
  on launch.

## Build

With Visual Studio 2022 (or the Build Tools) installed, with the C++ workload:

```bat
build.bat
```

## Usage

Run `overlay.exe` and accept the UAC prompt.

| Hotkey | Action |
| --- | --- |
| Ctrl+Alt+O | Show/hide overlay |
| Ctrl+Alt+Q | Quit |

On Windows 11 a new tray icon starts in the hidden ^ menu; drag it onto the taskbar to keep it visible.

The overlay shows over windowed and borderless-fullscreen games, not exclusive fullscreen.
