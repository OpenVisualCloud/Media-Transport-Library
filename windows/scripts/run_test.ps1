#Requires -RunAsAdministrator
<#
.SYNOPSIS
The basic test of MTL on Windows: RxTxApp sends sets of two 1080p59 video,
two audio and two ancillary streams from one port and receives them on a
second port.

.DESCRIPTION
run_test.ps1 [-Sets <number>] [-Seconds <number>] [-Ports <PCI address>,<PCI address>]

-Sets: the number of sets. -Seconds: the test time. With no -Sets or no
-Seconds, the test asks for it (the defaults are 1 set and 30 s; the time
can have a unit: 90s, 20m, 1h). Each stream of a set has its own queue on each port, so a set needs 6
RX queues: a VF with 16 queues takes 2 sets. A VF with a 512-entry RSS LUT on
a Linux host takes more (doc/vm_WIN.md).

The two ports must reach each other: two VFs of one PF, or two PFs on one
network. The default ports are the first two NetUIO ports
(install_drivers.ps1 status). The lcores are the CPUs that windows_setup.ps1
reserved. The configuration, the source files and the log are in
C:\ProgramData\MTL\test.

The multicast group comes from the computer name, so two hosts on one network
do not send to one group. A VF of a KVM host with the in-tree ice driver gets
no flow rule: then the test runs again with --rss_mode l3_l4.

.EXAMPLE
.\run_test.ps1
.\run_test.ps1 -Sets 4 -Ports 0000:01:00.0,0000:02:00.0 -Seconds 60
#>
param(
    [int]$Sets = 0,
    [string[]]$Ports = @(),
    [int]$Seconds = 0
)

$ErrorActionPreference = 'Stop'
$Dir = Join-Path $env:ProgramData 'MTL\test'
New-Item -ItemType Directory -Force -Path $Dir | Out-Null

# The PCI addresses of the NetUIO ports that work, in order.
function Get-NetuioPorts {
    $list = foreach ($d in Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue) {
        if ($d.InstanceId -notlike 'PCI\*' -or $d.Status -ne 'OK') { continue }
        $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)"
        if ((Get-ItemProperty -Path $key -Name Service -ErrorAction SilentlyContinue).Service -ne 'netuio') { continue }
        $bus = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_BusNumber).Data
        $adr = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Address).Data
        '0000:{0:x2}:{1:x2}.{2:x}' -f $bus, ($adr -shr 16), ($adr -band 0xffff)
    }
    return , @($list | Sort-Object)
}

# SetConsoleCtrlHandler(NULL, TRUE): this process ignores Ctrl+C.
# GenerateConsoleCtrlEvent(0, 0): Ctrl+C to each process of the console.
# A second run in one window has the type already.
if (-not ('MtlTest.Console' -as [type])) {
    Add-Type -Namespace MtlTest -Name Console -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern bool SetConsoleCtrlHandler(System.IntPtr handler, bool add);
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);
'@
}

# 75 -> 01:15, 4000 -> 1:06:40
function Format-Time([int]$s) {
    $t = [TimeSpan]::FromSeconds($s)
    if ($t.TotalHours -ge 1) { return '{0}:{1:D2}:{2:D2}' -f [int][math]::Floor($t.TotalHours), $t.Minutes, $t.Seconds }
    return '{0:D2}:{1:D2}' -f $t.Minutes, $t.Seconds
}

# The CPUs that windows_setup.ps1 reserved, or '' (MTL then selects).
function Get-Lcores {
    $file = Join-Path $env:ProgramData 'mtl-windows-setup\state.json'
    if (-not (Test-Path $file)) { return '' }
    $st = Get-Content $file -Raw | ConvertFrom-Json
    if ($st.PSObject.Properties['SchedulerCpus']) { return [string]$st.SchedulerCpus }
    return ''
}

function New-SourceFile([string]$path, [int]$size) {
    if ((Test-Path $path) -and (Get-Item $path).Length -eq $size) { return }
    $b = New-Object byte[] $size
    (New-Object Random).NextBytes($b)
    [IO.File]::WriteAllBytes($path, $b)
}

if (-not $Ports.Count) { $Ports = Get-NetuioPorts }
if ($Ports.Count -lt 2) {
    Write-Host "The test needs two NetUIO ports; there are $($Ports.Count): $($Ports -join ' ')."
    Write-Host 'Run install_drivers.ps1 status, or give -Ports <TX port>,<RX port>.'
    exit 1
}

