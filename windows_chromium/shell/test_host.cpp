// Copyright 2026 The Windows Chromium Authors
// SPDX-License-Identifier: BSD-3-Clause

#include <windows.h>

#include <array>
#include <cstdio>
#include <string>

#include "windows_chromium_shell_api.h"

namespace {

using GetApiVersion = uint32_t(__stdcall*)();
using CreateShell = HRESULT(__stdcall*)(HWND, const WcsHostCallbacks*,
                                        WcsShellHandle*);
using DestroyShell = void(__stdcall*)(WcsShellHandle);
using UpdateWindowState = HRESULT(__stdcall*)(WcsShellHandle,
                                              const WcsWindowState*);
using PreTranslateMessage = BOOL(WINAPI*)(MSG*);

HMODULE g_shell_module = nullptr;
WcsShellHandle g_shell = nullptr;
UpdateWindowState g_update = nullptr;
DestroyShell g_destroy = nullptr;

std::array<std::wstring, 3> g_titles = {
    L"Windows Chromium", L"WinUI 3 documentation", L"Settings"};
std::array<std::wstring, 3> g_urls = {
    L"https://windowschromium.local/", L"https://learn.microsoft.com/windows/apps/winui/",
    L"chrome://settings/"};
int g_active = 0;

void PushState() {
  std::array<WcsTabState, 3> tabs{};
  for (int index = 0; index < 3; ++index) {
    tabs[index] = WcsTabState{sizeof(WcsTabState),
                              index + 1,
                              index,
                              g_titles[index].c_str(),
                              g_urls[index].c_str(),
                              index == g_active,
                              index == 0,
                              index == 1,
                              0,
                              0};
  }
  WcsWindowState state{sizeof(WcsWindowState), tabs.data(), tabs.size(),
                       g_active, 1, 0, 0, L"Local profile"};
  if (g_update && g_shell) {
    g_update(g_shell, &state);
  }
}

void __stdcall OnCommand(void*, const WcsCommandArgs* args) {
  if (!args) {
    return;
  }
  if (args->command == WCS_COMMAND_ACTIVATE_TAB) {
    g_active = static_cast<int>(args->tab_id - 1);
    PushState();
  } else if (args->command == WCS_COMMAND_NAVIGATE && args->text) {
    g_urls[g_active] = args->text;
    g_titles[g_active] = args->text;
    PushState();
  } else if (args->command == WCS_COMMAND_OPEN_SETTINGS) {
    g_active = 2;
    PushState();
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
      FillRect(dc, &client, background);
      DeleteObject(background);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(72, 72, 72));
      RECT message_rect{48, 150, client.right - 48, client.bottom - 48};
      DrawTextW(dc,
                L"Chromium web content renders in this area.\n\nThis preview "
                L"is the real in-process WinUI 3 shell DLL running inside a "
                L"Win32 host.",
                -1, &message_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int wmain() {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"WindowsChromiumShellPreview";
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassExW(&window_class)) {
    return static_cast<int>(GetLastError());
  }

  HWND window = CreateWindowExW(
      0, window_class.lpszClassName, L"Windows Chromium — WinUI 3 Shell Preview",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr,
      nullptr, instance, nullptr);
  if (!window) {
    return static_cast<int>(GetLastError());
  }

  g_shell_module = LoadLibraryW(L"windows_chromium_shell.dll");
  if (!g_shell_module) {
    std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
    return static_cast<int>(GetLastError());
  }
  const auto get_version = reinterpret_cast<GetApiVersion>(
      GetProcAddress(g_shell_module, "WcsGetApiVersion"));
  const auto create = reinterpret_cast<CreateShell>(
      GetProcAddress(g_shell_module, "WcsCreateShell"));
  g_update = reinterpret_cast<UpdateWindowState>(
      GetProcAddress(g_shell_module, "WcsUpdateWindowState"));
  g_destroy = reinterpret_cast<DestroyShell>(
      GetProcAddress(g_shell_module, "WcsDestroyShell"));
  if (!get_version || get_version() != WCS_API_VERSION || !create ||
      !g_update || !g_destroy) {
    std::fwprintf(stderr, L"Shell API mismatch\n");
    return ERROR_REVISION_MISMATCH;
  }

  WcsHostCallbacks callbacks{sizeof(WcsHostCallbacks), WCS_API_VERSION, nullptr,
                             &OnCommand};
  const HRESULT result = create(window, &callbacks, &g_shell);
  if (FAILED(result)) {
    std::fwprintf(stderr, L"WcsCreateShell failed: 0x%08X\n",
                  static_cast<unsigned int>(result));
    return result;
  }
  PushState();

  ShowWindow(window, SW_SHOWDEFAULT);
  UpdateWindow(window);

  HMODULE windowing = GetModuleHandleW(L"Microsoft.UI.Windowing.Core.dll");
  const auto pre_translate = windowing
      ? reinterpret_cast<PreTranslateMessage>(
            GetProcAddress(windowing, "ContentPreTranslateMessage"))
      : nullptr;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (pre_translate && pre_translate(&message)) {
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  g_destroy(g_shell);
  g_shell = nullptr;
  FreeLibrary(g_shell_module);
  return static_cast<int>(message.wParam);
}
