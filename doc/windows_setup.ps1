#Requires -RunAsAdministrator
<#
.SYNOPSIS
Prepare a Windows host for MTL: the DPDK drivers, large pages, and cores
that only the MTL scheduler threads use.

.DESCRIPTION
windows_setup.ps1 status|apply|undo [-SchedulerCpus <list>] [-Items <names>]

The items, in order:

  testsign   bcdedit testsigning on. NetUIO and virt2phys are test-signed.
             Secure Boot must be off.
  lockpages  "Lock pages in memory" for -User. DPDK uses large pages.
  dirs       C:\temp (the MTL lcore lock file) and C:\dpdk\lib (ice.pkg).
  path       -MsysBin (the MSYS2 UCRT64 programs and DLLs: libmtl.dll, DPDK)
             at the end of the machine PATH, so RxTxApp starts from any
             shell. At the end, it does not hide a program of Windows.
  cpusets    ReservedCpuSets = -SchedulerCpus. Windows then runs on these
             CPUs only a thread with a hard affinity to them
             (SetThreadGroupAffinity). Windows ignores a CPU Set selection
             (SetThreadSelectedCpuSets) of a reserved CPU.
  irq        The interrupt affinity policy of each PCI device: the CPUs that
             -SchedulerCpus does not name.
  power      The Ultimate Performance power plan (High performance when the
             first is not available), no core parking, minimum processor
             state 100 %.
  defender   Microsoft Defender exclusions for -ExcludePaths.
  vbs        Virtualization-based security and Memory Integrity off.

apply records the value of an item before it changes the item, in
C:\ProgramData\mtl-windows-setup\state.json, and only the first time. undo
puts these values back. Both are safe to run again. testsign, cpusets, irq
and vbs take effect after a restart of Windows; lockpages at the next logon.

Do not reserve CPU 0, keep two CPUs or more for Windows, and reserve both
threads of a core when SMT is on.

.EXAMPLE
.\windows_setup.ps1 apply -SchedulerCpus 8-15
.\windows_setup.ps1 status
.\windows_setup.ps1 undo -Items cpusets,irq
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'apply', 'undo')]
    [string]$Action = 'status',
    [string]$SchedulerCpus = '',
    [ValidateSet('testsign', 'lockpages', 'dirs', 'path', 'cpusets', 'irq', 'power', 'defender', 'vbs')]
    [string[]]$Items = @('testsign', 'lockpages', 'dirs', 'path', 'cpusets', 'irq', 'power', 'defender', 'vbs'),
    [string]$User = $env:USERNAME,
    [string[]]$ExcludePaths = @('C:\mtl', 'C:\msys64', 'C:\dpdk'),
    [string]$MsysBin = 'C:\msys64\ucrt64\bin',
    [switch]$Restart
)

$ErrorActionPreference = 'Stop'
$StateFile = Join-Path $env:ProgramData 'mtl-windows-setup\state.json'
$KernelKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\kernel'
$DgKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
$HvciKey = "$DgKey\Scenarios\HypervisorEnforcedCodeIntegrity"
$UltimateGuid = 'e9a42b02-d5df-448d-aa00-03f14749eb61'
$HighPerfGuid = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c'
$script:NeedRestart = @()
$script:Failed = $false

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MtlCpuSet {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetSystemCpuSetInformation(byte[] info, uint length,
        out uint returned, IntPtr process, uint flags);
}
'@

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Get-State {
    if (Test-Path $StateFile) { return Get-Content $StateFile -Raw | ConvertFrom-Json }
    return [pscustomobject]@{}
}

function Save-State($state) {
    New-Item -ItemType Directory -Force -Path (Split-Path $StateFile) | Out-Null
    $state | ConvertTo-Json -Depth 8 | Set-Content -Path $StateFile -Encoding UTF8
}

function Set-StateItem($state, [string]$name, $value) {
    $state | Add-Member -NotePropertyName $name -NotePropertyValue $value -Force
}

function Remove-StateItem($state, [string]$name) {
    $state.PSObject.Properties.Remove($name)
}

function Result([string]$item, [string]$st, [string]$detail) {
    if ($st -eq 'error') { $script:Failed = $true }
    if ($st -eq 'restart') { $script:NeedRestart += $item }
    [pscustomobject]@{ Item = $item; State = $st; Detail = $detail }
}

