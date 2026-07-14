// Copyright 2026 The Windows Chromium Authors
// SPDX-License-Identifier: BSD-3-Clause

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
