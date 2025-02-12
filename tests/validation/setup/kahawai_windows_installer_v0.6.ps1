
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022-2025 Intel Corporation

# This script is for development purposes only, use on your own risk

$defaultPath = (Get-Location)
$workPath = "C:\ws\workspace"
$dpdkPath = "$workPath\dpdk"
$kahawaiPath = "$workPath\libraries.media.st2110.kahawai"
$kahawaiPackage = "libraries.media.st2110.kahawai.tgz"
$dpdkVersion = "22.07"

$libdpdkPc = 'Libs.private:'
$libdpdkLibsPc = 'Libs:-L${libdir} -lrte_latencystats -lrte_gso -lrte_bus_pci -lrte_gro -lrte_cfgfile -lrte_bitratestats -lrte_timer -lrte_hash -lrte_metrics -lrte_cmdline -lrte_pci -lrte_ethdev -lrte_meter -lrte_net -lrte_net_ice -lrte_net_iavf -lrte_common_iavf -lrte_mbuf -lrte_mempool -lrte_stack -lrte_mempool_stack -lrte_mempool_ring -lrte_rcu -lrte_ring -lrte_eal -lrte_telemetry -lrte_kvargs -lrte_dmadev -lrte_dma_ioat'

# For proxy connections uncomment and fill below environment variables:
# $Env:http_proxy='http://my-fancy-proxy.intel.com:123'
# $Env:https_proxy='http://my-fancy-proxy.intel.com:123'
# $Env:no_proxy='127.0.0.1,localhost,intel.com'

# Check the test enviroment
function checkEnviroment
{
    $checkDPDKPath = (Test-Path 'C:\dpdk')
    if($checkDPDKPath -eq "True")
    {
       echo "-----------------remove C:\dpdk-----------------"
       Remove-Item 'C:\dpdk' -Recurse -Force
    }
    else
    {
       echo "-----------------no C:\dpdk-----------------"
    }
    $checkLibDPDKPath = (Test-Path 'C:\libst_dpdk')
    if($checkLibDPDKPath -eq "True")
    {
       echo "-----------------remove C:\libst_dpdk-----------------"
       Remove-Item 'C:\libst_dpdk' -Recurse -Force
    }
    else
    {
       echo "-----------------no C:\libst_dpdk-----------------"
    }

    $checkWorkpath = (Test-Path $workPath)
    if($checkWorkpath -eq "True")
    {
        echo "-----------------remove workspace-----------------"
        Remove-Item "$workPath\*" -Recurse -Force
    }
    else
    {
        echo "-----------------mkdir workspace-----------------"
        New-Item -Path $workPath -ItemType Directory
    }

    $checkKahwaiPackage = (Test-Path $kahawaiPackage)
    if ($checkKahwaiPackage -eq "True")
    {
        Copy-Item $kahawaiPackage -Destination $workPath
    }
    else
    {
        echo "-----------------No kahawai source package, please check it-----------------"
        exit
    }
}


function downloadDPDK
{
    cd $workPath
    echo "-----------------The DPDK package is downloading...-----------------"
    git clone https://github.com/DPDK/dpdk.git
	cd dpdk
	git checkout v$dpdkVersion
	git switch -c v$dpdkVersion
    echo "-----------------The DPDK package is git cloned-----------------"

    $DPDKRealVersion = (git branch | grep "$dpdkVersion")
    if ($DPDKRealVersion -like "*$dpdkVersion")
    {
        echo "-----------------The version($DPDKVersion) of DPDK is right-----------------"
    }
    else
    {
        echo "-----------------The version($DPDKVersion) of DPDK is not v$dpdkVersion-----------------"
    }
}


function unzipKahawai
{
    cd $workPath
    $CheckKahawaiPackage = (Test-Path $kahawaiPackage)

    if($CheckKahawaiPackage -eq "True")
    {
        echo "-----------------Kahawai package is unzipping....-----------------"
        7z x "$kahawaiPackage"
		7z x libraries.media.st2110.kahawai.tar -oC:\ws\workspace

    }
    else
    {
        echo "-----------------Kahawai package is not exist-----------------"
        exit
    }
    $checkKahawaiFolder = (Test-Path $kahawaiPath)
    if($checkKahawaiFolder -eq "True")
    {
        echo "-----------------The decompression of Kahawai package is successful-----------------"
    }
    else
    {
        echo "-----------------The decompression of Kahawai package is failed-----------------"
        exit
    }
}

