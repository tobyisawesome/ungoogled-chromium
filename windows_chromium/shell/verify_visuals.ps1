param(
  [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out\Release\x64'),
  [string]$ArtifactDirectory = (Join-Path $env:TEMP (
      'CurveBrowserVisualVerification-' + [Guid]::NewGuid().ToString('N')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$preview = Join-Path $output 'CurveBrowserShellPreview.exe'
if (-not (Test-Path -LiteralPath $preview -PathType Leaf)) {
  throw "Preview host not found: $preview"
}

$null = New-Item -ItemType Directory -Path $ArtifactDirectory -Force
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('CurveBrowserVisualNative' -as [type])) {
  Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class CurveBrowserVisualNative {
  [StructLayout(LayoutKind.Sequential)]
  public struct POINT {
    public int X;
    public int Y;
  }

  [DllImport("user32.dll")]
  public static extern bool ClientToScreen(IntPtr window, ref POINT point);

  [DllImport("user32.dll")]
  public static extern uint GetDpiForWindow(IntPtr window);
}
'@
}

function Assert-Visual {
  param(
    [bool]$Condition,
    [string]$Message
  )
  if (-not $Condition) {
    throw $Message
  }
}

function Find-DescendantByName {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name
  )
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
  $Root.FindFirst(
    [System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Count-CaptionControlsByName {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name
  )
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
  $count = 0
  $elements = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Descendants, $condition)
  foreach ($element in $elements) {
    if ($element.Current.BoundingRectangle.Width -ge 40) {
      ++$count
    }
  }
  $count
}

function Get-RelativeRectangle {
  param(
    [System.Windows.Automation.AutomationElement]$Element,
    [CurveBrowserVisualNative+POINT]$ClientOrigin
  )
  if (-not $Element) {
    return $null
  }
  $rectangle = $Element.Current.BoundingRectangle
  [pscustomobject]@{
    Left = $rectangle.Left - $ClientOrigin.X
    Top = $rectangle.Top - $ClientOrigin.Y
    Width = $rectangle.Width
    Height = $rectangle.Height
    Right = $rectangle.Right - $ClientOrigin.X
    Bottom = $rectangle.Bottom - $ClientOrigin.Y
  }
}

function Wait-PreviewWindow {
  param([System.Diagnostics.Process]$Process)
  $deadline = (Get-Date).AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 100
    $Process.Refresh()
  } while ($Process.MainWindowHandle -eq 0 -and
           -not $Process.HasExited -and
           (Get-Date) -lt $deadline)

  if ($Process.HasExited) {
    throw "Preview exited with code $($Process.ExitCode)."
  }
  if ($Process.MainWindowHandle -eq 0) {
    throw 'Preview did not create a window within 20 seconds.'
  }
}

function Wait-StablePng {
  param([string]$Path)
  $deadline = (Get-Date).AddSeconds(12)
  $lastLength = -1L
  $stableSamples = 0
  do {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
      try {
        $bitmap = [System.Drawing.Bitmap]::new($Path)
        try {
          $null = $bitmap.Width
          $null = $bitmap.Height
        } finally {
          $bitmap.Dispose()
        }
        $length = (Get-Item -LiteralPath $Path).Length
        if ($length -gt 0 -and $length -eq $lastLength) {
          ++$stableSamples
        } else {
          $lastLength = $length
          $stableSamples = 0
        }
        if ($stableSamples -ge 2) {
          return
        }
      } catch {
        $stableSamples = 0
      }
    }
    Start-Sleep -Milliseconds 120
  } while ((Get-Date) -lt $deadline)
  throw "Capture did not become a stable, decodable PNG: $Path"
}

function Get-CaseMetrics {
  param([System.Diagnostics.Process]$Process)
  $window = [IntPtr]$Process.MainWindowHandle
  $root = [System.Windows.Automation.AutomationElement]::FromHandle($window)
  $clientOrigin = [CurveBrowserVisualNative+POINT]::new()
  Assert-Visual -Condition (
    [CurveBrowserVisualNative]::ClientToScreen($window, [ref]$clientOrigin)) `
    -Message 'ClientToScreen failed for the preview window.'

  $tabCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::TabItem)
  $tabs = $root.FindAll(
    [System.Windows.Automation.TreeScope]::Descendants, $tabCondition)
  $selected = $null
  foreach ($tab in $tabs) {
    $selection = $tab.GetCurrentPattern(
      [System.Windows.Automation.SelectionItemPattern]::Pattern)
    if ($selection.Current.IsSelected) {
      $selected = $tab
      break
    }
  }
  Assert-Visual -Condition ($null -ne $selected) `
    -Message 'No selected tab was exposed to UI Automation.'

  $security = Find-DescendantByName -Root $root -Name 'Connection is secure'
  $favorite = Find-DescendantByName -Root $root `
    -Name 'Add this page to favorites'
  $address = Find-DescendantByName -Root $root `
    -Name 'Search or enter an address'
  $addButton = Find-DescendantByName -Root $root -Name 'Add New Tab'

  [pscustomobject]@{
    Dpi = [CurveBrowserVisualNative]::GetDpiForWindow($window)
    Selected = Get-RelativeRectangle -Element $selected `
      -ClientOrigin $clientOrigin
    Security = Get-RelativeRectangle -Element $security `
      -ClientOrigin $clientOrigin
    Favorite = Get-RelativeRectangle -Element $favorite `
      -ClientOrigin $clientOrigin
    Address = Get-RelativeRectangle -Element $address `
      -ClientOrigin $clientOrigin
    AddButton = Get-RelativeRectangle -Element $addButton `
      -ClientOrigin $clientOrigin
    MinimizeCount = Count-CaptionControlsByName -Root $root -Name 'Minimize'
    MaximizeCount = Count-CaptionControlsByName -Root $root -Name 'Maximize'
    CloseCount = Count-CaptionControlsByName -Root $root -Name 'Close'
  }
}

function Invoke-VisualCase {
  param(
    [string]$Name,
    [string[]]$Options
  )
  $path = Join-Path $ArtifactDirectory "$Name.png"
  $arguments = @($path) + $Options
  $process = Start-Process -FilePath $preview -WorkingDirectory $output `
    -ArgumentList $arguments -WindowStyle Normal -PassThru
  try {
    Wait-PreviewWindow -Process $process
    Wait-StablePng -Path $path
    # The preview writes only after the XAML tree has rendered. Reading UIA
    # first can observe the selected item during its initial 32-to-40-DIP
    # template measure even though the captured frame is already correct.
    $metrics = Get-CaseMetrics -Process $process
    [pscustomobject]@{
      Name = $Name
      Path = $path
      Dpi = $metrics.Dpi
      Selected = $metrics.Selected
      Security = $metrics.Security
      Favorite = $metrics.Favorite
      Address = $metrics.Address
      AddButton = $metrics.AddButton
      MinimizeCount = $metrics.MinimizeCount
      MaximizeCount = $metrics.MaximizeCount
      CloseCount = $metrics.CloseCount
    }
  } finally {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force
    }
  }
}

function Assert-TabGeometry {
  param([pscustomobject]$Case)
  $bitmap = [System.Drawing.Bitmap]::new($Case.Path)
  try {
    $scale = $Case.Dpi / 96.0
    $left = [Math]::Round($Case.Selected.Left)
    $right = [Math]::Round($Case.Selected.Right)
    $top = [Math]::Round($Case.Selected.Top)
    $toolbarTop = [Math]::Round(48 * $scale)
    $bottom = $toolbarTop - 1
    $shoulder = [Math]::Max(1, [Math]::Round(4 * $scale))
    $center = [Math]::Floor(($left + $right - 1) / 2)

    Assert-Visual -Condition (
      [Math]::Abs($top - [Math]::Round(8 * $scale)) -le 1) `
      -Message "$($Case.Name): selected tab has the wrong top inset."
    Assert-Visual -Condition (
      [Math]::Abs($Case.Selected.Height - 40 * $scale) -le 1) `
      -Message (
        "$($Case.Name): selected tab is not 40 DIPs tall " +
        "(actual $($Case.Selected.Height), DPI $($Case.Dpi)).")
    Assert-Visual -Condition ($right -gt $left -and $bottom -gt $top) `
      -Message "$($Case.Name): selected tab bounds are invalid."

    $body = $bitmap.GetPixel($center, $bottom)
    $toolbar = $bitmap.GetPixel($center, $toolbarTop)
    Assert-Visual -Condition ($body.A -gt 0) `
      -Message "$($Case.Name): selected body is transparent at the toolbar seam."
    Assert-Visual -Condition ($body.ToArgb() -eq $toolbar.ToArgb()) `
      -Message "$($Case.Name): selected body and toolbar materials diverge."
    # UI Automation includes slightly different template-margin pixels for a
    # first, middle, and last item. Locate the rendered silhouette run inside a
    # narrow UIA-derived window, then verify its exact structural invariants.
    $rendered = [System.Collections.Generic.List[int]]::new()
    $searchLeft = [Math]::Max(0, $left - 2 * $shoulder)
    $searchRight = [Math]::Min(
      $bitmap.Width - 1, $right + 2 * $shoulder)
    for ($x = $searchLeft; $x -le $searchRight; ++$x) {
      if ($bitmap.GetPixel($x, $bottom).A -gt 0) {
        $rendered.Add($x)
      }
    }
    Assert-Visual -Condition ($rendered.Count -gt 0) `
      -Message "$($Case.Name): selected silhouette is missing."
    $outerLeft = $rendered[0]
    $outerRight = $rendered[$rendered.Count - 1]
    for ($x = $outerLeft; $x -le $outerRight; ++$x) {
      Assert-Visual -Condition ($bitmap.GetPixel($x, $bottom).A -gt 0) `
        -Message "$($Case.Name): transparent underline/seam at x=$x."
    }
    $upperRow = $bottom - $shoulder
    $upperPixels = [System.Collections.Generic.List[int]]::new()
    for ($x = $searchLeft; $x -le $searchRight; ++$x) {
      if ($bitmap.GetPixel($x, $upperRow).A -gt 0) {
        $upperPixels.Add($x)
      }
    }
    Assert-Visual -Condition ($upperPixels.Count -gt 0) `
      -Message "$($Case.Name): selected tab body is missing above shoulders."
    $minimumExpansion = [Math]::Max(1, $shoulder - 1)
    Assert-Visual -Condition (
      $upperPixels[0] - $outerLeft -ge $minimumExpansion -and
      $outerRight - $upperPixels[$upperPixels.Count - 1] -ge
          $minimumExpansion) `
      -Message "$($Case.Name): bottom rows do not curve into both shoulders."
    Assert-Visual -Condition ($bitmap.GetPixel($outerLeft, $bottom).A -gt 0) `
      -Message "$($Case.Name): left tab shoulder is missing."
    Assert-Visual -Condition ($bitmap.GetPixel($outerRight, $bottom).A -gt 0) `
      -Message "$($Case.Name): right tab shoulder is missing."
    Assert-Visual -Condition (
      $bitmap.GetPixel($outerLeft - 1, $bottom).A -eq 0) `
      -Message "$($Case.Name): left shoulder extends beyond 4 DIPs."
    Assert-Visual -Condition (
      $bitmap.GetPixel($outerRight + 1, $bottom).A -eq 0) `
      -Message "$($Case.Name): right shoulder extends beyond 4 DIPs."

    for ($offset = 0; $offset -lt $shoulder; ++$offset) {
      $leftAlpha = $bitmap.GetPixel($outerLeft + $offset, $bottom).A
      $rightAlpha = $bitmap.GetPixel($outerRight - $offset, $bottom).A
      Assert-Visual -Condition (
        [Math]::Abs([int]$leftAlpha - [int]$rightAlpha) -le 12) `
        -Message "$($Case.Name): shoulder masks are asymmetric."
    }
  } finally {
    $bitmap.Dispose()
  }
}

function Assert-SilhouetteStable {
  param(
    [pscustomobject]$Baseline,
    [pscustomobject]$State
  )
  Assert-Visual -Condition (
    [Math]::Abs($Baseline.Selected.Left - $State.Selected.Left) -le 0.5 -and
    [Math]::Abs($Baseline.Selected.Right - $State.Selected.Right) -le 0.5) `
    -Message "$($State.Name): selected tab moved while changing visual state."

  $baseBitmap = [System.Drawing.Bitmap]::new($Baseline.Path)
  $stateBitmap = [System.Drawing.Bitmap]::new($State.Path)
  try {
    $scale = $Baseline.Dpi / 96.0
    $left = [Math]::Round($Baseline.Selected.Left - 4 * $scale)
    $right = [Math]::Round($Baseline.Selected.Right + 4 * $scale) - 1
    $bottom = [Math]::Round(48 * $scale) - 1
    $firstRow = $bottom - [Math]::Max(3, [Math]::Round(4 * $scale))
    for ($y = $firstRow; $y -le $bottom; ++$y) {
      for ($x = $left; $x -le $right; ++$x) {
        $baselinePixel = $baseBitmap.GetPixel($x, $y)
        if ($baselinePixel.A -gt 0) {
          $statePixel = $stateBitmap.GetPixel($x, $y)
          Assert-Visual -Condition (
            $baselinePixel.ToArgb() -eq $statePixel.ToArgb()) `
            -Message "$($State.Name): selected silhouette changed at $x,$y."
        }
      }
    }
  } finally {
    $baseBitmap.Dispose()
    $stateBitmap.Dispose()
  }
}

