param(
  [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out\Release\x64')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$preview = Join-Path $output 'CurveBrowserShellPreview.exe'
$manifest = Join-Path $output 'curve_browser_payload_manifest.txt'
$requiredPayload = @(
  'curve_browser_shell.dll',
  'curve_browser_shell.pri',
  'CurveBrowserShell.winmd'
)

if (-not (Test-Path -LiteralPath $preview -PathType Leaf)) {
  throw "Preview host not found: $preview"
}
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
  throw "Payload manifest not found: $manifest"
}

$payload = Get-Content -LiteralPath $manifest
foreach ($name in $requiredPayload) {
  if ($name -notin $payload) {
    throw "Payload manifest is missing $name"
  }
  if (-not (Test-Path -LiteralPath (Join-Path $output $name) -PathType Leaf)) {
    throw "Payload file is missing: $name"
  }
}

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

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

function Find-DescendantByNamePrefix {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Prefix
  )
  $all = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Descendants,
    [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($element in $all) {
    if ($element.Current.Name.StartsWith(
        $Prefix, [System.StringComparison]::Ordinal)) {
      return $element
    }
  }
  return $null
}

function Get-SelectedTabName {
  param([System.Windows.Automation.AutomationElement]$Root)
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::TabItem)
  $tabs = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Descendants, $condition)
  foreach ($tab in $tabs) {
    $selection = $tab.GetCurrentPattern(
      [System.Windows.Automation.SelectionItemPattern]::Pattern)
    if ($selection.Current.IsSelected) {
      return $tab.Current.Name
    }
  }
  throw 'No selected TabView item was exposed to UI Automation.'
}

function Find-TabByName {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name
  )
  $condition = [System.Windows.Automation.AndCondition]::new(@(
    [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::TabItem),
    [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
  ))
  $Root.FindFirst(
    [System.Windows.Automation.TreeScope]::Descendants, $condition)
}

$process = Start-Process -FilePath $preview -WorkingDirectory $output -PassThru
try {
  $deadline = (Get-Date).AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
  } while ($process.MainWindowHandle -eq 0 -and
           -not $process.HasExited -and
           (Get-Date) -lt $deadline)

  if ($process.HasExited) {
    throw "Preview host exited with code $($process.ExitCode)."
  }
  if ($process.MainWindowHandle -eq 0) {
    throw 'Preview host did not create a window within 20 seconds.'
  }

  $root = [System.Windows.Automation.AutomationElement]::FromHandle(
    [IntPtr]$process.MainWindowHandle)
  $requiredControls = @(
    'Curve Browser',
    'WinUI 3 documentation',
    'Settings',
    'Search or enter an address',
    'Add New Tab',
    'Back (Alt+Left)',
    'Forward (Alt+Right)',
    'Reload (Ctrl+R)',
    "Profiles $([char]0x2014) Local profile",
    'Settings and more',
    'Minimize',
    'Maximize',
    'Close'
  )
  foreach ($name in $requiredControls) {
    if (-not (Find-DescendantByName -Root $root -Name $name)) {
      throw "UI Automation control is missing: $name"
    }
  }

  $initialTab = Get-SelectedTabName -Root $root
  foreach ($flyoutButtonName in @(
      "Profiles $([char]0x2014) Local profile",
      'Settings and more')) {
    $button = Find-DescendantByName -Root $root -Name $flyoutButtonName
    $invoke = $button.GetCurrentPattern(
      [System.Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
    Start-Sleep -Milliseconds 300
    if ((Get-SelectedTabName -Root $root) -ne $initialTab) {
      throw "$flyoutButtonName dispatched a navigation command while opening."
    }
  }

  $loadingTab = Find-TabByName -Root $root -Name 'WinUI 3 documentation'
  $loadingSelection = $loadingTab.GetCurrentPattern(
    [System.Windows.Automation.SelectionItemPattern]::Pattern)
  $loadingSelection.Select()
  Start-Sleep -Milliseconds 300
  if (-not (Find-DescendantByName -Root $root -Name 'Stop loading (Esc)')) {
    throw 'The active loading tab did not switch Reload to Stop.'
  }

  $firstTab = Find-TabByName -Root $root -Name 'Curve Browser'
  $firstSelection = $firstTab.GetCurrentPattern(
    [System.Windows.Automation.SelectionItemPattern]::Pattern)
  $firstSelection.Select()
  Start-Sleep -Milliseconds 300
  if (-not (Find-DescendantByName -Root $root -Name 'Reload (Ctrl+R)')) {
    throw 'The inactive loading tab left the toolbar in Stop state.'
  }

  $settingsTab = Find-TabByName -Root $root -Name 'Settings'
  $settingsSelection = $settingsTab.GetCurrentPattern(
    [System.Windows.Automation.SelectionItemPattern]::Pattern)
  $settingsSelection.Select()
  Start-Sleep -Milliseconds 300
  $startup = Find-DescendantByName -Root $root -Name 'Restore previous session'
  if (-not $startup) {
    throw 'The Chromium-backed startup setting is missing.'
  }
  $toggle = $startup.GetCurrentPattern(
    [System.Windows.Automation.TogglePattern]::Pattern)
  if ($toggle.Current.ToggleState -ne
      [System.Windows.Automation.ToggleState]::On) {
    throw 'The startup setting did not reflect the host state.'
  }
  $toggle.Toggle()
  Start-Sleep -Milliseconds 300
  $startup = Find-DescendantByName -Root $root -Name 'Restore previous session'
  $toggle = $startup.GetCurrentPattern(
    [System.Windows.Automation.TogglePattern]::Pattern)
  if ($toggle.Current.ToggleState -ne
      [System.Windows.Automation.ToggleState]::Off) {
    throw 'The startup setting did not round-trip through the host command API.'
  }
  if (-not (Find-DescendantByName -Root $root -Name 'Manage search engine')) {
    throw 'The Chromium search settings entry point is missing.'
  }

  Write-Output "Curve Browser WinUI shell verification passed (PID $($process.Id))."
} finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
}