# "8-15,20" -> 8,9,...,15,20
function ConvertTo-CpuList([string]$text) {
    $cpus = @()
    foreach ($part in ($text -split ',')) {
        $p = $part.Trim()
        if ($p -eq '') { continue }
        if ($p -match '^(\d+)-(\d+)$') { $cpus += [int]$Matches[1]..[int]$Matches[2] }
        elseif ($p -match '^\d+$') { $cpus += [int]$p }
        else { throw "bad CPU list part: '$p'" }
    }
    return @($cpus | Sort-Object -Unique)
}

# Each CPU set of the system. Cpu is the number that DPDK gives the CPU: the
# rank by group, then by logical processor index (lib/eal/windows/eal_lcore.c).
function Get-CpuSets {
    $len = [uint32]0
    [void][MtlCpuSet]::GetSystemCpuSetInformation($null, 0, [ref]$len, [IntPtr]::Zero, 0)
    $buf = New-Object byte[] $len
    if (-not [MtlCpuSet]::GetSystemCpuSetInformation($buf, $len, [ref]$len, [IntPtr]::Zero, 0)) {
        throw "GetSystemCpuSetInformation failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $sets = @()
    $off = 0
    while ($off -lt $len) {
        $size = [BitConverter]::ToUInt32($buf, $off)
        $group = [BitConverter]::ToUInt16($buf, $off + 12)
        $sets += [pscustomobject]@{
            Id    = [BitConverter]::ToUInt32($buf, $off + 8)
            Group = $group
            Lp    = $buf[$off + 14]
            Core  = $buf[$off + 15]
            Numa  = $buf[$off + 17]
            Flags = $buf[$off + 19]
            Cpu   = 0
        }
        $off += $size
    }
    $sets = @($sets | Sort-Object Group, Lp)
    for ($i = 0; $i -lt $sets.Count; $i++) { $sets[$i].Cpu = $i }
    return $sets
}

# A bit mask of CPUs, least significant byte first, in $bytes bytes.
function ConvertTo-Mask([int[]]$cpus, [int]$bytes) {
    $mask = New-Object byte[] $bytes
    foreach ($c in $cpus) { $mask[[math]::Floor($c / 8)] = $mask[[math]::Floor($c / 8)] -bor (1 -shl ($c % 8)) }
    return , $mask
}

function ConvertTo-Hex($bytes) {
    if ($null -eq $bytes) { return '(none)' }
    return (@($bytes) | ForEach-Object { '{0:x2}' -f [int]$_ }) -join ''
}

function Get-RegValue([string]$key, [string]$name) {
    $p = Get-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
    if ($null -eq $p) { return $null }
    return $p.$name
}

function Restore-RegValue([string]$key, [string]$name, $value, [string]$type) {
    if ($null -eq $value) {
        Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
    } else {
        if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
        if ($type -eq 'Binary') { $value = [byte[]]@($value) }
        Set-ItemProperty -Path $key -Name $name -Value $value -Type $type
    }
}

# The CPUs to reserve: -SchedulerCpus, or the CPUs of the last apply.
function Get-SchedulerCpus($state) {
    if ($SchedulerCpus -ne '') { return , (ConvertTo-CpuList $SchedulerCpus) }
    if ($state.PSObject.Properties['SchedulerCpus']) { return , (ConvertTo-CpuList $state.SchedulerCpus) }
    return , @()
}

function Test-SchedulerCpus([int[]]$cpus, $sets) {
    $all = @($sets | ForEach-Object Cpu)
    foreach ($c in $cpus) { if ($all -notcontains $c) { throw "CPU $c is not in this system (0-$($all.Count - 1))" } }
    if ($cpus -contains 0) { throw 'do not reserve CPU 0: Windows needs it' }
    if ($all.Count - $cpus.Count -lt 2) { throw 'keep two CPUs or more for Windows' }
    foreach ($s in ($sets | Where-Object { $cpus -contains $_.Cpu })) {
        $siblings = $sets | Where-Object { $_.Group -eq $s.Group -and $_.Core -eq $s.Core -and $cpus -notcontains $_.Cpu }
        foreach ($t in $siblings) { Write-Warning "CPU $($s.Cpu) is reserved, but its SMT sibling CPU $($t.Cpu) is not" }
    }
}

# ---------------------------------------------------------------------------
# Items. Each one takes the action and the state, and gives one result.
# ---------------------------------------------------------------------------

function Item-TestSign([string]$act, $state) {
    $on = [bool](bcdedit.exe /enum '{current}' | Select-String -Pattern '^testsigning\s+Yes' -Quiet)
    $sb = $false
    try { $sb = Confirm-SecureBootUEFI } catch { $sb = $false }
    switch ($act) {
        'status' { return Result 'testsign' $(if ($on) { 'ok' } else { 'differs' }) "testsigning $on, Secure Boot $sb" }
        'apply' {
            if ($on) { return Result 'testsign' 'ok' 'testsigning on' }
            if ($sb) { return Result 'testsign' 'error' 'Secure Boot is on: turn it off in the firmware setup first' }
            if (-not $state.PSObject.Properties['testsign']) { Set-StateItem $state 'testsign' @{ on = $on } }
            bcdedit.exe /set testsigning on | Out-Null
            return Result 'testsign' 'restart' 'testsigning on'
        }
        'undo' {
            if (-not $state.PSObject.Properties['testsign']) { return Result 'testsign' 'ok' 'nothing to undo' }
            $old = [bool]$state.testsign.on
            if (-not $old) { bcdedit.exe /set testsigning off | Out-Null }
            Remove-StateItem $state 'testsign'
            return Result 'testsign' 'restart' "testsigning $old"
        }
    }
}

function Get-LockPagesLines {
    $cfg = Join-Path $env:windir 'Temp\mtl-setup-sec.inf'
    secedit.exe /export /cfg $cfg /areas USER_RIGHTS | Out-Null
    return , @(Get-Content $cfg)
}

function Set-LockPagesLines([string[]]$lines) {
    $cfg = Join-Path $env:windir 'Temp\mtl-setup-sec.inf'
    $lines | Set-Content $cfg -Encoding Unicode
    secedit.exe /configure /db (Join-Path $env:windir 'Temp\mtl-setup-sec.sdb') /cfg $cfg /areas USER_RIGHTS | Out-Null
}

function Item-LockPages([string]$act, $state) {
    $sid = (New-Object System.Security.Principal.NTAccount($User)).Translate([System.Security.Principal.SecurityIdentifier]).Value
    $lines = Get-LockPagesLines
    $i = [array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -like 'SeLockMemoryPrivilege*' })
    # secedit writes a local account by name, and other accounts as *SID.
    $forms = @("*$sid", $User, "$env:COMPUTERNAME\$User")
    $entries = @()
    if ($i -ge 0) { $entries = @(($lines[$i] -split '=', 2)[1] -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }) }
    $has = @($entries | Where-Object { $forms -contains $_ }).Count -gt 0
    switch ($act) {
        'status' { return Result 'lockpages' $(if ($has) { 'ok' } else { 'differs' }) "$User has the right: $has" }
        'apply' {
            if ($has) { return Result 'lockpages' 'ok' "$User has the right" }
            if (-not $state.PSObject.Properties['lockpages']) { Set-StateItem $state 'lockpages' @{ sid = $sid } }
            if ($i -ge 0) { $lines[$i] = $lines[$i] + ",*$sid" }
            else {
                $j = [array]::IndexOf([string[]]$lines, '[Privilege Rights]')
                $lines = $lines[0..$j] + "SeLockMemoryPrivilege = *$sid" + $lines[($j + 1)..($lines.Count - 1)]
            }
            Set-LockPagesLines $lines
            return Result 'lockpages' 'set' "$User has the right from the next logon"
        }
        'undo' {
            if (-not $state.PSObject.Properties['lockpages']) { return Result 'lockpages' 'ok' 'nothing to undo' }
            $old = $state.lockpages.sid
            if ($i -ge 0) {
                $kept = @($entries | Where-Object { $forms -notcontains $_ -and $_ -ne "*$old" })
                $lines[$i] = if ($kept.Count) { "SeLockMemoryPrivilege = $($kept -join ',')" } else { 'SeLockMemoryPrivilege =' }
                Set-LockPagesLines $lines
            }
            Remove-StateItem $state 'lockpages'
            return Result 'lockpages' 'set' "right removed from $old"
        }
    }
}

