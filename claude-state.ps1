# Claude Code hook: records session/subagent state for hw-overlay.
# Usage (from a hook): pwsh -NoProfile -File claude-state.ps1 <working|idle|end|agent-start|agent-stop>
# Hook input JSON arrives on stdin. Never fails the hook.
#
# Session file, one line each: state, project folder name, first prompt.
param([string]$State)
try {
    $in = [Console]::In.ReadToEnd() | ConvertFrom-Json
    $root = Join-Path $env:USERPROFILE '.claude\hw-overlay'
    $sessions = Join-Path $root 'sessions'
    $agents = Join-Path $root 'agents'
    New-Item -ItemType Directory -Force -Path $sessions, $agents | Out-Null

    $sid = ($in.session_id -replace '[^\w-]', '')
    if (-not $sid) { exit 0 }
    $file = Join-Path $sessions $sid

    switch ($State) {
        { $_ -in 'working', 'idle' } {
            $old = @(if (Test-Path $file) { Get-Content $file -Encoding utf8 })
            $folder = if ($in.cwd) { Split-Path $in.cwd -Leaf } elseif ($old.Count -gt 1) { $old[1] } else { '' }
            $prompt = if ($old.Count -gt 2 -and $old[2]) { $old[2] } else { '' }
            if (-not $prompt -and $in.prompt) {
                $prompt = ($in.prompt -replace '\s+', ' ').Trim()
                if ($prompt.Length -gt 60) { $prompt = $prompt.Substring(0, 60) + '...' }
            }
            Set-Content -Path $file -Encoding utf8NoBOM -Value @($State, $folder, $prompt)
        }
        'end' {
            Remove-Item -Force -ErrorAction SilentlyContinue $file
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