Write-Host "Making the test files in $Dir"
if ($Sets -lt 1) {
    $Sets = 1
    if (-not [Console]::IsInputRedirected) {
        $a = Read-Host 'How many sets of 2 video, 2 audio and 2 ancillary streams? [1]'
        if ($a -match '^\s*\d+\s*$' -and [int]$a -ge 1) { $Sets = [int]$a }
    }
}
if ($Seconds -lt 1) {
    $Seconds = 30
    if (-not [Console]::IsInputRedirected) {
        $a = Read-Host 'How long: seconds, or with a unit (90s, 20m, 1h)? [30s]'
        if ($a -match '^\s*(\d+)\s*([smh]?)\s*$' -and [int]$Matches[1] -ge 1) {
            $Seconds = [int]$Matches[1] * @{ '' = 1; 's' = 1; 'm' = 60; 'h' = 3600 }[$Matches[2]]
        }
    }
}
$yuv = Join-Path $Dir 'test_1080p.yuv'
$pcm = Join-Path $Dir 'test.pcm'
$txt = Join-Path $Dir 'test.txt'
New-SourceFile $yuv (1920 * 1080 * 5 / 2 * 10) # 10 frames, YUV 4:2:2 10-bit
New-SourceFile $pcm (48000 * 2 * 2 * 4) # 4 s, PCM16 stereo 48 kHz
if (-not (Test-Path $txt)) { Set-Content -Path $txt -Value 'MTL test' -Encoding ASCII }

$h = [Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($env:COMPUTERNAME))
$group = "239.96.$(10 + $h[0] % 240).$(1 + $h[1] % 250)"
$video = @{ replicas = 2 * $Sets; type = 'frame'; pacing = 'gap'; start_port = 20000; payload_type = 112
    tr_offset = 'default'; video_format = 'i1080p59'; pg_format = 'YUV_422_10bit' }
