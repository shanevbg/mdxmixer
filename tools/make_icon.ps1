# make_icon.ps1 — draws res/mdxmixer.ico: headphones with the Bluetooth rune.
#
# Checked in as a SCRIPT rather than only as the .ico it produces, so the
# icon can be re-cut at a different size or colour without anyone having to
# reverse-engineer a binary. Pure System.Drawing: no ImageMagick, no
# Inkscape, nothing to install.
#
# Each size is DRAWN rather than downscaled from one big render. At 16px a
# scaled-down rune turns into four grey smudges; redrawing lets the stroke
# widths stay legible proportions of the canvas.
#
#   pwsh -File tools/make_icon.ps1

Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'
$outDir = Join-Path $PSScriptRoot '..\res'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$outIco = Join-Path $outDir 'mdxmixer.ico'

function New-IconBitmap([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $u = $S / 256.0                      # one design unit
    function px([double]$v) { return [float]($v * $u) }

    # ── Three parts, three jobs, no two of them a similar blue ───────────
    #
    # The blue is confined to a BADGE behind the rune, which is the shape
    # people actually recognise -- a white Bluetooth rune on blue. The whole
    # icon used to be blue, so the tile, the muffs and the rune were three
    # shades of nearly the same thing and none of them said anything: "I just
    # didn't want the entire background to be a light shade of blue, the
    # earmuffs another subtly different shade and the bluetooth background
    # another subtly different shade."
    #
    # The headphones are one object and get one colour. Red against blue is
    # the largest separation available, and both survive a light taskbar and
    # a dark one, which a transparent icon has to do.
    $ink = [System.Drawing.Color]::FromArgb(255, 214, 46, 50)       # band + muffs
    $cupInk = $ink
    $badgeInk = [System.Drawing.Color]::FromArgb(255, 20, 110, 245) # the Bluetooth blue
    $runeInk = [System.Drawing.Color]::FromArgb(255, 255, 255, 255) # white on it

    # Below 32px the full headset does not fit in the pixels available.
    #
    # Measured, not assumed: at 16 the band is one pixel, each cup is two,
    # and the rune has six to work with -- they merge into a grey smudge.
    # The band survives as a single arc and the rune grows into the space
    # the cups were using, which keeps BOTH halves of the idea legible
    # instead of keeping all of neither.
    $compact = ($S -lt 32)

    # ── Headphones ───────────────────────────────────────────────────────
    #
    # The band is an arc and the cups are capsules hanging off its ends, so
    # the silhouette is a headset even when the rune inside it is too small
    # to resolve.
    $bandPen = New-Object System.Drawing.Pen($ink, (px $(if ($compact) { 22 } else { 19 })))
    $bandPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $bandPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    if ($compact) {
        # Wider and shallower: an arc across the top that still reads as a
        # headband when it is two pixels tall.
        $bandBox = New-Object System.Drawing.RectangleF((px 36), (px 40), (px 184), (px 132))
        $g.DrawArc($bandPen, $bandBox, 185, 170)
    } else {
        $bandBox = New-Object System.Drawing.RectangleF((px 48), (px 48), (px 160), (px 150))
        $g.DrawArc($bandPen, $bandBox, 180, 180)
    }
    $bandPen.Dispose()

    # No `if` wrapper and so NO braces: a bare { } block in PowerShell is a
    # scriptblock LITERAL, which goes to the output stream -- the function
    # then returns that alongside the bitmap and the caller gets a
    # ScriptBlock where it wanted an image.
    $cupBrush = New-Object System.Drawing.SolidBrush($cupInk)
    # Compact keeps the muffs -- they are the red, and dropping them would
    # leave the small sizes a different colour from the large ones. They just
    # become stubbier so they still read as two blocks rather than two smears.
    $cupW = px $(if ($compact) { 52 } else { 40 })
    $cupH = px $(if ($compact) { 66 } else { 74 })
    $cupY = px $(if ($compact) { 128 } else { 116 })
    $cupX = px $(if ($compact) { 20 } else { 38 })
    foreach ($cx in @($cupX, ($S - $cupX - $cupW))) {
        $cp = New-Object System.Drawing.Drawing2D.GraphicsPath
        $cd = $cupW
        $cp.AddArc($cx, $cupY, $cd, $cd, 180, 180)
        $cp.AddArc($cx, ($cupY + $cupH - $cd), $cd, $cd, 0, 180)
        $cp.CloseFigure()
        $g.FillPath($cupBrush, $cp)
        $cp.Dispose()
    }
    $cupBrush.Dispose()

    # ── The Bluetooth badge ──────────────────────────────────────────────
    #
    # A blue disc with the white rune on it: the mark as it is normally
    # drawn, and the thing a person identifies without reading the icon.
    # It sits between the muffs, below the band's apex, so nothing has to be
    # cut to make room for anything else.
    #
    # The disc also buys the rune its contrast back. With no tile behind the
    # icon, a bare white rune would disappear on a light taskbar; on its own
    # blue ground it reads the same wherever the icon lands.
    $cx2 = $S / 2.0
    # Pulled down and in a little: at the first size the disc touched the
    # band and crowded both muffs, so the three shapes read as one blob
    # instead of as a headset wearing a badge.
    $cy2 = px $(if ($compact) { 162 } else { 160 })
    $badgeR = px $(if ($compact) { 58 } else { 50 })
    $badgeBrush = New-Object System.Drawing.SolidBrush($badgeInk)
    $g.FillEllipse($badgeBrush, ($cx2 - $badgeR), ($cy2 - $badgeR), ($badgeR * 2), ($badgeR * 2))
    $badgeBrush.Dispose()

    # The standard single-stroke glyph: the stem, then a triangle off each
    # side crossing it. Sized to sit inside the disc with a margin, so the
    # blue reads as a badge rather than as a halo.
    $hh  = px $(if ($compact) { 40 } else { 36 })   # half height
    $hw  = px $(if ($compact) { 20 } else { 19 })   # half width
    $pen = New-Object System.Drawing.Pen($runeInk, (px $(if ($compact) { 15 } else { 13 })))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    # A typed array: PowerShell hands an untyped @() to DrawLines as
    # Object[], and the overload resolver then tries to make a single Point
    # out of it rather than seeing a sequence.
    $pts = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(($cx2 - $hw), ($cy2 - $hh / 2)),
        [System.Drawing.PointF]::new(($cx2 + $hw), ($cy2 + $hh / 2)),
        [System.Drawing.PointF]::new($cx2, ($cy2 + $hh)),
        [System.Drawing.PointF]::new($cx2, ($cy2 - $hh)),
        [System.Drawing.PointF]::new(($cx2 + $hw), ($cy2 - $hh / 2)),
        [System.Drawing.PointF]::new(($cx2 - $hw), ($cy2 + $hh / 2))
    )
    $g.DrawLines($pen, $pts)
    $pen.Dispose()

    $g.Dispose()
    return $bmp
}

