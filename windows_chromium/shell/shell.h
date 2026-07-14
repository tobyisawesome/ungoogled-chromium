// Copyright 2026 The Windows Chromium Authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef WINDOWS_CHROMIUM_SHELL_SHELL_H_
#define WINDOWS_CHROMIUM_SHELL_SHELL_H_

#include <map>
#include <memory>
#include <string>

#include <windows.h>
#include <commctrl.h>

// Win32 defines GetCurrentTime as a macro for GetTickCount. That macro breaks
// the projected Microsoft.UI.Xaml.Media.Animation API of the same name.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "windows_chromium_shell_api.h"

namespace windows_chromium {

class Shell final {
 public:
  Shell(HWND parent, const WcsHostCallbacks& callbacks);
  Shell(const Shell&) = delete;
  Shell& operator=(const Shell&) = delete;
  ~Shell();

  HRESULT Update(const WcsWindowState& state);
  void SetVisible(bool visible);
  HRESULT Capture(const wchar_t* output_path);

 private:
  using Button = winrt::Microsoft::UI::Xaml::Controls::Button;
  using MenuFlyout = winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
  using TabViewItem = winrt::Microsoft::UI::Xaml::Controls::TabViewItem;

  void BuildVisualTree();
  void BuildToolbar();
  void BuildMenus();
  Button MakeGlyphButton(std::wstring_view glyph,
                         std::wstring_view tooltip,
                         WcsCommand command);
  winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem MakeMenuItem(
      std::wstring_view text,
      WcsCommand command,
      int64_t tab_id = -1);
  void Invoke(WcsCommand command,
              int64_t tab_id = -1,
              int32_t tab_index = -1,
              const wchar_t* text = nullptr,
              uint32_t event_flags = 0) const;
  void UpdateTabs(const WcsWindowState& state);
  void UpdateNativePage(std::wstring_view url);
  void ResizeIsland();
  void UpdateWindowRegion();
  void ApplySystemTheme();
  bool IsNativePage(std::wstring_view url) const;
  std::wstring NativePageTitle(std::wstring_view url) const;
  void AttachWindowSubclass();
  void DetachWindowSubclass();
  winrt::fire_and_forget CaptureAsync(std::wstring output_path);

  static LRESULT CALLBACK ParentSubclassProc(HWND window,
                                              UINT message,
                                              WPARAM wparam,
                                              LPARAM lparam,
                                              UINT_PTR subclass_id,
                                              DWORD_PTR reference_data);

  HWND parent_ = nullptr;
  HWND island_window_ = nullptr;
  WcsHostCallbacks callbacks_{};
  bool visible_ = true;
  bool updating_ = false;
  bool native_page_visible_ = false;
  int32_t active_index_ = -1;
  int64_t active_tab_id_ = -1;
  std::wstring active_url_;

  winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{
      nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TabView tab_view_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid toolbar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox address_box_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid native_page_host_{nullptr};
  Button back_button_{nullptr};
  Button forward_button_{nullptr};
  Button reload_button_{nullptr};
  Button profile_button_{nullptr};
  Button menu_button_{nullptr};
  MenuFlyout profile_menu_{nullptr};
  MenuFlyout app_menu_{nullptr};
  std::map<int64_t, TabViewItem> tab_items_;
};

}  // namespace windows_chromium

#endif  // WINDOWS_CHROMIUM_SHELL_SHELL_H_