$historyProcess = Start-Process -FilePath $preview -WorkingDirectory $output `
  -ArgumentList '--history' -PassThru
try {
  $deadline = (Get-Date).AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 250
    $historyProcess.Refresh()
  } while ($historyProcess.MainWindowHandle -eq 0 -and
           -not $historyProcess.HasExited -and
           (Get-Date) -lt $deadline)

  if ($historyProcess.HasExited -or $historyProcess.MainWindowHandle -eq 0) {
    throw 'Native history preview did not create a window.'
  }

  $historyRoot = [System.Windows.Automation.AutomationElement]::FromHandle(
    [IntPtr]$historyProcess.MainWindowHandle)
  if (-not (Find-DescendantByName -Root $historyRoot -Name 'Example Domain')) {
    throw 'Native history data was not rendered.'
  }
  $open = Find-DescendantByName -Root $historyRoot -Name 'Open'
  if (-not $open) {
    throw 'Native history entry action is missing.'
  }
  $open.GetCurrentPattern(
    [System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  Start-Sleep -Milliseconds 300
  $address = Find-DescendantByName -Root $historyRoot `
    -Name 'Search or enter an address'
  $value = $address.GetCurrentPattern(
    [System.Windows.Automation.ValuePattern]::Pattern).Current.Value
  if ($value -ne 'https://curvebrowser.local/') {
    throw 'Native history entry did not navigate through the host command API.'
  }
  Write-Output "Curve Browser native history verification passed (PID $($historyProcess.Id))."
} finally {
  if (-not $historyProcess.HasExited) {
    Stop-Process -Id $historyProcess.Id -Force
  }
}

$downloadsProcess = Start-Process -FilePath $preview -WorkingDirectory $output `
  -ArgumentList '--downloads' -PassThru
try {
  $deadline = (Get-Date).AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 250
    $downloadsProcess.Refresh()
  } while ($downloadsProcess.MainWindowHandle -eq 0 -and
           -not $downloadsProcess.HasExited -and
           (Get-Date) -lt $deadline)

  if ($downloadsProcess.HasExited -or
      $downloadsProcess.MainWindowHandle -eq 0) {
    throw 'Native downloads preview did not create a window.'
  }

  $downloadsRoot = [System.Windows.Automation.AutomationElement]::FromHandle(
    [IntPtr]$downloadsProcess.MainWindowHandle)
  if (-not (Find-DescendantByName -Root $downloadsRoot `
      -Name 'CurveBrowserSetup.exe')) {
    throw 'Native download-manager data was not rendered.'
  }
  if (-not (Find-DescendantByName -Root $downloadsRoot `
      -Name 'Show in folder')) {
    throw 'Native show-download-in-folder action is missing.'
  }
  $open = Find-DescendantByName -Root $downloadsRoot -Name 'Open'
  if (-not $open) {
    throw 'Native open-download action is missing.'
  }
  $open.GetCurrentPattern(
    [System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  Start-Sleep -Milliseconds 300
  if (-not (Find-DescendantByNamePrefix -Root $downloadsRoot `
      -Prefix 'Open requested')) {
    throw 'Native download action did not round-trip through the host API.'
  }
  Write-Output "Curve Browser native downloads verification passed (PID $($downloadsProcess.Id))."
} finally {
  if (-not $downloadsProcess.HasExited) {
    Stop-Process -Id $downloadsProcess.Id -Force
  }
}
