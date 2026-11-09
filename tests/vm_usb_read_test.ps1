param(
    [string]$Root='C:\USBWarp-Test',
    [string]$BusId='a5a8ea84',
    [string]$Model='ASUS adol U300',
    [string]$Serial='FC131A137FFE8',
    [string]$Vendor='346d',
    [string]$Product='5678',
    [int]$Rounds=3,
    [switch]$CrashRecovery,
    [switch]$ResetRecovery
)
$ErrorActionPreference='Stop'
$controller=Join-Path $Root 'bin\USBWarpController.exe'
$baseline=Get-Content -Raw -LiteralPath (Join-Path $Root 'backup\usb-read-baseline.json') | ConvertFrom-Json
$partition=@(Get-Content -Raw -LiteralPath (Join-Path $Root 'backup\usb-partitions.json') | ConvertFrom-Json)[0]
$volumePath=@($partition.AccessPaths | Where-Object { $_ -like '\\?\Volume{*' })[0]
if (-not $volumePath) { throw 'Original volume identity was not recorded' }
function Find-TestDisk {
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    do {
        $disks=@(Get-Disk | Where-Object { $_.BusType -eq 'USB' -and $_.FriendlyName -eq $Model -and $_.SerialNumber -eq $Serial })
        if ($disks.Count -gt 1 -or ($disks.Count -eq 1 -and ($disks[0].IsBoot -or $disks[0].IsSystem))) {
            throw 'Disk identity no longer matches the original non-system USB device'
        }
        if ($disks.Count -eq 1 -and $disks[0].Size -eq $baseline.Size) { return $disks[0] }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Original USB disk did not return with its expected capacity'
}
function Dismount-TestVolume {
    $disk=Find-TestDisk
    $mounted=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if ($mounted) {
        if ($mounted.DiskNumber -ne $disk.Number) { throw 'D: belongs to another disk' }
        mountvol.exe D:\ /p
        if ($LASTEXITCODE) { throw 'Target USB filesystem could not be dismounted' }
    }
}
function Wait-UsbSession {
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    do {
        try { $state=(& $controller status 2>&1) -join "`n" }
        catch { $state='' }
        if ($state -match 'Session:\s+RUNNING') { return $state }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'USB session did not recover'
}
function Read-TestDisk([int]$Count) {
    $instance="USB\VID_$Vendor&PID_$Product\$Serial"
    $service=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data
    if ($service -ne 'UsbWarpFilter') { throw "Target is still owned by Windows function driver: $service" }
    $stack=@((Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Stack).Data)
    if ($stack -notcontains '\Driver\UsbWarpFilter' -or $stack -contains '\Driver\USBSTOR') {
        throw "Unexpected target driver stack: $stack"
    }
    "Target driver stack: $($stack -join ' -> ')"
    $accessHelper=Join-Path $Root 'capture_reset.exe'
    if(Test-Path -LiteralPath $accessHelper) {
        $container=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ContainerId).Data
        & $accessHelper $instance $container --verify-access
        if($LASTEXITCODE) { throw 'Capture interface user-mode access check failed' }
    }
    $nativeDisks=@(Get-Disk | Where-Object { $_.BusType -eq 'USB' -and $_.SerialNumber -eq $Serial })
    if ($nativeDisks.Count) { throw 'Captured target still exposes a Windows disk' }
    "Capture stack verified: $instance -> UsbWarpFilter; native disk absent"
    $result=& wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/usb_read_test.py `
        --vid $Vendor --pid $Product --serial $Serial --sha256 $baseline.Sha256 --rounds $Count
    $code=$LASTEXITCODE
    $result | Add-Content -LiteralPath (Join-Path $Root 'hardware-validation.jsonl')
    $result
    if ($code) { throw 'Read-only hash comparison failed' }
}
$failure=$null
try {
    Dismount-TestVolume
    & $controller bind $BusId
    if ($LASTEXITCODE) { throw 'USB binding failed' }
    Start-Sleep -Seconds 3
    Read-TestDisk $Rounds
    if ($ResetRecovery) {
        $instance="USB\VID_$Vendor&PID_$Product\$Serial"
        $container=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ContainerId).Data
        & (Join-Path $Root 'capture_reset.exe') $instance $container
        if ($LASTEXITCODE) { throw 'Identity-checked reset failed' }
        $null=Find-TestDisk
        $state=(& $controller status) -join "`n"
        if ($state -notmatch 'Bound devices:\s+0') { throw 'Reset left a stale kernel binding' }
        'External reset recovery: binding removed; original Windows disk returned'
        Dismount-TestVolume
        & $controller bind $BusId
        if ($LASTEXITCODE) { throw 'Rebind after external reset failed' }
        Start-Sleep -Seconds 3
        Read-TestDisk 1
    }
    if ($CrashRecovery) {
        $service=Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'"
        $pidBefore=[int]$service.ProcessId
        $process=Get-Process -Id $pidBefore
        if ($process.Path -ne (Join-Path $Root 'bin\USBWarpService.exe')) { throw 'Unexpected service executable' }
        Stop-Process -Id $pidBefore -Force
        Start-Sleep -Seconds 6
        $state=Wait-UsbSession
        $service=Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'"
        if ($service.ProcessId -eq $pidBefore -or $state -notmatch 'Bound devices:\s+0') {
            throw 'Crash recovery reused the old process or retained a binding'
        }
        # Allow the orphaned guest controller to observe loss and retire.
        Start-Sleep -Seconds 10
        'SCM crash recovery: new process, clean kernel session'
        Dismount-TestVolume
        & $controller bind $BusId
        if ($LASTEXITCODE) { throw 'Rebind after crash failed' }
        Start-Sleep -Seconds 3
        Read-TestDisk 1
    }
} catch { $failure=$_ }
finally {
    $unbound=$false
    try { & $controller unbind $BusId; $unbound=$LASTEXITCODE -eq 0 } catch { $unbound=$false }
    if (-not $unbound) {
        Stop-Service UsbWarpService -ErrorAction Stop
        (Get-Service UsbWarpService).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(30))
    }
    # Restore the original Windows volume mount after generic driver restoration;
    # never replace a drive letter now owned by another disk.
    $disk=Find-TestDisk
    $mounted=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if ($mounted -and $mounted.DiskNumber -ne $disk.Number) { throw 'Refusing to replace another disk at D:' }
    if (-not $mounted) {
        mountvol.exe D:\ $volumePath
        if ($LASTEXITCODE) { throw 'Original USB mount point could not be restored' }
    }
    $mounted=Get-Partition -DriveLetter D -ErrorAction Stop
    if ($mounted.DiskNumber -ne $disk.Number) { throw 'Restored mount identity mismatch' }
    Get-Volume -DriveLetter D | Select-Object DriveLetter,FileSystemLabel,HealthStatus | Format-List
    if ((Get-Service UsbWarpService).Status -ne 'Running') { Start-Service UsbWarpService }
}
if ($failure) { throw $failure }
'Hardware test complete; USB unbound and original Windows volume restored.'