function Get-DifferenceMask {
  param(
    [pscustomobject]$Baseline,
    [pscustomobject]$State
  )
  $baseBitmap = [System.Drawing.Bitmap]::new($Baseline.Path)
  $stateBitmap = [System.Drawing.Bitmap]::new($State.Path)
  try {
    Assert-Visual -Condition (
      $baseBitmap.Width -eq $stateBitmap.Width -and
      $baseBitmap.Height -eq $stateBitmap.Height) `
      -Message "$($State.Name): capture dimensions changed."
    $points = [System.Collections.Generic.List[object]]::new()
    $minX = $baseBitmap.Width
    $minY = $baseBitmap.Height
    $maxX = -1
    $maxY = -1
    for ($y = 0; $y -lt $baseBitmap.Height; ++$y) {
      for ($x = 0; $x -lt $baseBitmap.Width; ++$x) {
        if ($baseBitmap.GetPixel($x, $y).ToArgb() -ne
            $stateBitmap.GetPixel($x, $y).ToArgb()) {
          $points.Add([pscustomobject]@{ X = $x; Y = $y })
          $minX = [Math]::Min($minX, $x)
          $minY = [Math]::Min($minY, $y)
          $maxX = [Math]::Max($maxX, $x)
          $maxY = [Math]::Max($maxY, $y)
        }
      }
    }
    [pscustomobject]@{
      Points = $points
      Count = $points.Count
      Left = $minX
      Top = $minY
      Right = $maxX
      Bottom = $maxY
      Width = if ($maxX -ge $minX) { $maxX - $minX + 1 } else { 0 }
      Height = if ($maxY -ge $minY) { $maxY - $minY + 1 } else { 0 }
    }
  } finally {
    $baseBitmap.Dispose()
    $stateBitmap.Dispose()
  }
}

function Get-MaskKeySet {
  param([pscustomobject]$Mask)
  $set = [System.Collections.Generic.HashSet[string]]::new()
  foreach ($point in $Mask.Points) {
    $null = $set.Add("$($point.X - $Mask.Left),$($point.Y - $Mask.Top)")
  }
  $set
}

function Assert-OmniboxPlate {
  param(
    [pscustomobject]$Baseline,
    [pscustomobject]$State,
    [pscustomobject]$ButtonRectangle
  )
  $mask = Get-DifferenceMask -Baseline $Baseline -State $State
  Assert-Visual -Condition ($mask.Count -gt 0) `
    -Message "$($State.Name): button state produced no visual change."
  Assert-Visual -Condition ($mask.Width -eq 30 -and $mask.Height -eq 22) `
    -Message "$($State.Name): hover/press plate is not 30 x 22 pixels."

  $buttonLeft = [Math]::Round($ButtonRectangle.Left)
  $buttonTop = [Math]::Round($ButtonRectangle.Top)
  Assert-Visual -Condition (
    $mask.Left -eq $buttonLeft -and $mask.Top -eq $buttonTop -and
    $mask.Right -eq [Math]::Round($ButtonRectangle.Right) - 1 -and
    $mask.Bottom -eq [Math]::Round($ButtonRectangle.Bottom) - 1) `
    -Message "$($State.Name): state plate escaped its WinUI button bounds."

  $keys = Get-MaskKeySet -Mask $mask
  Assert-Visual -Condition (-not $keys.Contains('0,0')) `
    -Message "$($State.Name): top-left corner is square."
  Assert-Visual -Condition (-not $keys.Contains('29,0')) `
    -Message "$($State.Name): top-right corner is square."
  Assert-Visual -Condition (-not $keys.Contains('0,21')) `
    -Message "$($State.Name): bottom-left corner is square."
  Assert-Visual -Condition (-not $keys.Contains('29,21')) `
    -Message "$($State.Name): bottom-right corner is square."
  Assert-Visual -Condition ($keys.Contains('15,0') -and
                           $keys.Contains('15,21') -and
                           $keys.Contains('0,11') -and
                           $keys.Contains('29,11')) `
    -Message "$($State.Name): rounded plate edges are clipped."
  foreach ($point in $mask.Points) {
    $localX = $point.X - $mask.Left
    $localY = $point.Y - $mask.Top
    $mirror = "$($mask.Width - 1 - $localX),$($mask.Height - 1 - $localY)"
    Assert-Visual -Condition ($keys.Contains($mirror)) `
      -Message "$($State.Name): rounded plate is not rotationally symmetric."
  }
  $mask
}

