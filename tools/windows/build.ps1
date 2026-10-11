[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateSet('Native','All')][string]$Scope = 'All',
    [string]$MSBuildPath,
    [string]$NuGetPath,
    [string]$PlatformToolset = 'v145'
)
. (Join-Path $PSScriptRoot 'Common.ps1')
Assert-GlassWallWindows
$msbuild = Get-GlassWallMSBuild -MSBuildPath $MSBuildPath
$logs = Join-Path $script:GlassWallRoot 'artifacts/logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$projects = @('GlassWall.Engine','GlassWall.Probe')
if ($Configuration -eq 'Release') { $projects += 'GlassWall.Setup' }
if ($Scope -eq 'All') {
    $nuget = Get-GlassWallNuGet -NuGetPath $NuGetPath
    Invoke-GlassWallNative $nuget @('restore', (Join-Path $script:GlassWallRoot 'windows/projects/GlassWall.Manager/packages.config'),
        '-PackagesDirectory', (Join-Path $script:GlassWallRoot 'windows/packages'),
        '-ConfigFile', (Join-Path $script:GlassWallRoot 'windows/nuget.config'), '-NonInteractive')
    $projects += 'GlassWall.Manager'
}
foreach ($name in $projects) {
    $file = if ($name -eq 'GlassWall.Manager') { 'GlassWall.vcxproj' } else { "$name.vcxproj" }
    $path = Join-Path $script:GlassWallRoot "windows/projects/$name/$file"
    Invoke-GlassWallNative $msbuild @($path, '/m', '/t:Build', "/p:Configuration=$Configuration", '/p:Platform=x64',
        "/p:PlatformToolset=$PlatformToolset", "/bl:$logs/$name-$Configuration.binlog")
}
Write-Output "Compilation passed: $($projects -join ', '). Runtime verification is separate."
