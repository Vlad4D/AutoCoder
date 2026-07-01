# Bigtest level 2 -- modify-existing (~5-10 min). A small fixture project has
# a planted bug that makes its self-test fail; the agent must find and fix it
# WITHOUT touching the tests. Exercises read/grep/outline + targeted edits --
# the code paths an empty-folder test never reaches.

. "$PSScriptRoot\common.ps1"
Test-ApiKey

$dirs = New-RunDirs 'fixbug'

# Copy the fixture into the sandbox.
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot 'fixture_calc\*') $dirs.Work
$mainBefore = (Get-FileHash (Join-Path $dirs.Work 'src\main.cpp')).Hash

$prompt = @'
This project's self-test fails. Configure with CMake into the ./build
directory, build the Debug config, and run calc_app to see the failure.
Then find and fix the bug in the library code. Do NOT modify the checks in
src/main.cpp -- the expected values there are correct. Rebuild and run
calc_app again to confirm it prints "all tests passed" and exits with code 0.
'@

$exit = Invoke-AutoCoder -WorkDir $dirs.Work -Prompt $prompt -ResultsDir $dirs.Results `
    -TimeoutMinutes 15 -MaxIter 40 -CheckCommand 'cmake --build build'

$pass = $true
$notes = @()
if ($exit -ne 0) { $pass = $false; $notes += "CLI exited with code $exit (0 expected)" }

# Anti-cheat: the tests must be untouched.
$mainAfter = (Get-FileHash (Join-Path $dirs.Work 'src\main.cpp')).Hash
if ($mainAfter -ne $mainBefore) {
    $pass = $false
    $notes += 'src/main.cpp was modified -- the agent changed the tests instead of the bug'
} else {
    $notes += 'tests left untouched'
}

# Independent verification: clean configure + build + run, ignoring whatever
# build state the agent left behind.
$verifyBuild = Join-Path $dirs.Work 'verify_build'
cmake -S $dirs.Work -B $verifyBuild 2>$null | Out-Null
cmake --build $verifyBuild --config Debug 2>$null | Out-Null
$exe = Join-Path $verifyBuild 'Debug\calc_app.exe'
if (-not (Test-Path $exe)) {
    $pass = $false
    $notes += 'independent rebuild failed (calc_app.exe missing)'
} else {
    $out = (& $exe) -join "`n"
    if ($LASTEXITCODE -eq 0 -and $out -match 'all tests passed') {
        $notes += 'independent rebuild passes the self-test'
    } else {
        $pass = $false
        $notes += "self-test still failing (exit $LASTEXITCODE): $out"
    }
}

exit (Complete-Run -Name 'fixbug' -Pass $pass -Notes $notes -Dirs $dirs)