function Item-Dirs([string]$act, $state) {
    $dirs = @('C:\temp', 'C:\dpdk\lib')
    $missing = @($dirs | Where-Object { -not (Test-Path $_) })
    $pkg = Test-Path 'C:\dpdk\lib\ice.pkg'
    switch ($act) {
        'status' { return Result 'dirs' $(if ($missing.Count) { 'differs' } else { 'ok' }) "missing: $($missing -join ' '); ice.pkg: $pkg" }
        'apply' {
            if (-not $missing.Count) { return Result 'dirs' 'ok' "ice.pkg: $pkg (the ice PMD needs it for a PF)" }
            if (-not $state.PSObject.Properties['dirs']) { Set-StateItem $state 'dirs' @{ made = $missing } }
            $missing | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }
            return Result 'dirs' 'set' "made: $($missing -join ' ')"
        }
        'undo' {
            if (-not $state.PSObject.Properties['dirs']) { return Result 'dirs' 'ok' 'nothing to undo' }
            foreach ($d in @($state.dirs.made)) {
                if ((Test-Path $d) -and -not (Get-ChildItem $d -Force)) { Remove-Item $d }
            }
            Remove-StateItem $state 'dirs'
            return Result 'dirs' 'set' 'empty folders removed'
        }
    }
}

