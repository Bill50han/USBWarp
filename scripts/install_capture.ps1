# Stages the signed, built capture package beside the installed service.
# It never selects a physical USB device or installs by VID/PID.
param(
    [Parameter(Mandatory=$true)][string]$ServiceDirectory,
    [string]$PackageDirectory
)
$ErrorActionPreference='Stop'
if(-not $PackageDirectory) {
    $PackageDirectory=Join-Path $PSScriptRoot 'capture'
    if(-not(Test-Path -LiteralPath (Join-Path $PackageDirectory 'UsbWarpFilter.inf'))) {
        $PackageDirectory=Join-Path $PSScriptRoot '..\x64\Release\UsbWarpFilter'
    }
}
$ServiceDirectory=[IO.Path]::GetFullPath($ServiceDirectory)
$PackageDirectory=[IO.Path]::GetFullPath($PackageDirectory)
if (-not (Test-Path -LiteralPath (Join-Path $ServiceDirectory 'USBWarpService.exe'))) {
    throw 'ServiceDirectory must contain the installed USBWarpService.exe'
}
$files=@('UsbWarpFilter.inf','UsbWarpFilter.sys','usbwarpfilter.cat')
foreach($file in $files) {
    if(-not(Test-Path -LiteralPath (Join-Path $PackageDirectory $file))) { throw "Missing built package file: $file" }
}
$service=Get-Service UsbWarpService -ErrorAction SilentlyContinue
if($service -and $service.Status -ne 'Stopped') {
    Stop-Service UsbWarpService
    $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(180))
}
foreach($key in @('HKLM:\SOFTWARE\UsbWarp\Service\StorageCapture','HKLM:\SOFTWARE\UsbWarp\Service\DeviceCapture')) {
    if(Test-Path -LiteralPath $key) {
        if((Get-Item -LiteralPath $key).GetValueNames().Count) {
            throw "Pending restoration in $key. Reconnect those devices and restore them with the installed service before upgrading."
        }
    }
}
$target=Join-Path $ServiceDirectory 'capture'
New-Item -ItemType Directory -Force -Path $target | Out-Null
if($target.TrimEnd([IO.Path]::DirectorySeparatorChar) -ne $PackageDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar)) {
    foreach($file in $files) { Copy-Item -LiteralPath (Join-Path $PackageDirectory $file) -Destination (Join-Path $target $file) }
}
& pnputil.exe /add-driver (Join-Path $target 'UsbWarpFilter.inf')
if($LASTEXITCODE -ne 0) { throw "Package staging failed: $LASTEXITCODE. Use a signed package trusted by this machine." }
# Upgrade from the historical class filter, preserving every other filter.
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\{36fc9e60-c465-11cf-8056-444553540000}'
$filters=@((Get-ItemProperty -LiteralPath $class -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters | Where-Object {$_})
if($filters -contains 'UsbWarpFilter') {
    $backup=Join-Path $ServiceDirectory 'capture-class-filter-backup.clixml'
    if(-not(Test-Path -LiteralPath $backup)) { $filters | Export-Clixml -LiteralPath $backup }
    $remaining=@($filters | Where-Object {$_ -ne 'UsbWarpFilter'})
    if($remaining.Count) { Set-ItemProperty -LiteralPath $class -Name UpperFilters -Value ([string[]]$remaining) }
    else { Remove-ItemProperty -LiteralPath $class -Name UpperFilters }
    Write-Output 'Capture package staged. Reboot Windows before starting the upgraded service to unload the old class filter.'
} else {
    Write-Output 'Capture package staged. Start the service, then explicitly bind a target device.'
}
