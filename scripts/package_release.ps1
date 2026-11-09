# SPDX-License-Identifier: GPL-2.0-only
# Package an already-built, clean source revision. Never exports a private key.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9A-Fa-f]{40}$')][string]$CertificateThumbprint,
    [Parameter(Mandatory=$true)][string]$SignTool,
    [Parameter(Mandatory=$true)][string]$Inf2Cat,
    [string]$LinuxModule,
    [string]$LinuxBuildInfo,
    [ValidatePattern('^[A-Za-z0-9._+-]+$')][string]$LinuxKernelRelease='6.6.87.2-microsoft-standard-WSL2'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Native([string]$exe,[string[]]$arguments) {
    & $exe @arguments
    if($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}
function Utf8([string]$path,[string]$value) {
    [IO.File]::WriteAllText($path,$value,[Text.UTF8Encoding]::new($false))
}
function CopyRequired([string]$relative,[string]$destination) {
    $source=Join-Path $repo $relative
    if(-not(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing build output: $relative" }
    Copy-Item -LiteralPath $source -Destination $destination
}
function Sign([string]$path) {
    Native $SignTool @('sign','/fd','SHA256','/s','My','/sha1',$CertificateThumbprint,$path)
    $signature=Get-AuthenticodeSignature -LiteralPath $path
    if(-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $CertificateThumbprint) {
        throw "Signing certificate mismatch: $path"
    }
}
function Manifest([string]$directory,[string]$kind) {
    $files=@(Get-ChildItem -LiteralPath $directory -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path=$_.FullName.Substring($directory.Length+1).Replace('\','/')
            size=$_.Length
            sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    $data=[ordered]@{
        product='USBWarp'; version=$version; sourceCommit=$revision; package=$kind
        configuration='Release|x64'; createdUtc=[DateTime]::UtcNow.ToString('o')
        signing='Self-signed test certificate; not WHQL certified'
        certificateThumbprint=$CertificateThumbprint.ToUpperInvariant()
        certificateExpiresUtc=$cert.NotAfter.ToUniversalTime().ToString('o')
        files=$files
    }
    Utf8 (Join-Path $directory 'manifest.json') ($data | ConvertTo-Json -Depth 6)
}
Push-Location $repo
try {
    $status=@(& git status --porcelain --untracked-files=normal)
    if($LASTEXITCODE -ne 0 -or $status.Count) { throw 'Commit the reviewed source before packaging; checkout must be clean.' }
    $revision=(& git rev-parse HEAD).Trim()
    if($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit' }
    $match=[regex]::Match((Get-Content include/usbwarp_version.h -Raw),'USBWARP_VERSION_STRING\s+"([0-9]+\.[0-9]+\.[0-9]+-alpha)"')
    if(-not $match.Success) { throw 'Expected an explicit alpha product version' }
    $version=$match.Groups[1].Value
    $cert=Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
    if(-not $cert.HasPrivateKey -or $cert.NotAfter -le (Get-Date) -or $cert.NotBefore -gt (Get-Date)) {
        throw 'A currently valid code-signing certificate with a private key is required'
    }
    if($cert.Subject -ne $cert.Issuer) { throw 'This alpha packager expects a self-signed test certificate' }
    foreach($tool in @($SignTool,$Inf2Cat)) {
        if(-not(Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Tool missing: $tool" }
    }
    if([bool]$LinuxModule -ne [bool]$LinuxBuildInfo) { throw 'Provide both LinuxModule and LinuxBuildInfo, or neither' }
    $output=Join-Path $repo "x64\ReleaseBundles\$version"
    if(Test-Path -LiteralPath $output) { throw "Output already exists; retain or move it before packaging again: $output" }
    $stage=Join-Path $output 'stage'
    $win=Join-Path $stage "USBWarp-$version-windows-x64"
    $symbols=Join-Path $stage "USBWarp-$version-symbols-x64"
    $capture=Join-Path $win 'capture'
    New-Item -ItemType Directory -Path $capture,$symbols -Force | Out-Null
    CopyRequired 'x64/Release/USBWarp.sys' (Join-Path $win 'USBWarp.sys')
    CopyRequired 'x64/Release/USBWarpService.exe' $win
    CopyRequired 'x64/Release/USBWarpController.exe' $win
    CopyRequired 'x64/Release/UsbWarpFilter/UsbWarpFilter.inf' $capture
    CopyRequired 'x64/Release/UsbWarpFilter/UsbWarpFilter.sys' $capture
    CopyRequired 'scripts/install_capture.ps1' $win
    CopyRequired 'docs/release-install.txt' (Join-Path $win 'INSTALL.txt')
    CopyRequired "docs/release-$version.txt" (Join-Path $win 'RELEASE-NOTES.txt')
    CopyRequired 'LICENSE' $win
    foreach($project in @('USBWarp','USBWarpService','USBWarpController')) {
        CopyRequired "x64/Release/$project.pdb" $symbols
    }
    CopyRequired 'x64/Release/UsbWarpFilter.pdb' $symbols
    CopyRequired 'LICENSE' $symbols
    Sign (Join-Path $win 'USBWarp.sys')
    Sign (Join-Path $capture 'UsbWarpFilter.sys')
    # The catalog must hash the final signed SYS, not an unsigned build artifact.
    Native $Inf2Cat @("/driver:$capture",'/os:10_GE_X64','/uselocaltime')
    Sign (Join-Path $capture 'usbwarpfilter.cat')
    [IO.File]::WriteAllBytes((Join-Path $win 'USBWarp-Test.cer'),$cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    Manifest $win 'windows-x64'
    Manifest $symbols 'symbols-x64'
    foreach($directory in @($win,$symbols)) {
        Compress-Archive -LiteralPath $directory -DestinationPath (Join-Path $output ((Split-Path $directory -Leaf)+'.zip')) -CompressionLevel Optimal
    }
    $sourceZip=Join-Path $output "USBWarp-$version-source.zip"
    Native git @('archive','--format=zip',"--prefix=USBWarp-$version/","--output=$sourceZip",$revision)
    if($LinuxModule) {
        $linux=Join-Path $stage "USBWarp-$version-linux-$LinuxKernelRelease-x86_64"
        New-Item -ItemType Directory -Path $linux | Out-Null
        Copy-Item -LiteralPath $LinuxModule -Destination (Join-Path $linux 'usbwarp.ko')
        Copy-Item -LiteralPath $LinuxBuildInfo -Destination (Join-Path $linux 'build-info.txt')
        CopyRequired 'LICENSE' $linux
        Utf8 (Join-Path $linux 'README.txt') "Kernel-specific test module. Read build-info.txt and check uname -r and modinfo before use. Never force-load. Full corresponding source is USBWarp-$version-source.zip, commit $revision. Build with KBUILD_MODPOST_WARN=1 make against the matching prepared kernel tree."
        Manifest $linux 'linux-x86_64-kernel-specific'
        Compress-Archive -LiteralPath $linux -DestinationPath (Join-Path $output ((Split-Path $linux -Leaf)+'.zip')) -CompressionLevel Optimal
    }
    CopyRequired "docs/release-$version.txt" (Join-Path $output 'RELEASE-NOTES.txt')
    $hashes=@(Get-ChildItem -LiteralPath $output -File | Sort-Object Name | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$_.Name
    })
    Utf8 (Join-Path $output 'SHA256SUMS.txt') (($hashes -join "`n")+"`n")
    Write-Output "Prepared $version from $revision in $output. Validate the staged binaries before publication."
} finally { Pop-Location }
