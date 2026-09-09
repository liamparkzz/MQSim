[CmdletBinding()]
param([string]$OutputRoot = 'C:\CODEX\MQsimExperiments\20260909-pr79-integration', [switch]$RelinkOnly)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $OutputRoot 'build'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Push-Location $buildRoot
try {
    $sources = @(Get-ChildItem "$sourceRoot\src" -Recurse -Filter '*.cpp' | Where-Object Name -ne 'main.cpp' | ForEach-Object FullName)
    $flags = @('/nologo', '/EHsc', '/std:c++14', '/Od', '/Zi', '/MD', '/D_CRT_SECURE_NO_WARNINGS', "/I$sourceRoot\src")
    if (-not $RelinkOnly) {
        & cl.exe @flags /MP2 /c @sources *> "$OutputRoot\build.log"
        if ($LASTEXITCODE -ne 0) { throw 'Source compilation failed; see build.log' }
    }
    $objects = @($sources | ForEach-Object { Join-Path $buildRoot ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
    & cl.exe @flags "$sourceRoot\src\main.cpp" @objects "/Fe:$OutputRoot\MQSim.exe" /link /DEBUG *> "$OutputRoot\link.log"
    if ($LASTEXITCODE -ne 0) { throw 'Executable link failed; see link.log' }
    & cl.exe @flags "$sourceRoot\tests\pr79_observer.cpp" @objects "/Fe:$OutputRoot\MQSim-observer.exe" /link /DEBUG *> "$OutputRoot\observer-link.log"
    if ($LASTEXITCODE -ne 0) { throw 'Observer link failed; see observer-link.log' }
    Write-Output 'BUILD_OK: full simulator and read-only observer'
} finally { Pop-Location }
