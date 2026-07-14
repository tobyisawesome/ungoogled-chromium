# Curve Browser

Curve Browser is a Windows 11-native browser shell for ungoogled-chromium.
It keeps Chromium's renderer, security model, DevTools, PDF viewer, WebUI
fallbacks, and extension system while replacing the top-level browser chrome
with an in-process WinUI 3 XAML Island.

Its tabbed title bar uses the Windows 11 Mica Alt system backdrop. The toolbar
uses compact native icon controls and a standard WinUI text box so it follows
Windows interaction and accessibility behavior instead of imitating another
browser's custom control shapes.

## Architecture

- `shell/WindowsChromiumShell.vcxproj` builds a self-contained WinUI 3 DLL.
- Chromium loads the DLL through the versioned C ABI in
  `shell/windows_chromium_shell_api.h`; Chromium never includes WinUI headers.
- The island owns the native `TabView`, toolbar, menus, profile entry point, and
  selected native internal pages. Chromium continues to own web contents and
  browser services.
- A 40 px `TabView` strip sits at the bottom of the 48 px custom title-bar row,
  giving system caption buttons the full row height. Windows keeps ownership of
  Minimize, Maximize, and Close, while the shell marks tab controls as
  non-client passthrough regions and leaves a native drag region beside them.
- The island window leaves the system caption buttons uncovered and expands
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

For a deterministic visual-regression capture, pass a PNG output path. Add a
page option to capture an expanded native surface:

```powershell
shell\out\Release\x64\WindowsChromiumShellPreview.exe C:\Temp\shell.png
shell\out\Release\x64\WindowsChromiumShellPreview.exe C:\Temp\settings.png --settings
shell\out\Release\x64\WindowsChromiumShellPreview.exe C:\Temp\profiles.png --profiles
shell\out\Release\x64\WindowsChromiumShellPreview.exe C:\Temp\about.png --about
```

Each shell build also writes
`shell/out/<Configuration>/x64/windows_chromium_payload_manifest.txt`. The
manifest lists the self-contained DLL, PRI, XBF, and WinMD payload copied beside
Chromium and consumed by the Curve Browser portable packager. Preview-host
artifacts and development symbols are excluded.

## Compatibility boundary

Normal URLs, diagnostics pages, DevTools, the PDF viewer, security
interstitials, and extension-owned pages continue to render through Chromium.
Only the explicitly allowlisted browser-owned URLs in `shell.cpp` receive a
native surface, with the Chromium WebUI still loaded underneath as a fallback.
