# Claude Code hook: records session/subagent state for hw-overlay.
# Usage (from a hook): pwsh -NoProfile -File claude-state.ps1 <working|idle|end|agent-start|agent-stop>
# Hook input JSON arrives on stdin. Never fails the hook.
param([string]$State)
try {
    $in = [Console]::In.ReadToEnd() | ConvertFrom-Json
    $root = Join-Path $env:USERPROFILE '.claude\hw-overlay'
    $sessions = Join-Path $root 'sessions'
    $agents = Join-Path $root 'agents'
    New-Item -ItemType Directory -Force -Path $sessions, $agents | Out-Null

    $sid = ($in.session_id -replace '[^\w-]', '')
    if (-not $sid) { exit 0 }

    switch ($State) {
        'working' { Set-Content -NoNewline -Path (Join-Path $sessions $sid) -Value 'working' }
        'idle'    { Set-Content -NoNewline -Path (Join-Path $sessions $sid) -Value 'idle' }
        'end'     {
            Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $sessions $sid)
            Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.*")
        }
        'agent-start' {
            $aid = ($in.agent_id -replace '[^\w-]', '')
            if ($aid) { Set-Content -NoNewline -Path (Join-Path $agents "$sid.$aid") -Value $in.agent_type }
        }
        'agent-stop' {
            $aid = ($in.agent_id -replace '[^\w-]', '')
            if ($aid) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.$aid") }
        }
    }
} catch { }
exit 0
