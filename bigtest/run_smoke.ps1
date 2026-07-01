# Bigtest level 1 -- smoke (~2-5 min, cheap). Empty folder -> hello-world C
# program with CMake, built and run by the agent. Catches plumbing regressions.

. "$PSScriptRoot\common.ps1"
Test-ApiKey

$dirs = New-RunDirs 'smoke'

$prompt = @'
Create a tiny C program in this empty folder.
Requirements:
- main.c that prints exactly: Hello, AutoCoder! (followed by a single newline)
- a CMakeLists.txt for it; the executable target must be named hello
- configure with CMake into the ./build directory, build the Debug config,
  then run the built hello executable and confirm it prints the expected line
Do not declare success until you have actually run the executable.
'@

$exit = Invoke-AutoCoder -WorkDir $dirs.Work -Prompt $prompt -ResultsDir $dirs.Results `
    -TimeoutMinutes 10 -MaxIter 30 -CheckCommand 'cmake --build build'

$pass = $true
$notes = @()
if ($exit -ne 0) { $pass = $false; $notes += "CLI exited with code $exit (0 expected)" }

$exe = Get-ChildItem -Path (Join-Path $dirs.Work 'build') -Recurse -Filter 'hello.exe' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) {
    $pass = $false
    $notes += 'hello.exe not found under build/'
} else {
    $out = (& $exe.FullName) -join "`n"
    if ($out -eq 'Hello, AutoCoder!') {
        $notes += 'hello.exe prints the expected line'
    } else {
        $pass = $false
        $notes += "hello.exe printed: '$out' (expected 'Hello, AutoCoder!')"
    }
}

exit (Complete-Run -Name 'smoke' -Pass $pass -Notes $notes -Dirs $dirs)
