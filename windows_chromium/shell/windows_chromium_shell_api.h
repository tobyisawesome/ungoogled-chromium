// Copyright 2026 The Curve Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef WINDOWS_CHROMIUM_SHELL_WINDOWS_CHROMIUM_SHELL_API_H_
#define WINDOWS_CHROMIUM_SHELL_WINDOWS_CHROMIUM_SHELL_API_H_

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#if defined(WINDOWS_CHROMIUM_SHELL_IMPLEMENTATION)
#define WCS_EXPORT __declspec(dllexport)
#else
#define WCS_EXPORT __declspec(dllimport)
#endif

#define WCS_API_VERSION 3u

typedef void* WcsShellHandle;

typedef enum WcsCommand {
  WCS_COMMAND_NEW_TAB = 1,
  WCS_COMMAND_ACTIVATE_TAB = 2,
  WCS_COMMAND_CLOSE_TAB = 3,
  WCS_COMMAND_DUPLICATE_TAB = 4,
  WCS_COMMAND_TOGGLE_PIN_TAB = 5,
  WCS_COMMAND_CLOSE_OTHER_TABS = 6,
  WCS_COMMAND_CLOSE_TABS_TO_RIGHT = 7,
  WCS_COMMAND_BACK = 10,
  WCS_COMMAND_FORWARD = 11,
  WCS_COMMAND_RELOAD = 12,
  WCS_COMMAND_STOP = 13,
  WCS_COMMAND_NAVIGATE = 14,
  WCS_COMMAND_HOME = 15,
  WCS_COMMAND_OPEN_HISTORY = 20,
  WCS_COMMAND_OPEN_DOWNLOADS = 21,
  WCS_COMMAND_OPEN_BOOKMARKS = 22,
  WCS_COMMAND_OPEN_EXTENSIONS = 23,
  WCS_COMMAND_OPEN_PASSWORDS = 24,
  WCS_COMMAND_OPEN_SETTINGS = 25,
  WCS_COMMAND_OPEN_PROFILES = 26,
  WCS_COMMAND_NEW_WINDOW = 30,
  WCS_COMMAND_NEW_INCOGNITO_WINDOW = 31,
  WCS_COMMAND_FIND = 32,
  WCS_COMMAND_PRINT = 33,
  WCS_COMMAND_SAVE_PAGE = 34,
  WCS_COMMAND_ZOOM_IN = 35,
  WCS_COMMAND_ZOOM_OUT = 36,
  WCS_COMMAND_ZOOM_RESET = 37,
  WCS_COMMAND_TOGGLE_FULLSCREEN = 38,
  WCS_COMMAND_OPEN_ABOUT = 39,
  WCS_COMMAND_EXIT = 40,
  WCS_COMMAND_SET_RESTORE_ON_STARTUP = 41,
  WCS_COMMAND_OPEN_SEARCH_SETTINGS = 42,
  WCS_COMMAND_BOOKMARK_PAGE = 43,
  WCS_COMMAND_SHOW_SITE_INFO = 44,
} WcsCommand;

typedef struct WcsCommandArgs {
  uint32_t size;
  WcsCommand command;
  int64_t tab_id;
  int32_t tab_index;
  const wchar_t* text;
  uint32_t event_flags;
} WcsCommandArgs;

typedef void(__stdcall* WcsInvokeCommand)(void* context,
                                          const WcsCommandArgs* args);

typedef struct WcsHostCallbacks {
  uint32_t size;
  uint32_t api_version;
  void* context;
  WcsInvokeCommand invoke_command;
} WcsHostCallbacks;

typedef struct WcsTabState {
  uint32_t size;
  int64_t tab_id;
  int32_t index;
  const wchar_t* title;
  const wchar_t* url;
  const wchar_t* favicon_url;
  int32_t active;
  int32_t pinned;
  int32_t loading;
  int32_t audible;
  int32_t muted;
} WcsTabState;

typedef struct WcsWindowState {
  uint32_t size;
  const WcsTabState* tabs;
  size_t tab_count;
  int32_t active_index;
  int32_t can_go_back;
  int32_t can_go_forward;
  int32_t is_incognito;
  const wchar_t* profile_name;
  int32_t restore_on_startup;
} WcsWindowState;

extern "C" {

WCS_EXPORT uint32_t __stdcall WcsGetApiVersion(void);
WCS_EXPORT HRESULT __stdcall WcsCreateShell(HWND parent,
                                            const WcsHostCallbacks* callbacks,
                                            WcsShellHandle* shell);
WCS_EXPORT void __stdcall WcsDestroyShell(WcsShellHandle shell);
WCS_EXPORT HRESULT __stdcall WcsUpdateWindowState(
    WcsShellHandle shell,
    const WcsWindowState* state);
WCS_EXPORT void __stdcall WcsSetVisible(WcsShellHandle shell, BOOL visible);
// Renders the live XAML visual tree to a PNG for visual regression testing.
// The operation is asynchronous; callers may watch for the output file.
WCS_EXPORT HRESULT __stdcall WcsCaptureShell(WcsShellHandle shell,
                                             const wchar_t* output_path);

}  // extern "C"

#endif  // WINDOWS_CHROMIUM_SHELL_WINDOWS_CHROMIUM_SHELL_API_H_
