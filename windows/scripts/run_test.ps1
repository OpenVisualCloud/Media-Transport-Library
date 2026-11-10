#Requires -RunAsAdministrator
<#
.SYNOPSIS
The basic test of MTL on Windows: RxTxApp sends two 1080p59 video, two audio
and two ancillary streams from one port and receives them on a second port.

.DESCRIPTION
run_test.ps1 [-Ports <PCI address>,<PCI address>] [-Seconds 30]

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
.\run_test.ps1 -Ports 0000:01:00.0,0000:02:00.0 -Seconds 60
#>
param(
    [string[]]$Ports = @(),
    [int]$Seconds = 30
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

$yuv = Join-Path $Dir 'test_1080p.yuv'
$pcm = Join-Path $Dir 'test.pcm'
$txt = Join-Path $Dir 'test.txt'
New-SourceFile $yuv (1920 * 1080 * 5 / 2 * 10) # 10 frames, YUV 4:2:2 10-bit
New-SourceFile $pcm (48000 * 2 * 2 * 4) # 4 s, PCM16 stereo 48 kHz
if (-not (Test-Path $txt)) { Set-Content -Path $txt -Value 'MTL test' -Encoding ASCII }

$h = [Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($env:COMPUTERNAME))
$group = "239.96.$(10 + $h[0] % 240).$(1 + $h[1] % 250)"
$video = @{ replicas = 2; type = 'frame'; pacing = 'gap'; start_port = 20000; payload_type = 112
    tr_offset = 'default'; video_format = 'i1080p59'; pg_format = 'YUV_422_10bit' }
$audio = @{ replicas = 2; type = 'frame'; start_port = 30000; payload_type = 111; audio_format = 'PCM16'
    audio_channel = @('ST'); audio_sampling = '48kHz'; audio_ptime = '1'; audio_url = $pcm.Replace('\', '/') }
$anc = @{ replicas = 2; type = 'frame'; start_port = 40000; payload_type = 113 }
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

# Run RxTxApp. Its log shows in the window and goes to $log. Gives the
# result lines and the exit code.
function Invoke-Test([string[]]$extra, [string]$log) {
    # A line of RxTxApp on its standard error must not stop the script.
    $ErrorActionPreference = 'Continue'
    Write-Host "RxTxApp $($base + $extra -join ' ')"
    Write-Host "  $Seconds s; log $log"
    # "$_": a line of the standard error as text, not as an error record.
    & RxTxApp.exe @base @extra 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log | Write-Host
    $code = $LASTEXITCODE
    $lines = @(Select-String -Path $log -Pattern '\w+_result\(\d+\), (OK|FAILED).*' | ForEach-Object {
            [pscustomobject]@{ State = $_.Matches[0].Groups[1].Value; Text = $_.Matches[0].Value } })
    return [pscustomobject]@{ Code = $code; Lines = $lines }
}

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
# 2 TX video, then 2 RX of each of video, audio and ancillary.
$pass = $r.Code -eq 0 -and $ok -ge 8 -and $ok -eq $r.Lines.Count
Write-Host ''
if ($pass) { Write-Host "PASS: $ok streams OK" -ForegroundColor Green }
else { Write-Host "FAIL: $ok of $($r.Lines.Count) results OK, exit code $($r.Code). See $log" -ForegroundColor Red }
if (-not $pass) { exit 1 }
