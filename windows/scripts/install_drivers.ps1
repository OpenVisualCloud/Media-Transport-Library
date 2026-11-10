#Requires -RunAsAdministrator
<#
.SYNOPSIS
Build and install the NetUIO and virt2phys drivers that DPDK needs on Windows.

.DESCRIPTION
install_drivers.ps1 status|apply|undo [-Ewdk <folder or ISO>] [-HardwareIds <IDs>]

DPDK on Windows needs two kernel drivers of dpdk-kmods: virt2phys gives the
physical address of a large page, and NetUIO gives a NIC to DPDK. Nobody signs
them. apply builds them on this host with the Enterprise WDK (EWDK), so the
build makes its test certificate here, and only this host trusts it.

The items, in order:

  build      Build both drivers in -WorkDir with the EWDK. When the drivers
             are already there, apply does not build them again (-Rebuild
             does).
  trust      Put the test certificates of the build in the Root and the
             TrustedPublisher stores of the machine, so the driver install
             shows no prompt.
  virt2phys  Make the virt2phys device (Root\virt2phys).
  netuio     Give NetUIO to each device of -HardwareIds. A device that is
             not there yet (a VF that you make later) gets NetUIO when it
             comes.

The drivers load only with test signing on and Secure Boot off, after a
restart (windows_setup.ps1 apply -Items testsign). undo removes the driver
packages, the virt2phys device and the certificates; Windows then gives each
NIC its own driver again.

-Ewdk: a folder (the EWDK ISO mounted, for example E:\) or the ISO file.
With no -Ewdk, the script looks for a mounted EWDK on each drive, then for an
EWDK ISO (*ewdk*.iso) in the Downloads folder, which it mounts.

-HardwareIds: the IDs of the last apply, or the Intel Ethernet Adaptive
Virtual Function (the iavf VF of E810 and E830). For a PF, give its ID, for
example 'PCI\VEN_8086&DEV_1592' (E810-C for QSFP) or 'PCI\VEN_8086&DEV_12D2'
(E830-CC for QSFP). NetUIO takes each device with that ID: never give the ID
of the NIC that Windows uses for its own network.

-Source: the windows folder of dpdk-kmods. The default is the copy of the MTL
installer (..\drivers\dpdk-kmods\windows).

.EXAMPLE
.\install_drivers.ps1 apply
.\install_drivers.ps1 apply -Ewdk D:\iso\EWDK.iso -HardwareIds 'PCI\VEN_8086&DEV_1592'
.\install_drivers.ps1 status
.\install_drivers.ps1 undo
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'apply', 'undo')]
    [string]$Action = 'status',
    [string]$Ewdk = '',
    [string[]]$HardwareIds = @(),
    [string]$Source = '',
    [string]$WorkDir = (Join-Path $env:ProgramData 'MTL\drivers'),
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
# PowerShell 5.1 has no $PSScriptRoot yet in the param() block.
if ($Source -eq '') { $Source = Join-Path $PSScriptRoot '..\drivers\dpdk-kmods\windows' }
$StateFile = Join-Path $WorkDir 'state.json'
$Devcon = Join-Path $WorkDir 'devcon.exe'
# Where the build puts each driver package and its test certificate.
$Drivers = @{
    virt2phys = @{ Pkg = 'virt2phys\x64\Release\virt2phys'; Cer = 'virt2phys\x64\Release\virt2phys.cer'; Inf = 'virt2phys.inf' }
    netuio    = @{ Pkg = 'netuio\x64\Release\netuio\netuio'; Cer = 'netuio\x64\Release\netuio\netuio.cer'; Inf = 'netuio.inf' }
}
# DPDK drives E830 and E835 PFs, but netuio.inf of dpdk-kmods does not list
# them (drivers/net/intel/ice/base/ice_devids.h of DPDK).
$AddedIds = [ordered]@{
    '12D1' = 'E830-CC for backplane'; '12D2' = 'E830-CC for QSFP'; '12D3' = 'E830-CC for SFP'
    '12D5' = 'E830-C for backplane'; '12D8' = 'E830-C for QSFP'; '12DA' = 'E830-C for SFP'
    '12DC' = 'E830-L for backplane'; '12DD' = 'E830-L for QSFP'; '12DE' = 'E830-L for SFP'
    '1248' = 'E835-CC for backplane'; '1249' = 'E835-CC for QSFP'; '124A' = 'E835-CC for SFP'
    '1261' = 'E835-C for backplane'; '1262' = 'E835-C for QSFP'; '1263' = 'E835-C for SFP'
    '1265' = 'E835-L for backplane'; '1266' = 'E835-L for QSFP'; '1267' = 'E835-L for SFP'
}
# Change it when the build steps change: apply then builds again.
$BuildId = 'netuio-e830-e835-1'
$script:Failed = $false
$script:NeedRestart = $false
$script:Mounted = ''

