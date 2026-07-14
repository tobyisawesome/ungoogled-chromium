# Windows Chromium

Windows Chromium is a Windows 11-native browser shell for ungoogled-chromium.
It keeps Chromium's renderer, security model, DevTools, PDF viewer, WebUI
fallbacks, and extension system while replacing the top-level browser chrome
with an in-process WinUI 3 XAML Island.

## Architecture

- `shell/WindowsChromiumShell.vcxproj` builds a self-contained WinUI 3 DLL.
- Chromium loads the DLL through the versioned C ABI in
  `shell/windows_chromium_shell_api.h`; Chromium never includes WinUI headers.
- The island owns the native `TabView`, toolbar, menus, profile entry point, and
  selected native internal pages. Chromium continues to own web contents and
  browser services.
- The island window leaves Chromium's caption buttons uncovered and expands
  over web contents only for a supported native internal page.
- If the DLL or Windows App SDK cannot initialize, Chromium remains usable with
  its existing Views UI.

## Build the shell preview

From a Visual Studio Developer PowerShell:

```powershell
msbuild shell\WindowsChromiumShell.vcxproj /restore /p:Configuration=Release /p:Platform=x64
msbuild shell\WindowsChromiumShellPreview.vcxproj /restore /p:Configuration=Release /p:Platform=x64
shell\out\Release\x64\WindowsChromiumShellPreview.exe
```

The preview is a real Win32 host loading the same shell DLL that Chromium uses.
It supplies representative tab and navigation state so native interactions can
be tested before a full Chromium relink.

For a deterministic visual-regression capture, pass a PNG output path. Add
`--settings` to capture the expanded native Settings page:

```powershell
shell\out\Release\x64\WindowsChromiumShellPreview.exe C:\Temp\shell.png
shell\out\Release\x64\WindowsChromiumShellPreview.exe C:\Temp\settings.png --settings
```

## Compatibility boundary

Normal URLs, diagnostics pages, DevTools, the PDF viewer, security
interstitials, and extension-owned pages continue to render through Chromium.
Only the explicitly allowlisted browser-owned URLs in `shell.cpp` receive a
native surface, with the Chromium WebUI still loaded underneath as a fallback.
