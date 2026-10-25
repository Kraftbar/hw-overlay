# hw-overlay

Small always-on-top hardware monitor for Windows, in plain C.

Shows CPU/RAM usage, GPU load and temp, motherboard temps and fan speeds in the corner of the screen, plus a tray icon with the CPU %.

It can also show how many Claude Code sessions are busy/idle and how many subagents are running. For that, copy `claude-state.ps1` to `~/.claude/hw-overlay/` and call it from Claude Code hooks (`UserPromptSubmit` → `working`, `Stop` → `idle`, `SessionEnd` → `end`, `SubagentStart`/`SubagentStop` → `agent-start`/`agent-stop`).

Board sensors come from the ASUS BIOS WMI interface (`ASUSHW`), GPU stats from NVIDIA's `nvml.dll`. No drivers or extra programs. Made on an ASUS ROG STRIX B450-F, other boards/GPUs just get fewer readings.

Needs admin because of the ASUS sensors.

Build with `build.bat` (needs Visual Studio C++ tools).

Ctrl+Alt+O to hide/show, Ctrl+Alt+Q to quit.

## Phone

The same stats are served at `http://<PC>:8787` (the address is in the tray menu). Open it in a browser, or use the Android app in `android/`: it shows the page and has a toggle that keeps the screen on and goes full screen.

Build the app with the Android SDK (build-tools 35, platform 35) and a JDK 17+, from `android/` in PowerShell:

```powershell
$sdk = "$env:LOCALAPPDATA\Android\Sdk"; $bt = "$sdk\build-tools\35.0.0"; $jar = "$sdk\platforms\android-35\android.jar"
mkdir build\gen, build\classes, build\dex
& $bt\aapt2 compile --dir res -o build\res.zip
& $bt\aapt2 link -o build\base.apk -I $jar --manifest AndroidManifest.xml --min-sdk-version 30 --target-sdk-version 35 --java build\gen build\res.zip
javac --release 11 -classpath $jar -d build\classes (Get-ChildItem -Recurse src, build\gen -Filter *.java).FullName
& $bt\d8.bat --lib $jar --min-api 30 --output build\dex (Get-ChildItem -Recurse build\classes -Filter *.class).FullName
Copy-Item build\base.apk build\unsigned.apk; Compress-Archive -Update build\dex\classes.dex build\unsigned.apk
& $bt\zipalign -p -f 4 build\unsigned.apk build\aligned.apk
& $bt\apksigner.bat sign --ks ~\.android\debug.keystore --ks-pass pass:android --out build\hw-overlay.apk build\aligned.apk
adb install -r build\hw-overlay.apk
```
