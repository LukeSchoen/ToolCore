# build_guard.ps1 - the close-and-start-again half of a build script.
#
# A running exe cannot be overwritten, so build_common.bat calls this before it
# compiles and again when it is done:
#
#     build_guard.ps1 close   <exe> [<exe> ...]   stop them, remember how
#     build_guard.ps1 restore <exe> [<exe> ...]   start those the same way
#     build_guard.ps1 restore                     ...start every exe still on
#                                                 the list, whose build was cut
#                                                 short before it could
#
# Only copies running out of the exact paths it is given are touched. Matching
# the whole path is the point: this PC's own endpoint runs out of
# C:\Luke\Applications\Instrument, and a build in the source tree must never
# take it down.
#
# What was stopped is remembered in one file per exe, named for that exe, so a
# build of one program can only ever close and start that program: a script's
# restore names its own exe and reads its own file, and a list left behind by
# another program's build is not something it can see.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Verb,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Exes
)

# Where the copy of one exe that a build stopped is remembered. One file per
# exe name, so no build reads another program's list.
function Get-StatePath {
    param([string] $Exe)

    $leaf = [System.IO.Path]::GetFileName($Exe)
    return (Join-Path $env:TEMP "tools_build_running_$leaf.txt")
}

# What a process was started with, after its own exe path.
function Get-StartedAs {
    param([string] $CommandLine, [string] $Exe)

    if ([string]::IsNullOrWhiteSpace($CommandLine)) { return '' }
    $line = $CommandLine.Trim()

    if ($line.StartsWith('"')) {
        $end = $line.IndexOf('"', 1)
        if ($end -lt 1) { return '' }
        return $line.Substring($end + 1).Trim()
    }
    $space = $line.IndexOf(' ')
    if ($space -lt 1) { return '' }
    return $line.Substring($space + 1).Trim()
}

switch ($Verb.ToLowerInvariant()) {
    'close' {
        if ($Exes.Count -eq 0) {
            Write-Host 'usage: build_guard.ps1 close <exe> [<exe> ...]'
            exit 2
        }

        Write-Host 'Stopping what this build is about to overwrite...'

        foreach ($raw in $Exes) {
            $exe = [System.IO.Path]::GetFullPath($raw)
            $leaf = [System.IO.Path]::GetFileName($exe)
            $statePath = Get-StatePath $exe
            $state = New-Object System.Collections.Generic.List[string]

            # Get-CimInstance is the one place that answers "which path is this
            # process running from"; Get-Process alone would match by name.
            $mine = @(Get-CimInstance Win32_Process -Filter "Name = '$leaf'" `
                      -ErrorAction SilentlyContinue |
                      Where-Object { $_.ExecutablePath -and ($_.ExecutablePath -ieq $exe) })

            if ($mine.Count -eq 0) {
                Write-Host "  ${leaf}: not running"
            } else {
                foreach ($p in $mine) {
                    $startedAs = Get-StartedAs -CommandLine $p.CommandLine -Exe $exe
                    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
                    try { Wait-Process -Id $p.ProcessId -Timeout 10 -ErrorAction Stop } catch { }
                    $state.Add("$exe`t$startedAs")
                    if ($startedAs) {
                        Write-Host "  ${leaf}: stopped (pid $($p.ProcessId)), was started as '$startedAs'"
                    } else {
                        Write-Host "  ${leaf}: stopped (pid $($p.ProcessId))"
                    }
                }
            }

            [System.IO.File]::WriteAllLines($statePath, $state)
        }

        # Windows is still letting go of the images it just unmapped, and cpc
        # fails on an exe it cannot write. An empty state file is written on
        # purpose, so a run that closed nothing cannot pick up a list left
        # behind by a run that did.
        Start-Sleep -Milliseconds 400
        exit 0
    }

    'restore' {
        # Every restore a build script makes names its own exe. The form with
        # nothing named is the repair: whatever list is still lying about, from
        # a build that was cut short between its close and its restore.
        if ($Exes.Count -eq 0) {
            $paths = @(Get-ChildItem -LiteralPath $env:TEMP `
                       -Filter 'tools_build_running_*.txt' `
                       -ErrorAction SilentlyContinue |
                       ForEach-Object { $_.FullName })
        } else {
            $paths = @($Exes | ForEach-Object { Get-StatePath $_ })
        }

        $start = New-Object System.Collections.Generic.List[string]
        foreach ($path in $paths) {
            if (-not $path) { continue }
            if (-not (Test-Path -LiteralPath $path)) { continue }
            foreach ($line in @([System.IO.File]::ReadAllLines($path))) {
                if ($line) { $start.Add($line) }
            }
            Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        }
        if ($start.Count -eq 0) { exit 0 }

        Write-Host 'Starting what the build closed...'
        foreach ($line in $start) {
            $parts = $line.Split("`t", 2)
            $exe = $parts[0]
            $startedAs = ''
            if ($parts.Count -gt 1) { $startedAs = $parts[1] }
            $leaf = [System.IO.Path]::GetFileName($exe)

            # A build that did not link leaves no exe to start. Saying so beats
            # a Start-Process error nobody reads.
            if (-not (Test-Path -LiteralPath $exe)) {
                Write-Host "  ${leaf}: not started, there is no $leaf to start"
                continue
            }

            $dir = [System.IO.Path]::GetDirectoryName($exe)
            if ($startedAs) {
                Start-Process -FilePath $exe -ArgumentList $startedAs -WorkingDirectory $dir
                Write-Host "  ${leaf}: started again as '$startedAs'"
            } else {
                Start-Process -FilePath $exe -WorkingDirectory $dir
                Write-Host "  ${leaf}: started again"
            }
        }
        exit 0
    }

    default {
        Write-Host 'usage: build_guard.ps1 close <exe> [<exe> ...] | restore [<exe> ...]'
        exit 2
    }
}
