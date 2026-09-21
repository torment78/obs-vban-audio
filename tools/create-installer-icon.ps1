# Convert the existing plugin artwork to Windows icon resolutions; no new artwork.
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
$source = [Drawing.Image]::FromFile((Join-Path $repoRoot 'data\vban-audio.png'))
$frames = @()
try {
    if ($source.Width -ne $source.Height) { throw 'The plugin icon must be square.' }
    foreach ($size in @(16,24,32,48,64,128,256)) {
        $bitmap = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $attributes = New-Object Drawing.Imaging.ImageAttributes
        $stream = New-Object IO.MemoryStream
        try {
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $attributes.SetWrapMode([Drawing.Drawing2D.WrapMode]::TileFlipXY)
            $rectangle = New-Object Drawing.Rectangle(0, 0, $size, $size)
            $graphics.DrawImage($source, $rectangle, 0, 0, $source.Width, $source.Height, [Drawing.GraphicsUnit]::Pixel, $attributes)
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            $frames += [PSCustomObject]@{ Size = $size; Bytes = $stream.ToArray() }
        } finally {
            $stream.Dispose()
            $attributes.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
} finally { $source.Dispose() }

$iconPath = Join-Path $repoRoot 'installer\vban-audio.ico'
$iconStream = [IO.File]::Create($iconPath)
$writer = New-Object IO.BinaryWriter($iconStream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
} finally { $writer.Dispose(); $iconStream.Dispose() }
Write-Output "Installer icon: $iconPath (16, 24, 32, 48, 64, 128 and 256 pixels)"
