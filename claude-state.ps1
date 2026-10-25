# Claude Code hook: records session/subagent state for hw-overlay.
# Usage (from a hook): pwsh -NoProfile -File claude-state.ps1 <working|idle|needs|end|agent-start|agent-stop|bg-start>
# "needs" = waiting for the user (permission prompt or question).
# Hook input JSON arrives on stdin. Never fails the hook.
#
# Session file, one line each: state, project folder name, latest prompt,
# session title. System messages (task notifications etc. start with "<")
# are not prompts, and the desktop app's throwaway "scratch-..." folders are
# left out.
param([string]$State)

# The session's title as the app shows it: the last "custom-title" entry in
# its transcript. Only the end of the file is read; transcripts get large.
function Get-SessionTitle([string]$path) {
    if (-not $path -or -not (Test-Path $path)) { return '' }
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek([Math]::Max(0, $fs.Length - 262144), 'Begin') | Out-Null
        $tail = (New-Object IO.StreamReader($fs)).ReadToEnd()
    } finally { $fs.Dispose() }
    $m = [regex]::Matches($tail, '"customTitle":("(?:[^"\\]|\\.)*")')
    if ($m.Count) { return ($m[$m.Count - 1].Groups[1].Value | ConvertFrom-Json) }
    return ''
}
try {
    $in = [Console]::In.ReadToEnd() | ConvertFrom-Json
    $root = Join-Path $env:USERPROFILE '.claude\hw-overlay'
    $sessions = Join-Path $root 'sessions'
    $agents = Join-Path $root 'agents'
    New-Item -ItemType Directory -Force -Path $sessions, $agents | Out-Null

    $sid = ($in.session_id -replace '[^\w-]', '')
    if (-not $sid) { exit 0 }
    $file = Join-Path $sessions $sid

    # A turn that ended by asking the user something (the desktop app's
    # next-step box) leaves a flag in asks\; until the next prompt the
    # session counts as "needs" rather than idle.
    $ask = Join-Path $root "asks\$sid"
    if ($State -eq 'working') { Remove-Item -Force -ErrorAction SilentlyContinue $ask }
    elseif ($State -eq 'idle' -and (Test-Path $ask)) { $State = 'needs' }

    # A finished background task comes back as a <task-notification> prompt
    # naming the tool call that started it.
    if ($in.prompt -match '<tool-use-id>([\w-]+)</tool-use-id>') {
        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.bg-$($Matches[1])")
    }

    switch ($State) {
        { $_ -in 'working', 'idle', 'needs' } {
            $old = @(if (Test-Path $file) { Get-Content $file -Encoding utf8 })
            $folder = if ($in.cwd) { Split-Path $in.cwd -Leaf } elseif ($old.Count -gt 1) { $old[1] } else { '' }
            if ($folder -like 'scratch-*') { $folder = '' }
            $prompt = if ($old.Count -gt 2 -and -not $old[2].StartsWith('<')) { $old[2] } else { '' }
            $new = if ($in.prompt) { ($in.prompt -replace '\s+', ' ').Trim() } else { '' }
            if ($new -and -not $new.StartsWith('<')) {
                $prompt = if ($new.Length -gt 60) { $new.Substring(0, 60) + '...' } else { $new }
            }
            $title = Get-SessionTitle $in.transcript_path
            if (-not $title -and $old.Count -gt 3) { $title = $old[3] }
            Set-Content -Path $file -Encoding utf8NoBOM -Value @($State, $folder, $prompt, $title)
        }
        'end' {
            # The desktop app ends a session's process once its turn is done,
            # even while it waits for an answer, so only forget sessions that
            # aren't waiting on the user.
            Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $agents "$sid.*")
            $waiting = (Test-Path $ask) -or ((Test-Path $file) -and (Get-Content $file -TotalCount 1) -eq 'needs')
            if (-not $waiting) { Remove-Item -Force -ErrorAction SilentlyContinue $file, $ask }
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
