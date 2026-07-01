# Shared helpers for the AutoCoder bigtest harness (PowerShell 5.1 compatible).
# Dot-source from the run_*.ps1 scripts:  . "$PSScriptRoot\common.ps1"

$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot   # bigtest/ -> repo root

function Get-AutoCoderCli {
    if ($env:AUTOCODER_CLI -and (Test-Path $env:AUTOCODER_CLI)) { return $env:AUTOCODER_CLI }
    $candidates = @(
        (Join-Path $script:RepoRoot 'build\default\Debug\autocoder_cli.exe'),
        (Join-Path $script:RepoRoot 'build\default\Release\autocoder_cli.exe'),
        (Join-Path $script:RepoRoot 'build\ninja\autocoder_cli.exe')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    throw 'autocoder_cli.exe not found. Build it first: cmake --build --preset default'
}

# Advisory only: the CLI falls back to the key saved by the AutoCoder app
# (Windows Credential Manager), so a missing env var is not an error here.
# If neither source has a key, the CLI itself exits 2 with a clear message.
function Test-ApiKey {
    $provider = $env:AUTOCODER_PROVIDER
    if (-not $provider) { $provider = 'deepseek' }
    $envName = 'DEEPSEEK_API_KEY'
    if ($provider -eq 'claude') { $envName = 'ANTHROPIC_API_KEY' }
    if (-not (Get-Item "Env:$envName" -ErrorAction SilentlyContinue)) {
        Write-Host "[bigtest] $envName not set -- using the key stored by the AutoCoder app (if any)"
    }
}

# Fresh sandbox work dir (under TEMP) + results dir (under bigtest/results).
function New-RunDirs([string]$name) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $work = Join-Path $env:TEMP "autocoder_bigtest\$name-$stamp"
    $results = Join-Path $PSScriptRoot "results\$name-$stamp"
    New-Item -ItemType Directory -Force $work | Out-Null
    New-Item -ItemType Directory -Force $results | Out-Null
    return @{ Work = $work; Results = $results }
}

function Format-CliArg([string]$a) {
    if ($a -match '[\s"]') { return '"' + ($a -replace '"', '\"') + '"' }
    return $a
}

# Run one autocoder_cli turn with a hard timeout. Transcript/stderr/metrics
# land in $ResultsDir. Returns the CLI exit code (124 on timeout).
function Invoke-AutoCoder {
    param(
        [Parameter(Mandatory)] [string]$WorkDir,
        [Parameter(Mandatory)] [string]$Prompt,
        [Parameter(Mandatory)] [string]$ResultsDir,
        [int]$TimeoutMinutes = 15,
        [int]$MaxIter = 50,
        [string]$CheckCommand = ''
    )
    $cli = Get-AutoCoderCli
    $provider = $env:AUTOCODER_PROVIDER
    if (-not $provider) { $provider = 'deepseek' }

    $metrics = Join-Path $ResultsDir 'metrics.json'
    $stdout  = Join-Path $ResultsDir 'transcript.txt'
    $stderr  = Join-Path $ResultsDir 'stderr.txt'

    $argList = @('--provider', $provider, '--project', $WorkDir,
                 '--max-iter', "$MaxIter", '--auto-approve', '--metrics', $metrics)
    if ($CheckCommand) { $argList += @('--check', $CheckCommand) }
    $argList += $Prompt
    $argLine = ($argList | ForEach-Object { Format-CliArg $_ }) -join ' '

    Write-Host "[bigtest] cli      : $cli"
    Write-Host "[bigtest] provider : $provider (set AUTOCODER_PROVIDER to override)"
    Write-Host "[bigtest] work dir : $WorkDir"
    Write-Host "[bigtest] results  : $ResultsDir"
    Write-Host "[bigtest] timeout  : $TimeoutMinutes min, max-iter $MaxIter"

    $p = Start-Process -FilePath $cli -ArgumentList $argLine `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
            -NoNewWindow -PassThru
    $null = $p.Handle   # cache the handle or ExitCode is null after exit (PS 5.1)
    if (-not $p.WaitForExit($TimeoutMinutes * 60 * 1000)) {
        Write-Host "[bigtest] TIMEOUT after $TimeoutMinutes minutes -- killing the run"
        try { $p.Kill() } catch {}
        return 124
    }
    $p.WaitForExit()   # flush exit code after timed wait
    return $p.ExitCode
}

# True if the BMP exists, parses as an image, is at least 64x64, and the
# sampled pixels contain at least $MinDistinctColors distinct colors (a
# triangle on a clear color yields 2+; a uniform/black image yields 1).
# A corrupt/unparseable file returns $false -- GDI+ reports malformed images
# as a misleading "Out of memory" exception, which must not kill the script.
function Test-BmpHasContent {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [int]$MinDistinctColors = 2,
        [int]$SampleStep = 4
    )
    if (-not (Test-Path $Path)) { return $false }
    Add-Type -AssemblyName System.Drawing
    $bmp = $null
    try {
        $bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Path).Path)
    } catch {
        Write-Host "[bigtest] image does not parse ($($_.Exception.InnerException.Message)) -- likely a malformed header"
        return $false
    }
    try {
        if ($bmp.Width -lt 64 -or $bmp.Height -lt 64) { return $false }
        $colors = @{}
        for ($y = 0; $y -lt $bmp.Height; $y += $SampleStep) {
            for ($x = 0; $x -lt $bmp.Width; $x += $SampleStep) {
                $c = $bmp.GetPixel($x, $y).ToArgb()
                $colors[$c] = $true
                if ($colors.Count -gt $MinDistinctColors + 8) { return $true }
            }
        }
        return ($colors.Count -ge $MinDistinctColors)
    } finally {
        $bmp.Dispose()
    }
}

# Print the verdict, snapshot the work-dir file list, show key metrics.
# Returns 0 on pass, 1 on fail (use as the script exit code).
function Complete-Run {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [bool]$Pass,
        [string[]]$Notes = @(),
        [Parameter(Mandatory)] [hashtable]$Dirs
    )
    Get-ChildItem -Path $Dirs.Work -Recurse -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName.Substring($Dirs.Work.Length + 1) } |
        Out-File -Encoding utf8 (Join-Path $Dirs.Results 'workdir-files.txt')

    Write-Host ''
    foreach ($n in $Notes) { Write-Host "[bigtest]   $n" }

    $metricsPath = Join-Path $Dirs.Results 'metrics.json'
    if (Test-Path $metricsPath) {
        try {
            $m = Get-Content $metricsPath -Raw | ConvertFrom-Json
            $toolTotal = 0
            $m.tool_calls.PSObject.Properties | ForEach-Object { $toolTotal += $_.Value }
            Write-Host ("[bigtest]   metrics: {0} LLM requests, {1} tool calls, tokens cached={2} uncached={3} output={4}, wall {5:n0}s" -f `
                $m.llm_requests, $toolTotal, $m.tokens.cached_input, $m.tokens.uncached_input, $m.tokens.output, ($m.wall_ms / 1000))
        } catch {}
    }

    if ($Pass) {
        Write-Host "[bigtest] $Name : PASS" -ForegroundColor Green
        return 0
    }
    Write-Host "[bigtest] $Name : FAIL (transcript: $(Join-Path $Dirs.Results 'transcript.txt'))" -ForegroundColor Red
    return 1
}
