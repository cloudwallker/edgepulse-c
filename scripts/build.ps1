param(
    [ValidateSet('build', 'test', 'demo')][string]$Mode = 'test',
    [string]$Compiler = 'gcc',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force 'build' | Out-Null
    $flags = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-O2', '-g', '-Iinclude')
    $core = @('src/protocol.c', 'src/store.c', 'src/device.c', 'src/receiver.c')
    & $Compiler @flags '-municode' @core 'src/file_store.c' 'src/main.c' '-o' 'build/edgepulse.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Simulator compilation failed.' }
    if ($Mode -eq 'test') {
        # Suppress Windows abort dialogs and bound each native test execution.
        $runner = @'
import ctypes, os, subprocess, sys
if os.name == 'nt':
    ctypes.windll.kernel32.SetErrorMode(0x0002)
sys.exit(subprocess.run([sys.argv[1]], timeout=60).returncode)
'@
        foreach ($name in @('protocol', 'store', 'device', 'reliable', 'receiver')) {
            $source = "tests/test_$name.c"
            $binary = "build/test_$name.exe"
            & $Compiler @flags @core $source '-o' $binary
            if ($LASTEXITCODE -ne 0) { throw "Test compilation failed: $name" }
            & $Python '-c' $runner $binary
            if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
        }
        & $Python 'tests/test_cli.py' 'build/edgepulse.exe'
        if ($LASTEXITCODE -ne 0) { throw 'CLI contract tests failed.' }
        & $Compiler @flags '-municode' '-DEP_FILE_TESTING' @core 'src/file_store.c' 'tests/file_store_worker.c' '-o' 'build/file_store_worker.exe'
        if ($LASTEXITCODE -ne 0) { throw 'File-store test worker compilation failed.' }
        & $Python 'tests/test_file_store.py' 'build/file_store_worker.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Persistent file-store contract tests failed.' }
    }
    if ($Mode -eq 'demo') {
        & './build/edgepulse.exe' '--demo'
        if ($LASTEXITCODE -ne 0) { throw 'Simulator accounting failed.' }
    }
} finally {
    Pop-Location
}