# ── Pack ─────────────────────────────────────────────────────────────────
#
# PNG payloads inside the .ico, which Windows has accepted since Vista and
# which keeps the 256px entry from costing a quarter of a megabyte as a raw
# bitmap. The 1-byte size fields store 0 for 256 -- that is the format, not
# a bug.
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$blobs = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $blobs += , @{ size = $s; bytes = $ms.ToArray() }
    $ms.Dispose(); $bmp.Dispose()
}

$fs = [System.IO.File]::Create($outIco)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)                 # reserved
$bw.Write([uint16]1)                 # type: icon
$bw.Write([uint16]$blobs.Count)
$offset = 6 + 16 * $blobs.Count
foreach ($b in $blobs) {
    $dim = if ($b.size -ge 256) { 0 } else { $b.size }
    $bw.Write([byte]$dim)            # width
    $bw.Write([byte]$dim)            # height
    $bw.Write([byte]0)               # palette
    $bw.Write([byte]0)               # reserved
    $bw.Write([uint16]1)             # planes
    $bw.Write([uint16]32)            # bits per pixel
    $bw.Write([uint32]$b.bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $b.bytes.Length
}
foreach ($b in $blobs) { $bw.Write($b.bytes) }
$bw.Flush(); $bw.Close(); $fs.Dispose()

Write-Output ("wrote {0} ({1} bytes, {2} sizes)" -f $outIco, (Get-Item $outIco).Length, $blobs.Count)
