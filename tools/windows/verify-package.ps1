# Audit normal and delay-loaded PE dependencies without starting any application.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$ReportPath)
$ErrorActionPreference = 'Stop'
$Directory = [IO.Path]::GetFullPath($Directory)
$files = @(Get-ChildItem -LiteralPath $Directory -Recurse -File)
$bundled = @{}
foreach ($file in $files) { $bundled[$file.Name.ToLowerInvariant()] = $file.FullName }
$results = @()
foreach ($file in $files | Where-Object Extension -in @('.exe','.dll')) {
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    $pe = [BitConverter]::ToInt32($bytes,60)
    if ([BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550) { throw "Invalid PE: $($file.Name)" }
    $machine = [BitConverter]::ToUInt16($bytes,$pe+4)
    if ($machine -ne 0x8664) { throw "Non-x64 payload binary: $($file.Name)" }
    $sectionCount = [BitConverter]::ToUInt16($bytes,$pe+6)
    $optionalLength = [BitConverter]::ToUInt16($bytes,$pe+20)
    $optional = $pe+24
    if ([BitConverter]::ToUInt16($bytes,$optional) -ne 0x20b) { throw 'Expected PE32+.' }
    $sections = @()
    for ($i=0;$i -lt $sectionCount;$i++) {
        $offset = $optional+$optionalLength+40*$i
        $sections += [pscustomobject]@{rva=[BitConverter]::ToUInt32($bytes,$offset+12);size=[Math]::Max([BitConverter]::ToUInt32($bytes,$offset+8),[BitConverter]::ToUInt32($bytes,$offset+16));raw=[BitConverter]::ToUInt32($bytes,$offset+20)}
    }
    function RvaOffset([UInt32]$rva) {
        foreach ($section in $sections) { if ($rva -ge $section.rva -and $rva -lt $section.rva+$section.size) { return [int]($section.raw+$rva-$section.rva) } }
        throw "Invalid import address in $($file.Name)"
    }
    $imports = @()
    foreach ($table in @(@{index=1;stride=20;name=12},@{index=13;stride=32;name=4})) {
        $rva = [BitConverter]::ToUInt32($bytes,$optional+112+8*$table.index)
        if (-not $rva) { continue }
        $offset = RvaOffset $rva
        while ($true) {
            $nameRva = [BitConverter]::ToUInt32($bytes,$offset+$table.name)
            if (-not $nameRva) { break }
            $nameOffset = RvaOffset $nameRva
            $end = $nameOffset
            while ($bytes[$end]) { $end++ }
            $imports += [Text.Encoding]::ASCII.GetString($bytes,$nameOffset,$end-$nameOffset).ToLowerInvariant()
            $offset += $table.stride
        }
    }
    $resolutions = @()
    foreach ($dependency in $imports | Sort-Object -Unique) {
        if ($bundled.ContainsKey($dependency)) { $resolution='bundled' }
        elseif ($dependency -match '^(api-ms-|ext-ms-)') { $resolution='Windows API set' }
        elseif ($dependency -match '^(msvcp|vcruntime|concrt|vccorlib)') { throw "Missing app-local CRT $dependency, required by $($file.Name)" }
        elseif (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32/$dependency")) { $resolution='Windows system (host)' }
        else { throw "Unresolved dependency $dependency, required by $($file.Name)" }
        $resolutions += [ordered]@{name=$dependency;resolution=$resolution}
    }
    $subsystem = [BitConverter]::ToUInt16($bytes,$optional+68)
    if ($file.Name -in @('GlassWall.Engine.exe','GlassWall.exe','GlassWall.Setup.exe') -and $subsystem -ne 2) { throw "Expected windowless GUI subsystem: $($file.Name)" }
    $results += [ordered]@{binary=$file.Name;machine='x64';subsystem=$subsystem;imports=$resolutions}
}
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
Write-Output "Checked $($results.Count) x64 binaries, GUI subsystem and ordinary/delay-loaded dependencies. No application was started."
