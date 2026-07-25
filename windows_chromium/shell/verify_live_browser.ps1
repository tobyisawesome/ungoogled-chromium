param(
  [Parameter(Mandatory)]
  [string]$ChromeDirectory,
  [int]$NewTabIterations = 20,
  [int]$WindowStateIterations = 10,
  [ValidateSet('Search', 'Direct')]
  [string]$Navigation = 'Search',
  [switch]$VerifyBundledExtension,
  [int]$BundledExtensionTimeoutSeconds = 240
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$chrome = Join-Path $ChromeDirectory 'chrome.exe'
if (-not (Test-Path -LiteralPath $chrome -PathType Leaf)) {
  throw "Curve Browser executable not found: $chrome"
}

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('CurveBrowserLiveNative' -as [type])) {
  Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class CurveBrowserLiveNative {
  [StructLayout(LayoutKind.Sequential)]
  private struct GuiThreadInfo {
    public int cbSize;
    public uint flags;
    public IntPtr hwndActive;
    public IntPtr hwndFocus;
    public IntPtr hwndCapture;
    public IntPtr hwndMenuOwner;
    public IntPtr hwndMoveSize;
    public IntPtr hwndCaret;
    public int caretLeft;
    public int caretTop;
    public int caretRight;
    public int caretBottom;
  }

  [DllImport("user32.dll")]
  private static extern uint GetWindowThreadProcessId(
      IntPtr window, out uint processId);

  [DllImport("user32.dll")]
  private static extern bool GetGUIThreadInfo(
      uint threadId, ref GuiThreadInfo info);

  [DllImport("user32.dll", SetLastError = true)]
  private static extern bool PostMessageW(
      IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

  public static bool PressEnter(IntPtr window) {
    uint processId;
    uint threadId = GetWindowThreadProcessId(window, out processId);
    var info = new GuiThreadInfo();
    info.cbSize = Marshal.SizeOf<GuiThreadInfo>();
    if (threadId == 0 || !GetGUIThreadInfo(threadId, ref info) ||
        info.hwndFocus == IntPtr.Zero) {
      return false;
    }
    const uint WM_KEYDOWN = 0x0100;
    const uint WM_KEYUP = 0x0101;
    const int VK_RETURN = 0x0D;
    return PostMessageW(info.hwndFocus, WM_KEYDOWN,
                        new IntPtr(VK_RETURN), new IntPtr(1)) &&
           PostMessageW(info.hwndFocus, WM_KEYUP,
                        new IntPtr(VK_RETURN),
                        new IntPtr(unchecked((int)0xC0000001)));
  }

  public static bool TypeCharacter(IntPtr window, char character) {
    uint processId;
    uint threadId = GetWindowThreadProcessId(window, out processId);
    var info = new GuiThreadInfo();
    info.cbSize = Marshal.SizeOf<GuiThreadInfo>();
    if (threadId == 0 || !GetGUIThreadInfo(threadId, ref info) ||
        info.hwndFocus == IntPtr.Zero) {
      return false;
    }
    const uint WM_CHAR = 0x0102;
    return PostMessageW(info.hwndFocus, WM_CHAR,
                        new IntPtr(character), new IntPtr(1));
  }
}
'@
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

function Find-DescendantByAutomationId {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$AutomationId
  )
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
    $AutomationId)
  $Root.FindFirst(
    [System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Test-IsDescendantOrSelf {
  param(
    [System.Windows.Automation.AutomationElement]$Element,
    [System.Windows.Automation.AutomationElement]$Ancestor
  )
  if (-not $Element -or -not $Ancestor) {
    return $false
  }
  $ancestorId = $Ancestor.GetRuntimeId() -join ','
  $current = $Element
  while ($current) {
    if (($current.GetRuntimeId() -join ',') -eq $ancestorId) {
      return $true
    }
    $current = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetParent(
      $current)
  }
  $false
}

function Get-Root {
  param([System.Diagnostics.Process]$Process)
  $Process.Refresh()
  if ($Process.HasExited) {
    throw "Curve Browser exited unexpectedly with code $($Process.ExitCode)."
  }
  [System.Windows.Automation.AutomationElement]::FromHandle(
    [IntPtr]$Process.MainWindowHandle)
}

function Invoke-NamedButton {
  param(
    [System.Diagnostics.Process]$Process,
    [string[]]$Names
  )
  $deadline = (Get-Date).AddSeconds(5)
  $button = $null
  do {
    $root = Get-Root -Process $Process
    foreach ($name in $Names) {
      $button = Find-DescendantByName -Root $root -Name $name
      if ($button) {
        break
      }
    }
    if (-not $button) {
      Start-Sleep -Milliseconds 50
    }
  } while (-not $button -and (Get-Date) -lt $deadline)
  if (-not $button) {
    throw "Button not found: $($Names -join ' / ')"
  }
  $button.GetCurrentPattern(
    [System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Submit-Address {
  param(
    [System.Diagnostics.Process]$Process,
    [string]$Text
  )
  $root = Get-Root -Process $Process
  $address = Find-DescendantByName -Root $root `
    -Name 'Search or enter an address'
  if (-not $address) {
    throw 'The native omnibox was not exposed to UI Automation.'
  }
  $value = $address.GetCurrentPattern(
    [System.Windows.Automation.ValuePattern]::Pattern)
  $address.SetFocus()
  $value.SetValue($Text)
  Start-Sleep -Milliseconds 200
  $address.SetFocus()
  Start-Sleep -Milliseconds 200
  if (-not [CurveBrowserLiveNative]::PressEnter(
      [IntPtr]$Process.MainWindowHandle)) {
    throw 'Could not submit Enter to the focused native omnibox.'
  }
}

function Wait-Address {
  param(
    [System.Diagnostics.Process]$Process,
    [scriptblock]$Predicate,
    [string]$Description,
    [string]$AutomationName = 'Search or enter an address',
    [int]$TimeoutSeconds = 20
  )
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $lastValue = ''
  do {
    Start-Sleep -Milliseconds 200
    $root = Get-Root -Process $Process
    $address = Find-DescendantByName -Root $root `
      -Name $AutomationName
    if ($address) {
      $lastValue = $address.GetCurrentPattern(
        [System.Windows.Automation.ValuePattern]::Pattern).Current.Value
      if (& $Predicate $lastValue) {
        return $lastValue
      }
    }
  } while ((Get-Date) -lt $deadline)
  throw "$Description did not appear in the native omnibox; last value: $lastValue"
}

function Submit-Navigation {
  param(
    [System.Diagnostics.Process]$Process,
    [string]$Text,
    [scriptblock]$Predicate,
    [string]$Description
  )
  $lastError = $null
  for ($attempt = 0; $attempt -lt 3; ++$attempt) {
    Submit-Address -Process $Process -Text $Text
    try {
      # Chromium's hidden Views omnibox remains in the accessibility tree even
      # though it is transparent. Unlike the native edit, its value changes
      # only after Browser::OpenURL accepted the command, so it is an exact
      # integration oracle rather than merely echoing ValuePattern.SetValue.
      return Wait-Address -Process $Process -Predicate $Predicate `
        -Description $Description -AutomationName 'Address and search bar' `
        -TimeoutSeconds 6
    } catch {
      $lastError = $_
    }
  }
  throw $lastError
}

function Count-LargeControlsByName {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name
  )
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
  $count = 0
  foreach ($element in $Root.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $condition)) {
    $bounds = $element.Current.BoundingRectangle
    if ($bounds.Width -ge 40 -and $bounds.Height -ge 40) {
      $count += 1
    }
  }
  $count
}

function Get-NativeTabCount {
  param([System.Diagnostics.Process]$Process)
  $root = Get-Root -Process $Process
  $tabList = Find-DescendantByAutomationId -Root $root `
    -AutomationId 'TabListView'
  if (-not $tabList) {
    throw 'The native TabView list was not exposed.'
  }
  $tabCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::TabItem)
  $tabList.FindAll(
    [System.Windows.Automation.TreeScope]::Children, $tabCondition).Count
}

function Describe-ControlsByName {
  param(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name
  )
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
  $descriptions = foreach ($element in $Root.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $condition)) {
    $bounds = $element.Current.BoundingRectangle
    '{0}x{1}@{2},{3}' -f $bounds.Width, $bounds.Height,
        $bounds.Left, $bounds.Top
  }
  if ($descriptions) {
    $descriptions -join '; '
  } else {
    '<none>'
  }
}

$profile = Join-Path $env:TEMP (
  'CurveBrowserLiveVerification-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $profile
$arguments = @(
  "--user-data-dir=$profile",
  '--no-first-run',
  'about:blank'
)
if (-not $VerifyBundledExtension) {
  $arguments = @('--disable-default-apps') + $arguments
}
$process = Start-Process -FilePath $chrome -WorkingDirectory $ChromeDirectory `
  -ArgumentList $arguments -PassThru

try {
  $deadline = (Get-Date).AddSeconds(30)
  do {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
  } while ($process.MainWindowHandle -eq 0 -and
           -not $process.HasExited -and
           (Get-Date) -lt $deadline)
  if ($process.HasExited -or $process.MainWindowHandle -eq 0) {
    throw 'Curve Browser did not create a test window within 30 seconds.'
  }

  $bundledExtensionVersion = $null
  if ($VerifyBundledExtension) {
    $extensionId = 'cjpalhdlnbpafiamejdnhcphjbkeiagm'
    $extensionRoot = Join-Path $profile "Default\Extensions\$extensionId"
    $extensionDeadline =
      (Get-Date).AddSeconds($BundledExtensionTimeoutSeconds)
    do {
      $versionDirectory = Get-ChildItem -LiteralPath $extensionRoot `
        -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
      if ($versionDirectory) {
        break
      }
      Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $extensionDeadline)
    if (-not $versionDirectory) {
      throw (
        "Bundled uBlock Origin was not installed within " +
        "$BundledExtensionTimeoutSeconds seconds.")
    }
    $manifestPath = Join-Path $versionDirectory.FullName 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
      throw "Bundled extension manifest is missing: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
      ConvertFrom-Json
    if ($manifest.version -ne '1.72.2' -or
        $manifest.manifest_version -ne 2) {
      throw (
        'Unexpected bundled uBlock Origin manifest: ' +
        "version=$($manifest.version), " +
        "manifest_version=$($manifest.manifest_version)")
    }
    $bundledExtensionVersion = $manifest.version
  }

  $searchUrl = $null
  $directUrl = $null
  if ($Navigation -eq 'Search') {
    $searchUrl = Submit-Navigation -Process $process `
      -Text 'curve browser integration test' `
      -Predicate {
        param($value)
        $value -match '^https://www\.google\.com/search\?'
      } -Description 'A search URL'
  } else {
    $directUrl = Submit-Navigation -Process $process `
      -Text 'https://example.com/' `
      -Predicate { param($value) $value -match '^https://example\.com/?$' } `
      -Description 'The explicit URL'
  }

  $root = Get-Root -Process $process
  $address = Find-DescendantByName -Root $root `
    -Name 'Search or enter an address'
  $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
  if (Test-IsDescendantOrSelf -Element $focused -Ancestor $address) {
    throw 'The native omnibox retained focus after navigation.'
  }
  $addressValue = $address.GetCurrentPattern(
    [System.Windows.Automation.ValuePattern]::Pattern)
  $addressBeforeType = $addressValue.Current.Value
  if (-not [CurveBrowserLiveNative]::TypeCharacter(
      [IntPtr]$process.MainWindowHandle, [char]'x')) {
    throw 'Could not inject the post-navigation focus probe.'
  }
  Start-Sleep -Milliseconds 250
  $addressAfterType = $address.GetCurrentPattern(
    [System.Windows.Automation.ValuePattern]::Pattern).Current.Value
  if ($addressAfterType -ne $addressBeforeType) {
    throw 'Post-navigation typing still targeted the native omnibox.'
  }

  $tabCount = Get-NativeTabCount -Process $process
  for ($index = 0; $index -lt $NewTabIterations; ++$index) {
    $expectedTabCount = $tabCount + 1
    Invoke-NamedButton -Process $process -Names @('Add New Tab')
    $tabDeadline = (Get-Date).AddSeconds(3)
    do {
      Start-Sleep -Milliseconds 100
      $tabCount = Get-NativeTabCount -Process $process
    } while ($tabCount -lt $expectedTabCount -and
             (Get-Date) -lt $tabDeadline)
    if ($tabCount -ne $expectedTabCount) {
      throw (
        "New-tab iteration $($index + 1) expected $expectedTabCount " +
        "native tabs, found $tabCount.")
    }
  }
  if ($tabCount -ne $NewTabIterations + 1) {
    throw "Expected $($NewTabIterations + 1) native tabs, found $tabCount."
  }

  for ($index = 0; $index -lt $WindowStateIterations; ++$index) {
    Invoke-NamedButton -Process $process -Names @('Maximize')
    Start-Sleep -Milliseconds 150
    Invoke-NamedButton -Process $process -Names @('Restore', 'Restore Down')
    Start-Sleep -Milliseconds 150
  }

  $captionDeadline = (Get-Date).AddSeconds(5)
  do {
    $root = Get-Root -Process $process
    $minimizeCount = Count-LargeControlsByName -Root $root -Name 'Minimize'
    $maximizeCount = Count-LargeControlsByName -Root $root -Name 'Maximize'
    $closeCount = Count-LargeControlsByName -Root $root -Name 'Close'
    if ($minimizeCount -eq 1 -and $maximizeCount -eq 1 -and
        $closeCount -eq 1) {
      break
    }
    Start-Sleep -Milliseconds 100
  } while ((Get-Date) -lt $captionDeadline)
  if ($minimizeCount -ne 1 -or $maximizeCount -ne 1 -or
      $closeCount -ne 1) {
    $minimizeDetails = Describe-ControlsByName -Root $root -Name 'Minimize'
    $maximizeDetails = Describe-ControlsByName -Root $root -Name 'Maximize'
    $closeDetails = Describe-ControlsByName -Root $root -Name 'Close'
    throw (
      'Duplicate caption controls detected: ' +
      "Minimize=$minimizeCount [$minimizeDetails] " +
      "Maximize=$maximizeCount [$maximizeDetails] " +
      "Close=$closeCount [$closeDetails]")
  }

  [pscustomobject]@{
    Passed = $true
    ProcessId = $process.Id
    Profile = $profile
    SearchUrl = $searchUrl
    DirectUrl = $directUrl
    NativeTabs = $tabCount
    WindowStateCycles = $WindowStateIterations
    CaptionButtons = 3
    BundledExtensionVersion = $bundledExtensionVersion
  } | ConvertTo-Json
} finally {
  if ($process -and -not $process.HasExited) {
    $null = $process.CloseMainWindow()
    Start-Sleep -Seconds 2
  }
  $children = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
    Where-Object { $_.CommandLine -like "*$profile*" }
  foreach ($child in $children) {
    Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue
  }
}
