[CmdletBinding()]
param(
    [string]$TaskName = 'MQSim Git Auto Sync',
    [int]$IntervalMinutes = 5
)

$ErrorActionPreference = 'Stop'

if ($IntervalMinutes -lt 1) {
    throw 'IntervalMinutes must be at least 1.'
}

$syncScript = Join-Path $PSScriptRoot 'mqsim-auto-sync.ps1'
$hiddenLauncher = Join-Path $PSScriptRoot 'run-auto-sync-hidden.vbs'
if (-not (Test-Path -LiteralPath $syncScript -PathType Leaf)) {
    throw "Auto-sync script not found: $syncScript"
}
if (-not (Test-Path -LiteralPath $hiddenLauncher -PathType Leaf)) {
    throw "Hidden launcher not found: $hiddenLauncher"
}

$wscriptPath = Join-Path $env:SystemRoot 'System32\wscript.exe'
if (-not (Test-Path -LiteralPath $wscriptPath -PathType Leaf)) {
    throw "Windows Script Host not found: $wscriptPath"
}

$gitExecutable = (Get-Command git.exe -ErrorAction Stop).Source
$actionArguments = '"{0}" "{1}"' -f $hiddenLauncher, $gitExecutable
$action = New-ScheduledTaskAction -Execute $wscriptPath -Argument $actionArguments
$trigger = New-ScheduledTaskTrigger `
    -Once `
    -At (Get-Date).AddMinutes(1) `
    -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes) `
    -RepetitionDuration (New-TimeSpan -Days 3650)
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
$principal = New-ScheduledTaskPrincipal `
    -UserId ("{0}\{1}" -f $env:USERDOMAIN, $env:USERNAME) `
    -LogonType Interactive `
    -RunLevel Limited

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Description 'Silently commit, rebase, and push MQSim changes to origin/main every few minutes.' `
    -Force | Out-Null

Get-ScheduledTask -TaskName $TaskName | Select-Object TaskName, State
