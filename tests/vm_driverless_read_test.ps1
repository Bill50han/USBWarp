# Run inside the authorized Windows test VM. All USB data access is read-only.
param(
    [string]$Root='C:\USBWarp-Test',
    [string]$BusId='a5a8ea84',
    [string]$Serial='FC131A137FFE8',
    [string]$Vendor='346d',
    [string]$Product='5678',
    [switch]$CrashRecovery,
    [switch]$ResetRecovery,
    [switch]$Problem28
)
$ErrorActionPreference='Stop'
$controller=Join-Path $Root 'bin\USBWarpController.exe'
$fixture=Join-Path $Root 'capture_driverless_fixture.exe'
$instance="USB\VID_$Vendor&PID_$Product\$Serial"
$container=[string](Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ContainerId).Data
$baseline=Get-Content -Raw -LiteralPath (Join-Path $Root 'backup\usb-read-baseline.json') | ConvertFrom-Json
$partition=@(Get-Content -Raw -LiteralPath (Join-Path $Root 'backup\usb-partitions.json') | ConvertFrom-Json)[0]
$volumePath=@($partition.AccessPaths | Where-Object {$_ -like '\\?\Volume{*'})[0]
if(-not $volumePath) { throw 'Missing original volume identity' }
function Find-OriginalDisk {
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    do {
        $disks=@(Get-Disk | Where-Object {$_.BusType -eq 'USB' -and $_.SerialNumber -eq $Serial})
        if($disks.Count -gt 1 -or ($disks.Count -eq 1 -and ($disks[0].IsBoot -or $disks[0].IsSystem))) {
            throw 'Ambiguous/system disk; no disk changes allowed'
        }
        if($disks.Count -eq 1 -and $disks[0].Size -eq $baseline.Size) { return $disks[0] }
        Start-Sleep -Milliseconds 200
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Original USB capacity did not return'
}
function Wait-Running {
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    do {
        try { $state=(& $controller status 2>&1) -join "`n" } catch { $state='' }
        if($state -match 'Session:\s+RUNNING') { return $state }
        Start-Sleep -Milliseconds 300
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Guest session did not become ready'
}
function Verify-NoDriver {
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    do {
        $output=& $fixture verify-none $instance $container
        if($LASTEXITCODE -eq 0) { $output; return }
        Start-Sleep -Milliseconds 200
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Original no-driver state did not return'
}
function Read-Captured {
    $stack=@((Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Stack).Data)
    if($stack -notcontains '\Driver\UsbWarpFilter' -or $stack -contains '\Driver\USBSTOR') { throw 'Capture stack is not exclusive' }
    "Driverless capture stack: $($stack -join ' -> ')"
    if(@(Get-Disk | Where-Object {$_.BusType -eq 'USB' -and $_.SerialNumber -eq $Serial}).Count) { throw 'Native disk still present' }
    $record=(Get-Item 'HKLM:\SOFTWARE\UsbWarp\Service\DeviceCapture').GetValue($container)
    if($record -notcontains 'binding=none' -or $record -notcontains 'version=3') { throw 'No-driver undo state was not recorded' }
    $output=& wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/usb_read_test.py `
        --vid $Vendor --pid $Product --serial $Serial --sha256 $baseline.Sha256 --rounds 1
    $code=$LASTEXITCODE; $output
    $output | Add-Content -LiteralPath (Join-Path $Root 'driverless-validation.jsonl')
    if($code) { throw 'Driverless capture read/hash check failed' }
}
function Bind-Driverless {
    Verify-NoDriver
    $listing=(& $controller list) -join "`n"
    if($listing -notmatch [regex]::Escape($BusId)) { throw 'Driverless USB device is missing from controller enumeration' }
    $listing
    & $controller bind $BusId
    if($LASTEXITCODE) { throw 'Driverless USB bind failed' }
    Start-Sleep -Seconds 3
    Read-Captured
}
$failure=$null; $prepared=$false
try {
    Stop-Service UsbWarpService; Stop-Service USBWarp
    $disk=Find-OriginalDisk
    $mounted=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($mounted) {
        if($mounted.DiskNumber -ne $disk.Number) { throw 'D: belongs to another device' }
        mountvol.exe D:\ /p
        if($LASTEXITCODE) { throw 'Test volume dismount failed' }
    }
    $prepared=$true # fixture journals before changing anything, including failure
    $prepare=if($Problem28) {'prepare-code28'} else {'prepare'}
    & $fixture $prepare $instance $container
    if($LASTEXITCODE) { throw 'No-driver fixture preparation failed' }
    Start-Service UsbWarpService
    $null=Wait-Running
    Bind-Driverless
    & $controller unbind $BusId
    if($LASTEXITCODE) { throw 'Driverless release failed' }
    Verify-NoDriver
    'Normal release restored no INF/service/model, without selecting an available Windows driver.'
    if($ResetRecovery) {
        Bind-Driverless
        & (Join-Path $Root 'capture_reset.exe') $instance $container
        if($LASTEXITCODE) { throw 'External USB port reset failed' }
        Verify-NoDriver
        $state=(& $controller status) -join "`n"
        if($state -notmatch 'Bound devices:\s+0') { throw 'Reset left a kernel binding' }
        'External reset recovered the original no-driver state.'
    }
    if($CrashRecovery) {
        Bind-Driverless
        $service=Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'"
        $oldPid=[int]$service.ProcessId
        if((Get-Process -Id $oldPid).Path -ne (Join-Path $Root 'bin\USBWarpService.exe')) { throw 'Unexpected service executable' }
        Stop-Process -Id $oldPid -Force
        Start-Sleep -Seconds 6
        $state=Wait-Running
        if((Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'").ProcessId -eq $oldPid -or $state -notmatch 'Bound devices:\s+0') {
            throw 'SCM crash recovery retained the old process/binding'
        }
        Verify-NoDriver
        'SCM crash recovery restored no INF/service/model.'
        Start-Sleep -Seconds 10
        Bind-Driverless
    }
} catch { $failure=$_ }
finally {
    try { & $controller unbind $BusId | Out-Host } catch { }
    Stop-Service UsbWarpService
    Stop-Service USBWarp
    if($prepared) {
        $pending=(Get-Item 'HKLM:\SOFTWARE\UsbWarp\Service\DeviceCapture' -ErrorAction SilentlyContinue)
        if($pending -and $pending.GetValueNames().Count) { throw 'Production restoration still pending; fixture undo journal retained' }
        & $fixture restore $instance $container
        if($LASTEXITCODE) { throw 'Native fixture restoration failed; durable undo journal retained' }
    }
    $disk=Find-OriginalDisk
    $mounted=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($mounted -and $mounted.DiskNumber -ne $disk.Number) { throw 'D: belongs to another disk' }
    if(-not $mounted) { mountvol.exe D:\ $volumePath; if($LASTEXITCODE) { throw 'Original mount restoration failed' } }
    if((Get-Partition -DriveLetter D).DiskNumber -ne $disk.Number -or (Get-Volume -DriveLetter D).HealthStatus -ne 'Healthy') {
        throw 'Original disk/volume restoration verification failed'
    }
    'Fixture cleanup: original Windows driver, exact capacity and Healthy D: restored.'
}
if($failure) { throw $failure }
'Driverless hardware test passed.'
