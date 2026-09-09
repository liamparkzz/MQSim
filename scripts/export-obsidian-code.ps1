[CmdletBinding()]
param([string]$Vault, [string]$Reason = 'Periodic source snapshot', [string]$GitExecutable = 'git')
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (-not $Vault) {
    # Only the verified laptop layout is selected automatically.
    $Vault = Join-Path (Split-Path -Parent $repository) 'Obsidian\LiamObsidian'
}
$bundledPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
$pythonPath = if (Test-Path -LiteralPath $bundledPython) { $bundledPython } elseif ($pythonCommand -and $pythonCommand.Source -notlike '*\WindowsApps\*') { $pythonCommand.Source } else { '' }
if (-not (Test-Path -LiteralPath $pythonPath)) { throw 'Python runtime unavailable; install Python or update the launcher.' }
& $pythonPath (Join-Path $PSScriptRoot 'export-obsidian-code.py') --repo $repository --vault $Vault --git $GitExecutable --reason $Reason
if ($LASTEXITCODE -ne 0) { throw 'Obsidian source export failed.' }
