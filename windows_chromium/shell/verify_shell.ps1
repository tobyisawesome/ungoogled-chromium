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
    'Profiles — Local profile',
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
  foreach ($flyoutButtonName in @('Profiles — Local profile', 'Settings and more')) {
    $button = Find-DescendantByName -Root $root -Name $flyoutButtonName
    $invoke = $button.GetCurrentPattern(
      [System.Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
    Start-Sleep -Milliseconds 300
    if ((Get-SelectedTabName -Root $root) -ne $initialTab) {
      throw "$flyoutButtonName dispatched a navigation command while opening."
    }
  }

  Write-Output "Curve Browser WinUI shell verification passed (PID $($process.Id))."
} finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
}
