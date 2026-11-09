# Matched, raw, read-only Windows/WSL comparison inside Win11Debug.
param(
    [string]$Root='C:\USBWarp-Test',
    [string[]]$Order=@('W1','L1','L2','W2','W3','L3'),
    [string[]]$Cases=@('seq_1m_q1','seq_1m_q4','seq_64k_q1','rand_4k_q1','rand_4k_q4'),
    [string]$Results='results.jsonl',
    [switch]$Fingerprint
)
$ErrorActionPreference='Stop'
$bench=Join-Path $Root 'benchmark'
$controller=Join-Path $Root 'bin\USBWarpController.exe'
$instance='USB\VID_346D&PID_5678\FC131A137FFE8'
$container='{1EA241E7-1CA5-507E-B2F0-EB05BE4B0973}'
$baseline=Get-Content -Raw (Join-Path $Root 'backup\usb-read-baseline.json') | ConvertFrom-Json
$partition=@(Get-Content -Raw (Join-Path $Root 'backup\usb-partitions.json') | ConvertFrom-Json)[0]
$volumePath=@($partition.AccessPaths | Where-Object {$_ -like '\\?\Volume{*'})[0]
if(-not $volumePath) { throw 'Missing original volume identity' }
$resultFile=Join-Path $bench $Results
if(Test-Path -LiteralPath $resultFile) { throw 'Refusing to mix benchmark runs in an existing results file' }
function Find-Target {
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    do {
        $disks=@(Get-Disk | Where-Object {$_.BusType -eq 'USB' -and $_.SerialNumber -eq 'FC131A137FFE8'})
        if($disks.Count -gt 1 -or ($disks.Count -eq 1 -and ($disks[0].IsBoot -or $disks[0].IsSystem))) { throw 'Ambiguous/system disk' }
        if($disks.Count -eq 1 -and $disks[0].Size -eq 62914560000) { return $disks[0] }
        Start-Sleep -Milliseconds 200
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Original USB disk/capacity unavailable'
}
function Dismount-Target {
    $disk=Find-Target
    $atD=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($atD) {
        if($atD.DiskNumber -ne $disk.Number) { throw 'D: belongs to another disk' }
        mountvol.exe D:\ /p
        if($LASTEXITCODE) { throw 'Target dismount failed' }
    }
}
function Check-WindowsHash {
    $disk=Find-Target
    $file=[IO.File]::Open(('\\.\PhysicalDrive'+$disk.Number),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $sha=[Security.Cryptography.SHA256]::Create()
    try {
        $buffer=New-Object byte[] (1MB); $left=[int64]$baseline.Bytes
        while($left -gt 0) {
            $n=$file.Read($buffer,0,[int][Math]::Min($buffer.Length,$left))
            if($n -le 0) { throw 'Short validation read' }
            $null=$sha.TransformBlock($buffer,0,$n,$buffer,0); $left-=$n
        }
        $null=$sha.TransformFinalBlock([byte[]]@(),0,0)
        $hash=([BitConverter]::ToString($sha.Hash)).Replace('-','').ToLowerInvariant()
        if($hash -ne $baseline.Sha256) { throw 'Windows baseline hash mismatch' }
        "Validation outside benchmark timing: Windows SHA256=$hash"
    } finally { $sha.Dispose(); $file.Dispose() }
}
function Wait-Running {
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    do {
        try { $state=(& $controller status 2>&1) -join "`n" } catch {$state=''}
        if($state -match 'Session:\s+RUNNING') { return }
        Start-Sleep -Milliseconds 300
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Guest session not ready'
}
function Save-Result([string]$line,[string]$label,[string]$case) {
    $r=$line | ConvertFrom-Json
    if($r.label -ne $label -or $r.case -ne $case -or -not $r.direct -or -not $r.read_only -or $r.ops -le 0) { throw 'Invalid benchmark result' }
    $line | Add-Content -LiteralPath $resultFile
    $line
}
$failure=$null; $bound=$false
try {
    Start-Service UsbWarpService
    Wait-Running
    Dismount-Target
    Check-WindowsHash
    foreach($label in $Order) {
        if($label -notmatch '^[WL][0-9]+$') { throw 'Invalid benchmark group' }
        Dismount-Target
        if($label.StartsWith('W')) {
            # Fresh device state for every group, including consecutive Windows groups.
            & (Join-Path $Root 'capture_reset.exe') $instance $container
            if($LASTEXITCODE) { throw 'Native port reset failed' }
            $disk=Find-Target
            Dismount-Target
            $stack=@((Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Stack).Data)
            if($stack -notcontains '\Driver\USBSTOR' -or $stack -contains '\Driver\UsbWarpFilter') { throw 'Invalid native stack' }
            "BEGIN $label Windows native; PhysicalDrive$($disk.Number); $($stack -join ' -> ')"
            if($Fingerprint -and $label -eq 'W1') {
                $fp=& (Join-Path $bench 'usb_read_bench.exe') ('\\.\PhysicalDrive'+$disk.Number) $label 'FC131A137FFE8' '62914560000' fingerprint
                if($LASTEXITCODE) { throw 'Windows range fingerprint failed' }
                $fp | Add-Content -LiteralPath (Join-Path $bench 'fingerprints.jsonl'); $fp
            }
            foreach($case in $Cases) {
                $output=& (Join-Path $bench 'usb_read_bench.exe') ('\\.\PhysicalDrive'+$disk.Number) $label 'FC131A137FFE8' '62914560000' $case
                if($LASTEXITCODE) { throw "Windows benchmark failed: $case" }
                foreach($line in $output) { Save-Result $line $label $case }
            }
        } else {
            & $controller bind a5a8ea84
            if($LASTEXITCODE) { throw 'Benchmark capture failed' }
            $bound=$true
            $stack=@((Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Stack).Data)
            if($stack -notcontains '\Driver\UsbWarpFilter' -or $stack -contains '\Driver\USBSTOR') { throw 'Invalid capture stack' }
            "BEGIN $label WSL through USBWarp; $($stack -join ' -> ')"
            $metadata=& wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/benchmark/usb_bench_linux.py --label $label --metadata
            if($LASTEXITCODE) { throw 'Guest metadata failed' }
            $metadata | Add-Content -LiteralPath (Join-Path $bench 'linux-metadata.jsonl')
            if($Fingerprint -and $label -eq 'L1') {
                $fp=& wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/benchmark/usb_bench_linux.py --label $label --case fingerprint
                if($LASTEXITCODE) { throw 'Linux range fingerprint failed' }
                $fp | Add-Content -LiteralPath (Join-Path $bench 'fingerprints.jsonl'); $fp
                $fingerprints=@(Get-Content (Join-Path $bench 'fingerprints.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
                if($fingerprints.Count -ne 2 -or $fingerprints[0].hash -ne $fingerprints[1].hash) { throw 'Windows/Linux benchmark-range data mismatch' }
            }
            if($label -eq 'L1') {
                & wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/benchmark/usb_bench_linux.py --label $label --verify
                if($LASTEXITCODE) { throw 'Guest baseline hash mismatch' }
            }
            foreach($case in $Cases) {
                $output=& wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/benchmark/usb_bench_linux.py --label $label --case $case
                if($LASTEXITCODE) { throw "WSL benchmark failed: $case" }
                foreach($line in $output) { Save-Result $line $label $case }
            }
            & $controller unbind a5a8ea84
            if($LASTEXITCODE) { throw 'Benchmark release failed' }
            $bound=$false
        }
        "END $label"
    }
    Check-WindowsHash
} catch { $failure=$_ }
finally {
    if($bound) { try { & $controller unbind a5a8ea84 | Out-Host } catch { } }
    Stop-Service UsbWarpService
    $disk=Find-Target
    $atD=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($atD -and $atD.DiskNumber -ne $disk.Number) { throw 'D: belongs to another disk' }
    if(-not $atD) { mountvol.exe D:\ $volumePath; if($LASTEXITCODE) { throw 'Original mount restoration failed' } }
    if((Get-Partition -DriveLetter D).DiskNumber -ne $disk.Number -or (Get-Volume -DriveLetter D).HealthStatus -ne 'Healthy') { throw 'Original volume health/identity mismatch' }
    if((Get-Item 'HKLM:\SOFTWARE\UsbWarp\Service\DeviceCapture').GetValueNames().Count) { throw 'Capture restoration pending' }
    'Original Windows driver, disk capacity and Healthy D: restored.'
}
if($failure) { throw $failure }
'Matched read-only benchmark complete.'
