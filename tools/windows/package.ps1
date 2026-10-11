[CmdletBinding()]
param(
    [string]$MSBuildPath,
    [string]$NuGetPath,
    [string]$ISCCPath,
    [string]$CRTRedistDirectory,
    [string]$PlatformToolset = 'v145',
    [string]$Version = '1.0.0',
    [string]$ResumeBuildDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'Common.ps1')
if ($env:OS -ne 'Windows_NT') { throw 'Build this installer only on native Windows.' }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must be numeric major.minor.patch.' }
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$MSBuildPath = Get-GlassWallMSBuild -MSBuildPath $MSBuildPath
if (-not $ISCCPath) {
    $candidates = @((Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Inno Setup 6/ISCC.exe'),
        (Join-Path $root 'artifacts/tools/InnoSetup/ISCC.exe'))
    $ISCCPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $ISCCPath) { throw 'Install Inno Setup 6 or supply -ISCCPath.' }
}
if (-not $CRTRedistDirectory) {
    $vsRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $MSBuildPath) '../../..'))
    $crtName = switch ($PlatformToolset) { 'v145' { 'Microsoft.VC145.CRT' } 'v143' { 'Microsoft.VC143.CRT' } default { throw 'Supply -CRTRedistDirectory for this toolset.' } }
    $redist = @(Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC/Redist/MSVC') -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } | Sort-Object { [version]$_.Name } -Descending)
    $CRTRedistDirectory = $redist | ForEach-Object { Join-Path $_.FullName "x64/$crtName" } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $CRTRedistDirectory) { throw 'Matching x64 Release CRT not found; supply -CRTRedistDirectory.' }
}
foreach ($tool in @($MSBuildPath,$ISCCPath)) { if (-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" } }
if (-not (Test-Path -LiteralPath $CRTRedistDirectory)) { throw 'Supply the matching Visual C++ Release CRT redistribution directory.' }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$work = Join-Path $root "artifacts/installer/build-$stamp"
if ($ResumeBuildDirectory) {
    $candidate = [IO.Path]::GetFullPath($ResumeBuildDirectory)
    $boundary = [IO.Path]::GetFullPath((Join-Path $root 'artifacts/installer')) + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $candidate) -or (Split-Path $candidate -Leaf) -notlike 'build-*') {
        throw 'Resume directory must be an existing installer build folder beneath this workspace.'
    }
    $work = $candidate
}
$release = Join-Path $work 'Release'
$payload = Join-Path $work 'payload'
$logs = Join-Path $work 'logs'
$output = Join-Path $root 'artifacts/installer'
New-Item -ItemType Directory -Force $release,$payload,$logs | Out-Null
$nuget = Get-GlassWallNuGet -NuGetPath $NuGetPath
& $nuget restore (Join-Path $root 'windows/projects/GlassWall.Manager/packages.config') -PackagesDirectory (Join-Path $root 'windows/packages') -ConfigFile (Join-Path $root 'windows/nuget.config') -NonInteractive | Tee-Object -FilePath (Join-Path $logs 'nuget-restore.txt')
if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed. Diagnostics preserved in $logs" }
# Separate native projects retain independence from WinUI. Nothing is launched.
$projects = @('GlassWall.Engine','GlassWall.Probe','GlassWall.Setup','GlassWall.Manager')
foreach ($project in $projects) {
    $file = if ($project -eq 'GlassWall.Manager') { 'GlassWall.vcxproj' } else { "$project.vcxproj" }
    $projectPath = Join-Path $root "windows/projects/$project/$file"
    # Both NuGet projections and native object files belong to this fresh build.
    $arguments = @($projectPath,'/m','/t:Build','/p:Configuration=Release','/p:Platform=x64',
        "/p:PlatformToolset=$PlatformToolset",'/p:PreferredToolArchitecture=x64',
        "/p:OutDir=$release\", "/p:IntDir=$work\obj\$project\",
        "/p:GeneratedFilesDir=$work\generated\$project\",
        "/bl:$logs\$project-$stamp.binlog", "/flp:logfile=$logs\$project-$stamp.txt",'/v:minimal')
    & $MSBuildPath @arguments
    if ($LASTEXITCODE -ne 0) { throw "Release build failed for $project. Diagnostics preserved in $logs" }
}
$developmentOutputs = @('.pdb','.ilk','.lib','.exp','.map','.ipdb','.iobj')
foreach ($file in Get-ChildItem -LiteralPath $release -Recurse -File) {
    if ($file.Extension.ToLowerInvariant() -in $developmentOutputs) { continue }
    $relative = $file.FullName.Substring($release.Length + 1)
    $target = Join-Path $payload $relative
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
Get-ChildItem -LiteralPath $CRTRedistDirectory -Filter '*.dll' -File | Copy-Item -Destination $payload
foreach ($name in @('GlassWall.exe','GlassWall.Engine.exe','GlassWall.Setup.exe','GlassWall.Probe.exe',
    'GlassWall.pri','App.xbf','MainWindow.xbf','GlassWall.ico','Microsoft.UI.Xaml.dll',
    'Microsoft.WindowsAppRuntime.dll','resources.pri','vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')) {
    # Some modular WinUI SDKs place resources.pri inside Microsoft.UI.Xaml.
    if ($name -eq 'resources.pri') {
        if (-not (Get-ChildItem -LiteralPath $payload -Recurse -Filter '*.pri' | Where-Object Name -ne 'GlassWall.pri')) { throw 'Runtime PRI resources missing.' }
    } elseif (-not (Test-Path -LiteralPath (Join-Path $payload $name))) { throw "Incomplete runtime payload: $name" }
}
if (-not (Test-Path -LiteralPath (Join-Path $payload 'Assets/GlassWall.png'))) { throw 'Official title-bar icon missing.' }
& (Join-Path $PSScriptRoot 'verify-package.ps1') -Directory $payload -ReportPath (Join-Path $logs 'dependency-check.json')
$manifest = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object Name -ne 'payload-sha256.json' | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($payload.Length+1);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $payload 'payload-sha256.json') -Encoding UTF8
& $ISCCPath "/DPayloadDir=$payload" "/DOutputDir=$output" "/DAppVersion=$Version" (Join-Path $root 'windows/installer/GlassWall.iss') | Tee-Object -FilePath (Join-Path $logs "installer-compile-$stamp.txt")
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed. Diagnostics preserved in $logs" }
$installer = Join-Path $output "GlassWall-Setup-$Version-x64.exe"
$identity = Get-GlassWallSourceIdentity
$record = [ordered]@{
    version=$Version;builtUtc=[DateTime]::UtcNow.ToString('o');sourceIdentity=$identity;
    tools=[ordered]@{msbuild=$MSBuildPath;nuget=$nuget;iscc=$ISCCPath;toolset=$PlatformToolset;crt=$CRTRedistDirectory};
    installer=$installer;sha256=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash;
    payload=$payload;logs=$logs;compilation='passed';dependencyAudit='passed';
    manualWindowsAcceptance='pending';signed=$false
}
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'build-record.json') -Encoding UTF8
$record.sha256 + '  ' + (Split-Path $installer -Leaf) | Set-Content -LiteralPath "$installer.sha256" -Encoding ASCII
Write-Output "Installer ready: $installer"
Write-Output 'Compilation and dependency checks passed. Manual sign-in, UI, playback and resource acceptance remain separate.'