$audio = @{ replicas = 2 * $Sets; type = 'frame'; start_port = 30000; payload_type = 111; audio_format = 'PCM16'
    audio_channel = @('ST'); audio_sampling = '48kHz'; audio_ptime = '1'; audio_url = $pcm.Replace('\', '/') }
$anc = @{ replicas = 2 * $Sets; type = 'frame'; start_port = 40000; payload_type = 113 }
$config = [ordered]@{
    interfaces  = @(@{ name = $Ports[0]; ip = '192.168.96.10' }, @{ name = $Ports[1]; ip = '192.168.96.20' })
    tx_sessions = @(@{ dip = @($group); interface = @(0)
            video = @($video + @{ packing = 'BPM'; video_url = $yuv.Replace('\', '/') })
            audio = @($audio)
            ancillary = @($anc + @{ ancillary_format = 'closed_caption'; ancillary_url = $txt.Replace('\', '/'); ancillary_fps = 'p59' }) })
    rx_sessions = @(@{ ip = @($group); interface = @(1)
            video = @($video + @{ display = $false })
            audio = @($audio)
            ancillary = @($anc) })
}
$json = Join-Path $Dir 'test.json'
$config | ConvertTo-Json -Depth 8 | Set-Content -Path $json -Encoding ASCII

$lcores = Get-Lcores
$base = @('--config_file', $json, '--test_time', "$Seconds")
if ($lcores -ne '') { $base += @('--lcores', $lcores) }

# Run RxTxApp in the background. Its log goes to $log, and the window shows
# each new line of it with no time stamp ($log keeps the full line), under a
# status line that turns while RxTxApp runs. A key (or Ctrl+C) stops RxTxApp
# as Ctrl+C does: it stops its sessions and gives its results. A second key
# kills it. Gives the result lines and the exit code.
function Invoke-Test([string[]]$extra, [string]$log) {
    # A line of a program on its standard error must not stop the script.
    $ErrorActionPreference = 'Continue'
    $all = @($base + $extra | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
    Write-Host "RxTxApp $($all -join ' ')"
    Write-Host "  $Seconds s; log $log"
    Remove-Item -Path $log -ErrorAction SilentlyContinue
    # cmd.exe writes the standard output and error of RxTxApp to one file, in order.
    $p = Start-Process cmd.exe -ArgumentList "/d /s /c `"RxTxApp.exe $($all -join ' ') > `"$log`" 2>&1`"" -NoNewWindow -PassThru
    $null = $p.Handle # Without the handle, PowerShell 5.1 loses the exit code.
    # Ctrl+C is for RxTxApp now: the script goes on and gives the results.
    $keys = -not [Console]::IsInputRedirected
    if ($keys) { [void][MtlTest.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) }
    $stops = 0
    # No status line when the output goes to a file or a pipe.
    $spin = if ([Console]::IsOutputRedirected) { '' } else { '|/-\' }
    $width = 79
    try { $width = $Host.UI.RawUI.WindowSize.Width - 1 } catch { }
    $t0 = Get-Date
    $pos = 0
    $rest = ''
    $i = 0
    try {
        while ($true) {
            $exited = $p.HasExited
            $new = @()
            if (Test-Path $log) {
                $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
                try {
                    if ($fs.Length -gt $pos) {
                        $null = $fs.Seek($pos, 'Begin')
                        $buf = New-Object byte[] ($fs.Length - $pos)
                        $n = $fs.Read($buf, 0, $buf.Length)
                        $pos += $n
                        $parts = ($rest + [Text.Encoding]::UTF8.GetString($buf, 0, $n)) -split "`r?`n"
                        $rest = $parts[-1]
                        if ($parts.Count -gt 1) { $new = @($parts[0..($parts.Count - 2)]) }
                    }
                } finally { $fs.Close() }
            }
            if ($exited -and $rest -ne '') { $new += $rest; $rest = '' }
            if ($new.Count -and $spin) { Write-Host -NoNewline ("`r" + ' ' * $width + "`r") }
            foreach ($l in $new) { Write-Host ($l -replace '^MTL: \d{4}-\d\d-\d\d \d\d:\d\d:\d\d, ', 'MTL: ') }
            if ($exited) { break }
            if ($keys -and [Console]::KeyAvailable) {
                $null = [Console]::ReadKey($true)
                $stops++
                if ($spin) { Write-Host -NoNewline ("`r" + ' ' * $width + "`r") }
                if ($stops -eq 1) {
                    Write-Host 'Stopping RxTxApp after a key; it gives its results. A second key kills it.' -ForegroundColor Yellow
                    [void][MtlTest.Console]::GenerateConsoleCtrlEvent(0, 0)
                } else {
                    Write-Host 'Killing RxTxApp.' -ForegroundColor Yellow
                    & taskkill.exe /PID $p.Id /T /F *> $null
                }
            }
            if ($spin) {
                $sec = [int]((Get-Date) - $t0).TotalSeconds
                $hint = if ($keys) { ' (a key stops it)' } else { '' }
                Write-Host -NoNewline ("`r{0} RxTxApp runs: {1} of {2}{3}" -f $spin[$i % 4], (Format-Time $sec), (Format-Time $Seconds), $hint)
                $i++
            }
            Start-Sleep -Milliseconds 200
        }
        if ($spin) { Write-Host -NoNewline ("`r" + ' ' * $width + "`r") }
    } finally {
        if ($keys) { [void][MtlTest.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, $false) }
        # The script stops for another cause: stop RxTxApp too.
        if (-not $p.HasExited) { & taskkill.exe /PID $p.Id /T /F *> $null }
    }
    $code = $p.ExitCode
    $lines = @(Select-String -Path $log -Pattern '\w+_result\(\d+\), (OK|FAILED).*' | ForEach-Object {
            [pscustomobject]@{ State = $_.Matches[0].Groups[1].Value; Text = $_.Matches[0].Value } })
    return [pscustomobject]@{ Code = $code; Lines = $lines }
}

Write-Host "Sets: $Sets ($(2 * $Sets) video, $(2 * $Sets) audio, $(2 * $Sets) ancillary); time $(Format-Time $Seconds)"
Write-Host "Ports: TX $($Ports[0]), RX $($Ports[1]); group $group; lcores $(if ($lcores) { $lcores } else { 'MTL selects' })"
$log = Join-Path $Dir 'test.log'
$r = Invoke-Test @() $log
if ($r.Code -ne 0 -and (Select-String -Path $log -Pattern 'rte_flow_create fail|Init FDIR .* failed' -Quiet)) {
    Write-Host 'The VF gets no flow rule (in-tree ice driver on the host?): run again with --rss_mode l3_l4.'
    $log = Join-Path $Dir 'test_rss.log'
    $r = Invoke-Test @('--rss_mode', 'l3_l4') $log
}

Write-Host ''
foreach ($l in $r.Lines) { Write-Host "  $($l.Text)" }
$ok = @($r.Lines | Where-Object { $_.State -eq 'OK' }).Count
# Each set: 2 TX video, then 2 RX of each of video, audio and ancillary.
$pass = $r.Code -eq 0 -and $ok -ge 8 * $Sets -and $ok -eq $r.Lines.Count
Write-Host ''
if ($pass) { Write-Host "PASS: $ok streams OK ($Sets sets)" -ForegroundColor Green }
else {
    Write-Host "FAIL: $ok of $(8 * $Sets) results OK, exit code $($r.Code). See $log" -ForegroundColor Red
    $cut = Select-String -Path $log -Pattern 'cut to the device max tx (\d+) rx (\d+)' | Select-Object -First 1
    if ($cut) {
        Write-Host "The port has $($cut.Matches[0].Groups[2].Value) RX queues, and each set needs 6: use fewer sets, or a VF with a 512-entry RSS LUT (doc/vm_WIN.md)." -ForegroundColor Yellow
    }
}
if (-not $pass) { exit 1 }
