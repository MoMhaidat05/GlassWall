# Derive Windows sizes from the supplied artwork, preserving its original ICO entry.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = Join-Path $root 'windows/assets/GlassWall.original.ico'
$original = [IO.File]::ReadAllBytes($source)
if ([BitConverter]::ToUInt16($original,2) -ne 1 -or [BitConverter]::ToUInt16($original,4) -ne 1 -or $original[6] -ne 32 -or $original[7] -ne 32) { throw 'Expected the supplied single 32x32 ICO.' }
$sourceIcon = [Drawing.Icon]::new($source)
$artwork = $sourceIcon.ToBitmap()
$frames = @()
try {
    $artwork.Save((Join-Path $root 'windows/assets/GlassWall.png'),[Drawing.Imaging.ImageFormat]::Png)
    foreach ($size in @(16,24,32,48,64,128,256)) {
        if ($size -eq 32) {
            $length = [BitConverter]::ToUInt32($original,14)
            $start = [BitConverter]::ToUInt32($original,18)
            $frames += [pscustomobject]@{size=$size;bytes=[byte[]]$original[$start..($start+$length-1)]}
            continue
        }
        $bitmap = [Drawing.Bitmap]::new($size,$size)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $stream = [IO.MemoryStream]::new()
        try {
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.CompositingMode = 'SourceCopy'
            $graphics.CompositingQuality = 'HighQuality'
            $graphics.InterpolationMode = 'HighQualityBicubic'
            $graphics.PixelOffsetMode = 'HighQuality'
            $attributes = [Drawing.Imaging.ImageAttributes]::new()
            try {
                $attributes.SetWrapMode([Drawing.Drawing2D.WrapMode]::TileFlipXY)
                $graphics.DrawImage($artwork,[Drawing.Rectangle]::new(0,0,$size,$size),0,0,32,32,[Drawing.GraphicsUnit]::Pixel,$attributes)
            } finally { $attributes.Dispose() }
            $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
            $frames += [pscustomobject]@{size=$size;bytes=$stream.ToArray()}
        } finally { $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $artwork.Dispose(); $sourceIcon.Dispose() }
$writer = [IO.BinaryWriter]::new([IO.File]::Create((Join-Path $root 'windows/assets/GlassWall.ico')))
try {
    $writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]$frames.Count)
    $offset = 6 + 16*$frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.size -eq 256) {0} else {$frame.size}
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([UInt16]1); $writer.Write([UInt16]32)
        $writer.Write([UInt32]$frame.bytes.Length); $writer.Write([UInt32]$offset)
        $offset += $frame.bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.bytes) }
} finally { $writer.Dispose() }