function Result([string]$item, [string]$st, [string]$detail) {
    if ($st -eq 'error') { $script:Failed = $true }
    if ($st -eq 'restart') { $script:NeedRestart = $true }
    [pscustomobject]@{ Item = $item; State = $st; Detail = $detail }
}

function Get-State {
    if (Test-Path $StateFile) { return Get-Content $StateFile -Raw | ConvertFrom-Json }
    return [pscustomobject]@{ certs = @() }
}

function Save-State($state) {
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    $state | ConvertTo-Json -Depth 4 | Set-Content -Path $StateFile -Encoding UTF8
}

# The root folder of the EWDK: -Ewdk, a mounted EWDK on a drive, or an EWDK
# ISO of the Downloads folder. The script mounts an ISO, and the end of the
# script unmounts it.
function Find-Ewdk {
    $iso = $Ewdk
    if ($iso -eq '') {
        foreach ($d in Get-PSDrive -PSProvider FileSystem) {
            if (Test-Path (Join-Path $d.Root 'BuildEnv\SetupBuildEnv.cmd')) { return $d.Root }
        }
        $f = Get-ChildItem (Join-Path $env:USERPROFILE 'Downloads') -Filter '*ewdk*.iso' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($null -eq $f) { return '' }
        $iso = $f.FullName
    }
    if ($iso -notlike '*.iso') { return $iso }
    $img = Mount-DiskImage -ImagePath (Resolve-Path $iso).Path -PassThru
    $script:Mounted = $img.ImagePath
    return "$(($img | Get-Volume).DriveLetter):\"
}

function Test-Built {
    if ((Get-Content (Join-Path $WorkDir 'build.id') -ErrorAction SilentlyContinue) -ne $BuildId) { return $false }
    foreach ($n in $Drivers.Keys) {
        if (-not (Test-Path (Join-Path $WorkDir "$n\$n.sys"))) { return $false }
    }
    return (Test-Path $Devcon)
}

# The present devices that use a driver service, with their PCI address.
function Get-ServiceDevices([string]$service) {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
        $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($_.InstanceId)"
        $svc = (Get-ItemProperty -Path $key -Name Service -ErrorAction SilentlyContinue).Service
        if ($svc -eq $service) {
            $bus = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_BusNumber -ErrorAction SilentlyContinue).Data
            $adr = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Address -ErrorAction SilentlyContinue).Data
            $pci = ''
            if ($_.InstanceId -like 'PCI\*' -and $null -ne $bus -and $null -ne $adr) {
                $pci = '0000:{0:x2}:{1:x2}.{2:x}' -f $bus, ($adr -shr 16), ($adr -band 0xffff)
            }
            [pscustomobject]@{ Id = $_.InstanceId; Status = $_.Status; Pci = $pci }
        }
    }
}

function Get-HwidDevices([string]$hwid) {
    @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$hwid*" })
}

# The driver packages of the driver store that came from netuio.inf or
# virt2phys.inf (oemNN.inf names).
function Get-StorePackages {
    @(Get-WindowsDriver -Online -ErrorAction SilentlyContinue | Where-Object {
            @('netuio.inf', 'virt2phys.inf') -contains (Split-Path $_.OriginalFileName -Leaf) })
}

# Run a program. A line on its standard error must not stop the script, so
# the error action is Continue here.
function Invoke-Native([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $out = & $exe @arguments 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out.Trim() }
}

