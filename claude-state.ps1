# Claude Code hook: records session/subagent state for hw-overlay.
# Usage (from a hook): pwsh -NoProfile -File claude-state.ps1 <working|idle|end|agent-start|agent-stop|bg-start>
# Hook input JSON arrives on stdin. Never fails the hook.
#
# Session file, one line each: state, project folder name, latest prompt.
# System messages (task notifications etc. start with "<") are not prompts,
# and the desktop app's throwaway "scratch-..." folders are left out.
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

    # A finished background task comes back as a <task-notification> prompt
    # naming the tool call that started it.
    if ($in.prompt -match '<tool-use-id>([\w-]+)</tool-use-id>') {
        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.bg-$($Matches[1])")
    }

    switch ($State) {
        { $_ -in 'working', 'idle' } {
            $old = @(if (Test-Path $file) { Get-Content $file -Encoding utf8 })
            $folder = if ($in.cwd) { Split-Path $in.cwd -Leaf } elseif ($old.Count -gt 1) { $old[1] } else { '' }
            if ($folder -like 'scratch-*') { $folder = '' }
            $prompt = if ($old.Count -gt 2 -and -not $old[2].StartsWith('<')) { $old[2] } else { '' }
            $new = if ($in.prompt) { ($in.prompt -replace '\s+', ' ').Trim() } else { '' }
            if ($new -and -not $new.StartsWith('<')) {
                $prompt = if ($new.Length -gt 60) { $new.Substring(0, 60) + '...' } else { $new }
            }
            Set-Content -Path $file -Encoding utf8NoBOM -Value @($State, $folder, $prompt)
        }
        'end' {
            Remove-Item -Force -ErrorAction SilentlyContinue $file
            Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.*")
        }
        'agent-start' {
            $aid = ($in.agent_id -replace '[^\w-]', '')
            $name = if ($in.agent_type) { "$($in.agent_type) agent" } else { 'agent' }
            if ($aid) { Set-Content -NoNewline -Encoding utf8NoBOM -Path (Join-Path $agents "$sid.$aid") -Value $name }
        }
        'bg-start' {
            # Named after the tool call's description, or its command.
            $tid = ($in.tool_use_id -replace '[^\w-]', '')
            $what = if ($in.tool_input.description) { $in.tool_input.description } else { "$($in.tool_input.command)" }
            $what = ($what -replace '\s+', ' ').Trim()
            if ($what.Length -gt 50) { $what = $what.Substring(0, 50) + '...' }
            if ($tid) { Set-Content -NoNewline -Encoding utf8NoBOM -Path (Join-Path $agents "$sid.bg-$tid") -Value "$($in.tool_name): $what" }
        }
        'agent-stop' {
            $aid = ($in.agent_id -replace '[^\w-]', '')
            if ($aid) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.$aid") }
        }
    }
} catch { }
exit 0
