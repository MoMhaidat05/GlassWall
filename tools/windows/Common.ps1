Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:GlassWallRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

function Assert-GlassWallWindows {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) {
        throw 'Use a 64-bit PowerShell session on Windows 11. These scripts do not build on Linux.'
    }
    $os = Get-CimInstance Win32_OperatingSystem
    if ([int]$os.BuildNumber -lt 22000 -or $os.ProductType -ne 1) {
        throw 'GlassWall requires a Windows 11 client, not Windows Server.'
    }
}

function Get-GlassWallConfig {
    Get-Content (Join-Path $script:GlassWallRoot 'windows\toolchain-versions.json') -Raw | ConvertFrom-Json
}

function Get-GlassWallMSBuild {
    param([string]$MSBuildPath)
    if ($MSBuildPath) {
        if (-not (Test-Path $MSBuildPath -PathType Leaf)) { throw "MSBuild not found: $MSBuildPath" }
        return (Resolve-Path $MSBuildPath).Path
    }
    $installer = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $installer)) { throw 'Install Visual Studio with native C++ and MSBuild tools.' }
    $matches = @(& $installer -latest -products '*' -version '[17.14,)' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe')
    if ($LASTEXITCODE -ne 0 -or $matches.Count -eq 0) {
        throw 'No suitable Windows MSBuild/native C++ installation found. See README.md.'
    }
    return $matches[0]
}

function Get-GlassWallNuGet {
    param([string]$NuGetPath)
    if ($NuGetPath) {
        if (-not (Test-Path $NuGetPath -PathType Leaf)) { throw "NuGet not found: $NuGetPath" }
        return (Resolve-Path $NuGetPath).Path
    }
    $local = Join-Path $script:GlassWallRoot '.tools\nuget.exe'
    if (Test-Path $local) { return $local }
    $command = Get-Command nuget.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'nuget.exe is required for the manager. Put it in .tools or supply -NuGetPath. See README.md.'
}

function Invoke-GlassWallNative {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable exited with code $LASTEXITCODE." }
}

function Write-GlassWallUtf8 {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Get-GlassWallSourceIdentity {
    $identity = [ordered]@{ root = $script:GlassWallRoot; commit = $null; commitStatus = 'checkout has no .git metadata' }
    if (-not (Test-Path -LiteralPath (Join-Path $script:GlassWallRoot '.git'))) { return $identity }
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $gitCommand) { $identity.commitStatus = 'git.exe unavailable'; return $identity }
    $savedErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $commit = (& $gitCommand.Source -C $script:GlassWallRoot rev-parse --verify HEAD 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and $commit -match '^[0-9a-fA-F]{40,64}$') {
            $identity.commit = $commit; $identity.commitStatus = 'recorded; working tree may contain changes'
        } else { $identity.commitStatus = 'HEAD unavailable' }
    } finally { $ErrorActionPreference = $savedErrorPreference }
    return $identity
}
