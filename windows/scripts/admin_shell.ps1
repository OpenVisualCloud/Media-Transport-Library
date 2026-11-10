<#
.SYNOPSIS
Run an MTL script in a PowerShell window as administrator. The Start menu
entries of the MTL installer use it.

.DESCRIPTION
admin_shell.ps1 status|drivers|test

  status   windows_setup.ps1 status and install_drivers.ps1 status
  drivers  install_drivers.ps1 apply (put the EWDK ISO in Downloads first)
  test     run_test.ps1: 2 video, 2 audio and 2 ancillary streams, 30 s

Windows asks for consent. The window stays open, so you can read the result.
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'drivers', 'test')]
    [string]$Action = 'status'
)

$s = $PSScriptRoot
$cmd = switch ($Action) {
    'status' { "& '$s\windows_setup.ps1' status; & '$s\install_drivers.ps1' status" }
    'drivers' { "& '$s\install_drivers.ps1' apply" }
    'test' { "& '$s\run_test.ps1'" }
}
# An encoded command keeps the quotes of a path with spaces.
$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("Set-Location '$s'; $cmd"))
Start-Process powershell.exe -Verb RunAs -ArgumentList '-NoProfile', '-NoExit', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $b64