function Invoke-Devcon([string[]]$arguments) {
    $r = Invoke-Native $Devcon $arguments
    # 0: done; 1: done, a restart is necessary.
    if ($r.Code -gt 1) { throw "devcon $($arguments -join ' ') failed ($($r.Code)): $($r.Out)" }
    return $r.Code
}

# ---------------------------------------------------------------------------
# Items
# ---------------------------------------------------------------------------

function Item-Build([string]$act, $state) {
    $built = Test-Built
    switch ($act) {
        'status' { return Result 'build' $(if ($built) { 'ok' } else { 'differs' }) "drivers in $WorkDir`: $built" }
        'undo' { return Result 'build' 'ok' 'removed with the folder' }
        'apply' {
            if ($built -and -not $Rebuild) { return Result 'build' 'ok' "drivers in $WorkDir" }
            if (-not (Test-Path (Join-Path $Source 'netuio\netuio.sln'))) {
                return Result 'build' 'error' "no dpdk-kmods source in $Source (give -Source <dpdk-kmods>\windows)"
            }
            $root = Find-Ewdk
            if ($root -eq '' -or -not (Test-Path (Join-Path $root 'BuildEnv\SetupBuildEnv.cmd'))) {
                return Result 'build' 'error' 'no EWDK: put the EWDK ISO in Downloads, or give -Ewdk <ISO file>'
            }
            $src = Join-Path $WorkDir 'src'
            if (Test-Path $src) { Remove-Item -Recurse -Force $src }
            New-Item -ItemType Directory -Force -Path $src | Out-Null
            Copy-Item -Recurse (Join-Path $Source 'netuio'), (Join-Path $Source 'virt2phys') $src
            # netuio.inf: the WDK 10.0.28000 refuses an INF with no PnpLockdown,
            # and E830 and E835 are not in it.
            $inf = Join-Path $src 'netuio\netuio.inf'
            $lines = [System.Collections.Generic.List[string]](Get-Content $inf)
            foreach ($h in '[Version]', '[Standard.NT$ARCH$]', '[Strings]') {
                if ($lines.IndexOf($h) -lt 0) { return Result 'build' 'error' "no $h in $inf" }
            }
            if (-not ($lines -match '^\s*PnpLockdown').Count) { $lines.Insert($lines.IndexOf('[Version]') + 1, 'PnpLockdown=1') }
            foreach ($id in $AddedIds.Keys) {
                if (($lines -match "DEV_$id").Count) { continue }
                $lines.Insert($lines.IndexOf('[Standard.NT$ARCH$]') + 1, "%Intel.F$id.Description%=netuio_Device, PCI\VEN_8086&DEV_$id")
                $lines.Insert($lines.IndexOf('[Strings]') + 1, "Intel.F$id.Description = `"Intel(R) Ethernet Controller $($AddedIds[$id])`"")
            }
            $lines | Set-Content $inf -Encoding ASCII
            $log = Join-Path $WorkDir 'build.log'
            $bat = Join-Path $WorkDir 'build.cmd'
            @(
                '@echo off'
                "call `"$(Join-Path $root 'BuildEnv\SetupBuildEnv.cmd')`" amd64 >NUL || exit /b 1"
                "cd /d `"$src\virt2phys`" || exit /b 2"
                'msbuild virt2phys.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /v:minimal /nologo || exit /b 3'
                "cd /d `"$src\netuio`" || exit /b 4"
                'msbuild netuio.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /v:minimal /nologo || exit /b 5'
            ) | Set-Content $bat -Encoding ASCII
            # Wait for cmd.exe only. Start-Process -Wait also waits for each
            # child, and VCTIP.EXE of the EWDK compiler stays after the build.
            $p = Start-Process cmd.exe -ArgumentList '/d', '/c', "`"$bat`"" -RedirectStandardOutput $log `
                -RedirectStandardError "$log.err" -NoNewWindow -PassThru
            $null = $p.Handle # Without the handle, PowerShell 5.1 loses the exit code.
            $p.WaitForExit()
            $rc = $p.ExitCode
            if ($rc -ne 0) { return Result 'build' 'error' "the build failed ($rc): see $log" }
            foreach ($n in $Drivers.Keys) {
                $dst = Join-Path $WorkDir $n
                if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
                Copy-Item -Recurse (Join-Path $src $Drivers[$n].Pkg) $dst
                Copy-Item (Join-Path $src $Drivers[$n].Cer) (Join-Path $dst "$n.cer")
            }
            $dc = Get-ChildItem (Join-Path $root 'Program Files\Windows Kits\10\Tools\*\x64\devcon.exe') | Select-Object -First 1
            if ($null -eq $dc) { return Result 'build' 'error' "no devcon.exe in the EWDK $root" }
            # A file of the ISO is read-only, also in its copy.
            Copy-Item $dc.FullName $Devcon -Force
            (Get-Item $Devcon).Attributes = 'Normal'
            # A build tool that is still running can hold a file of src.
            Remove-Item -Recurse -Force $src -ErrorAction SilentlyContinue
            Set-Content (Join-Path $WorkDir 'build.id') $BuildId -Encoding ASCII
            return Result 'build' 'set' "drivers built in $WorkDir (log $log)"
        }
    }
}

function Item-Trust([string]$act, $state) {
    $have = @($state.certs | Where-Object { (Test-Path "Cert:\LocalMachine\Root\$_") -and (Test-Path "Cert:\LocalMachine\TrustedPublisher\$_") })
    switch ($act) {
        'status' {
            $ok = $state.certs.Count -gt 0 -and $have.Count -eq $state.certs.Count
            return Result 'trust' $(if ($ok) { 'ok' } else { 'differs' }) "$($have.Count) test certificates trusted"
        }
        'apply' {
            if (-not (Test-Built)) { return Result 'trust' 'error' 'no drivers: the build item failed' }
            $thumbs = @()
            foreach ($n in $Drivers.Keys) {
                $cer = Join-Path $WorkDir "$n\$n.cer"
                $thumbs += (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
                foreach ($store in 'Root', 'TrustedPublisher') {
                    $r = Invoke-Native certutil.exe @('-f', '-addstore', $store, $cer)
                    if ($r.Code -ne 0) { return Result 'trust' 'error' "certutil $store $cer`: $($r.Out)" }
                }
            }
            $thumbs = @($thumbs | Sort-Object -Unique)
            $state.certs = @(@($state.certs) + $thumbs | Sort-Object -Unique)
            return Result 'trust' 'set' "trusted: $($thumbs -join ' ')"
        }
        'undo' {
            foreach ($t in @($state.certs)) {
                foreach ($store in 'Root', 'TrustedPublisher') { Remove-Item "Cert:\LocalMachine\$store\$t" -ErrorAction SilentlyContinue }
            }
            $n = @($state.certs).Count
            $state.certs = @()
            return Result 'trust' 'set' "$n test certificates removed"
        }
    }
}

function Item-Virt2phys([string]$act, $state) {
    $devs = @(Get-ServiceDevices 'virt2phys')
    $good = @($devs | Where-Object { $_.Status -eq 'OK' }).Count -gt 0
    switch ($act) {
        'status' {
            $detail = if ($devs.Count) { "device $($devs[0].Status)" } else { 'no device' }
            return Result 'virt2phys' $(if ($good) { 'ok' } else { 'differs' }) $detail
        }
        'apply' {
            if (-not (Test-Built)) { return Result 'virt2phys' 'error' 'no drivers: the build item failed' }
            $inf = Join-Path $WorkDir 'virt2phys\virt2phys.inf'
            $verb = if ($devs.Count) { 'update' } else { 'install' }
            $rc = Invoke-Devcon @($verb, $inf, 'Root\virt2phys')
            $now = @(Get-ServiceDevices 'virt2phys')
            if (-not $now.Count) { return Result 'virt2phys' 'error' 'devcon made no device' }
            if ($rc -eq 1 -or $now[0].Status -ne 'OK') { return Result 'virt2phys' 'restart' "device $($now[0].Status) until the restart" }
            return Result 'virt2phys' 'set' 'device OK'
        }
        'undo' {
            foreach ($d in $devs) { [void](Invoke-Native pnputil.exe @('/remove-device', $d.Id)) }
            return Result 'virt2phys' 'set' "$($devs.Count) devices removed"
        }
    }
}

function Item-Netuio([string]$act, $state) {
    $devs = @(Get-ServiceDevices 'netuio')
    $want = @($HardwareIds | ForEach-Object { Get-HwidDevices $_ })
    $missing = @($want | Where-Object { $devs.Id -notcontains $_.InstanceId })
    $list = ($devs | ForEach-Object { "$($_.Pci) $($_.Status)" }) -join ', '
    switch ($act) {
        'status' {
            $ok = $devs.Count -gt 0 -and -not $missing.Count -and -not @($devs | Where-Object { $_.Status -ne 'OK' }).Count
            return Result 'netuio' $(if ($ok) { 'ok' } else { 'differs' }) "ports: $list; not on NetUIO: $($missing.Count)"
        }
        'apply' {
            if (-not (Test-Built)) { return Result 'netuio' 'error' 'no drivers: the build item failed' }
            $inf = Join-Path $WorkDir 'netuio\netuio.inf'
            $state | Add-Member -NotePropertyName hardwareIds -NotePropertyValue @($HardwareIds) -Force
            # Put the driver in the store, so a device that comes later gets it.
            $r = Invoke-Native pnputil.exe @('/add-driver', $inf)
            # 3010: added, a restart is necessary.
            if ($r.Code -ne 0 -and $r.Code -ne 3010) { return Result 'netuio' 'error' "pnputil /add-driver ($($r.Code)): $($r.Out)" }
            $restart = $false
            foreach ($id in $HardwareIds) {
                if ((Get-HwidDevices $id).Count) { if ((Invoke-Devcon @('update', $inf, $id)) -eq 1) { $restart = $true } }
            }
            $now = @(Get-ServiceDevices 'netuio')
            $list = ($now | ForEach-Object { "$($_.Pci) $($_.Status)" }) -join ', '
            if (-not $now.Count) { return Result 'netuio' 'set' "driver in the store; no device of $($HardwareIds -join ' ') now" }
            if ($restart -or @($now | Where-Object { $_.Status -ne 'OK' }).Count) { return Result 'netuio' 'restart' "ports: $list" }
            return Result 'netuio' 'set' "ports: $list"
        }
        'undo' { return Result 'netuio' 'ok' 'removed with the driver packages' }
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

$state = Get-State
if (-not $HardwareIds.Count) {
    $HardwareIds = if ($state.PSObject.Properties['hardwareIds']) { @($state.hardwareIds) } else { @('PCI\VEN_8086&DEV_1889') }
}
$order = @('build', 'trust', 'virt2phys', 'netuio')
$map = @{ build = 'Item-Build'; trust = 'Item-Trust'; virt2phys = 'Item-Virt2phys'; netuio = 'Item-Netuio' }
if ($Action -eq 'undo') { [array]::Reverse($order) }
$results = @()
try {
    foreach ($name in $order) {
        try { $results += & $map[$name] $Action $state }
        catch { $results += Result $name 'error' $_.Exception.Message }
        if ($Action -eq 'apply') { Save-State $state }
    }
    if ($Action -eq 'undo') {
        $pkgs = @(Get-StorePackages)
        foreach ($p in $pkgs) { [void](Invoke-Native pnputil.exe @('/delete-driver', $p.Driver, '/uninstall', '/force')) }
        [void](Invoke-Native pnputil.exe @('/scan-devices'))
        $results += Result 'packages' 'set' "$($pkgs.Count) driver packages removed: $(($pkgs.Driver) -join ' ')"
        if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
    }
} finally {
    if ($script:Mounted -ne '') { Dismount-DiskImage -ImagePath $script:Mounted | Out-Null }
}
if ($Action -eq 'status') {
    $on = [bool](bcdedit.exe /enum '{current}' | Select-String -Pattern '^testsigning\s+Yes' -Quiet)
    $results += Result 'testsign' $(if ($on) { 'ok' } else { 'differs' }) "testsigning $on (windows_setup.ps1 apply -Items testsign)"
}
$results | Format-Table -AutoSize -Wrap | Out-String -Width 200 | Write-Host
if ($script:NeedRestart) { Write-Host 'Restart Windows to load the drivers.' }
if ($script:Failed) { exit 1 }