function Assert-MasksMatch {
  param(
    [pscustomobject]$First,
    [pscustomobject]$Second,
    [string]$Message
  )
  Assert-Visual -Condition (
    $First.Width -eq $Second.Width -and
    $First.Height -eq $Second.Height -and
    $First.Count -eq $Second.Count) -Message $Message
  $firstKeys = Get-MaskKeySet -Mask $First
  $secondKeys = Get-MaskKeySet -Mask $Second
  foreach ($key in $firstKeys) {
    Assert-Visual -Condition ($secondKeys.Contains($key)) -Message $Message
  }
}

function Assert-TabFocusCue {
  param(
    [pscustomobject]$Baseline,
    [pscustomobject]$Focused
  )
  $mask = Get-DifferenceMask -Baseline $Baseline -State $Focused
  Assert-Visual -Condition ($mask.Count -gt 0) `
    -Message 'Keyboard focus produced no visible tab focus cue.'
  $bottom = [Math]::Round(48 * ($Baseline.Dpi / 96.0)) - 1
  $bottomChanges = @($mask.Points | Where-Object { $_.Y -eq $bottom })
  Assert-Visual -Condition ($bottomChanges.Count -eq 0) `
    -Message 'Tab keyboard focus reintroduced a full-width bottom underline.'
}

