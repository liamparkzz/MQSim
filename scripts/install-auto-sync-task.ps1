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
if (-not (Test-Path -LiteralPath $syncScript -PathType Leaf)) {
    throw "Auto-sync script not found: $syncScript"
}

$powerShellPath = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path -LiteralPath $powerShellPath -PathType Leaf)) {
    $powerShellPath = (Get-Command powershell.exe -ErrorAction Stop).Source
}

$actionArguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}"' -f $syncScript
$action = New-ScheduledTaskAction -Execute $powerShellPath -Argument $actionArguments
$trigger = New-ScheduledTaskTrigger `
    -Once `
    -At (Get-Date).AddMinutes(1) `
    -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes) `
    -RepetitionDuration (New-TimeSpan -Days 3650)
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes ([Math]::Max(2, $IntervalMinutes)))
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
    -Description 'Commit, rebase, and push MQSim changes to origin/main every few minutes.' `
    -Force | Out-Null

Get-ScheduledTask -TaskName $TaskName | Select-Object TaskName, State
