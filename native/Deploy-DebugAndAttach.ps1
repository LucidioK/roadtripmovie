<#
.SYNOPSIS
Commits the working tree, builds and deploys the Debug MSIX, launches the app,
and attaches Visual Studio's native debugger.

.EXAMPLE
.\Deploy-DebugAndAttach.ps1 -CommitMessage "Describe the changes"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string] $CommitMessage,

    [Parameter()]
    [ValidateRange(30, 600)]
    [int] $StartupTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Git {
    param([string[]] $Arguments)

    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

$nativeRoot = $PSScriptRoot
$solution = Join-Path $nativeRoot 'RoadTripMovie.sln'
$repositoryRoot = (& git -C $nativeRoot rev-parse --show-toplevel)
if ($LASTEXITCODE -ne 0 -or -not $repositoryRoot) {
    throw "Could not locate the Git repository from '$nativeRoot'."
}
$repositoryRoot = $repositoryRoot.Trim()

Push-Location $repositoryRoot
try {
    Invoke-Git -Arguments @('add', '--all', '--', '.')
    $stagedChanges = & git diff --cached --quiet
    if ($LASTEXITCODE -eq 0) {
        throw 'There are no changes to commit.'
    }
    if ($LASTEXITCODE -ne 1) {
        throw "Could not inspect staged changes (git exit code $LASTEXITCODE)."
    }
    Invoke-Git -Arguments @(
        'commit',
        '-m',
        $CommitMessage,
        '-m',
        'Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>'
    )
}
finally {
    Pop-Location
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$installationPath = & $vswhere -latest -products '*' -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installationPath) {
    throw 'Could not locate a Visual Studio installation.'
}
$installationVersion = & $vswhere -latest -products '*' -property installationVersion
if ($LASTEXITCODE -ne 0 -or -not $installationVersion) {
    throw 'Could not determine the Visual Studio version.'
}
$visualStudioMajorVersion = ($installationVersion -split '\.')[0]
$msbuild = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
    throw "MSBuild was not found at '$msbuild'."
}

Write-Host 'Building Debug|x64 MSIX package...'
& $msbuild $solution `
    '/t:Build' `
    '/p:Configuration=Debug' `
    '/p:Platform=x64' `
    '/p:AppxPackage=true' `
    '/p:AppxBundle=Always' `
    '/m' `
    '/v:minimal'
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$deployScript = Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'artifacts') `
        -Filter 'Add-AppDevPackage.ps1' -File -Recurse |
    Where-Object { $_.DirectoryName -match '_Debug_Test$' } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $deployScript) {
    throw 'The Debug MSIX deployment script was not generated under native\artifacts.'
}

Write-Host "Deploying package from '$($deployScript.DirectoryName)'..."
& $deployScript.FullName -Force -SkipLoggingTelemetry
if ($LASTEXITCODE -ne 0) {
    throw "MSIX deployment failed with exit code $LASTEXITCODE."
}

$package = Get-AppxPackage -Name 'RoadTripMovie' |
    Sort-Object Version -Descending |
    Select-Object -First 1
if (-not $package) {
    throw 'RoadTripMovie was not found in the current user package registrations after deployment.'
}

$appId = "$($package.PackageFamilyName)!RoadTripMovie"
Write-Host "Starting packaged app '$appId'..."
Start-Process -FilePath "$env:WINDIR\explorer.exe" `
    -ArgumentList "shell:AppsFolder\$appId"

$deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
$appProcess = $null
do {
    Start-Sleep -Milliseconds 500
    $appProcess = Get-Process -Name 'RoadTripMovie' -ErrorAction SilentlyContinue |
        Select-Object -First 1
} while (-not $appProcess -and [DateTime]::UtcNow -lt $deadline)

if (-not $appProcess) {
    throw "RoadTripMovie did not start within $StartupTimeoutSeconds seconds."
}

$dteProgId = "VisualStudio.DTE.$visualStudioMajorVersion.0"
$dteType = [Type]::GetTypeFromProgID($dteProgId)
if (-not $dteType) {
    throw "The Visual Studio automation component '$dteProgId' is not registered."
}
$dte = [Activator]::CreateInstance($dteType)
$dte.UserControl = $true
$dte.MainWindow.Visible = $true
$dte.Solution.Open($solution)

$deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
$attached = $false
do {
    foreach ($debugProcess in $dte.Debugger.LocalProcesses) {
        if ($debugProcess.ProcessID -eq $appProcess.Id) {
            $debugProcess.Attach()
            $attached = $true
            break
        }
    }
    if (-not $attached) {
        Start-Sleep -Milliseconds 500
    }
} while (-not $attached -and [DateTime]::UtcNow -lt $deadline)

if (-not $attached) {
    throw "Visual Studio did not expose process $($appProcess.Id) for debugger attachment within $StartupTimeoutSeconds seconds."
}

Write-Host "Visual Studio is attached to RoadTripMovie.exe (PID $($appProcess.Id))."
