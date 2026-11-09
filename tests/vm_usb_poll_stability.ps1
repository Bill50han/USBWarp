param([int]$SoakSeconds=600,[int]$CaptureCycles=20,[int]$RestartCycles=3,[int]$CrashCycles=3,
      [ValidateRange(1,3600)][int]$RecoveryTimeoutSeconds=120,[switch]$FunctionsOnly)
$ErrorActionPreference='Stop'
$root='C:\USBWarp-Test'
$lab=Join-Path $root 'PollingStability'
$controller=Join-Path $root 'bin\USBWarpController.exe'
$serial='FC131A137FFE8'
$capacity=62914560000
$busid='a5a8ea84'
$instance='USB\VID_346D&PID_5678\FC131A137FFE8'
$results=Join-Path $lab 'events.jsonl'
$active=$null
$fingerprint=$null
$volume=$null
if($SoakSeconds -lt 1 -or $SoakSeconds -gt 86400 -or $CaptureCycles -lt 1 -or $CaptureCycles -gt 1000 -or $RestartCycles -lt 0 -or $RestartCycles -gt 100 -or $CrashCycles -lt 0 -or $CrashCycles -gt 20){throw 'Invalid bounded test duration/count'}

function Record([string]$Event,$Data) {
    $row=[ordered]@{Event=$Event;Utc=[DateTime]::UtcNow.ToString('o');Data=$Data}
    $json=$row | ConvertTo-Json -Depth 12 -Compress
    $json | Add-Content -LiteralPath $results -Encoding UTF8
    $json
}
function Native([string]$Exe,[string[]]$Arguments) {
    $previous=$ErrorActionPreference
    try {$ErrorActionPreference='Continue'; $output=& $Exe @Arguments 2>&1; $code=$LASTEXITCODE}
    finally {$ErrorActionPreference=$previous}
    if($code){throw "$Exe failed ($code): $($output -join ' ')"}
    return @($output | ForEach-Object { [string]$_ })
}
function Target {
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        $found=@(Get-Disk | Where-Object {$_.BusType -eq 'USB' -and $_.SerialNumber -eq $serial})
        if($found.Count -gt 1){throw 'Ambiguous authorized disk'}
        if($found.Count -eq 1) {
            if($found[0].IsBoot -or $found[0].IsSystem -or $found[0].Number -eq 0 -or $found[0].Size -ne $capacity){throw 'Disk identity/capacity guard failed'}
            return $found[0]
        }
        Start-Sleep -Milliseconds 100
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Authorized USB disk did not return to Windows'
}
function Wait-Session([int]$TimeoutSeconds=45) {
    $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {$state=(Native $controller @('status')) -join "`n"} catch {$state=''}
        if($state -match 'Session:\s+RUNNING' -and $state -match 'Bound devices:\s+0'){return $state}
        Start-Sleep -Milliseconds 200
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Clean running session did not appear'
}
function Restore-Disk {
    $disk=Target
    $part=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($part -and $part.DiskNumber -ne $disk.Number){throw 'D: belongs to another disk'}
    if(!$part){$null=Native mountvol.exe @('D:\',$volume)}
    if((Get-Partition -DriveLetter D).DiskNumber -ne $disk.Number -or (Get-Volume -DriveLetter D).HealthStatus -ne 'Healthy'){throw 'D: restoration failed'}
    if((Get-Item 'HKLM:\SOFTWARE\UsbWarp\Service\DeviceCapture').GetValueNames().Count){throw 'Capture journal retained an entry'}
    $service=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data
    if($service -ne 'USBSTOR'){throw "Unexpected restored function driver: $service"}
}
function Capture {
    $disk=Target
    $part=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($part) {
        if($part.DiskNumber -ne $disk.Number){throw 'Wrong disk mounted at D:'}
        $null=Native mountvol.exe @('D:\','/p')
    }
    $null=Native $controller @('bind',$busid)
    $service=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data
    if($service -ne 'UsbWarpFilter'){throw "Unexpected capture driver: $service"}
    if(@(Get-Disk | Where-Object {$_.BusType -eq 'USB' -and $_.SerialNumber -eq $serial}).Count){throw 'Native Windows disk still present during capture'}
}
function Release {
    $null=Native $controller @('unbind',$busid)
    Restore-Disk
}
function Guest([string]$Mode,[string]$Label,[bool]$WithFingerprint=$false,[int]$Seconds=600) {
    $arguments=@('-d','Ubuntu-22.04','-u','root','--exec','python3','/mnt/c/USBWarp-Test/PollingStability/usb_poll_stability.py','--mode',$Mode,'--label',$Label,'--seconds',[string]$Seconds)
    if($WithFingerprint){$arguments+=@('--fingerprint',$fingerprint)}
    $output=Native wsl.exe $arguments
    foreach($line in $output) {if($line.StartsWith('{')){Record ('guest_'+$Mode) ($line | ConvertFrom-Json)}}
}
function Resources([string]$Label) {
    $svc=Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'"
    $process=if($svc.ProcessId){Get-Process -Id $svc.ProcessId}else{$null}
    $memoryError=$null
    try {$memory=Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory}
    catch {$memory=$null; $memoryError=$_.Exception.Message}
    Record 'resources' ([ordered]@{Label=$Label;ProcessId=$svc.ProcessId;PrivateBytes=$process.PrivateMemorySize64;Handles=$process.HandleCount;Threads=$process.Threads.Count;PoolNonpagedBytes=$memory.PoolNonpagedBytes;PoolPagedBytes=$memory.PoolPagedBytes;AvailableBytes=$memory.AvailableBytes;MemoryCounterError=$memoryError})
}
function Start-Reader([string]$Label) {
    if($Label -notmatch '^[a-z0-9-]+$'){throw 'Invalid reader label'}
    $marker=Join-Path $lab ($Label+'-active.txt')
    Remove-Item -LiteralPath $marker -ErrorAction SilentlyContinue
    $arguments="-d Ubuntu-22.04 -u root --exec python3 /mnt/c/USBWarp-Test/PollingStability/usb_poll_stability.py --mode active --label $Label --marker /mnt/c/USBWarp-Test/PollingStability/$Label-active.txt"
    $reader=Start-Process wsl.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $lab ($Label+'.out')) -RedirectStandardError (Join-Path $lab ($Label+'.err'))
    $null=$reader.Handle
    $deadline=[DateTime]::UtcNow.AddSeconds(25)
    while(!(Test-Path -LiteralPath $marker)) {
        if($reader.HasExited -or [DateTime]::UtcNow -gt $deadline){throw 'Reader did not become active'}
        Start-Sleep -Milliseconds 100
    }
    return $reader
}
function Finish-Reader($Reader,[string]$Label) {
    if(!$Reader.WaitForExit(20000)){throw 'Reader remained blocked after removal/stop'}
    $Reader.WaitForExit()
    $errorText=Get-Content -LiteralPath (Join-Path $lab ($Label+'.err')) -Raw
    if($Reader.ExitCode -eq 0 -or $errorText -notmatch 'pread error=(5|6|19)\b'){throw "Unexpected fault-injection result: exit=$($Reader.ExitCode) $errorText"}
    Record 'reader_removed' @{Label=$Label;Exit=$Reader.ExitCode;Error=$errorText.Trim()}
}
function Stop-TestService {
    $null=Native sc.exe @('stop','UsbWarpService')
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    while((Get-Service UsbWarpService).Status -ne 'Stopped') {
        if([DateTime]::UtcNow -gt $deadline){throw 'Service stop timed out'}
        Start-Sleep -Milliseconds 100
    }
}

if($FunctionsOnly){return}

try {
    $disk=Target
    $part=Get-Partition -DriveLetter D
    if($part.DiskNumber -ne $disk.Number){throw 'D: is not the authorized target'}
    $volume=@($part.AccessPaths | Where-Object {$_ -like '\\?\Volume{*'})[0]
    if(!$volume){throw 'Original volume identity unavailable'}
    $part | Select-Object DiskNumber,PartitionNumber,DriveLetter,AccessPaths | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $lab 'original-volume.json')
    $null=Wait-Session
    Record 'recovery_policy' @{ScmQuery=(Native sc.exe @('qfailure','UsbWarpService')) -join "`n";WaitBudgetSeconds=$RecoveryTimeoutSeconds}
    $null=Native mountvol.exe @('D:\','/p')
    $nativeFingerprint=Native (Join-Path $root 'benchmark\usb_read_bench.exe') @(('\\.\PhysicalDrive'+$disk.Number),'poll-stability-windows',$serial,[string]$capacity,'fingerprint')
    $reference=($nativeFingerprint -join "`n") | ConvertFrom-Json
    $fingerprint=$reference.hash
    if($fingerprint -notmatch '^[0-9a-f]{16}$'){throw 'Invalid independent fingerprint'}
    Record 'windows_reference' $reference
    Restore-Disk
    Guest 'snapshot' 'initial'
    Resources 'initial'

    Capture
    $soakArgs="-d Ubuntu-22.04 -u root --exec python3 /mnt/c/USBWarp-Test/PollingStability/usb_poll_stability.py --mode soak --seconds $SoakSeconds --fingerprint $fingerprint"
    $active=Start-Process wsl.exe -ArgumentList $soakArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $lab 'soak.jsonl') -RedirectStandardError (Join-Path $lab 'soak.err')
    $null=$active.Handle
    $deadline=[DateTime]::UtcNow.AddSeconds($SoakSeconds+90)
    $sample=0
    while(!$active.WaitForExit(1000)) {
        if([DateTime]::UtcNow -gt $deadline){throw 'Soak watchdog expired'}
        if(++$sample % 30 -eq 0){Resources ('soak-'+$sample)}
    }
    $active.WaitForExit()
    if($active.ExitCode){throw "Soak failed: $(Get-Content (Join-Path $lab 'soak.err') -Raw)"}
    $active=$null
    Record 'soak_passed' ((Get-Content (Join-Path $lab 'soak.jsonl') -Tail 1) | ConvertFrom-Json)
    Guest 'idle' 'idle-resume' $true 30
    Release
    Guest 'snapshot' 'after-soak'
    Resources 'after-soak'

    for($i=1;$i -le $CaptureCycles;$i++) {
        $watch=[Diagnostics.Stopwatch]::StartNew()
        Capture
        Guest 'verify' ('capture-'+$i) ($i % 5 -eq 0)
        if($i % 5 -eq 0) {
            $label='unbind-'+$i
            $active=Start-Reader $label
            Release
            Finish-Reader $active $label
            $active=$null
        } else {Release}
        Guest 'snapshot' ('released-'+$i)
        $watch.Stop()
        Record 'capture_cycle' @{Cycle=$i;ElapsedMs=$watch.Elapsed.TotalMilliseconds;ActiveUnbind=($i % 5 -eq 0)}
        if($i % 5 -eq 0){Start-Sleep -Seconds 2; Resources ('capture-'+$i)}
    }

    for($i=1;$i -le $RestartCycles;$i++) {
        Capture
        $label='restart-'+$i
        $active=Start-Reader $label
        $watch=[Diagnostics.Stopwatch]::StartNew()
        Stop-TestService
        $watch.Stop()
        Finish-Reader $active $label
        $active=$null
        Restore-Disk
        Start-Service UsbWarpService
        $null=Wait-Session
        Capture
        Guest 'verify' ($label+'-recovered') $true
        Release
        Guest 'snapshot' $label
        Record 'restart_cycle' @{Cycle=$i;StopMs=$watch.Elapsed.TotalMilliseconds}
        Resources $label
    }

    for($i=1;$i -le $CrashCycles;$i++) {
        Capture
        $label='crash-'+$i
        $active=Start-Reader $label
        $svc=Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'"
        $pidBefore=[int]$svc.ProcessId
        $process=Get-Process -Id $pidBefore
        if($process.Path -ne (Join-Path $root 'bin\USBWarpService.exe')){throw 'Unexpected service process; refusing fault injection'}
        $watch=[Diagnostics.Stopwatch]::StartNew()
        Stop-Process -Id $pidBefore -Force
        # SCM backoff in this lab is 5/15/60 seconds. Startup waiting and
        # crash-recovery waiting have different bounds; keep the SCM policy.
        $null=Wait-Session $RecoveryTimeoutSeconds
        $watch.Stop()
        $newService=Get-CimInstance Win32_Service -Filter "Name='UsbWarpService'"
        if(!$newService.ProcessId -or $newService.ProcessId -eq $pidBefore){throw 'SCM recovery did not replace the failed process'}
        Finish-Reader $active $label
        $active=$null
        Restore-Disk
        Capture
        Guest 'verify' ($label+'-recovered') $true
        Release
        Guest 'snapshot' $label
        Record 'crash_cycle' @{Cycle=$i;OldProcessId=$pidBefore;NewProcessId=$newService.ProcessId;RecoveryMs=$watch.Elapsed.TotalMilliseconds}
        Resources $label
    }
    Start-Sleep -Seconds 3
    Guest 'snapshot' 'final'
    Resources 'final'
    Record 'complete' @{CaptureCycles=$CaptureCycles;RestartCycles=$RestartCycles;CrashCycles=$CrashCycles;SoakRequestedSeconds=$SoakSeconds;ReadOnly=$true}
} finally {
    if($active -and !$active.HasExited){$active.Kill(); $null=$active.WaitForExit(10000)}
    try {$null=Native $controller @('unbind',$busid)} catch {}
    if($volume){Restore-Disk}
}
