<#
.SYNOPSIS
Run an MTL script in a PowerShell window as administrator. The Start menu
entries of the MTL installer use it.

.DESCRIPTION
admin_shell.ps1 status|drivers

  status   windows_setup.ps1 status and install_drivers.ps1 status
  drivers  install_drivers.ps1 apply (mount the EWDK ISO first)

Windows asks for consent. The window stays open, so you can read the result.
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'drivers')]
    [string]$Action = 'status'
)

$s = $PSScriptRoot
$cmd = switch ($Action) {
    'status' { "& '$s\windows_setup.ps1' status; & '$s\install_drivers.ps1' status" }
    'drivers' { "& '$s\install_drivers.ps1' apply" }
}
# An encoded command keeps the quotes of a path with spaces.
$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("Set-Location '$s'; $cmd"))
Start-Process powershell.exe -Verb RunAs -ArgumentList '-NoProfile', '-NoExit', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $b64