# The machine PATH as the registry keeps it (REG_EXPAND_SZ, with %...%).
# [Environment]::SetEnvironmentVariable would write it back as REG_SZ, and
# then %SystemRoot% in it does not expand.
$EnvKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
function Get-MachinePath { (Get-Item $EnvKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames') }
function Set-MachinePath([string]$value) {
    Set-ItemProperty -Path $EnvKey -Name Path -Value $value -Type ExpandString
    # Tell the running programs that the environment changed.
    Add-Type -Namespace MtlEnv -Name Win32 -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@ -ErrorAction SilentlyContinue
    $r = [UIntPtr]::Zero
    [void][MtlEnv.Win32]::SendMessageTimeout([IntPtr]0xffff, 0x1a, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$r)
}

function Item-Path([string]$act, $state) {
    $now = Get-MachinePath
    $parts = @($now -split ';' | Where-Object { $_ -ne '' })
    $has = @($parts | Where-Object { $_.TrimEnd('\') -ieq $MsysBin.TrimEnd('\') }).Count -gt 0
    switch ($act) {
        'status' { return Result 'path' $(if ($has) { 'ok' } else { 'differs' }) "$MsysBin in the machine PATH: $has" }
        'apply' {
            if ($has) { return Result 'path' 'ok' "$MsysBin in the machine PATH" }
            if (-not (Test-Path $MsysBin)) { return Result 'path' 'error' "$MsysBin is not there: install MSYS2 and the packages first" }
            if (-not $state.PSObject.Properties['path']) { Set-StateItem $state 'path' @{ entry = $MsysBin } }
            Set-MachinePath (($parts + $MsysBin) -join ';')
            return Result 'path' 'set' "$MsysBin added at the end of the machine PATH (a new shell sees it)"
        }
        'undo' {
            if (-not $state.PSObject.Properties['path']) { return Result 'path' 'ok' 'nothing to undo' }
            # Remove only the entry that apply added: keep each later change.
            $entry = if ($state.path.entry) { $state.path.entry } else { $MsysBin }
            Set-MachinePath ((@($parts | Where-Object { $_.TrimEnd('\') -ine $entry.TrimEnd('\') })) -join ';')
            Remove-StateItem $state 'path'
            return Result 'path' 'set' "$entry removed from the machine PATH"
        }
    }
}

function Item-CpuSets([string]$act, $state) {
    $sets = Get-CpuSets
    $cpus = Get-SchedulerCpus $state
    $bytes = 8 * [math]::Ceiling($sets.Count / 64)
    $now = Get-RegValue $KernelKey 'ReservedCpuSets'
    $alloc = @($sets | Where-Object { $_.Flags -band 2 } | ForEach-Object Cpu) -join ','
    switch ($act) {
        'status' {
            if (-not $cpus.Count) { return Result 'cpusets' 'differs' "ReservedCpuSets $(ConvertTo-Hex $now); no -SchedulerCpus" }
            $want = ConvertTo-Mask $cpus $bytes
            $st = if ((ConvertTo-Hex $now) -eq (ConvertTo-Hex $want)) { 'ok' } else { 'differs' }
            return Result 'cpusets' $st "ReservedCpuSets $(ConvertTo-Hex $now), want $(ConvertTo-Hex $want) (CPUs $($cpus -join ',')); allocated now: $alloc"
        }
        'apply' {
            if (-not $cpus.Count) { return Result 'cpusets' 'error' 'give -SchedulerCpus, for example 8-15' }
            Test-SchedulerCpus $cpus $sets
            $want = ConvertTo-Mask $cpus $bytes
            Set-StateItem $state 'SchedulerCpus' ($cpus -join ',')
            if ((ConvertTo-Hex $now) -eq (ConvertTo-Hex $want)) { return Result 'cpusets' 'ok' "CPUs $($cpus -join ',') reserved" }
            if (-not $state.PSObject.Properties['cpusets']) { Set-StateItem $state 'cpusets' @{ value = $now } }
            Set-ItemProperty -Path $KernelKey -Name 'ReservedCpuSets' -Value $want -Type Binary
            return Result 'cpusets' 'restart' "CPUs $($cpus -join ',') reserved ($(ConvertTo-Hex $want))"
        }
        'undo' {
            if (-not $state.PSObject.Properties['cpusets']) { return Result 'cpusets' 'ok' 'nothing to undo' }
            $old = $state.cpusets.value
            Restore-RegValue $KernelKey 'ReservedCpuSets' $old 'Binary'
            Remove-StateItem $state 'cpusets'
            Remove-StateItem $state 'SchedulerCpus'
            return Result 'cpusets' 'restart' "ReservedCpuSets $(ConvertTo-Hex $old)"
        }
    }
}

# Present PCI devices with an interrupt management key, but not NetUIO ones:
# DPDK polls those.
function Get-IrqDevices {
    Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\*' } | ForEach-Object {
        $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($_.InstanceId)"
        $svc = Get-RegValue $key 'Service'
        if ((Test-Path "$key\Device Parameters\Interrupt Management") -and $svc -ne 'netuio') {
            [pscustomobject]@{ Id = $_.InstanceId; Name = $_.FriendlyName; Key = "$key\Device Parameters\Interrupt Management\Affinity Policy" }
        }
    }
}

function Item-Irq([string]$act, $state) {
    $cpus = Get-SchedulerCpus $state
    $sets = Get-CpuSets
    $house = @($sets | Where-Object { $_.Group -eq 0 -and $cpus -notcontains $_.Cpu } | ForEach-Object Cpu)
    $want = ConvertTo-Hex (ConvertTo-Mask $house 8)
    $devs = @(Get-IrqDevices)
    $off = @($devs | Where-Object {
            (Get-RegValue $_.Key 'DevicePolicy') -ne 4 -or (ConvertTo-Hex (Get-RegValue $_.Key 'AssignmentSetOverride')) -ne $want })
    switch ($act) {
        'status' {
            if (-not $cpus.Count) { return Result 'irq' 'differs' "$($devs.Count) devices; no -SchedulerCpus" }
            return Result 'irq' $(if ($off.Count) { 'differs' } else { 'ok' }) "$($devs.Count - $off.Count) of $($devs.Count) devices on CPUs $($house -join ',')"
        }
        'apply' {
            if (-not $cpus.Count) { return Result 'irq' 'error' 'give -SchedulerCpus, for example 8-15' }
            if (-not $off.Count) { return Result 'irq' 'ok' "$($devs.Count) devices on CPUs $($house -join ',')" }
            $saved = @{}
            if ($state.PSObject.Properties['irq']) { $state.irq.PSObject.Properties | ForEach-Object { $saved[$_.Name] = $_.Value } }
            foreach ($d in $off) {
                if (-not $saved.ContainsKey($d.Id)) {
                    $saved[$d.Id] = @{ exists = (Test-Path $d.Key)
                        policy = Get-RegValue $d.Key 'DevicePolicy'
                        mask = Get-RegValue $d.Key 'AssignmentSetOverride' }
                }
                New-Item -Path $d.Key -Force | Out-Null
                Set-ItemProperty -Path $d.Key -Name 'DevicePolicy' -Value 4 -Type DWord
                Set-ItemProperty -Path $d.Key -Name 'AssignmentSetOverride' -Value (ConvertTo-Mask $house 8) -Type Binary
            }
            Set-StateItem $state 'irq' $saved
            return Result 'irq' 'restart' "$($off.Count) devices set to CPUs $($house -join ',')"
        }
        'undo' {
            if (-not $state.PSObject.Properties['irq']) { return Result 'irq' 'ok' 'nothing to undo' }
            $n = 0
            foreach ($p in $state.irq.PSObject.Properties) {
                $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($p.Name)\Device Parameters\Interrupt Management\Affinity Policy"
                if (-not $p.Value.exists) { Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue }
                else {
                    Restore-RegValue $key 'DevicePolicy' $p.Value.policy 'DWord'
                    Restore-RegValue $key 'AssignmentSetOverride' $p.Value.mask 'Binary'
                }
                $n++
            }
            Remove-StateItem $state 'irq'
            return Result 'irq' 'restart' "$n devices back to their old policy"
        }
    }
}

function Get-ActiveScheme { ((powercfg.exe /getactivescheme) -replace '.*GUID: ([0-9a-f-]+).*', '$1').Trim() }

# The AC value of a processor setting of a plan, from the registry: powercfg
# /query does not show a hidden setting such as CPMINCORES.
$PowerGuids = @{ CPMINCORES = '0cc5b647-c1df-4637-891a-dec35c318583'; PROCTHROTTLEMIN = '893dee8e-2bef-41e0-89c6-b55d0929964c' }
function Get-PowerValue([string]$scheme, [string]$setting) {
    $key = "HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\$scheme\54533251-82be-4824-96c1-47b60b740d00\$($PowerGuids[$setting])"
    $v = Get-RegValue $key 'ACSettingIndex'
    if ($null -eq $v) { return -1 }
    return [int]$v
}

function Item-Power([string]$act, $state) {
    $active = Get-ActiveScheme
    $ours = if ($state.PSObject.Properties['power']) { $state.power.scheme } else { '' }
    $good = ($active -eq $ours) -and (Get-PowerValue $active 'CPMINCORES') -eq 100 -and (Get-PowerValue $active 'PROCTHROTTLEMIN') -eq 100
    switch ($act) {
        'status' { return Result 'power' $(if ($good) { 'ok' } else { 'differs' }) "active $active, core parking min $(Get-PowerValue $active 'CPMINCORES') %, min state $(Get-PowerValue $active 'PROCTHROTTLEMIN') %" }
        'apply' {
            if ($good) { return Result 'power' 'ok' "plan $active" }
            $scheme = $ours
            $made = $false
            if ($scheme -eq '') {
                foreach ($base in $UltimateGuid, $HighPerfGuid) {
                    # A native error line must not stop the script here.
                    $out = & { $ErrorActionPreference = 'Continue'; powercfg.exe -duplicatescheme $base 2>&1 }
                    if ($LASTEXITCODE -eq 0 -and "$out" -match 'GUID: ([0-9a-f-]+)') { $scheme = $Matches[1]; $made = $true; break }
                }
                if ($scheme -eq '') { return Result 'power' 'error' 'no Ultimate Performance and no High performance plan' }
                Set-StateItem $state 'power' @{ before = $active; scheme = $scheme; made = $made }
            }
            powercfg.exe -setacvalueindex $scheme SUB_PROCESSOR CPMINCORES 100 | Out-Null
            powercfg.exe -setacvalueindex $scheme SUB_PROCESSOR PROCTHROTTLEMIN 100 | Out-Null
            powercfg.exe /setactive $scheme | Out-Null
            return Result 'power' 'set' "plan $scheme active"
        }
        'undo' {
            if (-not $state.PSObject.Properties['power']) { return Result 'power' 'ok' 'nothing to undo' }
            $old = $state.power.before
            powercfg.exe /setactive $old | Out-Null
            if ($state.power.made) { powercfg.exe -delete $state.power.scheme | Out-Null }
            Remove-StateItem $state 'power'
            return Result 'power' 'set' "plan $old active"
        }
    }
}

function Item-Defender([string]$act, $state) {
    try { $have = @((Get-MpPreference).ExclusionPath) } catch { return Result 'defender' 'skip' "Defender is not available: $($_.Exception.Message)" }
    $missing = @($ExcludePaths | Where-Object { $have -notcontains $_ })
    switch ($act) {
        'status' { return Result 'defender' $(if ($missing.Count) { 'differs' } else { 'ok' }) "missing exclusions: $($missing -join ' ')" }
        'apply' {
            if (-not $missing.Count) { return Result 'defender' 'ok' "excluded: $($ExcludePaths -join ' ')" }
            $added = @()
            if ($state.PSObject.Properties['defender']) { $added = @($state.defender.added) }
            Add-MpPreference -ExclusionPath $missing
            Set-StateItem $state 'defender' @{ added = @($added + $missing | Sort-Object -Unique) }
            return Result 'defender' 'set' "excluded: $($missing -join ' ')"
        }
        'undo' {
            if (-not $state.PSObject.Properties['defender']) { return Result 'defender' 'ok' 'nothing to undo' }
            $old = @($state.defender.added)
            Remove-MpPreference -ExclusionPath $old
            Remove-StateItem $state 'defender'
            return Result 'defender' 'set' "removed: $($old -join ' ')"
        }
    }
}

function Item-Vbs([string]$act, $state) {
    $vbs = Get-RegValue $DgKey 'EnableVirtualizationBasedSecurity'
    $hvci = Get-RegValue $HvciKey 'Enabled'
    $running = 'unknown'
    try { $running = (Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard).VirtualizationBasedSecurityStatus } catch { }
    $good = ($vbs -eq 0) -and ($hvci -eq 0)
    switch ($act) {
        'status' { return Result 'vbs' $(if ($good) { 'ok' } else { 'differs' }) "VBS key $vbs, HVCI key $hvci, VBS status $running (0 off, 2 running)" }
        'apply' {
            if ($good) { return Result 'vbs' 'ok' "VBS status $running" }
            if (-not $state.PSObject.Properties['vbs']) { Set-StateItem $state 'vbs' @{ vbs = $vbs; hvci = $hvci } }
            Restore-RegValue $DgKey 'EnableVirtualizationBasedSecurity' 0 'DWord'
            Restore-RegValue $HvciKey 'Enabled' 0 'DWord'
            return Result 'vbs' 'restart' 'VBS and Memory Integrity off'
        }
        'undo' {
            if (-not $state.PSObject.Properties['vbs']) { return Result 'vbs' 'ok' 'nothing to undo' }
            Restore-RegValue $DgKey 'EnableVirtualizationBasedSecurity' $state.vbs.vbs 'DWord'
            Restore-RegValue $HvciKey 'Enabled' $state.vbs.hvci 'DWord'
            Remove-StateItem $state 'vbs'
            return Result 'vbs' 'restart' 'VBS keys back'
        }
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

$state = Get-State
$map = @{
    testsign = 'Item-TestSign'; lockpages = 'Item-LockPages'; dirs = 'Item-Dirs'; path = 'Item-Path'; cpusets = 'Item-CpuSets'
    irq = 'Item-Irq'; power = 'Item-Power'; defender = 'Item-Defender'; vbs = 'Item-Vbs'
}
$order = @('testsign', 'lockpages', 'dirs', 'path', 'cpusets', 'irq', 'power', 'defender', 'vbs')
if ($Action -eq 'undo') { [array]::Reverse($order) }
$results = foreach ($name in ($order | Where-Object { $Items -contains $_ })) {
    try { & $map[$name] $Action $state }
    catch { Result $name 'error' $_.Exception.Message }
    if ($Action -ne 'status') { Save-State $state }
}
$results | Format-Table -AutoSize -Wrap | Out-String -Width 200 | Write-Host
if ($script:NeedRestart.Count) {
    Write-Host "Restart Windows to finish: $($script:NeedRestart -join ', ')"
    if ($Restart) { Restart-Computer -Force }
}
if ($script:Failed) { exit 1 }