function Assert-BookmarkDivider {
  param([pscustomobject]$Case)
  $bitmap = [System.Drawing.Bitmap]::new($Case.Path)
  try {
    $scale = $Case.Dpi / 96.0
    $dividerY = [Math]::Round((48 + 48 + 36) * $scale) - 1
    Assert-Visual -Condition ($dividerY -eq $bitmap.Height - 1) `
      -Message 'Bookmark divider is not on the final chrome row.'

    $divider = $bitmap.GetPixel([Math]::Floor($bitmap.Width / 2), $dividerY)
    $rowAbove = $bitmap.GetPixel(
      [Math]::Floor($bitmap.Width / 2), $dividerY - 1)
    Assert-Visual -Condition ($divider.ToArgb() -ne $rowAbove.ToArgb()) `
      -Message 'Bookmark divider is not visually distinct at the content edge.'
    for ($x = 0; $x -lt $bitmap.Width; ++$x) {
      Assert-Visual -Condition (
        $bitmap.GetPixel($x, $dividerY).ToArgb() -eq $divider.ToArgb()) `
        -Message "Bookmark divider is inset or interrupted at x=$x."
    }
  } finally {
    $bitmap.Dispose()
  }
}

$cases = @{}
$cases.RestFirst = Invoke-VisualCase -Name 'rest-first' `
  -Options @('--visual-normal')
$cases.RestMiddle = Invoke-VisualCase -Name 'rest-middle' `
  -Options @('--active-middle', '--visual-normal')
$cases.RestLast = Invoke-VisualCase -Name 'rest-last' `
  -Options @('--active-last', '--visual-normal')
$cases.HoverSelected = Invoke-VisualCase -Name 'hover-selected' `
  -Options @('--hover-selected')
$cases.PressSelected = Invoke-VisualCase -Name 'press-selected' `
  -Options @('--press-selected')
$cases.HoverRight = Invoke-VisualCase -Name 'hover-right-adjacent' `
  -Options @('--hover-unselected')
$cases.PressRight = Invoke-VisualCase -Name 'press-right-adjacent' `
  -Options @('--press-unselected')
$cases.HoverLeft = Invoke-VisualCase -Name 'hover-left-adjacent' `
  -Options @('--active-middle', '--hover-unselected')
$cases.PressLeft = Invoke-VisualCase -Name 'press-left-adjacent' `
  -Options @('--active-middle', '--press-unselected')
$cases.FocusSelected = Invoke-VisualCase -Name 'focus-selected' `
  -Options @('--focus-selected')
$cases.FocusAddress = Invoke-VisualCase -Name 'focus-address' `
  -Options @('--focus-address')
$cases.HoverSecurity = Invoke-VisualCase -Name 'hover-security' `
  -Options @('--hover-security')
$cases.PressSecurity = Invoke-VisualCase -Name 'press-security' `
  -Options @('--press-security')
$cases.HoverFavorite = Invoke-VisualCase -Name 'hover-favorite' `
  -Options @('--hover-favorite')
$cases.PressFavorite = Invoke-VisualCase -Name 'press-favorite' `
  -Options @('--press-favorite')
$cases.HoverAdd = Invoke-VisualCase -Name 'hover-add' `
  -Options @('--hover-add')

foreach ($case in @($cases.RestFirst, $cases.RestMiddle, $cases.RestLast)) {
  Assert-TabGeometry -Case $case
}
Assert-BookmarkDivider -Case $cases.RestFirst
Assert-Visual -Condition (
  $cases.RestFirst.MinimizeCount -eq 1 -and
  $cases.RestFirst.MaximizeCount -eq 1 -and
  $cases.RestFirst.CloseCount -eq 1) `
  -Message 'A duplicate native caption-button provider is still exposed.'
foreach ($case in @(
    $cases.HoverSelected,
    $cases.PressSelected,
    $cases.HoverRight,
    $cases.PressRight)) {
  Assert-SilhouetteStable -Baseline $cases.RestFirst -State $case
}
foreach ($case in @($cases.HoverLeft, $cases.PressLeft)) {
  Assert-SilhouetteStable -Baseline $cases.RestMiddle -State $case
}
Assert-TabFocusCue -Baseline $cases.RestFirst -Focused $cases.FocusSelected

Assert-Visual -Condition ($null -ne $cases.RestFirst.Address -and
                          $null -ne $cases.RestFirst.Security -and
                          $null -ne $cases.RestFirst.Favorite) `
  -Message 'Omnibox UI Automation geometry is incomplete.'
$address = $cases.RestFirst.Address
$security = $cases.RestFirst.Security
$favorite = $cases.RestFirst.Favorite
Assert-Visual -Condition (
  [Math]::Abs(($security.Left - $address.Left) - 5) -le 1 -and
  [Math]::Abs(($security.Top - $address.Top) - 5) -le 1) `
  -Message 'Security button is not inset 5 DIPs from the omnibox border.'
Assert-Visual -Condition (
  [Math]::Abs(($address.Right - $favorite.Right) - 5) -le 1 -and
  [Math]::Abs(($favorite.Top - $address.Top) - 5) -le 1) `
  -Message 'Favorite button is not inset 5 DIPs from the omnibox border.'

$securityHoverMask = Assert-OmniboxPlate -Baseline $cases.RestFirst `
  -State $cases.HoverSecurity -ButtonRectangle $security
$securityPressMask = Assert-OmniboxPlate -Baseline $cases.RestFirst `
  -State $cases.PressSecurity -ButtonRectangle $security
$favoriteHoverMask = Assert-OmniboxPlate -Baseline $cases.RestFirst `
  -State $cases.HoverFavorite -ButtonRectangle $favorite
$favoritePressMask = Assert-OmniboxPlate -Baseline $cases.RestFirst `
  -State $cases.PressFavorite -ButtonRectangle $favorite
Assert-MasksMatch -First $securityHoverMask -Second $securityPressMask `
  -Message 'Security hover and pressed geometries differ.'
Assert-MasksMatch -First $favoriteHoverMask -Second $favoritePressMask `
  -Message 'Favorite hover and pressed geometries differ.'
Assert-MasksMatch -First $securityHoverMask -Second $favoriteHoverMask `
  -Message 'Security and favorite button geometries differ.'

$summary = [pscustomobject]@{
  Passed = $true
  Dpi = $cases.RestFirst.Dpi
  Cases = $cases.Keys.Count
  ArtifactDirectory = $ArtifactDirectory
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (
  Join-Path $ArtifactDirectory 'summary.json') -Encoding utf8
Write-Output (
  "Curve Browser visual verification passed ($($cases.Keys.Count) states, " +
  "DPI $($cases.RestFirst.Dpi)). Artifacts: $ArtifactDirectory")
