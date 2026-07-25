// Copyright 2026 The Curve Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef WINDOWS_CHROMIUM_SHELL_SHELL_H_
#define WINDOWS_CHROMIUM_SHELL_SHELL_H_

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

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
  void ShowFindFlyout();
  void ShowBookmarkFlyout();
  void ShowSiteInfoFlyout();
  void ShowRestorePrompt();
  void ShowDefaultBrowserPrompt(bool can_pin_to_taskbar);
  void SetVisualStateForTesting(std::wstring_view state);
  HRESULT Capture(const wchar_t* output_path);
  int32_t ShowContextMenu(const WcsContextMenuItem* items,
                          size_t item_count,
                          int32_t screen_x,
                          int32_t screen_y);

 private:
  using ToolbarButton = winrt::Microsoft::UI::Xaml::Controls::Button;
  using MenuFlyout = winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
  using TabViewItem = winrt::Microsoft::UI::Xaml::Controls::TabViewItem;

  void BuildVisualTree();
  void BuildToolbar();
  void BuildMenus();
  void BuildSidebar();
  void ToggleSidebar();
  void ConfigureTitleBar();
  void ScheduleTabChromeUpdate();
  void UpdateTitleBarRegions();
  ToolbarButton MakeGlyphButton(std::wstring_view glyph,
                                std::wstring_view tooltip,
                                WcsCommand command,
                                bool invoke_on_click = true);
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
  void UpdateBookmarks(const WcsWindowState& state);
  void UpdateNativePageData(const WcsWindowState& state);
  void UpdateAddressSuggestions(std::wstring_view query);
  void UpdateAddressSecurityState(std::wstring_view url);
  void UpdateTabShoulders();
  void ToggleWorkAreaMaximize();
  void UpdateMaximizeGlyph();
  void UpdateNativePage(std::wstring_view url);
  void UpdateTitleBarMetrics();
  void ResizeIsland();
  void UpdateWindowRegion();
  void ApplySystemTheme();
  bool IsNativePage(std::wstring_view url) const;
  std::wstring NativePageTitle(std::wstring_view url) const;
  void AttachWindowSubclass();
  void DetachWindowSubclass();
  static winrt::fire_and_forget CaptureAsync(
      winrt::Microsoft::UI::Xaml::Controls::Grid root,
      std::wstring output_path);

  static LRESULT CALLBACK ParentSubclassProc(HWND window,
                                              UINT message,
                                              WPARAM wparam,
                                              LPARAM lparam,
                                              UINT_PTR subclass_id,
                                              DWORD_PTR reference_data);
  static LRESULT CALLBACK IslandSubclassProc(HWND window,
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
  bool address_editing_ = false;
  bool suppress_address_suggestions_ = false;
  bool address_submission_pending_ = false;
  bool native_page_visible_ = false;
  bool bookmark_bar_visible_ = false;
  bool sidebar_visible_ = false;
  bool active_tab_loading_ = false;
  bool restore_on_startup_ = false;
  bool tab_chrome_update_queued_ = false;
  bool new_tab_request_pending_ = false;
  bool tab_keyboard_focus_visible_ = false;
  bool island_hidden_for_window_transition_ = false;
  bool native_title_bar_suppressed_ = false;
  bool work_area_maximized_ = false;
  RECT restored_window_bounds_{};
  size_t last_tab_count_ = 0;
  int32_t active_index_ = -1;
  int64_t active_tab_id_ = -1;
  int64_t focused_tab_id_ = -1;
  int32_t pending_focus_tab_index_ = -1;
  int64_t pending_focus_tab_id_ = -1;
  std::wstring active_url_;
  std::wstring active_title_;
  std::wstring submitted_address_text_;
  bool active_page_bookmarked_ = false;

  struct CallbackLifetime {
    std::atomic_bool alive{true};
  };
  std::shared_ptr<CallbackLifetime> callback_lifetime_ =
      std::make_shared<CallbackLifetime>();

  struct NativeBookmark {
    std::wstring title;
    std::wstring url;
    bool is_folder = false;
  };
  struct NativeHistoryEntry {
    std::wstring title;
    std::wstring url;
    std::wstring visit_time;
  };
  struct NativeDownload {
    uint32_t id = 0;
    std::wstring title;
    std::wstring url;
    std::wstring target_path;
    std::wstring status;
    bool complete = false;
    bool in_progress = false;
  };
  std::vector<NativeBookmark> native_bookmarks_;
  std::vector<NativeHistoryEntry> native_history_;
  std::vector<NativeDownload> native_downloads_;
  bool history_loading_ = false;
  bool pending_restore_prompt_ = false;
  bool pending_default_browser_prompt_ = false;
  bool pending_default_browser_can_pin_ = false;

  winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{
      nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer
      focus_content_timer_{nullptr};
  winrt::Microsoft::UI::Windowing::AppWindowTitleBar title_bar_{nullptr};
  winrt::Microsoft::UI::Windowing::OverlappedPresenter
      overlapped_presenter_{nullptr};
  winrt::Microsoft::UI::Input::InputNonClientPointerSource
      non_client_pointer_source_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush selected_tab_fill_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TabView tab_view_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Canvas selected_tab_layer_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Path selected_tab_path_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Canvas tab_focus_layer_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border tab_focus_border_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer
      tab_scroll_viewer_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid toolbar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border bookmark_divider_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid bookmark_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel bookmark_items_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid sidebar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid caption_host_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox address_box_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid native_page_host_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Flyout find_flyout_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Flyout bookmark_flyout_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Flyout site_info_flyout_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox bookmark_title_box_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog
      restore_dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog
      default_browser_dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox find_box_{nullptr};
  ToolbarButton back_button_{nullptr};
  ToolbarButton forward_button_{nullptr};
  ToolbarButton reload_button_{nullptr};
  ToolbarButton security_button_{nullptr};
  ToolbarButton favorite_button_{nullptr};
  ToolbarButton profile_button_{nullptr};
  ToolbarButton menu_button_{nullptr};
  ToolbarButton minimize_button_{nullptr};
  ToolbarButton maximize_button_{nullptr};
  ToolbarButton close_button_{nullptr};
  MenuFlyout profile_menu_{nullptr};
  MenuFlyout app_menu_{nullptr};
  std::map<int64_t, TabViewItem> tab_items_;
  std::map<int64_t, std::wstring> tab_titles_;
  std::map<int64_t, std::wstring> tab_favicon_urls_;
  std::set<int64_t> tab_audio_icons_;
  std::map<int64_t, bool> tab_audio_muted_;
  std::vector<std::wstring> tab_suggestions_;
  std::map<int64_t, winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>
      tab_pin_menu_items_;
};

}  // namespace windows_chromium

#endif  // WINDOWS_CHROMIUM_SHELL_SHELL_H_