function patchDPDK
{
    echo "-----------------------------patching $dpdkVersion patches-----------------------------"
    cd $dpdkPath
    git config --global user.email "you@example.com"
    git config --global user.name "Your Name"
    $patchList = Get-ChildItem -Path "$kahawaiPath\patches\dpdk\$dpdkVersion\*.patch" -Name
    foreach($file1 in $patchList)
    {
		$fileType = file("$kahawaiPath\patches\dpdk\$dpdkVersion\$file1").split(":")[1]
		if ($fileType -like "*symbolic link*")
		{
			echo "The $file1 is a symbolic link file"
			$real_file = ($fileType).split("")[-1]
		}
		else
		{
			echo "The $file1 is a normal file"
			$real_file = $file1
		}
        echo "git am $kahawaiPath\patches\dpdk\$dpdkVersion\$real_file"
        git am "$kahawaiPath\patches\dpdk\$dpdkVersion\$real_file"
        #echo "done"
    }

	$windowsPathType = file("$kahawaiPath\patches\dpdk\$dpdkVersion\windows").split(":")[1]
	if($windowsPathType -like "*symbolic link*")
	{
		echo "The windows path is a symbolic link"
		$windowsRealPath = ($windowsPathType).split("")[-1]
	}
	else
	{
		echo "The windows path is a normal path"
		$windowsRealPath = "windows"
	}

    $windowsPatchList = Get-ChildItem -Path "$kahawaiPath\patches\dpdk\$dpdkVersion\$windowsRealPath\*.patch" -Name
    foreach($file2 in $windowsPatchList)
    {
		$fileType = file("$kahawaiPath\patches\dpdk\$dpdkVersion\$windowsRealPath\$file2").split(":")[1]
		if ($fileType -like "*symbolic link*")
		{
			echo "The $file2 is a symbolic link file"
			$realFile = ($fileType).split("")[-1]
		}
		else
		{
			echo "The $file2 is a normal file"
			$realFile = $file2
		}
        echo "-----------------------------paching windows patches-----------------------------"
        echo "git am $kahawaiPath\patches\dpdk\$dpdkVersion\$windowsRealPath\$realFile"
        git am "$kahawaiPath\patches\dpdk\$dpdkVersion\$windowsRealPath\$realFile"
        #echo "done"
    }
}

function buildDPDK
{
    cd $dpdkPath
    echo "-----------------------------build DPDK-----------------------------"
    meson build --prefix=c:\dpdk
    ninja -C build install
    Remove-Item "C:\dpdk\lib\*.dll.a"
    (Get-Content "C:\dpdk\lib\pkgconfig\libdpdk.pc") | ForEach-Object {$_ -replace("Libs\..*","$libdpdkPc")} | Out-File -Encoding  ASCII "C:\dpdk\lib\pkgconfig\libdpdk.pc"
    (Get-Content "C:\dpdk\lib\pkgconfig\libdpdk-libs.pc") | ForEach-Object {$_ -replace("^Libs:.*","$libdpdkLibsPc")} | Out-File -Encoding ASCII "C:\dpdk\lib\pkgconfig\libdpdk-libs.pc"
}

function buildKahawai
{
    cd $kahawaiPath
    echo "-----------------------------libst_dpdk is building ....-----------------------------"
    $Env:PKG_CONFIG_PATH="C:\dpdk\lib\pkgconfig;C:\libst_dpdk\lib\pkgconfig;C:\kahawai\json-c\lib\pkgconfig;C:\kahawai\gtest\lib\pkgconfig;C:\kahawai\openssl\lib\pkgconfig;C:\kahawai\SDL2-2.0.22\x86_64-w64-mingw32\lib\pkgconfig"
    echo "meson build --prefix=C:\libst_dpdk -Ddpdk_root_dir=$dpdkPath"
    meson build --prefix=C:\libst_dpdk -Ddpdk_root_dir="$dpdkPath"
    ninja -C build install
    echo "-----------------------------bulid kahawai app-----------------------------"

    cd ./app
    meson build
    ninja -C build
    echo "-----------------------------build test-----------------------------"
    cd ../tests
    meson build
    ninja -C build
}

checkEnviroment
downloadDPDK
unzipKahawai
patchDPDK
buildDPDK
buildKahawai

echo "Copy library to execting path"
cp C:\kahawai\lib_need_to_copy\* $kahawaiPath\tests\build
cp C:\kahawai\lib_need_to_copy\* $kahawaiPath\app\build
