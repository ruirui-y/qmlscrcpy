# 生成 resources\app.ico：深色圆角底 + 蓝色手机 + 播放三角
#
# 为什么用脚本生成：图标是二进制资源，手画不出版本管理可读的差异；
# 改配色/形状只要改这里再跑一次。
#
# 为什么用 BMP/DIB 而不是 PNG 压缩：rc.exe 对 PNG 压缩的 ico 支持不稳，
# GDI+ 的 Icon 类也读不了，用最传统 32bpp DIB + AND 掩码最保险。
#
# 用法：powershell -ExecutionPolicy Bypass -File resources\make_icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$outPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'resources\app.ico'

# 每个尺寸单独画：小尺寸不能直接从大图缩，线条会糊
$sizes = @(16, 24, 32, 48, 64, 128, 256)

function New-IconBitmap([int]$size)
{
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = [float]$size

    # 背景：深色圆角方块
    $pad = $s * 0.03
    $radius = $s * 0.22
    $d = $radius * 2
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($pad, $pad, $d, $d, 180, 90)
    $path.AddArc($s - $pad - $d, $pad, $d, $d, 270, 90)
    $path.AddArc($s - $pad - $d, $s - $pad - $d, $d, $d, 0, 90)
    $path.AddArc($pad, $s - $pad - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $bgBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 18, 32, 47))
    $g.FillPath($bgBrush, $path)

    # 手机：圆角矩形描边
    $phoneW = $s * 0.40
    $phoneH = $s * 0.68
    $phoneX = ($s - $phoneW) / 2
    $phoneY = ($s - $phoneH) / 2
    $phoneR = $s * 0.10
    $pd = $phoneR * 2
    $phonePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $phonePath.AddArc($phoneX, $phoneY, $pd, $pd, 180, 90)
    $phonePath.AddArc($phoneX + $phoneW - $pd, $phoneY, $pd, $pd, 270, 90)
    $phonePath.AddArc($phoneX + $phoneW - $pd, $phoneY + $phoneH - $pd, $pd, $pd, 0, 90)
    $phonePath.AddArc($phoneX, $phoneY + $phoneH - $pd, $pd, $pd, 90, 90)
    $phonePath.CloseFigure()

    $stroke = [Math]::Max(1.0, $s * 0.055)
    $phonePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 76, 154, 255)), $stroke
    $g.DrawPath($phonePen, $phonePath)

    # 播放三角：屏幕中央
    $triH = $s * 0.22
    $triW = $s * 0.18
    $cx = $s / 2
    $cy = $s / 2
    $points = @(
        (New-Object System.Drawing.PointF (($cx - $triW / 2), ($cy - $triH / 2))),
        (New-Object System.Drawing.PointF (($cx - $triW / 2), ($cy + $triH / 2))),
        (New-Object System.Drawing.PointF (($cx + $triW / 2), $cy))
    )
    $triBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 127, 211, 255))
    $g.FillPolygon($triBrush, $points)

    $g.Dispose()
    return $bmp
}

# 把一张位图转成 ICO 里的 DIB 数据（BITMAPINFOHEADER + BGRA 倒序 + AND 掩码）
function ConvertTo-IconDib([System.Drawing.Bitmap]$bmp)
{
    $w = $bmp.Width
    $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $rgba = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $rgba, 0, $rgba.Length)
    $bmp.UnlockBits($data)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $ms

    # BITMAPINFOHEADER：高度写两倍（XOR + AND）
    $bw.Write([UInt32]40)
    $bw.Write([Int32]$w)
    $bw.Write([Int32]($h * 2))
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]32)
    $bw.Write([UInt32]0)
    $bw.Write([UInt32]($w * $h * 4))
    $bw.Write([Int32]0)
    $bw.Write([Int32]0)
    $bw.Write([UInt32]0)
    $bw.Write([UInt32]0)

    # XOR 位图：自下而上，BGRA
    for ($y = $h - 1; $y -ge 0; $y--)
    {
        $rowStart = $y * $stride
        for ($x = 0; $x -lt $w; $x++)
        {
            $i = $rowStart + ($x * 4)
            $bw.Write($rgba[$i + 0])
            $bw.Write($rgba[$i + 1])
            $bw.Write($rgba[$i + 2])
            $bw.Write($rgba[$i + 3])
        }
    }

    # AND 掩码：1 表示透明，每行按 4 字节对齐
    $maskStride = [Math]::Floor(($w + 31) / 32) * 4
    for ($y = $h - 1; $y -ge 0; $y--)
    {
        $row = New-Object byte[] $maskStride
        $rowStart = $y * $stride
        for ($x = 0; $x -lt $w; $x++)
        {
            $alpha = $rgba[$rowStart + ($x * 4) + 3]
            if ($alpha -lt 128)
            {
                $row[[Math]::Floor($x / 8)] = $row[[Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8))
            }
        }
        $bw.Write($row)
    }

    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose()
    $ms.Dispose()
    # 前面加逗号：PowerShell 会把 byte[] 展开成多个输出，包一层才是一个对象
    return ,$bytes
}

# 生成每层的 DIB
$layers = @()
foreach ($size in $sizes)
{
    $bmp = New-IconBitmap $size
    $dib = ConvertTo-IconDib $bmp
    $layers += [PSCustomObject]@{ Size = $size; Data = $dib }
    $bmp.Dispose()
}

$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $out

# ICONDIR：保留 0，类型 1（图标），数量
$writer.Write([UInt16]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]$layers.Count)

# 目录区先占位
$entryStart = $out.Position
foreach ($layer in $layers)
{
    $dim = if ($layer.Size -ge 256) { 0 } else { $layer.Size }
    $writer.Write([Byte]$dim)
    $writer.Write([Byte]$dim)
    $writer.Write([Byte]0)
    $writer.Write([Byte]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]$layer.Data.Length)
    $writer.Write([UInt32]0)
}

# 写数据并记录偏移
$offsets = @()
foreach ($layer in $layers)
{
    $offsets += [UInt32]$out.Position
    $writer.Write($layer.Data)
}

# 回填偏移
for ($i = 0; $i -lt $layers.Count; $i++)
{
    $out.Position = $entryStart + ($i * 16) + 12
    $writer.Write($offsets[$i])
}

$writer.Flush()
[System.IO.File]::WriteAllBytes($outPath, $out.ToArray())
$writer.Dispose()
$out.Dispose()

Write-Host ("生成完毕：{0}（{1} 字节，{2} 个尺寸，DIB 格式）" -f $outPath, (Get-Item $outPath).Length, $layers.Count)
