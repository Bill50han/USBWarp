param(
    [string]$FixtureDirectory,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$repoPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputPath = Join-Path $repoPath 'x64\ReviewChecks\ServiceTests'
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $outputPath 'Logs') -Force | Out-Null
if (-not $FixtureDirectory) { $FixtureDirectory = $outputPath }
if ($FixtureDirectory -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { throw 'FixtureDirectory must be an absolute target path' }
$FixtureDirectory = [IO.Path]::GetFullPath($FixtureDirectory)
if (-not $BuildOnly -and $FixtureDirectory -ne $outputPath) { throw 'Use -BuildOnly when preparing tests for another machine' }
$escapedOutput = $FixtureDirectory.Replace('\', '\\').Replace('"', '\"')
$fixtureHeader = @"
#define USBWARP_SERVICE_NAME L"UsbWarpService.SelfTest"
#define USBWARP_SERVICE_REG_KEY L"SOFTWARE\\UsbWarp\\SelfTest"
#define USBWARP_SERVICE_DEPENDENCIES L""
#define USBWARP_PIPE_NAME "\\\\.\\pipe\\UsbWarpService.SelfTest"
#define USBWARP_TEST_DIRECTORY L"$escapedOutput"
#define USBWARP_LOG_DIRECTORY L"$escapedOutput\\Logs"
"@
[IO.File]::WriteAllText((Join-Path $outputPath 'fixture_config.h'), $fixtureHeader, [Text.UTF8Encoding]::new($false))
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ tools not found' }
$setup = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
Push-Location $repoPath
try {
    $commands = @(
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_config_test.cpp USBWarpService\config.cpp USBWarpService\target_value.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\config_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\vm_target_test.cpp USBWarpService\target_value.cpp USBWarpService\vm_target.cpp USBWarpService\hcs_discovery.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\vm_target_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\vm_controller_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\vm_controller_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_driver_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\driver_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_capture_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\capture_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE /I. tests\capture_pnp_selection_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\capture_selection_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE /I. tests\capture_port_reset.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\capture_reset.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE /I. tests\capture_driverless_fixture.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\capture_driverless_fixture.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_hdv_test.cpp USBWarpService\hdv_session.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\hdv_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_shm_test.cpp USBWarpService\shm_init.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\shm_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_status_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\status_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\controller_protocol_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\controller_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\pipe_security_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\pipe_security_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\pipe_timeout_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\pipe_timeout_test.exe /link advapi32.lib',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. tests\service_kernel_test.cpp USBWarpService\usbwarp_drv_client.cpp USBWarpService\shm_init.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\kernel_test.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. /FIx64\ReviewChecks\ServiceTests\fixture_config.h USBWarpService\main.cpp USBWarpService\config.cpp USBWarpService\target_value.cpp USBWarpService\vm_target.cpp USBWarpService\logging.cpp USBWarpService\winsvc.cpp USBWarpService\session.cpp USBWarpService\pipe_server.cpp tests\service_fixture_backend.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\service_fixture.exe',
        'cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I. /FIx64\ReviewChecks\ServiceTests\fixture_config.h tests\service_scm_test.cpp /Fox64\ReviewChecks\ServiceTests\ /Fex64\ReviewChecks\ServiceTests\scm_test.exe'
    )
    foreach ($command in $commands) {
        & $env:ComSpec /d /c ('"' + $setup + '" >nul && ' + $command)
        if ($LASTEXITCODE -ne 0) { throw "Test build failed: $command" }
    }
    if ($BuildOnly) { Write-Output "Built tests for target directory: $FixtureDirectory"; return }
    & (Join-Path $outputPath 'config_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Configuration regression failed' }
    & (Join-Path $outputPath 'vm_target_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'VM discovery/selection regression failed' }
    & (Join-Path $outputPath 'vm_controller_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'VM controller protocol regression failed' }
    & (Join-Path $outputPath 'driver_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Driver client regression failed' }
    & (Join-Path $outputPath 'capture_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Capture recovery regression failed' }
    & (Join-Path $outputPath 'capture_selection_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Capture SetupAPI model selection regression failed' }
    & (Join-Path $outputPath 'hdv_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'HDV lifecycle regression failed' }
    & (Join-Path $outputPath 'shm_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'SHM ownership/layout regression failed' }
    & (Join-Path $outputPath 'status_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'SCM status regression failed' }
    & (Join-Path $outputPath 'controller_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Controller protocol regression failed' }
    & (Join-Path $outputPath 'pipe_security_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Pipe security regression failed' }
    & (Join-Path $outputPath 'pipe_timeout_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Real pipe cancellation regression failed' }
    & (Join-Path $outputPath 'scm_test.exe') --console
    if ($LASTEXITCODE -ne 0) { throw 'Console/IPC/session regression failed' }
    Write-Output "SCM fixture built: $outputPath\scm_test.exe"
    Write-Output 'Run scm_test.exe from an elevated shell to test the private service. It never opens a real driver or VM.'
    Write-Output 'kernel_test.exe requires the real control driver in a test VM, with UsbWarpService stopped. It never binds USB devices.'
} finally { Pop-Location }
