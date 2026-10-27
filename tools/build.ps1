param([ValidateSet('release', 'debug')][string]$Preset = 'release', [switch]$ConfigureOnly, [switch]$Fresh)
$ErrorActionPreference = 'Stop'
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
$taskDevCmd = Join-Path $taskVs 'Common7\Tools\VsDevCmd.bat'
$taskEnvironment = & cmd.exe /d /c "call `"$taskDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
foreach ($taskLine in $taskEnvironment) {
    if ($taskLine -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
if (!$env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $taskVs 'VC\vcpkg' }
$taskCmake = Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$env:PATH = (Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $taskArguments = @('--preset', $Preset)
    if ($Fresh) { $taskArguments += '--fresh' }
    & $taskCmake @taskArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
    if (!$ConfigureOnly) {
        & $taskCmake --build --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed ($LASTEXITCODE)." }
    }
} finally { Pop-Location }
