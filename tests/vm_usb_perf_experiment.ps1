param(
    [string[]]$Scenarios=@('baseline'),
    [string[]]$Cases=@('prefix64m_1m_q1','prefix64m_1m_q4','rand_4k_q1'),
    [string]$Label='baseline',
    [switch]$Trace
)
$ErrorActionPreference='Stop'
$root='C:\USBWarp-Test'; $controller=Join-Path $root 'bin\USBWarpController.exe'
$parts=@(Get-Content -Raw (Join-Path $root 'backup\usb-partitions.json') | ConvertFrom-Json)
$volume=@($parts[0].AccessPaths | Where-Object {$_ -like '\\?\Volume{*'})[0]
if(-not $volume) { throw 'Missing original volume identity' }
function Find-Disk {
    $deadline=[DateTime]::UtcNow.AddSeconds(60)
    do {
        $d=@(Get-Disk | Where-Object {$_.BusType -eq 'USB' -and $_.SerialNumber -eq 'FC131A137FFE8'})
        if($d.Count -gt 1 -or ($d.Count -eq 1 -and ($d[0].IsBoot -or $d[0].IsSystem))) { throw 'Invalid target identity' }
        if($d.Count -eq 1 -and $d[0].Size -eq 62914560000) { return $d[0] }
        Start-Sleep -Milliseconds 200
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Target capacity unavailable'
}
$failure=$null
try {
    Start-Service UsbWarpService
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    do {
        try {$state=(& $controller status 2>&1) -join "`n"} catch {$state=''}
        if($state -match 'Session:\s+RUNNING') {break}
        Start-Sleep -Milliseconds 300
    } while([DateTime]::UtcNow -lt $deadline)
    if($state -notmatch 'Session:\s+RUNNING') {throw 'Session not ready'}
    $disk=Find-Disk; $atD=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($atD) {
        if($atD.DiskNumber -ne $disk.Number) {throw 'D: belongs to another disk'}
        mountvol.exe D:\ /p; if($LASTEXITCODE) {throw 'Dismount failed'}
    }
    & $controller bind a5a8ea84
    if($LASTEXITCODE) {throw 'Capture failed'}
    $sequence=0
    foreach($scenario in $Scenarios) {
        ++$sequence
        foreach($case in $Cases) {
            $name=$Label+'_s'+$sequence+'_'+$scenario+'_'+$case
            $params=@('-d','Ubuntu-22.04','--exec','python3','/mnt/c/USBWarp-Test/analysis/usb_perf_experiment.py','--label',$name,'--scenario',$scenario,'--case',$case)
            if($Trace) {$params+='--trace'}
            $savedPreference=$ErrorActionPreference
            try { $ErrorActionPreference='Continue'; $output=& wsl.exe @params 2>&1; $code=$LASTEXITCODE }
            finally { $ErrorActionPreference=$savedPreference }
            if($code) { $output | ForEach-Object {Write-Output ([string]$_)}; throw "Experiment failed: $name" }
            $output | Add-Content -LiteralPath (Join-Path $root 'analysis\results.jsonl')
            $output
        }
    }
    & wsl.exe -d Ubuntu-22.04 --exec python3 /mnt/c/USBWarp-Test/benchmark/usb_bench_linux.py --label integrity --verify
    if($LASTEXITCODE) {throw 'Read-only integrity check failed'}
} catch {$failure=$_}
finally {
    try {& $controller unbind a5a8ea84 | Out-Host} catch {}
    Stop-Service UsbWarpService
    $disk=Find-Disk; $atD=Get-Partition -DriveLetter D -ErrorAction SilentlyContinue
    if($atD -and $atD.DiskNumber -ne $disk.Number) {throw 'D: identity changed'}
    if(-not $atD) {mountvol.exe D:\ $volume; if($LASTEXITCODE) {throw 'Mount restoration failed'}}
    if((Get-Partition -DriveLetter D).DiskNumber -ne $disk.Number -or (Get-Volume -DriveLetter D).HealthStatus -ne 'Healthy') {throw 'Volume restoration failed'}
    'Original Windows disk, capacity and Healthy D: restored.'
}
if($failure) {throw $failure}
