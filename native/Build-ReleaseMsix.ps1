[CmdletBinding()]
param(
    [Parameter()]
    [string] $VcpkgPath,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string] $CertificateThumbprint,

    [Parameter()]
    [ValidateSet('CurrentUser', 'LocalMachine')]
    [string] $CertificateStoreLocation = 'CurrentUser',

    [Parameter()]
    [string] $OutputDirectory = (Join-Path $PSScriptRoot 'artifacts'),

    [Parameter()]
    [string] $MSBuildPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-MSBuildPath {
    param([string] $RequestedPath)

    if ($RequestedPath) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "MSBuild executable not found: $RequestedPath"
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer vswhere.exe was not found. Specify -MSBuildPath.'
    }

    $found = & $vswhere -latest -products '*' -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if ($LASTEXITCODE -ne 0 -or -not $found) {
        throw 'Could not locate MSBuild. Install Visual Studio with C++ and MSIX tools, or specify -MSBuildPath.'
    }
    return $found
}

function Get-VcpkgPath {
    param([string] $RequestedPath)

    if ($RequestedPath) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "vcpkg executable not found: $RequestedPath"
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $found = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($found) {
        return $found.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installPath = & $vswhere -latest -products '*' -property installationPath
        $bundled = Join-Path $installPath 'VC\vcpkg\vcpkg.exe'
        if (Test-Path -LiteralPath $bundled -PathType Leaf) {
            return $bundled
        }
    }
    throw 'Could not locate vcpkg. Install vcpkg or specify -VcpkgPath.'
}

$thumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
$storePath = if ($CertificateStoreLocation -eq 'LocalMachine') {
    "Cert:\LocalMachine\My\$thumbprint"
} else {
    "Cert:\CurrentUser\My\$thumbprint"
}
$certificate = Get-Item -LiteralPath $storePath -ErrorAction SilentlyContinue
if (-not $certificate) {
    throw "Signing certificate $thumbprint was not found in $CertificateStoreLocation\My. Install the Store signing certificate with its private key first."
}
if (-not $certificate.HasPrivateKey) {
    throw "Signing certificate $thumbprint does not have an accessible private key."
}
if ($certificate.NotAfter -le (Get-Date)) {
    throw "Signing certificate $thumbprint expired on $($certificate.NotAfter)."
}

$manifestPath = Join-Path $PSScriptRoot 'RoadTripMovie\Package.appxmanifest'
[xml] $manifest = Get-Content -LiteralPath $manifestPath
$publisher = $manifest.Package.Identity.Publisher
if ($certificate.Subject -notlike "*$publisher*") {
    throw "Certificate subject '$($certificate.Subject)' does not match package publisher '$publisher'. Set the manifest publisher to the Store identity and use its signing certificate."
}

$solution = Join-Path $PSScriptRoot 'RoadTripMovie.sln'
$msbuild = Get-MSBuildPath -RequestedPath $MSBuildPath
$vcpkg = Get-VcpkgPath -RequestedPath $VcpkgPath
Write-Host "Installing pinned FFmpeg shared libraries with $vcpkg"
Push-Location $PSScriptRoot
try {
    & $vcpkg install --triplet x64-windows
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg install failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$packageDirectory = $outputPath.TrimEnd('\') + '\'

$arguments = @(
    $solution
    '/restore'
    '/p:Configuration=Release'
    '/p:Platform=x64'
    '/p:AppxPackageSigningEnabled=true'
    "/p:PackageCertificateThumbprint=$thumbprint"
    "/p:PackageCertificateStoreLocation=$CertificateStoreLocation"
    '/p:PackageCertificateStoreName=My'
    "/p:AppxPackageDir=$packageDirectory"
    '/m'
    '/v:minimal'
)

Write-Host "Building and signing Release|x64 MSIX with $msbuild"
& $msbuild @arguments
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$bundle = Get-ChildItem -LiteralPath $outputPath -Filter '*.msixbundle' -File -Recurse |
    Where-Object Name -NotMatch '_Debug' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $bundle) {
    throw "Build completed but no Release .msixbundle was found under '$outputPath'."
}

$signature = Get-AuthenticodeSignature -LiteralPath $bundle.FullName
if (-not $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $thumbprint) {
    throw "The bundle '$($bundle.FullName)' is not signed with certificate $thumbprint."
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($bundle.FullName)
try {
    $innerPackage = $archive.Entries |
        Where-Object { $_.FullName -match '\.msix$' } |
        Select-Object -First 1
    if (-not $innerPackage) {
        throw 'The generated bundle does not contain an MSIX application package.'
    }

    $innerStream = $innerPackage.Open()
    try {
        $package = [System.IO.Compression.ZipArchive]::new(
            $innerStream, [System.IO.Compression.ZipArchiveMode]::Read, $false)
        try {
            $entries = @($package.Entries | ForEach-Object FullName)
            foreach ($library in 'avcodec', 'avfilter', 'avformat', 'avutil', 'swresample', 'swscale') {
                if (-not ($entries | Where-Object { $_ -match "(^|/)$library-[^/]+\.dll$" })) {
                    throw "The generated MSIX is missing the packaged $library DLL."
                }
            }
            if ($entries | Where-Object { $_ -match '(^|/)ffmpeg\.exe$' }) {
                throw 'The generated MSIX unexpectedly contains ffmpeg.exe.'
            }
        }
        finally {
            $package.Dispose()
        }
    }
    finally {
        $innerStream.Dispose()
    }
}
finally {
    $archive.Dispose()
}

Write-Host "Signed Release bundle: $($bundle.FullName)"
Write-Host "Signing certificate: $($signature.SignerCertificate.Subject) [$thumbprint]"
