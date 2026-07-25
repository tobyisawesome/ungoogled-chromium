// Copyright 2026 The Curve Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "pch.h"

#include "shell.h"

#include "App.xaml.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string_view>
#include <utility>

#include <dwmapi.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/base.h>

namespace {

using winrt::Microsoft::UI::Xaml::CornerRadius;
using winrt::Microsoft::UI::Xaml::ElementTheme;
using winrt::Microsoft::UI::Xaml::GridLength;
using winrt::Microsoft::UI::Xaml::GridUnitType;
using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
using winrt::Microsoft::UI::Xaml::Thickness;
using winrt::Microsoft::UI::Xaml::VerticalAlignment;
using winrt::Microsoft::UI::Xaml::Visibility;
using winrt::Microsoft::UI::Xaml::Media::Brush;
using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;
using namespace winrt::Microsoft::UI::Xaml::Controls;

constexpr double kTabRowHeight = 48.0;
constexpr double kTabItemHeight = 40.0;
constexpr double kToolbarHeight = 48.0;
constexpr double kBookmarkBarHeight = 36.0;
constexpr double kSidebarWidth = 320.0;
constexpr double kShellHeight = kTabRowHeight + kToolbarHeight;
constexpr double kOmniboxButtonWidth = 32.0;
constexpr double kOmniboxButtonHeight = 28.0;
constexpr double kOmniboxButtonCornerRadius = 4.0;
constexpr UINT_PTR kParentSubclassId = 0x57435331;  // "WCS1"
constexpr UINT_PTR kDeferredResizeTimerId = 0x57435332;  // "WCS2"
constexpr UINT_PTR kIslandSubclassId = 0x57435333;  // "WCS3"
constexpr UINT kDeferredResizeDelayMs = 200;

winrt::Microsoft::UI::Xaml::DependencyObject FindNamedDescendant(
    const winrt::Microsoft::UI::Xaml::DependencyObject& root,
    std::wstring_view name) {
  using winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper;
  const int count = VisualTreeHelper::GetChildrenCount(root);
  for (int index = 0; index < count; ++index) {
    const auto child = VisualTreeHelper::GetChild(root, index);
    const auto element = child.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
    if (element && element.Name() == name) {
      return child;
    }
    if (const auto nested = FindNamedDescendant(child, name)) {
      return nested;
    }
  }
  return nullptr;
}

void EnsureSelfContainedRuntimeLoaded();

struct ModuleActivationContext {
  ModuleActivationContext() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&kParentSubclassId), &module)) {
      winrt::throw_last_error();
    }

    ACTCTXW context{};
    context.cbSize = sizeof(context);
    context.dwFlags =
        ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = module;
    context.lpResourceName = MAKEINTRESOURCEW(2);
    handle = CreateActCtxW(&context);
    if (handle == INVALID_HANDLE_VALUE) {
      winrt::throw_last_error();
    }
    if (!ActivateActCtx(handle, &cookie)) {
      const DWORD error = GetLastError();
      ReleaseActCtx(handle);
      handle = INVALID_HANDLE_VALUE;
      winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
  }

  ~ModuleActivationContext() {
    if (cookie) {
      DeactivateActCtx(0, cookie);
    }
    if (handle != INVALID_HANDLE_VALUE) {
      ReleaseActCtx(handle);
    }
  }

  ModuleActivationContext(const ModuleActivationContext&) = delete;
  ModuleActivationContext& operator=(const ModuleActivationContext&) = delete;

  HANDLE handle = INVALID_HANDLE_VALUE;
  ULONG_PTR cookie = 0;
};

struct ThreadRuntime {
  ThreadRuntime() {
    EnsureSelfContainedRuntimeLoaded();
    try {
      winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (const winrt::hresult_error& error) {
      if (error.code() != RPC_E_CHANGED_MODE) {
        throw;
      }
    }

    try {
      dispatcher =
          winrt::Microsoft::UI::Dispatching::DispatcherQueueController::
              CreateOnCurrentThread();
    } catch (const winrt::hresult_error& error) {
      std::fwprintf(stderr, L"DispatcherQueue initialization failed: 0x%08X\n",
                    static_cast<unsigned int>(error.code().value));
      throw;
    }
    try {
      application = winrt::make<
          winrt::CurveBrowserShell::implementation::App>();
    } catch (const winrt::hresult_error& error) {
      std::fwprintf(stderr,
                    L"WinUI Application initialization failed: 0x%08X\n",
                    static_cast<unsigned int>(error.code().value));
      throw;
    }
  }

  ModuleActivationContext activation_context;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueController dispatcher{
      nullptr};
  winrt::Windows::Foundation::IInspectable application{nullptr};
};

thread_local std::unique_ptr<ThreadRuntime> g_thread_runtime;
using ContentPreTranslateMessage = BOOL(WINAPI*)(MSG*);
thread_local HHOOK g_message_hook = nullptr;
thread_local ContentPreTranslateMessage g_pre_translate_message = nullptr;

LRESULT CALLBACK XamlMessageHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0 && wparam == PM_REMOVE && g_pre_translate_message) {
    auto* message = reinterpret_cast<MSG*>(lparam);
    if (message && message->message != WM_NULL &&
        g_pre_translate_message(message)) {
      // WH_GETMESSAGE cannot prevent dispatch by its return value. Replacing a
      // handled message with WM_NULL is the documented hook pattern and keeps
      // Chromium from dispatching it a second time after WinUI consumes it.
      message->message = WM_NULL;
      message->hwnd = nullptr;
      message->wParam = 0;
      message->lParam = 0;
    }
  }
  return CallNextHookEx(g_message_hook, code, wparam, lparam);
}

void EnsureXamlMessageTranslation() {
  if (g_message_hook) {
    return;
  }
  HMODULE windowing = GetModuleHandleW(L"Microsoft.UI.Windowing.Core.dll");
  if (!windowing) {
    return;
  }
  g_pre_translate_message = reinterpret_cast<ContentPreTranslateMessage>(
      GetProcAddress(windowing, "ContentPreTranslateMessage"));
  if (!g_pre_translate_message) {
    return;
  }
  g_message_hook = SetWindowsHookExW(WH_GETMESSAGE, &XamlMessageHook, nullptr,
                                     GetCurrentThreadId());
  if (!g_message_hook) {
    g_pre_translate_message = nullptr;
    winrt::throw_last_error();
  }
}

void EnsureSelfContainedRuntimeLoaded() {
  static HMODULE runtime = [] {
    HMODULE module = LoadLibraryExW(L"Microsoft.WindowsAppRuntime.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                       LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      winrt::throw_last_error();
    }
    using EnsureIsLoaded = HRESULT(STDAPICALLTYPE*)();
    const auto ensure = reinterpret_cast<EnsureIsLoaded>(
        GetProcAddress(module, "WindowsAppRuntime_EnsureIsLoaded"));
    if (ensure) {
      winrt::check_hresult(ensure());
    }
    return module;
  }();
  (void)runtime;
}

void EnsureThreadRuntime() {
  if (!g_thread_runtime) {
    g_thread_runtime = std::make_unique<ThreadRuntime>();
  }
}

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (size_t index = 0; index < prefix.size(); ++index) {
    if (std::towlower(value[index]) != std::towlower(prefix[index])) {
      return false;
    }
  }
  return true;
}

bool EqualsInsensitive(std::wstring_view value, std::wstring_view expected) {
  return value.size() == expected.size() &&
         StartsWithInsensitive(value, expected);
}

winrt::Windows::UI::Color Color(uint8_t red,
                                uint8_t green,
                                uint8_t blue,
                                uint8_t alpha = 255) {
  return winrt::Windows::UI::Color{alpha, red, green, blue};
}

CornerRadius UniformCornerRadius(double radius) {
  return CornerRadius{radius, radius, radius, radius};
}

Thickness UniformThickness(double value) {
  return Thickness{value, value, value, value};
}

winrt::Microsoft::UI::Xaml::Media::PathGeometry TabSilhouetteGeometry(
    double width,
    double height,
    const CornerRadius& radius) {
  using winrt::Microsoft::UI::Xaml::Media::ArcSegment;
  using winrt::Microsoft::UI::Xaml::Media::LineSegment;
  using winrt::Microsoft::UI::Xaml::Media::PathFigure;
  using winrt::Microsoft::UI::Xaml::Media::PathGeometry;
  using winrt::Microsoft::UI::Xaml::Media::SweepDirection;
  using winrt::Windows::Foundation::Point;
  using winrt::Windows::Foundation::Size;

  // This is the same continuous silhouette generated by WinUI's
  // TabViewItem::UpdateTabGeometry: fixed 4-DIP outward shoulders and the
  // theme's independent upper-left and upper-right radii.
  const double left_radius =
      std::clamp(radius.TopLeft, 0.0, std::max(0.0, height - 4.0));
  const double right_radius =
      std::clamp(radius.TopRight, 0.0, std::max(0.0, height - 4.0));
  const double body_width =
      std::max(0.0, width - left_radius - right_radius);

  const auto append_line = [](PathFigure& figure, double x, double y) {
    LineSegment line;
    line.Point(Point{static_cast<float>(x), static_cast<float>(y)});
    figure.Segments().Append(line);
  };
  const auto append_arc = [](PathFigure& figure,
                             double x,
                             double y,
                             double arc_radius,
                             SweepDirection direction) {
    ArcSegment arc;
    arc.Point(Point{static_cast<float>(x), static_cast<float>(y)});
    arc.Size(Size{static_cast<float>(arc_radius),
                  static_cast<float>(arc_radius)});
    arc.SweepDirection(direction);
    arc.IsLargeArc(false);
    figure.Segments().Append(arc);
  };

  PathFigure figure;
  figure.StartPoint(Point{0, static_cast<float>(height)});
  append_arc(figure, 4.0, height - 4.0, 4.0,
             SweepDirection::Counterclockwise);
  append_line(figure, 4.0, left_radius);
  append_arc(figure, 4.0 + left_radius, 0.0, left_radius,
             SweepDirection::Clockwise);
  append_line(figure, 4.0 + left_radius + body_width, 0.0);
  append_arc(figure, 4.0 + width, right_radius, right_radius,
             SweepDirection::Clockwise);
  append_line(figure, 4.0 + width, height - 4.0);
  append_arc(figure, 8.0 + width, height, 4.0,
             SweepDirection::Counterclockwise);
  figure.IsClosed(true);

  PathGeometry geometry;
  geometry.Figures().Append(figure);
  return geometry;
}

FontIcon Glyph(std::wstring_view glyph, double size = 16.0) {
  FontIcon icon;
  icon.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{
      L"Segoe Fluent Icons, Segoe MDL2 Assets"});
  icon.Glyph(glyph);
  icon.FontSize(size);
  return icon;
}

FontIcon ToolbarGlyph(std::wstring_view glyph) {
  // Let WinUI center the glyph inside the compact pointer-state surface. A
  // manual baseline shift moves the ink away from the Button's actual center
  // and makes the hover shape appear offset even when its bounds are correct.
  return Glyph(glyph, 14.0);
}

FontIcon OmniboxGlyph(std::wstring_view glyph) {
  auto icon = Glyph(glyph, 12.0);
  icon.IsTextScaleFactorEnabled(false);
  return icon;
}

Brush ThemeBrush(std::wstring_view key,
                 winrt::Windows::UI::Color fallback) {
  try {
    const auto value =
        winrt::Microsoft::UI::Xaml::Application::Current()
            .Resources()
            .Lookup(winrt::box_value(winrt::hstring{key}));
    if (const auto brush = value.try_as<Brush>()) {
      return brush;
    }
  } catch (...) {
  }
  return SolidColorBrush{fallback};
}

void KeepNativeCaptionButtonsTransparent(
    const winrt::Microsoft::UI::Windowing::AppWindowTitleBar& title_bar) {
  if (!title_bar) {
    return;
  }
  const auto color_reference = [](winrt::Windows::UI::Color value) {
    return winrt::box_value(value)
        .as<winrt::Windows::Foundation::IReference<
            winrt::Windows::UI::Color>>();
  };
  const auto transparent = color_reference(Color(0, 0, 0, 0));
  title_bar.ButtonBackgroundColor(transparent);
  title_bar.ButtonInactiveBackgroundColor(transparent);
  title_bar.ButtonHoverBackgroundColor(transparent);
  title_bar.ButtonPressedBackgroundColor(transparent);
  title_bar.ButtonForegroundColor(transparent);
  title_bar.ButtonInactiveForegroundColor(transparent);
}

std::wstring StripMenuMnemonics(const wchar_t* source) {
  std::wstring result;
  if (!source) {
    return result;
  }
  for (size_t index = 0; source[index]; ++index) {
    if (source[index] != L'&') {
      result.push_back(source[index]);
      continue;
    }
    if (source[index + 1] == L'&') {
      result.push_back(L'&');
      ++index;
    }
  }
  return result;
}

std::wstring_view ContextMenuGlyph(std::wstring_view label) {
  if (label == L"Back") return L"\uE72B";
  if (label == L"Forward") return L"\uE72A";
  if (label == L"Reload") return L"\uE72C";
  if (label.starts_with(L"Save")) return L"\uE74E";
  if (label.starts_with(L"Print")) return L"\uE749";
  if (label.starts_with(L"Cast")) return L"\uE8A9";
  if (label.find(L"reading mode") != std::wstring_view::npos)
    return L"\uE736";
  if (label.find(L"QR Code") != std::wstring_view::npos) return L"\uED14";
  if (label.find(L"source") != std::wstring_view::npos) return L"\uE943";
  if (label.find(L"Inspect") != std::wstring_view::npos) return L"\uE9D9";
  if (label == L"Cut") return L"\uE8C6";
  if (label == L"Copy") return L"\uE8C8";
  if (label == L"Paste") return L"\uE77F";
  if (label.starts_with(L"Open link")) return L"\uE8A7";
  return {};
}

FontIcon ContextMenuIcon(std::wstring_view glyph) {
  FontIcon icon;
  icon.Glyph(glyph);
  icon.FontFamily(
      winrt::Microsoft::UI::Xaml::Media::FontFamily{L"Segoe Fluent Icons"});
  icon.FontSize(16);
  return icon;
}

winrt::Microsoft::UI::Xaml::DependencyObject FindVisualChildByName(
    const winrt::Microsoft::UI::Xaml::DependencyObject& root,
    std::wstring_view name) {
  using winrt::Microsoft::UI::Xaml::FrameworkElement;
  using winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper;
  if (!root) {
    return nullptr;
  }
  const int count = VisualTreeHelper::GetChildrenCount(root);
  for (int index = 0; index < count; ++index) {
    const auto child = VisualTreeHelper::GetChild(root, index);
    if (const auto element = child.try_as<FrameworkElement>();
        element && element.Name() == name) {
      return child;
    }
    if (const auto match = FindVisualChildByName(child, name)) {
      return match;
    }
  }
  return nullptr;
}

Border MakeSettingsCard(std::wstring_view title,
                        std::wstring_view description,
                        const winrt::Microsoft::UI::Xaml::FrameworkElement& trailing) {
  Border card;
  card.CornerRadius(UniformCornerRadius(8));
  card.Padding(Thickness{16, 12, 12, 12});
  card.Margin(Thickness{0, 0, 0, 8});
  card.BorderThickness(UniformThickness(1));

  Grid content;
  ColumnDefinition text_column;
  text_column.Width(GridLength{1, GridUnitType::Star});
  content.ColumnDefinitions().Append(text_column);
  ColumnDefinition control_column;
  control_column.Width(GridLength{0, GridUnitType::Auto});
  content.ColumnDefinitions().Append(control_column);

  StackPanel labels;
  TextBlock heading;
  heading.Text(title);
  heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  labels.Children().Append(heading);
  TextBlock details;
  details.Text(description);
  details.Opacity(0.72);
  details.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
  details.Margin(Thickness{0, 3, 18, 0});
  labels.Children().Append(details);
  content.Children().Append(labels);

  Grid::SetColumn(trailing, 1);
  trailing.VerticalAlignment(VerticalAlignment::Center);
  content.Children().Append(trailing);
  card.Child(content);
  return card;
}

}  // namespace

namespace windows_chromium {

Shell::Shell(HWND parent, const WcsHostCallbacks& callbacks)
    : parent_(parent), callbacks_(callbacks) {
  if (!parent_ || !IsWindow(parent_)) {
    winrt::throw_hresult(E_INVALIDARG);
  }

  EnsureThreadRuntime();
  ConfigureTitleBar();
  BuildVisualTree();

  xaml_source_ =
      winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource{};
  xaml_source_.Initialize(
      winrt::Microsoft::UI::GetWindowIdFromWindow(parent_));
  xaml_source_.GotFocus(
      [](const winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource&
             source,
         const winrt::Microsoft::UI::Xaml::Hosting::
             DesktopWindowXamlSourceGotFocusEventArgs& args) {
        // DesktopWindowXamlSource notifies its Win32 host of pointer focus but
        // does not automatically move keyboard focus into the XAML tree.
        // Complete the request so typing targets the clicked WinUI control
        // instead of Chromium's hidden Views omnibox.
        source.NavigateFocus(args.Request());
      });
  const auto callback_lifetime = callback_lifetime_;
  xaml_source_.TakeFocusRequested(
      [this, callback_lifetime](
          const winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource&,
          const winrt::Microsoft::UI::Xaml::Hosting::
              DesktopWindowXamlSourceTakeFocusRequestedEventArgs&) {
        if (!callback_lifetime->alive.load(std::memory_order_acquire)) {
          return;
        }
        Invoke(WCS_COMMAND_FOCUS_CONTENT, active_tab_id_, active_index_);
        SetFocus(parent_);
      });
  xaml_source_.Content(root_);
  winrt::Microsoft::UI::Xaml::Media::MicaBackdrop mica_alt;
  mica_alt.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
  xaml_source_.SystemBackdrop(mica_alt);
  island_window_ = winrt::Microsoft::UI::GetWindowFromWindowId(
      xaml_source_.SiteBridge().WindowId());
  EnsureXamlMessageTranslation();

  LONG_PTR style = GetWindowLongPtrW(island_window_, GWL_STYLE);
  SetWindowLongPtrW(island_window_, GWL_STYLE,
                    style | WS_CHILD | WS_VISIBLE | WS_TABSTOP);
  winrt::check_bool(SetWindowSubclass(
      island_window_, &Shell::IslandSubclassProc, kIslandSubclassId,
      reinterpret_cast<DWORD_PTR>(this)) != FALSE);
  AttachWindowSubclass();
  ApplySystemTheme();
  ResizeIsland();
  UpdateWindowRegion();

  // Mica Alt is the system backdrop intended for windows with tabbed title
  // bars. Older Windows versions safely ignore this window attribute.
  const DWM_SYSTEMBACKDROP_TYPE backdrop_type = DWMSBT_TABBEDWINDOW;
  DwmSetWindowAttribute(parent_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop_type,
                        sizeof(backdrop_type));
  SetPropW(parent_, L"CurveBrowserNativeShellActive",
           reinterpret_cast<HANDLE>(this));
}

Shell::~Shell() {
  // DispatcherQueue callbacks run on the XAML thread but may remain queued
  // after Chromium releases its shell handle. Keep their detached lifetime
  // token alive and invalidate it before any member or HWND teardown.
  callback_lifetime_->alive.store(false, std::memory_order_release);
  if (focus_content_timer_) {
    focus_content_timer_.Stop();
  }
  if (parent_ && IsWindow(parent_)) {
    KillTimer(parent_, kDeferredResizeTimerId);
    RemovePropW(parent_, L"CurveBrowserNativeShellActive");
  }
  DetachWindowSubclass();
  if (island_window_ && IsWindow(island_window_)) {
    RemoveWindowSubclass(island_window_, &Shell::IslandSubclassProc,
                         kIslandSubclassId);
  }
  if (non_client_pointer_source_) {
    non_client_pointer_source_.ClearAllRegionRects();
  }
  // AppWindow is owned by Chromium's HWND. Reconfiguring its presenter while
  // that HWND is already in BrowserView teardown re-enters the native caption
  // provider and can fail-fast inside Microsoft.UI.Xaml. Stock-shell fallback
  // occurs before a Shell is retained, so normal destruction only releases
  // these projections without mutating the dying window.
  non_client_pointer_source_ = nullptr;
  overlapped_presenter_ = nullptr;
  title_bar_ = nullptr;
  if (xaml_source_) {
    xaml_source_.Content(nullptr);
    xaml_source_.Close();
  }
}

void Shell::ConfigureTitleBar() {
  using namespace winrt::Microsoft::UI::Windowing;
  if (!AppWindowTitleBar::IsCustomizationSupported()) {
    return;
  }

  const auto window_id = winrt::Microsoft::UI::GetWindowIdFromWindow(parent_);
  const auto app_window = AppWindow::GetFromWindowId(window_id);
  title_bar_ = app_window.TitleBar();
  overlapped_presenter_ =
      app_window.Presenter().try_as<OverlappedPresenter>();
  if (overlapped_presenter_) {
    try {
      // Keep the native resize border, but remove AppWindow's 32-DIP caption
      // provider completely. ExtendsContentIntoTitleBar merely makes that
      // provider transparent; it remains an independent UIA and hit target
      // beneath Curve's full-height WinUI caption buttons.
      overlapped_presenter_.SetBorderAndTitleBar(true, false);
      native_title_bar_suppressed_ = true;
    } catch (...) {
      native_title_bar_suppressed_ = false;
    }
  }
  if (!native_title_bar_suppressed_) {
    // Conservative fallback for Windows App SDK versions that cannot change
    // the presenter contract of Chromium's existing HWND.
    title_bar_.ExtendsContentIntoTitleBar(true);
    title_bar_.PreferredHeightOption(TitleBarHeightOption::Standard);
  }
  non_client_pointer_source_ =
      winrt::Microsoft::UI::Input::InputNonClientPointerSource::
          GetForWindowId(window_id);
}

void Shell::BuildVisualTree() {
  root_ = Grid{};
  ColumnDefinition content_column;
  content_column.Width(GridLength{1, GridUnitType::Star});
  root_.ColumnDefinitions().Append(content_column);
  ColumnDefinition sidebar_column;
  sidebar_column.Width(GridLength{0, GridUnitType::Pixel});
  root_.ColumnDefinitions().Append(sidebar_column);
  RowDefinition tabs_row;
  tabs_row.Height(GridLength{kTabRowHeight, GridUnitType::Pixel});
  root_.RowDefinitions().Append(tabs_row);
  RowDefinition toolbar_row;
  toolbar_row.Height(GridLength{kToolbarHeight, GridUnitType::Pixel});
  root_.RowDefinitions().Append(toolbar_row);
  RowDefinition bookmark_row;
  bookmark_row.Height(GridLength{0, GridUnitType::Pixel});
  root_.RowDefinitions().Append(bookmark_row);
  RowDefinition page_row;
  page_row.Height(GridLength{1, GridUnitType::Star});
  root_.RowDefinitions().Append(page_row);

  tab_view_ = TabView{};
  tab_view_.IsAddTabButtonVisible(true);
  // WinUI TabView does not have a browser-window tear-out contract. Enabling
  // its generic drag source let a pointer leave the island while Chromium was
  // synchronizing the same item, which could tear down a live TabViewItem and
  // crash the process. Keep tab drag disabled until native window tear-out is
  // implemented deliberately.
  tab_view_.CanDragTabs(false);
  tab_view_.CanReorderTabs(false);
  tab_view_.TabWidthMode(TabViewWidthMode::SizeToContent);
  // The custom title bar uses a 48-DIP row so the caption buttons span its
  // complete height. A 40-DIP tab item leaves Explorer's 8-DIP top inset while
  // its selected item and shoulders sink directly into the command layer.
  tab_view_.Height(kTabRowHeight);
  tab_view_.Resources().Insert(
      winrt::box_value(
          winrt::hstring{L"TabViewItemAddButtonContainerPadding"}),
      winrt::box_value(Thickness{3, 0, 0, 8}));
  double caption_width = 138.0;
  if (title_bar_ && title_bar_.RightInset() > 0) {
    caption_width =
        title_bar_.RightInset() * 96.0 / GetDpiForWindow(parent_);
  }
  // TabView contributes the stock 4-DIP leading header. Do not add another
  // outer inset: Explorer's first shoulder begins 4 DIPs from the client edge
  // and the selected body begins at 8 DIPs.
  tab_view_.Margin(Thickness{0, 0, caption_width, 0});
  tab_view_.VerticalAlignment(VerticalAlignment::Bottom);
  Grid::SetRow(tab_view_, 0);
  Grid::SetColumnSpan(tab_view_, 2);
  Canvas::SetZIndex(tab_view_, 2);
  root_.Children().Append(tab_view_);

  // DesktopWindowXamlSource clips TabViewItem's native -4 DIP overhang at the
  // item boundary. Host the same one-piece geometry in the root row, behind
  // TabView's content, so both shoulders remain visible without overlapping
  // translucent body/connector primitives.
  selected_tab_layer_ = Canvas{};
  selected_tab_layer_.IsHitTestVisible(false);
  selected_tab_layer_.HorizontalAlignment(HorizontalAlignment::Stretch);
  selected_tab_layer_.VerticalAlignment(VerticalAlignment::Stretch);
  Grid::SetRow(selected_tab_layer_, 0);
  Grid::SetColumnSpan(selected_tab_layer_, 2);
  Canvas::SetZIndex(selected_tab_layer_, 1);
  selected_tab_path_ = winrt::Microsoft::UI::Xaml::Shapes::Path{};
  selected_tab_path_.Visibility(Visibility::Collapsed);
  selected_tab_layer_.Children().Append(selected_tab_path_);
  root_.Children().Append(selected_tab_layer_);

  // The stock rectangular focus visual spans the tab's bottom edge and reads
  // as an underline against the toolbar. Preserve a strong keyboard cue, but
  // render it as an inset rounded ring above the connection seam.
  tab_focus_layer_ = Canvas{};
  tab_focus_layer_.IsHitTestVisible(false);
  tab_focus_layer_.HorizontalAlignment(HorizontalAlignment::Stretch);
  tab_focus_layer_.VerticalAlignment(VerticalAlignment::Stretch);
  Grid::SetRow(tab_focus_layer_, 0);
  Grid::SetColumnSpan(tab_focus_layer_, 2);
  Canvas::SetZIndex(tab_focus_layer_, 4);
  tab_focus_border_ = Border{};
  tab_focus_border_.IsHitTestVisible(false);
  tab_focus_border_.Background(SolidColorBrush{Color(0, 0, 0, 0)});
  tab_focus_border_.BorderBrush(
      ThemeBrush(L"FocusStrokeColorOuterBrush", Color(255, 255, 255)));
  tab_focus_border_.BorderThickness(UniformThickness(2));
  tab_focus_border_.CornerRadius(UniformCornerRadius(6));
  tab_focus_border_.Visibility(Visibility::Collapsed);
  tab_focus_layer_.Children().Append(tab_focus_border_);
  root_.Children().Append(tab_focus_layer_);

  tab_view_.Loaded([this](const auto&, const auto&) {
    tab_view_.ApplyTemplate();
    // Follow horizontal bring-into-view and overflow scrolling. Because the
    // silhouette is intentionally outside the item clip, refresh its root
    // position whenever the list viewport moves.
    if (!tab_scroll_viewer_) {
      tab_scroll_viewer_ =
          FindVisualChildByName(tab_view_, L"ScrollViewer")
              .try_as<ScrollViewer>();
      if (tab_scroll_viewer_) {
        tab_scroll_viewer_.ViewChanged(
            [this](const auto&, const auto&) {
              ScheduleTabChromeUpdate();
            });
      }
    }
    ScheduleTabChromeUpdate();
  });
  tab_view_.SizeChanged(
      [this](const auto&, const auto&) {
        ScheduleTabChromeUpdate();
      });

  // The overlapped presenter keeps the resize border but has no native title
  // bar. This single WinUI layer therefore owns both visible caption geometry
  // and input, with no smaller AppWindow buttons underneath it.
  caption_host_ = Grid{};
  caption_host_.Width(caption_width);
  caption_host_.Height(kTabRowHeight);
  caption_host_.HorizontalAlignment(HorizontalAlignment::Right);
  caption_host_.VerticalAlignment(VerticalAlignment::Top);
  Grid::SetRow(caption_host_, 0);
  Grid::SetColumnSpan(caption_host_, 2);
  Canvas::SetZIndex(caption_host_, 10);
  for (int index = 0; index < 3; ++index) {
    ColumnDefinition column;
    column.Width(GridLength{1, GridUnitType::Star});
    caption_host_.ColumnDefinitions().Append(column);
  }
  const auto make_caption_button = [](std::wstring_view glyph,
                                      std::wstring_view name) {
    Button button;
    button.Content(Glyph(glyph, 10));
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    button.VerticalAlignment(VerticalAlignment::Stretch);
    button.BorderThickness(UniformThickness(0));
    button.CornerRadius(UniformCornerRadius(0));
    button.Padding(UniformThickness(0));
    button.Background(SolidColorBrush{Color(0, 0, 0, 0)});
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        button, name);
    return button;
  };
  minimize_button_ = make_caption_button(L"\uE921", L"Minimize");
  maximize_button_ = make_caption_button(L"\uE922", L"Maximize");
  close_button_ = make_caption_button(L"\uE8BB", L"Close");
  close_button_.Resources().Insert(
      winrt::box_value(winrt::hstring{L"ButtonBackgroundPointerOver"}),
      SolidColorBrush{Color(196, 43, 28)});
  close_button_.Resources().Insert(
      winrt::box_value(winrt::hstring{L"ButtonBackgroundPressed"}),
      SolidColorBrush{Color(162, 29, 17)});
  Grid::SetColumn(minimize_button_, 0);
  Grid::SetColumn(maximize_button_, 1);
  Grid::SetColumn(close_button_, 2);
  minimize_button_.Click(
      [this](const auto&, const auto&) { ShowWindow(parent_, SW_MINIMIZE); });
  maximize_button_.Click(
      [this](const auto&, const auto&) { ToggleWorkAreaMaximize(); });
  close_button_.Click(
      [this](const auto&, const auto&) { PostMessageW(parent_, WM_CLOSE, 0, 0); });
  caption_host_.Children().Append(minimize_button_);
  caption_host_.Children().Append(maximize_button_);
  caption_host_.Children().Append(close_button_);
  root_.Children().Append(caption_host_);

  tab_view_.AddTabButtonClick([this](const TabView&, const auto&) {
    if (updating_ || new_tab_request_pending_) {
      return;
    }
    new_tab_request_pending_ = true;
    address_editing_ = false;
    const auto callback_lifetime = callback_lifetime_;
    if (!root_.DispatcherQueue().TryEnqueue([this, callback_lifetime] {
          if (!callback_lifetime->alive.load(std::memory_order_acquire)) {
            return;
          }
          if (!parent_ || !IsWindow(parent_)) {
            new_tab_request_pending_ = false;
            return;
          }
          Invoke(WCS_COMMAND_NEW_TAB);
        })) {
      new_tab_request_pending_ = false;
    }
  });
  tab_view_.SelectionChanged([this](const auto&, const auto&) {
    ScheduleTabChromeUpdate();
    if (updating_) {
      return;
    }
    const auto selected = tab_view_.SelectedItem().try_as<TabViewItem>();
    if (selected && selected.Tag()) {
      address_editing_ = false;
      Invoke(WCS_COMMAND_ACTIVATE_TAB,
             winrt::unbox_value<int64_t>(selected.Tag()));
    }
  });
  tab_view_.TabCloseRequested(
      [this](const TabView&,
             const winrt::Microsoft::UI::Xaml::Controls::
                 TabViewTabCloseRequestedEventArgs& args) {
        const auto item = args.Item().try_as<TabViewItem>();
        if (item && item.Tag()) {
          Invoke(WCS_COMMAND_CLOSE_TAB,
                 winrt::unbox_value<int64_t>(item.Tag()));
        }
      });
  BuildToolbar();
  BuildMenus();
  BuildSidebar();

  bookmark_bar_ = Grid{};
  // Keep the row itself flush with the client edges so its bottom divider is
  // the exact boundary between browser chrome and web content.  Only the
  // scrollable bookmark contents receive the File Explorer-style inset.
  bookmark_bar_.Padding(UniformThickness(0));
  bookmark_bar_.Visibility(Visibility::Collapsed);
  Grid::SetRow(bookmark_bar_, 2);
  Grid::SetColumnSpan(bookmark_bar_, 2);
  ScrollViewer bookmark_scroll;
  bookmark_scroll.Margin(Thickness{8, 2, 8, 2});
  bookmark_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
  bookmark_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
  bookmark_scroll.HorizontalScrollMode(ScrollMode::Enabled);
  bookmark_items_ = StackPanel{};
  bookmark_items_.Orientation(Orientation::Horizontal);
  bookmark_items_.Spacing(2);
  bookmark_scroll.Content(bookmark_items_);
  bookmark_bar_.Children().Append(bookmark_scroll);
  bookmark_divider_ = Border{};
  bookmark_divider_.Height(1);
  bookmark_divider_.HorizontalAlignment(HorizontalAlignment::Stretch);
  bookmark_divider_.VerticalAlignment(VerticalAlignment::Bottom);
  bookmark_divider_.IsHitTestVisible(false);
  Canvas::SetZIndex(bookmark_divider_, 20);
  bookmark_bar_.Children().Append(bookmark_divider_);
  root_.Children().Append(bookmark_bar_);

  native_page_host_ = Grid{};
  native_page_host_.Visibility(Visibility::Collapsed);
  Grid::SetRow(native_page_host_, 3);
  Grid::SetColumnSpan(native_page_host_, 2);
  root_.Children().Append(native_page_host_);
}

Shell::ToolbarButton Shell::MakeGlyphButton(std::wstring_view glyph,
                                            std::wstring_view tooltip,
                                            WcsCommand command,
                                            bool invoke_on_click) {
  Button button;
  button.Content(ToolbarGlyph(glyph));
  // AutoSuggestBox uses a compact inset pointer-state plate rather than
  // filling its 32 px text-control row. Match that visual rhythm while the
  // surrounding 40 px grid cell retains a comfortable click spacing.
  button.Width(30);
  button.Height(24);
  button.HorizontalAlignment(HorizontalAlignment::Center);
  button.VerticalAlignment(VerticalAlignment::Center);
  button.Padding(UniformThickness(0));
  button.Margin(UniformThickness(0));
  button.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
      Color(0, 0, 0, 0)});
  button.BorderThickness(UniformThickness(0));
  button.CornerRadius(UniformCornerRadius(4));
  ToolTipService::SetToolTip(button, winrt::box_value(tooltip));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      button, winrt::hstring{tooltip});
  if (invoke_on_click) {
    button.Click(
        [this, command](const auto&, const auto&) { Invoke(command); });
  }
  return button;
}

void Shell::BuildToolbar() {
  toolbar_ = Grid{};
  // The 40 px controls plus 4 px above and below exactly fill this 48 px
  // commanding row. Keeping that arithmetic exact prevents XAML from
  // compressing or clipping the icon controls.
  toolbar_.Padding(Thickness{8, 4, 8, 4});
  // The stock Button disabled visual has a filled plate. Explorer-style
  // navigation glyphs remain backgroundless when unavailable.
  const auto transparent = SolidColorBrush{Color(0, 0, 0, 0)};
  toolbar_.Resources().Insert(
      winrt::box_value(winrt::hstring{L"ButtonBackgroundDisabled"}),
      transparent);
  toolbar_.Resources().Insert(
      winrt::box_value(winrt::hstring{L"ButtonBorderBrushDisabled"}),
      transparent);
  Grid::SetRow(toolbar_, 1);
  Grid::SetColumnSpan(toolbar_, 2);
  root_.Children().Append(toolbar_);

  const double button_width = 40;
  for (int i = 0; i < 3; ++i) {
    ColumnDefinition column;
    column.Width(GridLength{button_width, GridUnitType::Pixel});
    toolbar_.ColumnDefinitions().Append(column);
  }
  ColumnDefinition address_column;
  address_column.Width(GridLength{1, GridUnitType::Star});
  toolbar_.ColumnDefinitions().Append(address_column);
  for (int i = 0; i < 2; ++i) {
    ColumnDefinition column;
    column.Width(GridLength{button_width, GridUnitType::Pixel});
    toolbar_.ColumnDefinitions().Append(column);
  }

  back_button_ = MakeGlyphButton(L"\uE72B", L"Back (Alt+Left)",
                                 WCS_COMMAND_BACK);
  Grid::SetColumn(back_button_, 0);
  toolbar_.Children().Append(back_button_);
  forward_button_ = MakeGlyphButton(L"\uE72A", L"Forward (Alt+Right)",
                                    WCS_COMMAND_FORWARD);
  Grid::SetColumn(forward_button_, 1);
  toolbar_.Children().Append(forward_button_);
  reload_button_ = MakeGlyphButton(L"\uE72C", L"Reload (Ctrl+R)",
                                   WCS_COMMAND_RELOAD, false);
  reload_button_.Click([this](const auto&, const auto&) {
    Invoke(active_tab_loading_ ? WCS_COMMAND_STOP : WCS_COMMAND_RELOAD);
  });
  Grid::SetColumn(reload_button_, 2);
  toolbar_.Children().Append(reload_button_);

  Grid address_host;
  Grid::SetColumn(address_host, 3);
  toolbar_.Children().Append(address_host);

  address_box_ = AutoSuggestBox{};
  address_box_.PlaceholderText(L"Search or enter an address");
  address_box_.Height(32);
  address_box_.Margin(Thickness{4, 0, 8, 0});
  address_box_.QueryIcon(nullptr);
  address_box_.TextBoxStyle(
      winrt::Microsoft::UI::Xaml::Application::Current()
          .Resources()
          .Lookup(winrt::box_value(
              winrt::hstring{L"CurveOmniboxTextBoxStyle"}))
          .as<winrt::Microsoft::UI::Xaml::Style>());
  address_box_.Loaded([this](const auto&, const auto&) {
    address_box_.ApplyTemplate();
    // Apply the nested template synchronously. This prevents one startup frame
    // with WinUI's QueryButton underneath the two browser-owned buttons.
    if (const auto text_box =
            FindNamedDescendant(address_box_, L"TextBox")
                .try_as<TextBox>()) {
      text_box.ApplyTemplate();
      // The stock TextBox storyboard can reveal DeleteButton whenever a
      // non-empty omnibox receives focus. That slot coincides with Curve's
      // favorite button, so Visibility alone is not durable against animation
      // precedence. Collapse its layout and input contract permanently.
      if (const auto delete_button =
              FindNamedDescendant(text_box, L"DeleteButton")
                  .try_as<Button>()) {
        delete_button.Width(0);
        delete_button.MinWidth(0);
        delete_button.MaxWidth(0);
        delete_button.Opacity(0);
        delete_button.IsHitTestVisible(false);
        delete_button.IsTabStop(false);
        delete_button.Visibility(Visibility::Collapsed);
      }
    }
    if (const auto query_button =
            FindNamedDescendant(address_box_, L"QueryButton")
                .try_as<Button>()) {
      query_button.Visibility(Visibility::Collapsed);
    }
  });
  address_box_.GotFocus([this](const auto&, const auto&) {
    address_editing_ = true;
  });
  address_box_.PointerPressed([this](const auto&, const auto&) {
    address_submission_pending_ = false;
    submitted_address_text_.clear();
    if (focus_content_timer_) {
      focus_content_timer_.Stop();
    }
    address_editing_ = true;
  });
  address_box_.LostFocus([this](const auto&, const auto&) {
    address_editing_ = false;
    address_box_.IsSuggestionListOpen(false);
    address_box_.ItemsSource(nullptr);
  });
  address_host.Children().Append(address_box_);
  focus_content_timer_ = root_.DispatcherQueue().CreateTimer();
  focus_content_timer_.Interval(std::chrono::milliseconds{120});
  focus_content_timer_.IsRepeating(false);
  const auto focus_callback_lifetime = callback_lifetime_;
  focus_content_timer_.Tick(
      [this, focus_callback_lifetime](
          const winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer& timer,
          const winrt::Windows::Foundation::IInspectable&) {
        timer.Stop();
        if (!focus_callback_lifetime->alive.load(std::memory_order_acquire)) {
          return;
        }
        try {
          const auto island = root_.XamlRoot().ContentIsland();
          const auto focus_controller =
              winrt::Microsoft::UI::Input::InputFocusController::GetForIsland(
                  island);
          focus_controller.DepartFocus(
              winrt::Microsoft::UI::Input::FocusNavigationRequest::Create(
                  winrt::Microsoft::UI::Input::
                      FocusNavigationReason::Programmatic));
        } catch (...) {
          // Restoring the Chromium view below is the compatibility fallback.
        }
        Invoke(WCS_COMMAND_FOCUS_CONTENT, pending_focus_tab_id_,
               pending_focus_tab_index_);
        SetFocus(parent_);
      });
  address_box_.QuerySubmitted(
      [this](const AutoSuggestBox& sender,
             const AutoSuggestBoxQuerySubmittedEventArgs& args) {
        std::wstring value = args.QueryText().c_str();
        if (const auto chosen = args.ChosenSuggestion()) {
          if (const auto property =
                  chosen.try_as<winrt::Windows::Foundation::IPropertyValue>();
              property &&
              property.Type() ==
                  winrt::Windows::Foundation::PropertyType::String) {
            value = property.GetString().c_str();
          }
        }
        if (!value.empty()) {
          address_editing_ = false;
          address_submission_pending_ = true;
          submitted_address_text_ = sender.Text().c_str();
          sender.IsSuggestionListOpen(false);
          sender.ItemsSource(nullptr);
          Invoke(WCS_COMMAND_NAVIGATE, active_tab_id_, active_index_,
                 value.c_str());
          pending_focus_tab_id_ = active_tab_id_;
          pending_focus_tab_index_ = active_index_;
          focus_content_timer_.Stop();
          focus_content_timer_.Start();
          // AutoSuggestBox applies its own QuerySubmitted visual transition
          // after user handlers return. Close the popup at low dispatcher
          // priority so that internal transition cannot reopen stale results
          // or restore keyboard focus to the text box.
          sender.DispatcherQueue().TryEnqueue(
              winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
              [sender] {
                sender.IsSuggestionListOpen(false);
                sender.ItemsSource(nullptr);
              });
        }
      });
  address_box_.TextChanged(
      [this](const AutoSuggestBox& sender,
             const AutoSuggestBoxTextChangedEventArgs& args) {
        if (address_submission_pending_) {
          const bool new_user_edit =
              args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput &&
              sender.Text() != submitted_address_text_;
          if (!new_user_edit) {
            // AutoSuggestBox may report the submitted text as UserInput once
            // more after QuerySubmitted. Treat that delayed event, plus URL
            // synchronization from Chromium, as part of the completed query.
            sender.IsSuggestionListOpen(false);
            sender.ItemsSource(nullptr);
            return;
          }
          address_submission_pending_ = false;
          submitted_address_text_.clear();
        }
        if (suppress_address_suggestions_) {
          suppress_address_suggestions_ = false;
          sender.IsSuggestionListOpen(false);
          sender.ItemsSource(nullptr);
          return;
        }
        if (!updating_ &&
            args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput) {
          address_editing_ = true;
          UpdateAddressSuggestions(sender.Text().c_str());
        }
      });
  security_button_ = MakeGlyphButton(L"\uE72E", L"View site information",
                                     WCS_COMMAND_SHOW_SITE_INFO, false);
  security_button_.Style(
      winrt::Microsoft::UI::Xaml::Application::Current()
          .Resources()
          .Lookup(winrt::box_value(
              winrt::hstring{L"CurveOmniboxButtonStyle"}))
          .as<winrt::Microsoft::UI::Xaml::Style>());
  security_button_.Content(OmniboxGlyph(L"\uE72E"));
  security_button_.Width(kOmniboxButtonWidth);
  security_button_.Height(kOmniboxButtonHeight);
  security_button_.CornerRadius(
      UniformCornerRadius(kOmniboxButtonCornerRadius));
  security_button_.HorizontalAlignment(HorizontalAlignment::Left);
  security_button_.Margin(Thickness{8, 0, 0, 0});
  Canvas::SetZIndex(security_button_, 2);
  security_button_.Click(
      [this](const auto&, const auto&) { ShowSiteInfoFlyout(); });
  address_host.Children().Append(security_button_);

  favorite_button_ = MakeGlyphButton(L"\uE734", L"Add this page to favorites",
                                     WCS_COMMAND_BOOKMARK_PAGE, false);
  favorite_button_.Style(
      winrt::Microsoft::UI::Xaml::Application::Current()
          .Resources()
          .Lookup(winrt::box_value(
              winrt::hstring{L"CurveOmniboxButtonStyle"}))
          .as<winrt::Microsoft::UI::Xaml::Style>());
  favorite_button_.Content(OmniboxGlyph(L"\uE734"));
  favorite_button_.Width(kOmniboxButtonWidth);
  favorite_button_.Height(kOmniboxButtonHeight);
  favorite_button_.CornerRadius(
      UniformCornerRadius(kOmniboxButtonCornerRadius));
  favorite_button_.HorizontalAlignment(HorizontalAlignment::Right);
  favorite_button_.Margin(Thickness{0, 0, 12, 0});
  Canvas::SetZIndex(favorite_button_, 2);
  favorite_button_.Click(
      [this](const auto&, const auto&) { ShowBookmarkFlyout(); });
  address_host.Children().Append(favorite_button_);

  profile_button_ = MakeGlyphButton(L"\uE77B", L"Profiles",
                                    WCS_COMMAND_OPEN_PROFILES, false);
  Grid::SetColumn(profile_button_, 4);
  toolbar_.Children().Append(profile_button_);
  menu_button_ = MakeGlyphButton(L"\uE712", L"Settings and more",
                                 WCS_COMMAND_OPEN_SETTINGS, false);
  Grid::SetColumn(menu_button_, 5);
  toolbar_.Children().Append(menu_button_);
}

MenuFlyoutItem Shell::MakeMenuItem(std::wstring_view text,
                                   WcsCommand command,
                                   int64_t tab_id) {
  MenuFlyoutItem item;
  item.Text(text);
  item.Click([this, command, tab_id](const auto&, const auto&) {
    Invoke(command, tab_id);
  });
  return item;
}

void Shell::BuildMenus() {
  profile_menu_ = MenuFlyout{};
  profile_menu_.Items().Append(
      MakeMenuItem(L"Manage profiles", WCS_COMMAND_OPEN_PROFILES));
  profile_menu_.Items().Append(MakeMenuItem(
      L"New InPrivate window", WCS_COMMAND_NEW_INCOGNITO_WINDOW));
  profile_button_.Click([this](const auto&, const auto&) {
    profile_menu_.ShowAt(profile_button_);
  });

  app_menu_ = MenuFlyout{};
  app_menu_.Items().Append(MakeMenuItem(L"New tab", WCS_COMMAND_NEW_TAB));
  app_menu_.Items().Append(
      MakeMenuItem(L"New window", WCS_COMMAND_NEW_WINDOW));
  app_menu_.Items().Append(MakeMenuItem(
      L"New InPrivate window", WCS_COMMAND_NEW_INCOGNITO_WINDOW));
  app_menu_.Items().Append(MenuFlyoutSeparator{});
  app_menu_.Items().Append(
      MakeMenuItem(L"History", WCS_COMMAND_OPEN_HISTORY));
  app_menu_.Items().Append(
      MakeMenuItem(L"Downloads", WCS_COMMAND_OPEN_DOWNLOADS));
  app_menu_.Items().Append(
      MakeMenuItem(L"Bookmarks", WCS_COMMAND_OPEN_BOOKMARKS));
  app_menu_.Items().Append(
      MakeMenuItem(L"Extensions", WCS_COMMAND_OPEN_EXTENSIONS));
  app_menu_.Items().Append(MenuFlyoutSeparator{});
  MenuFlyoutItem find_item;
  find_item.Text(L"Find on page");
  find_item.Icon(ContextMenuIcon(L"\uE721"));
  find_item.Click(
      [this](const auto&, const auto&) { ShowFindFlyout(); });
  app_menu_.Items().Append(find_item);
  app_menu_.Items().Append(MakeMenuItem(L"Print", WCS_COMMAND_PRINT));
  app_menu_.Items().Append(
      MakeMenuItem(L"Save page as", WCS_COMMAND_SAVE_PAGE));
  MenuFlyoutItem sidebar_item;
  sidebar_item.Text(L"Sidebar");
  sidebar_item.Icon(ContextMenuIcon(L"\uE76C"));
  sidebar_item.Click(
      [this](const auto&, const auto&) { ToggleSidebar(); });
  app_menu_.Items().Append(sidebar_item);
  app_menu_.Items().Append(MenuFlyoutSeparator{});
  app_menu_.Items().Append(
      MakeMenuItem(L"Settings", WCS_COMMAND_OPEN_SETTINGS));
  app_menu_.Items().Append(
      MakeMenuItem(L"About Curve Browser", WCS_COMMAND_OPEN_ABOUT));
  app_menu_.Items().Append(MakeMenuItem(L"Exit", WCS_COMMAND_EXIT));
  menu_button_.Click(
      [this](const auto&, const auto&) { app_menu_.ShowAt(menu_button_); });
}

void Shell::BuildSidebar() {
  sidebar_ = Grid{};
  sidebar_.Visibility(Visibility::Collapsed);
  sidebar_.Padding(Thickness{16, 12, 16, 16});
  Grid::SetRow(sidebar_, 3);
  Grid::SetColumn(sidebar_, 1);
  Canvas::SetZIndex(sidebar_, 30);

  RowDefinition header_row;
  header_row.Height(GridLength{44, GridUnitType::Pixel});
  sidebar_.RowDefinitions().Append(header_row);
  RowDefinition content_row;
  content_row.Height(GridLength{1, GridUnitType::Star});
  sidebar_.RowDefinitions().Append(content_row);

  Grid header;
  ColumnDefinition title_column;
  title_column.Width(GridLength{1, GridUnitType::Star});
  header.ColumnDefinitions().Append(title_column);
  ColumnDefinition close_column;
  close_column.Width(GridLength{36, GridUnitType::Pixel});
  header.ColumnDefinitions().Append(close_column);
  TextBlock title;
  title.Text(L"Curve sidebar");
  title.FontSize(18);
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  title.VerticalAlignment(VerticalAlignment::Center);
  header.Children().Append(title);
  Button close;
  close.Content(ToolbarGlyph(L"\uE711"));
  close.Width(32);
  close.Height(32);
  close.Padding(UniformThickness(0));
  close.BorderThickness(UniformThickness(0));
  close.Background(SolidColorBrush{Color(0, 0, 0, 0)});
  Grid::SetColumn(close, 1);
  close.Click([this](const auto&, const auto&) { ToggleSidebar(); });
  header.Children().Append(close);
  sidebar_.Children().Append(header);

  StackPanel destinations;
  destinations.Spacing(4);
  Grid::SetRow(destinations, 1);
  const auto add_destination =
      [this, &destinations](std::wstring_view title,
                            std::wstring_view glyph,
                            WcsCommand command) {
        Button button;
        StackPanel content;
        content.Orientation(Orientation::Horizontal);
        content.Spacing(10);
        content.Children().Append(ToolbarGlyph(glyph));
        TextBlock label;
        label.Text(title);
        label.VerticalAlignment(VerticalAlignment::Center);
        content.Children().Append(label);
        button.Content(content);
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(HorizontalAlignment::Left);
        button.Height(40);
        button.BorderThickness(UniformThickness(0));
        button.Click([this, command](const auto&, const auto&) {
          Invoke(command);
        });
        destinations.Children().Append(button);
      };
  add_destination(L"Bookmarks", L"\uE734", WCS_COMMAND_OPEN_BOOKMARKS);
  add_destination(L"History", L"\uE81C", WCS_COMMAND_OPEN_HISTORY);
  add_destination(L"Downloads", L"\uE896", WCS_COMMAND_OPEN_DOWNLOADS);
  add_destination(L"Extensions", L"\uE7B8", WCS_COMMAND_OPEN_EXTENSIONS);
  sidebar_.Children().Append(destinations);
  root_.Children().Append(sidebar_);
}

void Shell::ToggleSidebar() {
  sidebar_visible_ = !sidebar_visible_;
  root_.ColumnDefinitions().GetAt(1).Width(
      GridLength{sidebar_visible_ ? kSidebarWidth : 0, GridUnitType::Pixel});
  sidebar_.Visibility(sidebar_visible_ ? Visibility::Visible
                                      : Visibility::Collapsed);
  ResizeIsland();
  UpdateWindowRegion();
}

void Shell::ShowFindFlyout() {
  if (!find_flyout_) {
    find_flyout_ = Flyout{};
    find_flyout_.Placement(
        winrt::Microsoft::UI::Xaml::Controls::Primitives::
            FlyoutPlacementMode::BottomEdgeAlignedRight);

    Grid panel;
    panel.Padding(UniformThickness(8));
    panel.ColumnSpacing(4);

    ColumnDefinition text_column;
    text_column.Width(GridLength{240, GridUnitType::Pixel});
    panel.ColumnDefinitions().Append(text_column);
    for (int index = 0; index < 3; ++index) {
      ColumnDefinition button_column;
      button_column.Width(GridLength{32, GridUnitType::Pixel});
      panel.ColumnDefinitions().Append(button_column);
    }

    find_box_ = TextBox{};
    find_box_.PlaceholderText(L"Find on page");
    find_box_.Height(32);
    find_box_.VerticalContentAlignment(VerticalAlignment::Center);
    find_box_.TextChanged([this](const auto&, const auto&) {
      const std::wstring query = find_box_.Text().c_str();
      if (!query.empty()) {
        Invoke(WCS_COMMAND_FIND_TEXT, active_tab_id_, active_index_,
               query.c_str());
      }
    });
    panel.Children().Append(find_box_);

    const auto make_find_button = [](std::wstring_view glyph,
                                     std::wstring_view name) {
      Button button;
      button.Width(28);
      button.Height(28);
      button.Padding(UniformThickness(0));
      button.Content(ToolbarGlyph(glyph));
      ToolTipService::SetToolTip(button, winrt::box_value(name));
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
          button, name);
      return button;
    };
    Button previous = make_find_button(L"\uE70E", L"Previous result");
    Grid::SetColumn(previous, 1);
    previous.Click([this](const auto&, const auto&) {
      const std::wstring query = find_box_.Text().c_str();
      Invoke(WCS_COMMAND_FIND_NEXT, active_tab_id_, active_index_,
             query.c_str(), 0);
    });
    panel.Children().Append(previous);

    Button next = make_find_button(L"\uE70D", L"Next result");
    Grid::SetColumn(next, 2);
    next.Click([this](const auto&, const auto&) {
      const std::wstring query = find_box_.Text().c_str();
      Invoke(WCS_COMMAND_FIND_NEXT, active_tab_id_, active_index_,
             query.c_str(), 1);
    });
    panel.Children().Append(next);

    Button close = make_find_button(L"\uE711", L"Close find");
    Grid::SetColumn(close, 3);
    close.Click(
        [this](const auto&, const auto&) { find_flyout_.Hide(); });
    panel.Children().Append(close);
    find_flyout_.Content(panel);
    find_flyout_.Closed([this](const auto&, const auto&) {
      Invoke(WCS_COMMAND_CLOSE_FIND, active_tab_id_, active_index_);
    });
  }
  find_flyout_.ShowAt(menu_button_);
  find_box_.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
  find_box_.SelectAll();
}

void Shell::ShowBookmarkFlyout() {
  bookmark_flyout_ = Flyout{};
  bookmark_flyout_.Placement(
      winrt::Microsoft::UI::Xaml::Controls::Primitives::
          FlyoutPlacementMode::BottomEdgeAlignedRight);

  StackPanel panel;
  panel.Width(320);
  panel.Spacing(12);
  panel.Padding(UniformThickness(8));

  TextBlock heading;
  heading.Text(active_page_bookmarked_ ? L"Edit favorite" : L"Favorite added");
  heading.FontSize(18);
  heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(heading);

  bookmark_title_box_ = TextBox{};
  bookmark_title_box_.Header(winrt::box_value(L"Name"));
  bookmark_title_box_.Text(active_title_.empty() ? active_url_ : active_title_);
  bookmark_title_box_.SelectAll();
  panel.Children().Append(bookmark_title_box_);

  TextBlock folder;
  folder.Text(L"Saved to the Bookmarks bar");
  folder.Opacity(0.72);
  panel.Children().Append(folder);

  Grid actions;
  actions.ColumnSpacing(8);
  ColumnDefinition spacer;
  spacer.Width(GridLength{1, GridUnitType::Star});
  actions.ColumnDefinitions().Append(spacer);
  ColumnDefinition remove_column;
  remove_column.Width(GridLength{1, GridUnitType::Auto});
  actions.ColumnDefinitions().Append(remove_column);
  ColumnDefinition done_column;
  done_column.Width(GridLength{1, GridUnitType::Auto});
  actions.ColumnDefinitions().Append(done_column);

  if (active_page_bookmarked_) {
    Button remove;
    remove.Content(winrt::box_value(L"Remove"));
    Grid::SetColumn(remove, 1);
    remove.Click([this](const auto&, const auto&) {
      Invoke(WCS_COMMAND_SAVE_BOOKMARK, active_tab_id_, active_index_, nullptr,
             0);
      bookmark_flyout_.Hide();
    });
    actions.Children().Append(remove);
  }

  Button done;
  done.Content(winrt::box_value(L"Done"));
  done.Style(root_.Resources()
                 .TryLookup(winrt::box_value(
                     winrt::hstring{L"AccentButtonStyle"}))
                 .try_as<winrt::Microsoft::UI::Xaml::Style>());
  Grid::SetColumn(done, 2);
  done.Click([this](const auto&, const auto&) {
    const std::wstring title = bookmark_title_box_.Text().c_str();
    Invoke(WCS_COMMAND_SAVE_BOOKMARK, active_tab_id_, active_index_,
           title.c_str(), 1);
    bookmark_flyout_.Hide();
  });
  actions.Children().Append(done);
  panel.Children().Append(actions);

  bookmark_flyout_.Content(panel);
  bookmark_flyout_.ShowAt(favorite_button_);
  bookmark_title_box_.Focus(
      winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
}

void Shell::ShowSiteInfoFlyout() {
  site_info_flyout_ = Flyout{};
  site_info_flyout_.Placement(
      winrt::Microsoft::UI::Xaml::Controls::Primitives::
          FlyoutPlacementMode::BottomEdgeAlignedLeft);

  StackPanel panel;
  panel.Width(340);
  panel.Spacing(12);
  panel.Padding(UniformThickness(8));

  const bool secure = StartsWithInsensitive(active_url_, L"https://");
  StackPanel heading_row;
  heading_row.Orientation(Orientation::Horizontal);
  heading_row.Spacing(10);
  FontIcon icon;
  icon.Glyph(secure ? L"\uE72E" : L"\uE7BA");
  icon.FontSize(20);
  heading_row.Children().Append(icon);
  TextBlock heading;
  heading.Text(secure ? L"Connection is secure" : L"Connection is not secure");
  heading.FontSize(18);
  heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  heading.VerticalAlignment(VerticalAlignment::Center);
  heading_row.Children().Append(heading);
  panel.Children().Append(heading_row);

  TextBlock address;
  address.Text(active_url_);
  address.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
  address.Opacity(0.72);
  panel.Children().Append(address);

  TextBlock explanation;
  explanation.Text(
      secure
          ? L"Information you send or receive through this site is private."
          : L"Do not enter sensitive information on this page.");
  explanation.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
  panel.Children().Append(explanation);

  Button settings;
  settings.Content(winrt::box_value(L"Site settings"));
  settings.HorizontalAlignment(HorizontalAlignment::Stretch);
  settings.HorizontalContentAlignment(HorizontalAlignment::Left);
  settings.Click([this](const auto&, const auto&) {
    Invoke(WCS_COMMAND_OPEN_SITE_SETTINGS, active_tab_id_, active_index_,
           active_url_.c_str());
    site_info_flyout_.Hide();
  });
  panel.Children().Append(settings);

  site_info_flyout_.Content(panel);
  site_info_flyout_.ShowAt(security_button_);
}

void Shell::SetVisualStateForTesting(std::wstring_view state) {
  using winrt::Microsoft::UI::Xaml::VisualStateManager;
  using winrt::Microsoft::UI::Xaml::FocusState;
  if (state == L"normal") {
    return;
  }
  if (state == L"security-pointer-over") {
    VisualStateManager::GoToState(security_button_, L"PointerOver", false);
    return;
  }
  if (state == L"security-pressed") {
    VisualStateManager::GoToState(security_button_, L"Pressed", false);
    return;
  }
  if (state == L"security-keyboard-focus") {
    security_button_.Focus(FocusState::Keyboard);
    return;
  }
  if (state == L"favorite-pointer-over") {
    VisualStateManager::GoToState(favorite_button_, L"PointerOver", false);
    return;
  }
  if (state == L"favorite-pressed") {
    VisualStateManager::GoToState(favorite_button_, L"Pressed", false);
    return;
  }
  if (state == L"favorite-keyboard-focus") {
    favorite_button_.Focus(FocusState::Keyboard);
    return;
  }
  if (state == L"address-keyboard-focus") {
    address_box_.Focus(FocusState::Keyboard);
    return;
  }
  if (state == L"add-pointer-over") {
    if (const auto add_button =
            FindVisualChildByName(tab_view_, L"AddButton").try_as<Button>()) {
      VisualStateManager::GoToState(add_button, L"PointerOver", false);
    }
    return;
  }

  const auto selected = tab_view_.SelectedItem().try_as<TabViewItem>();
  if (!selected) {
    return;
  }
  if (state == L"selected-keyboard-focus") {
    selected.Focus(FocusState::Keyboard);
    return;
  }
  if (state == L"selected-pointer-over") {
    VisualStateManager::GoToState(selected, L"PointerOverSelected", false);
    return;
  }
  if (state == L"selected-pressed") {
    VisualStateManager::GoToState(selected, L"PressedSelected", false);
    return;
  }
  for (const auto& [tab_id, item] : tab_items_) {
    (void)tab_id;
    if (item == selected) {
      continue;
    }
    if (state == L"unselected-pointer-over") {
      VisualStateManager::GoToState(item, L"PointerOver", false);
    } else if (state == L"unselected-pressed") {
      VisualStateManager::GoToState(item, L"Pressed", false);
    }
    return;
  }
}

void Shell::Invoke(WcsCommand command,
                   int64_t tab_id,
                   int32_t tab_index,
                   const wchar_t* text,
                   uint32_t event_flags) const {
  if (!callbacks_.invoke_command) {
    return;
  }
  WcsCommandArgs args{};
  args.size = sizeof(args);
  args.command = command;
  args.tab_id = tab_id;
  args.tab_index = tab_index;
  args.text = text;
  args.event_flags = event_flags;
  callbacks_.invoke_command(callbacks_.context, &args);
}

HRESULT Shell::Update(const WcsWindowState& state) {
  try {
    restore_on_startup_ = state.restore_on_startup != 0;
    active_page_bookmarked_ = state.active_page_bookmarked != 0;
    UpdateBookmarks(state);
    UpdateNativePageData(state);
    UpdateTabs(state);
    back_button_.IsEnabled(state.can_go_back != 0);
    forward_button_.IsEnabled(state.can_go_forward != 0);
    profile_button_.Content(
        ToolbarGlyph(state.is_incognito ? L"\uE727" : L"\uE77B"));
    ToolTipService::SetToolTip(
        profile_button_,
        winrt::box_value(state.profile_name ? state.profile_name : L"Profiles"));
    const std::wstring profile_accessible_name =
        L"Profiles — " +
        std::wstring(state.profile_name ? state.profile_name : L"Default");
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        profile_button_, winrt::hstring{profile_accessible_name});
    return S_OK;
  } catch (...) {
    return winrt::to_hresult();
  }
}

void Shell::UpdateBookmarks(const WcsWindowState& state) {
  const bool visible = state.bookmark_bar_visible != 0;
  if (bookmark_bar_visible_ != visible) {
    bookmark_bar_visible_ = visible;
    root_.RowDefinitions().GetAt(2).Height(
        GridLength{visible ? kBookmarkBarHeight : 0, GridUnitType::Pixel});
    bookmark_bar_.Visibility(visible ? Visibility::Visible
                                    : Visibility::Collapsed);
    ResizeIsland();
  }
  bookmark_items_.Children().Clear();
  if (!visible) {
    return;
  }
  for (size_t index = 0; index < state.bookmark_count; ++index) {
    const WcsBookmarkState& bookmark = state.bookmarks[index];
    Button button;
    const std::wstring title =
        bookmark.title && *bookmark.title ? bookmark.title : L"Bookmark";
    button.Content(winrt::box_value(title));
    button.Height(30);
    button.Padding(Thickness{10, 0, 10, 0});
    button.BorderThickness(UniformThickness(0));
    button.CornerRadius(UniformCornerRadius(4));
    button.Background(SolidColorBrush{Color(0, 0, 0, 0)});
    const std::wstring url = bookmark.url ? bookmark.url : L"";
    button.Click([this, url, folder = bookmark.is_folder != 0](
                     const auto&, const auto&) {
      if (folder || url.empty()) {
        Invoke(WCS_COMMAND_OPEN_BOOKMARKS);
      } else {
        Invoke(WCS_COMMAND_OPEN_BOOKMARK, -1, -1, url.c_str());
      }
    });
    bookmark_items_.Children().Append(button);
  }
  Button overflow;
  overflow.Content(ToolbarGlyph(L"\uE712"));
  overflow.Width(30);
  overflow.Height(30);
  overflow.Padding(UniformThickness(0));
  overflow.BorderThickness(UniformThickness(0));
  overflow.Background(SolidColorBrush{Color(0, 0, 0, 0)});
  ToolTipService::SetToolTip(overflow, winrt::box_value(L"All bookmarks"));
  overflow.Click(
      [this](const auto&, const auto&) { Invoke(WCS_COMMAND_OPEN_BOOKMARKS); });
  bookmark_items_.Children().Append(overflow);
}

void Shell::UpdateNativePageData(const WcsWindowState& state) {
  native_bookmarks_.clear();
  native_bookmarks_.reserve(state.bookmark_library_count);
  for (size_t index = 0; index < state.bookmark_library_count; ++index) {
    const WcsBookmarkState& bookmark = state.bookmark_library[index];
    if (bookmark.size < sizeof(WcsBookmarkState)) {
      continue;
    }
    native_bookmarks_.push_back(
        NativeBookmark{bookmark.title ? bookmark.title : L"",
                       bookmark.url ? bookmark.url : L"",
                       bookmark.is_folder != 0});
  }

  native_history_.clear();
  native_history_.reserve(state.history_entry_count);
  for (size_t index = 0; index < state.history_entry_count; ++index) {
    const WcsHistoryEntryState& entry = state.history_entries[index];
    if (entry.size < sizeof(WcsHistoryEntryState)) {
      continue;
    }
    native_history_.push_back(
        NativeHistoryEntry{entry.title ? entry.title : L"",
                           entry.url ? entry.url : L"",
                           entry.visit_time ? entry.visit_time : L""});
  }
  history_loading_ = state.history_loading != 0;

  native_downloads_.clear();
  native_downloads_.reserve(state.download_count);
  for (size_t index = 0; index < state.download_count; ++index) {
    const WcsDownloadState& download = state.downloads[index];
    if (download.size < sizeof(WcsDownloadState)) {
      continue;
    }
    native_downloads_.push_back(
        NativeDownload{download.id,
                       download.title ? download.title : L"",
                       download.url ? download.url : L"",
                       download.target_path ? download.target_path : L"",
                       download.status ? download.status : L"",
                       download.complete != 0,
                       download.in_progress != 0});
  }
}

void Shell::UpdateTabs(const WcsWindowState& state) {
  if (state.tab_count != last_tab_count_) {
    new_tab_request_pending_ = false;
    last_tab_count_ = state.tab_count;
  }
  updating_ = true;
  struct UpdatingGuard {
    explicit UpdatingGuard(bool& value) : value(value) {}
    ~UpdatingGuard() { value = false; }
    bool& value;
  } reset_updating(updating_);

  std::set<int64_t> live_tabs;
  tab_suggestions_.clear();
  active_tab_loading_ = false;
  for (size_t index = 0; index < state.tab_count; ++index) {
    const WcsTabState& tab = state.tabs[index];
    if (tab.url && *tab.url) {
      tab_suggestions_.emplace_back(tab.url);
    }
    live_tabs.insert(tab.tab_id);
    auto existing = tab_items_.find(tab.tab_id);
    TabViewItem item{nullptr};
    if (existing == tab_items_.end()) {
      item = TabViewItem{};
      item.Height(kTabItemHeight);
      item.Tag(winrt::box_value(tab.tab_id));
      item.IsClosable(true);
      item.UseSystemFocusVisuals(false);
      const int64_t tab_id = tab.tab_id;
      item.GotFocus([this, tab_id](const auto& sender, const auto&) {
        const auto focused_item = sender.template try_as<TabViewItem>();
        if (!focused_item) {
          return;
        }
        using winrt::Microsoft::UI::Xaml::FocusState;
        focused_tab_id_ = tab_id;
        tab_keyboard_focus_visible_ =
            focused_item.FocusState() != FocusState::Pointer;
        ScheduleTabChromeUpdate();
      });
      item.LostFocus([this, tab_id](const auto&, const auto&) {
        if (focused_tab_id_ == tab_id) {
          focused_tab_id_ = -1;
          tab_keyboard_focus_visible_ = false;
          ScheduleTabChromeUpdate();
        }
      });
      // Reserve the stock 16-DIP icon column from the first frame. Replacing
      // this placeholder with a favicon or audio glyph no longer changes a
      // size-to-content tab's width and makes the entire strip jitter.
      FontIconSource placeholder;
      placeholder.Glyph(L"\uE7C3");
      placeholder.FontFamily(
          winrt::Microsoft::UI::Xaml::Media::FontFamily{
              L"Segoe Fluent Icons, Segoe MDL2 Assets"});
      placeholder.FontSize(12);
      item.IconSource(placeholder);
      item.SizeChanged(
          [this](const auto&, const auto&) { ScheduleTabChromeUpdate(); });

      MenuFlyout context_menu;
      context_menu.Items().Append(
          MakeMenuItem(L"Reload", WCS_COMMAND_RELOAD, tab.tab_id));
      context_menu.Items().Append(MakeMenuItem(
          L"Duplicate tab", WCS_COMMAND_DUPLICATE_TAB, tab.tab_id));
      auto pin_item = MakeMenuItem(
          tab.pinned ? L"Unpin tab" : L"Pin tab", WCS_COMMAND_TOGGLE_PIN_TAB,
          tab.tab_id);
      context_menu.Items().Append(pin_item);
      tab_pin_menu_items_.emplace(tab.tab_id, pin_item);
      context_menu.Items().Append(MenuFlyoutSeparator{});
      context_menu.Items().Append(MakeMenuItem(
          L"Close tab", WCS_COMMAND_CLOSE_TAB, tab.tab_id));
      context_menu.Items().Append(MakeMenuItem(
          L"Close other tabs", WCS_COMMAND_CLOSE_OTHER_TABS, tab.tab_id));
      context_menu.Items().Append(MakeMenuItem(
          L"Close tabs to the right", WCS_COMMAND_CLOSE_TABS_TO_RIGHT,
          tab.tab_id));
      item.ContextFlyout(context_menu);

      tab_view_.TabItems().Append(item);
      tab_items_.emplace(tab.tab_id, item);
    } else {
      item = existing->second;
    }

    // The island clips the native path's overhang, so keep the item-owned path
    // transparent. UpdateTabShoulders renders the same one-piece geometry in
    // the unclipped root layer behind this item's content.
    const auto selected_background_key = winrt::box_value(
        winrt::hstring{L"TabViewItemHeaderBackgroundSelected"});
    const auto drag_background_key = winrt::box_value(
        winrt::hstring{L"TabViewItemHeaderDragBackground"});
    item.Resources().Insert(
        selected_background_key,
        SolidColorBrush{Color(0, 0, 0, 0)});
    item.Resources().Insert(drag_background_key, toolbar_.Background());

    const wchar_t* title = tab.title && *tab.title ? tab.title : L"New tab";
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        item, winrt::hstring{title});
    item.IsClosable(tab.pinned == 0);
    const bool was_audio_icon = tab_audio_icons_.contains(tab.tab_id);
    const bool audio_icon_changed =
        tab.audible &&
        (!was_audio_icon ||
         tab_audio_muted_.find(tab.tab_id) == tab_audio_muted_.end() ||
         tab_audio_muted_[tab.tab_id] != (tab.muted != 0));
    const std::wstring reported_favicon_url =
        tab.favicon_url && *tab.favicon_url ? tab.favicon_url : L"";
    const auto cached_favicon = tab_favicon_urls_.find(tab.tab_id);
    const std::wstring favicon_url =
        !reported_favicon_url.empty()
            ? reported_favicon_url
            : (cached_favicon != tab_favicon_urls_.end()
                   ? cached_favicon->second
                   : L"");
    const bool favicon_changed =
        !reported_favicon_url.empty() &&
        (cached_favicon == tab_favicon_urls_.end() ||
         cached_favicon->second != reported_favicon_url);
    if (!tab.audible && !favicon_url.empty() &&
        (favicon_changed || was_audio_icon)) {
      try {
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap{
            winrt::Windows::Foundation::Uri{favicon_url}};
        ImageIconSource source;
        source.ImageSource(bitmap);
        item.IconSource(source);
        tab_favicon_urls_[tab.tab_id] = favicon_url;
      } catch (...) {
        // Preserve the last successfully decoded favicon. A transient empty or
        // invalid URL during navigation must not collapse the icon presenter
        // and make every size-to-content tab jitter.
      }
    } else if (tab.audible && favicon_changed) {
      // Preserve the destination favicon while the same fixed icon slot is
      // temporarily showing its audio indicator.
      tab_favicon_urls_[tab.tab_id] = reported_favicon_url;
    }
    const std::wstring title_value{title};
    if (const auto previous = tab_titles_.find(tab.tab_id);
        previous == tab_titles_.end() || previous->second != title_value) {
      item.Header(winrt::box_value(title));
      tab_titles_[tab.tab_id] = title_value;
    }
    if (audio_icon_changed) {
      FontIconSource source;
      source.Glyph(tab.muted ? L"\uE74F" : L"\uE767");
      source.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{
          L"Segoe Fluent Icons"});
      source.FontSize(12);
      item.IconSource(source);
    }
    if (tab.audible) {
      tab_audio_icons_.insert(tab.tab_id);
      tab_audio_muted_[tab.tab_id] = tab.muted != 0;
    } else if (was_audio_icon) {
      // If there was never a favicon, explicitly restore the placeholder;
      // otherwise the speaker glyph remains after audio stops.
      if (favicon_url.empty()) {
        FontIconSource placeholder;
        placeholder.Glyph(L"\uE7C3");
        placeholder.FontFamily(
            winrt::Microsoft::UI::Xaml::Media::FontFamily{
                L"Segoe Fluent Icons, Segoe MDL2 Assets"});
        placeholder.FontSize(12);
        item.IconSource(placeholder);
      }
      tab_audio_icons_.erase(tab.tab_id);
      tab_audio_muted_.erase(tab.tab_id);
    }

    if (const auto pin = tab_pin_menu_items_.find(tab.tab_id);
        pin != tab_pin_menu_items_.end()) {
      pin->second.Text(tab.pinned ? L"Unpin tab" : L"Pin tab");
    }

    if (static_cast<int32_t>(index) == state.active_index) {
      tab_view_.SelectedItem(item);
      active_index_ = state.active_index;
      active_tab_id_ = tab.tab_id;
      active_tab_loading_ = tab.loading != 0;
      const std::wstring next_active_url = tab.url ? tab.url : L"";
      const bool active_url_changed = next_active_url != active_url_;
      active_url_ = next_active_url;
      active_title_ = title;
      if (address_submission_pending_ && active_url_changed &&
          focus_content_timer_) {
        pending_focus_tab_id_ = active_tab_id_;
        pending_focus_tab_index_ = active_index_;
        focus_content_timer_.Stop();
        focus_content_timer_.Start();
      }
      if (!address_editing_ && address_box_.Text() != active_url_) {
        suppress_address_suggestions_ = true;
        address_box_.Text(active_url_);
      }
      UpdateAddressSecurityState(active_url_);
    }
  }

  for (auto it = tab_items_.begin(); it != tab_items_.end();) {
    if (live_tabs.contains(it->first)) {
      ++it;
      continue;
    }
    uint32_t item_index = 0;
    if (tab_view_.TabItems().IndexOf(it->second, item_index)) {
      tab_view_.TabItems().RemoveAt(item_index);
    }
    tab_pin_menu_items_.erase(it->first);
    tab_titles_.erase(it->first);
    tab_favicon_urls_.erase(it->first);
    tab_audio_icons_.erase(it->first);
    tab_audio_muted_.erase(it->first);
    if (focused_tab_id_ == it->first) {
      focused_tab_id_ = -1;
      tab_keyboard_focus_visible_ = false;
    }
    it = tab_items_.erase(it);
  }

  const wchar_t* reload_tooltip =
      active_tab_loading_ ? L"Stop loading (Esc)" : L"Reload (Ctrl+R)";
  reload_button_.Content(
      ToolbarGlyph(active_tab_loading_ ? L"\uE71A" : L"\uE72C"));
  ToolTipService::SetToolTip(reload_button_,
                             winrt::box_value(reload_tooltip));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      reload_button_, winrt::hstring{reload_tooltip});
  favorite_button_.Content(
      OmniboxGlyph(active_page_bookmarked_ ? L"\uE735" : L"\uE734"));
  const wchar_t* favorite_tooltip =
      active_page_bookmarked_ ? L"Edit favorite" : L"Add this page to favorites";
  ToolTipService::SetToolTip(favorite_button_,
                             winrt::box_value(favorite_tooltip));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      favorite_button_, winrt::hstring{favorite_tooltip});

  UpdateNativePage(active_url_);
  // Selection activates SelectedBackgroundPath through x:Load during the next
  // layout pass. Coalesce the shoulder and non-client geometry work after that
  // pass; changing either from a TabView SizeChanged callback can re-enter XAML
  // measure and intermittently terminate the process.
  ScheduleTabChromeUpdate();
}

void Shell::UpdateAddressSuggestions(std::wstring_view query) {
  auto suggestions =
      winrt::single_threaded_observable_vector<winrt::Windows::Foundation::
                                                   IInspectable>();
  if (query.empty()) {
    address_box_.ItemsSource(suggestions);
    return;
  }

  const auto lowercase = [](std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t character) {
                     return static_cast<wchar_t>(std::towlower(character));
                   });
    return result;
  };
  const std::wstring lowered_query = lowercase(query);
  std::set<std::wstring> seen;
  const auto append = [&](std::wstring_view value) {
    if (value.empty() || suggestions.Size() >= 8) {
      return;
    }
    std::wstring candidate(value);
    if (seen.insert(lowercase(candidate)).second) {
      suggestions.Append(winrt::box_value(winrt::hstring{candidate}));
    }
  };

  if (query.find(L'.') != std::wstring_view::npos &&
      query.find(L"://") == std::wstring_view::npos) {
    append(std::wstring(L"https://") + std::wstring(query));
  }
  for (const auto& candidate : tab_suggestions_) {
    if (lowercase(candidate).find(lowered_query) != std::wstring::npos) {
      append(candidate);
    }
  }
  address_box_.ItemsSource(suggestions);
  address_box_.IsSuggestionListOpen(suggestions.Size() > 0);
}

void Shell::UpdateAddressSecurityState(std::wstring_view url) {
  const bool secure = StartsWithInsensitive(url, L"https://");
  security_button_.Content(OmniboxGlyph(secure ? L"\uE72E" : L"\uE946"));
  const wchar_t* label = secure ? L"Connection is secure"
                                : L"View site information";
  ToolTipService::SetToolTip(security_button_, winrt::box_value(label));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      security_button_, label);
}

void Shell::ScheduleTabChromeUpdate() {
  if (tab_chrome_update_queued_ || !root_) {
    return;
  }
  tab_chrome_update_queued_ = true;
  const auto callback_lifetime = callback_lifetime_;
  if (!root_.DispatcherQueue().TryEnqueue([this, callback_lifetime] {
        if (!callback_lifetime->alive.load(std::memory_order_acquire)) {
          return;
        }
        tab_chrome_update_queued_ = false;
        if (!parent_ || !IsWindow(parent_)) {
          return;
        }
        try {
          UpdateTitleBarRegions();
          UpdateTabShoulders();
        } catch (...) {
          OutputDebugStringW(
              L"Curve Browser deferred tab chrome update failed.\n");
        }
      })) {
    tab_chrome_update_queued_ = false;
  }
}

void Shell::UpdateTabShoulders() {
  if (!root_ || !tab_view_ || !selected_tab_fill_ ||
      !selected_tab_path_ || !selected_tab_layer_ ||
      !tab_focus_border_ || !tab_focus_layer_) {
    return;
  }
  try {
    selected_tab_path_.Visibility(Visibility::Collapsed);
    tab_focus_border_.Visibility(Visibility::Collapsed);
    const auto selected = tab_view_.SelectedItem().try_as<TabViewItem>();
    if (!selected || selected.ActualWidth() <= 0 ||
        selected.ActualHeight() <= 0) {
      return;
    }

    const auto transparent = SolidColorBrush{Color(0, 0, 0, 0)};
    const auto selected_key =
        winrt::box_value(winrt::hstring{L"TabViewItemHeaderBackgroundSelected"});
    selected.Resources().Insert(selected_key, transparent);
    if (const auto selected_background =
            FindVisualChildByName(selected, L"SelectedBackgroundPath")
                .try_as<winrt::Microsoft::UI::Xaml::Shapes::Shape>()) {
      selected_background.Fill(transparent);
    }

    const double width = selected.ActualWidth();
    const double height = selected.ActualHeight();
    const auto origin = selected.TransformToVisual(root_).TransformPoint(
        winrt::Windows::Foundation::Point{0, 0});
    selected_tab_path_.Data(
        TabSilhouetteGeometry(width, height, selected.CornerRadius()));
    selected_tab_path_.Width(width + 8.0);
    selected_tab_path_.Height(height);
    selected_tab_path_.Fill(selected_tab_fill_);
    selected_tab_path_.Clip(nullptr);
    Canvas::SetLeft(selected_tab_path_, origin.X - 4.0);
    // The tab item is bottom-aligned. Pinning the continuous geometry to the
    // exact row boundary avoids a fractional-pixel gap at non-integer DPI.
    Canvas::SetTop(selected_tab_path_, kTabRowHeight - height);

    int64_t selected_tab_id = -1;
    if (const auto tag = selected.Tag()) {
      if (const auto value =
              tag.try_as<winrt::Windows::Foundation::IPropertyValue>();
          value && value.Type() ==
                       winrt::Windows::Foundation::PropertyType::Int64) {
        selected_tab_id = value.GetInt64();
      }
    }
    if (tab_keyboard_focus_visible_ && focused_tab_id_ == selected_tab_id &&
        width > 12.0 && height > 8.0) {
      tab_focus_border_.Width(width - 12.0);
      tab_focus_border_.Height(height - 8.0);
      Canvas::SetLeft(tab_focus_border_, origin.X + 6.0);
      Canvas::SetTop(tab_focus_border_, kTabRowHeight - height + 4.0);
      tab_focus_border_.Visibility(Visibility::Visible);
    }

    // The selected item's native z=20 path would normally cover the tiny
    // anti-aliased fringe where its 4-DIP shoulders enter the adjacent item.
    // The island-clipped replacement lives below TabView, so reserve exactly
    // one physical pixel in each adjacent TabContainer instead. This preserves
    // the complete native hover/pressed surface without letting it tint that
    // fringe, and it also avoids a visible multi-DIP gap between tab bodies.
    std::vector<TabViewItem> ordered_items;
    ordered_items.reserve(tab_view_.TabItems().Size());
    int selected_index = -1;
    for (uint32_t index = 0; index < tab_view_.TabItems().Size(); ++index) {
      const auto item = tab_view_.TabItems().GetAt(index).try_as<TabViewItem>();
      if (!item) {
        continue;
      }
      ordered_items.push_back(item);
      item.ApplyTemplate();
      if (const auto container =
              FindVisualChildByName(item, L"TabContainer")
                  .try_as<Grid>()) {
        container.ClearValue(
            winrt::Microsoft::UI::Xaml::FrameworkElement::MarginProperty());
      }
      if (item == selected) {
        selected_index = static_cast<int>(ordered_items.size()) - 1;
      }
    }
    const double pixel = 96.0 / std::max<UINT>(96, GetDpiForWindow(parent_));
    const auto set_neighbor_margin = [&](int index, bool is_left) {
      if (index < 0 || index >= static_cast<int>(ordered_items.size())) {
        return;
      }
      if (const auto container =
              FindVisualChildByName(ordered_items[index], L"TabContainer")
                  .try_as<Grid>()) {
        container.Margin(is_left ? Thickness{0, 0, pixel, 0}
                                 : Thickness{pixel, 0, 0, 0});
      }
    };
    set_neighbor_margin(selected_index - 1, true);
    set_neighbor_margin(selected_index + 1, false);

    if (tab_scroll_viewer_ && tab_scroll_viewer_.ActualWidth() > 0) {
      const auto viewport_origin =
          tab_scroll_viewer_.TransformToVisual(root_).TransformPoint(
              winrt::Windows::Foundation::Point{0, 0});
      winrt::Microsoft::UI::Xaml::Media::RectangleGeometry viewport_clip;
      viewport_clip.Rect(winrt::Windows::Foundation::Rect{
          viewport_origin.X, 0,
          static_cast<float>(tab_scroll_viewer_.ActualWidth()),
          static_cast<float>(kTabRowHeight)});
      selected_tab_layer_.Clip(viewport_clip);
      winrt::Microsoft::UI::Xaml::Media::RectangleGeometry focus_viewport_clip;
      focus_viewport_clip.Rect(viewport_clip.Rect());
      tab_focus_layer_.Clip(focus_viewport_clip);
    } else {
      selected_tab_layer_.Clip(nullptr);
      tab_focus_layer_.Clip(nullptr);
    }
    selected_tab_path_.Visibility(Visibility::Visible);
  } catch (...) {
    selected_tab_path_.Visibility(Visibility::Collapsed);
    tab_focus_border_.Visibility(Visibility::Collapsed);
    OutputDebugStringW(L"Curve Browser tab material update failed.\n");
  }
}

void Shell::ToggleWorkAreaMaximize() {
  if (!parent_ || !IsWindow(parent_)) {
    return;
  }
  if (island_window_ && IsWindow(island_window_)) {
    ShowWindow(island_window_, SW_HIDE);
    island_hidden_for_window_transition_ = true;
  }
  RECT target{};
  if (work_area_maximized_) {
    target = restored_window_bounds_;
    work_area_maximized_ = false;
  } else {
    GetWindowRect(parent_, &restored_window_bounds_);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoW(MonitorFromWindow(parent_, MONITOR_DEFAULTTONEAREST),
                    &monitor_info);
    target = monitor_info.rcWork;
    work_area_maximized_ = true;
  }
  SetWindowPos(parent_, nullptr, target.left, target.top,
               target.right - target.left, target.bottom - target.top,
               SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
  SetTimer(parent_, kDeferredResizeTimerId, kDeferredResizeDelayMs, nullptr);
  const auto callback_lifetime = callback_lifetime_;
  root_.DispatcherQueue().TryEnqueue([this, callback_lifetime] {
    if (callback_lifetime->alive.load(std::memory_order_acquire)) {
      UpdateMaximizeGlyph();
    }
  });
}

void Shell::UpdateMaximizeGlyph() {
  if (!maximize_button_) {
    return;
  }
  maximize_button_.Content(
      Glyph(work_area_maximized_ ? L"\uE923" : L"\uE922", 10));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      maximize_button_, work_area_maximized_ ? L"Restore" : L"Maximize");
}

void Shell::UpdateTitleBarRegions() {
  if (!non_client_pointer_source_ || !IsWindow(parent_)) {
    return;
  }

  RECT client{};
  GetClientRect(parent_, &client);
  const UINT dpi = GetDpiForWindow(parent_);
  const double scale = static_cast<double>(dpi) / 96.0;
  double interactive_width_dip = 48.0;  // Add-tab button.
  for (const auto& [tab_id, item] : tab_items_) {
    (void)tab_id;
    interactive_width_dip +=
        item.ActualWidth() > 0 ? item.ActualWidth() : 160.0;
  }

  const double caption_host_width_dip =
      caption_host_ && caption_host_.ActualWidth() > 0
          ? caption_host_.ActualWidth()
          : 138.0;
  const int caption_host_width = std::clamp(
      static_cast<int>(std::ceil(caption_host_width_dip * scale)), 0,
      static_cast<int>(client.right));
  const int minimum_drag_width = static_cast<int>(48.0 * scale + 0.5);
  const int maximum_passthrough =
      std::max(0, static_cast<int>(client.right) - caption_host_width -
                      minimum_drag_width);
  const int passthrough_width = std::clamp(
      static_cast<int>(std::ceil(interactive_width_dip * scale)), 0,
      maximum_passthrough);
  const int title_height = static_cast<int>(kTabRowHeight * scale + 0.5);

  non_client_pointer_source_.ClearRegionRects(
      winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough);
  std::vector<winrt::Windows::Graphics::RectInt32> passthrough_rectangles;
  if (passthrough_width > 0) {
    passthrough_rectangles.push_back(
        winrt::Windows::Graphics::RectInt32{
            0, 0, passthrough_width, title_height});
  }
  if (caption_host_width > 0) {
    passthrough_rectangles.push_back(
        winrt::Windows::Graphics::RectInt32{
            std::max<LONG>(0, client.right - caption_host_width), 0,
            caption_host_width, title_height});
  }
  if (!passthrough_rectangles.empty()) {
    non_client_pointer_source_.SetRegionRects(
        winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough,
        passthrough_rectangles);
  }
  non_client_pointer_source_.ClearRegionRects(
      winrt::Microsoft::UI::Input::NonClientRegionKind::Caption);
  const int caption_left = passthrough_width;
  const int caption_right = std::max(caption_left,
                                     static_cast<int>(client.right) -
                                         caption_host_width);
  if (caption_right > caption_left) {
    const std::array rectangles = {winrt::Windows::Graphics::RectInt32{
        caption_left, 0, caption_right - caption_left, title_height}};
    non_client_pointer_source_.SetRegionRects(
        winrt::Microsoft::UI::Input::NonClientRegionKind::Caption,
        rectangles);
  }

}

bool Shell::IsNativePage(std::wstring_view url) const {
  if (EqualsInsensitive(url, L"chrome://settings") ||
      EqualsInsensitive(url, L"chrome://settings/") ||
      StartsWithInsensitive(url, L"chrome://settings/manageProfile") ||
      StartsWithInsensitive(url, L"chrome://settings/help")) {
    return true;
  }
  constexpr std::wstring_view prefixes[] = {
      L"chrome://downloads", L"chrome://history", L"chrome://bookmarks",
      L"chrome://extensions",
      L"chrome://password-manager"};
  return std::any_of(std::begin(prefixes), std::end(prefixes),
                     [url](std::wstring_view prefix) {
                       return StartsWithInsensitive(url, prefix);
                     });
}

std::wstring Shell::NativePageTitle(std::wstring_view url) const {
  if (StartsWithInsensitive(url, L"chrome://settings/manageProfile")) {
    return L"Profiles";
  }
  if (StartsWithInsensitive(url, L"chrome://settings/help")) {
    return L"About Curve Browser";
  }
  if (StartsWithInsensitive(url, L"chrome://downloads")) return L"Downloads";
  if (StartsWithInsensitive(url, L"chrome://history")) return L"History";
  if (StartsWithInsensitive(url, L"chrome://bookmarks")) return L"Bookmarks";
  if (StartsWithInsensitive(url, L"chrome://extensions")) return L"Extensions";
  if (StartsWithInsensitive(url, L"chrome://password-manager")) return L"Passwords";
  return L"Settings";
}

void Shell::UpdateNativePage(std::wstring_view url) {
  const bool should_show = IsNativePage(url);
  if (!should_show) {
    native_page_host_.Visibility(Visibility::Collapsed);
    native_page_host_.Children().Clear();
    native_page_visible_ = false;
    ResizeIsland();
    return;
  }

  native_page_visible_ = true;
  native_page_host_.Children().Clear();
  native_page_host_.Visibility(Visibility::Visible);

  ScrollViewer scroll;
  scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
  winrt::Microsoft::UI::Xaml::Media::Animation::TransitionCollection
      page_transitions;
  page_transitions.Append(
      winrt::Microsoft::UI::Xaml::Media::Animation::EntranceThemeTransition{});
  scroll.Transitions(page_transitions);
  scroll.HorizontalContentAlignment(HorizontalAlignment::Center);
  StackPanel page;
  const double available_width =
      root_.ActualWidth() > 96 ? root_.ActualWidth() - 64 : 840;
  page.Width(std::max(420.0, std::min(920.0, available_width)));
  page.HorizontalAlignment(HorizontalAlignment::Center);
  page.Padding(Thickness{32, 24, 32, 48});

  TextBlock title;
  title.Text(NativePageTitle(url));
  title.FontSize(28);
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  title.Margin(Thickness{0, 0, 0, 20});
  page.Children().Append(title);

  if (StartsWithInsensitive(url, L"chrome://settings/manageProfile")) {
    Button avatar;
    avatar.Content(Glyph(L"\uE77B", 24));
    avatar.Width(44);
    avatar.Height(44);
    page.Children().Append(MakeSettingsCard(
        L"Local profile",
        L"Bookmarks, history, passwords, and preferences stay in this local Chromium profile.",
        avatar));

    TextBox profile_name;
    profile_name.Text(L"Local profile");
    profile_name.MinWidth(220);
    page.Children().Append(MakeSettingsCard(
        L"Profile name", L"Choose the name shown in the browser toolbar.",
        profile_name));

    Button new_profile;
    new_profile.Content(winrt::box_value(L"Add profile"));
    new_profile.Click([this](const auto&, const auto&) {
      Invoke(WCS_COMMAND_NEW_WINDOW);
    });
    page.Children().Append(MakeSettingsCard(
        L"Other profiles",
        L"Create another isolated local browsing profile in a new window.",
        new_profile));
  } else if (StartsWithInsensitive(url, L"chrome://settings/help")) {
    TextBlock engine;
    engine.Text(L"Chromium engine");
    engine.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    page.Children().Append(MakeSettingsCard(
        L"Browser engine",
        L"Built on ungoogled Chromium with native Windows 11 browser chrome.",
        engine));

    TextBlock updates;
    updates.Text(L"Up to date");
    updates.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
        Color(16, 124, 16)});
    page.Children().Append(MakeSettingsCard(
        L"Updates",
        L"Update checks use the Curve Browser release channel and never require a Google account.",
        updates));
  } else if (StartsWithInsensitive(url, L"chrome://settings")) {
    TextBlock privacy;
    privacy.Text(L"uBlock Origin bundled");
    privacy.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    page.Children().Append(MakeSettingsCard(
        L"Tracking protection",
        L"uBlock Origin is installed from its signed Chrome Web Store package for every new local profile.",
        privacy));
    ToggleSwitch startup;
    startup.IsOn(restore_on_startup_);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        startup, L"Restore previous session");
    startup.Toggled([this](const auto& sender, const auto&) {
      const auto toggle = sender.template as<ToggleSwitch>();
      Invoke(WCS_COMMAND_SET_RESTORE_ON_STARTUP, -1, -1, nullptr,
             toggle.IsOn() ? 1u : 0u);
    });
    page.Children().Append(MakeSettingsCard(
        L"Continue where you left off",
        L"Restore your local windows and tabs when Curve Browser starts.",
        startup));
    Button search;
    search.Content(winrt::box_value(L"Manage"));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        search, L"Manage search engine");
    search.Click([this](const auto&, const auto&) {
      Invoke(WCS_COMMAND_OPEN_SEARCH_SETTINGS);
    });
    page.Children().Append(MakeSettingsCard(
        L"Search engine",
        L"Choose the service used for native address-bar searches.",
        search));
    Button passwords;
    passwords.Content(winrt::box_value(L"Open"));
    passwords.Click([this](const auto&, const auto&) {
      Invoke(WCS_COMMAND_OPEN_PASSWORDS);
    });
    page.Children().Append(MakeSettingsCard(
        L"Passwords and autofill",
        L"Manage locally stored passwords, addresses, and payment methods.",
        passwords));
  } else if (StartsWithInsensitive(url, L"chrome://downloads")) {
    if (native_downloads_.empty()) {
      TextBlock empty;
      empty.Text(L"No downloads are stored in this profile.");
      empty.FontSize(15);
      empty.Opacity(0.78);
      page.Children().Append(empty);
    }
    for (const auto& download : native_downloads_) {
      StackPanel actions;
      actions.Orientation(Orientation::Horizontal);
      actions.Spacing(6);
      if (download.in_progress) {
        ProgressRing progress;
        progress.IsActive(true);
        progress.Width(24);
        progress.Height(24);
        progress.Margin(UniformThickness(4));
        actions.Children().Append(progress);
      }
      if (download.complete) {
        Button open;
        open.Content(winrt::box_value(L"Open"));
        const uint32_t id = download.id;
        open.Click([this, id](const auto&, const auto&) {
          Invoke(WCS_COMMAND_OPEN_DOWNLOAD, id);
        });
        actions.Children().Append(open);

        Button show;
        show.Content(ToolbarGlyph(L"\uE838"));
        show.Width(32);
        show.Height(32);
        show.Padding(UniformThickness(0));
        ToolTipService::SetToolTip(show, winrt::box_value(L"Show in folder"));
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
            show, L"Show in folder");
        show.Click([this, id](const auto&, const auto&) {
          Invoke(WCS_COMMAND_SHOW_DOWNLOAD_IN_FOLDER, id);
        });
        actions.Children().Append(show);
      }
      std::wstring detail = download.status;
      if (!download.target_path.empty()) {
        if (!detail.empty()) {
          detail += L"\n";
        }
        detail += download.target_path;
      } else if (!download.url.empty()) {
        if (!detail.empty()) {
          detail += L"\n";
        }
        detail += download.url;
      }
      page.Children().Append(MakeSettingsCard(
          download.title.empty() ? L"Download" : download.title, detail,
          actions));
    }
  } else if (StartsWithInsensitive(url, L"chrome://bookmarks")) {
    if (native_bookmarks_.empty()) {
      TextBlock empty;
      empty.Text(L"Your bookmark library is empty.");
      empty.FontSize(15);
      empty.Opacity(0.78);
      page.Children().Append(empty);
    }
    for (const auto& bookmark : native_bookmarks_) {
      if (bookmark.is_folder) {
        FontIcon folder;
        folder.Glyph(L"\uE8B7");
        folder.FontSize(18);
        page.Children().Append(MakeSettingsCard(
            bookmark.title.empty() ? L"Bookmark folder" : bookmark.title,
            L"Folder", folder));
        continue;
      }
      Button open;
      open.Content(winrt::box_value(L"Open"));
      const std::wstring destination = bookmark.url;
      open.Click([this, destination](const auto&, const auto&) {
        Invoke(WCS_COMMAND_OPEN_BOOKMARK, -1, -1, destination.c_str());
      });
      page.Children().Append(MakeSettingsCard(
          bookmark.title.empty() ? bookmark.url : bookmark.title,
          bookmark.url, open));
    }
  } else if (StartsWithInsensitive(url, L"chrome://history")) {
    if (history_loading_) {
      ProgressRing loading;
      loading.IsActive(true);
      loading.Width(28);
      loading.Height(28);
      loading.HorizontalAlignment(HorizontalAlignment::Left);
      page.Children().Append(MakeSettingsCard(
          L"Loading browsing history",
          L"Reading recent visits from your local Chromium profile.",
          loading));
    } else if (native_history_.empty()) {
      TextBlock empty;
      empty.Text(L"No browsing history is stored in this profile.");
      empty.FontSize(15);
      empty.Opacity(0.78);
      page.Children().Append(empty);
    }
    for (const auto& entry : native_history_) {
      Button open;
      open.Content(winrt::box_value(L"Open"));
      const std::wstring destination = entry.url;
      open.Click([this, destination](const auto&, const auto&) {
        Invoke(WCS_COMMAND_OPEN_BOOKMARK, -1, -1, destination.c_str());
      });
      std::wstring detail = entry.url;
      if (!entry.visit_time.empty()) {
        detail += L"\n";
        detail += entry.visit_time;
      }
      page.Children().Append(MakeSettingsCard(
          entry.title.empty() ? entry.url : entry.title, detail, open));
    }
  } else {
    TextBlock description;
    description.Text(L"This native Windows 11 surface is connected to the active Chromium tab. More data controls will appear here as their browser services finish loading.");
    description.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
    description.FontSize(15);
    description.Opacity(0.78);
    page.Children().Append(description);
  }

  scroll.Content(page);
  native_page_host_.Children().Append(scroll);
  ResizeIsland();
}

void Shell::SetVisible(bool visible) {
  visible_ = visible;
  if (island_window_) {
    ShowWindow(island_window_, visible ? SW_SHOWNA : SW_HIDE);
  }
}

void Shell::ShowRestorePrompt() {
  restore_dialog_ = ContentDialog{};
  restore_dialog_.XamlRoot(root_.XamlRoot());
  restore_dialog_.Title(winrt::box_value(L"Restore pages?"));
  restore_dialog_.PrimaryButtonText(L"Restore");
  restore_dialog_.CloseButtonText(L"Not now");
  restore_dialog_.DefaultButton(ContentDialogButton::Primary);

  StackPanel content;
  content.Spacing(12);
  FontIcon icon;
  icon.Glyph(L"\uE777");
  icon.FontSize(28);
  icon.HorizontalAlignment(HorizontalAlignment::Left);
  content.Children().Append(icon);
  TextBlock message;
  message.Text(
      L"Curve Browser didn\u2019t shut down correctly. Restore your previous "
      L"windows and tabs?");
  message.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
  content.Children().Append(message);
  restore_dialog_.Content(content);
  restore_dialog_.PrimaryButtonClick([this](const auto&, const auto&) {
    Invoke(WCS_COMMAND_RESTORE_SESSION);
  });
  restore_dialog_.ShowAsync();
}

HRESULT Shell::Capture(const wchar_t* output_path) {
  if (!output_path || !*output_path) {
    return E_INVALIDARG;
  }
  try {
    CaptureAsync(root_, output_path);
    return S_OK;
  } catch (...) {
    return winrt::to_hresult();
  }
}

int32_t Shell::ShowContextMenu(const WcsContextMenuItem* items,
                               size_t item_count,
                               int32_t screen_x,
                               int32_t screen_y) {
  if (!items || !item_count || !root_ || !IsWindow(parent_)) {
    return -1;
  }

  MenuFlyout flyout;
  std::map<int32_t, MenuFlyoutSubItem> submenus;
  int32_t selected_command = -1;
  bool closed = false;

  const auto append_item =
      [&](int32_t parent_index, const MenuFlyoutItemBase& item) {
        if (parent_index >= 0) {
          const auto parent = submenus.find(parent_index);
          if (parent != submenus.end()) {
            parent->second.Items().Append(item);
          }
          return;
        }
        flyout.Items().Append(item);
      };

  for (size_t index = 0; index < item_count; ++index) {
    const WcsContextMenuItem& source = items[index];
    if (source.size < sizeof(WcsContextMenuItem)) {
      continue;
    }
    if (source.type == WCS_CONTEXT_MENU_SEPARATOR) {
      append_item(source.parent_index, MenuFlyoutSeparator{});
      continue;
    }
    if (source.type == WCS_CONTEXT_MENU_SUBMENU) {
      MenuFlyoutSubItem submenu;
      const std::wstring label = StripMenuMnemonics(source.label);
      submenu.Text(label);
      submenu.IsEnabled(source.enabled != 0);
      submenus.emplace(static_cast<int32_t>(index), submenu);
      append_item(source.parent_index, submenu);
      continue;
    }

    MenuFlyoutItem item;
    const std::wstring label = StripMenuMnemonics(source.label);
    item.Text(label);
    if (const std::wstring_view glyph = ContextMenuGlyph(label);
        !glyph.empty()) {
      item.Icon(ContextMenuIcon(glyph));
    }
    item.IsEnabled(source.enabled != 0);
    const int32_t command = source.command_id;
    item.Click([&flyout, &selected_command, command](
                   const winrt::Windows::Foundation::IInspectable&,
                   const winrt::Microsoft::UI::Xaml::RoutedEventArgs&) {
      selected_command = command;
      flyout.Hide();
    });
    append_item(source.parent_index, item);
  }

  flyout.Closed([&closed](
                    const winrt::Windows::Foundation::IInspectable&,
                    const winrt::Windows::Foundation::IInspectable&) {
    closed = true;
  });

  POINT client_point{screen_x, screen_y};
  ScreenToClient(parent_, &client_point);
  const float scale = static_cast<float>(GetDpiForWindow(parent_)) / 96.0f;
  winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
  options.Position(winrt::Windows::Foundation::Point{
      client_point.x / scale, client_point.y / scale});
  options.ShowMode(
      winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutShowMode::Standard);
  flyout.ShowAt(root_, options);

  MSG message{};
  while (!closed) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      if (result == 0) {
        PostQuitMessage(static_cast<int>(message.wParam));
      }
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return selected_command;
}

winrt::fire_and_forget Shell::CaptureAsync(
    winrt::Microsoft::UI::Xaml::Controls::Grid root,
    std::wstring output_path) {
  try {
    winrt::Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
    co_await bitmap.RenderAsync(root);
    const int32_t width = bitmap.PixelWidth();
    const int32_t height = bitmap.PixelHeight();
    if (width <= 0 || height <= 0) {
      co_return;
    }

    const auto buffer = co_await bitmap.GetPixelsAsync();
    std::vector<uint8_t> pixels(buffer.Length());
    auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(
        buffer);
    reader.ReadBytes(pixels);

    const std::filesystem::path destination(output_path);
    const auto folder = co_await winrt::Windows::Storage::StorageFolder::
        GetFolderFromPathAsync(destination.parent_path().wstring());
    const auto file = co_await folder.CreateFileAsync(
        destination.filename().wstring(),
        winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting);
    const auto stream = co_await file.OpenAsync(
        winrt::Windows::Storage::FileAccessMode::ReadWrite);
    const auto encoder =
        co_await winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
            winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(),
            stream);
    encoder.SetPixelData(
        winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
        winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,
        width, height, 96.0, 96.0, pixels);
    co_await encoder.FlushAsync();
    co_await stream.FlushAsync();
  } catch (const winrt::hresult_error& error) {
    std::wstring message = L"Curve Browser shell capture failed: ";
    message.append(error.message().c_str());
    message.push_back(L'\n');
    OutputDebugStringW(message.c_str());
    std::fwprintf(stderr, L"%ls", message.c_str());
  }
}

void Shell::UpdateTitleBarMetrics() {
  if (!parent_ || !IsWindow(parent_)) {
    return;
  }
  const UINT dpi = GetDpiForWindow(parent_);
  const double scale = static_cast<double>(dpi) / 96.0;
  double caption_width = 138.0;
  if (title_bar_ && title_bar_.RightInset() > 0) {
    caption_width = title_bar_.RightInset() / scale;
  }
  // AppWindow can refresh caption resources during a maximize, restore, DPI,
  // or presenter transaction. Reassert transparency whenever those metrics
  // settle so its reserved controls cannot flash under Curve's full-height
  // WinUI caption layer.
  if (!native_title_bar_suppressed_) {
    KeepNativeCaptionButtonsTransparent(title_bar_);
  }
  if (caption_host_) {
    caption_host_.Width(caption_width);
  }
  if (tab_view_) {
    tab_view_.Margin(Thickness{0, 0, caption_width, 0});
  }
  const MARGINS frame_margins{
      0, 0, static_cast<int>(kTabRowHeight * scale + 0.5), 0};
  DwmExtendFrameIntoClientArea(parent_, &frame_margins);
  ScheduleTabChromeUpdate();
}

void Shell::ResizeIsland() {
  if (!island_window_ || !IsWindow(parent_)) {
    return;
  }
  UpdateTitleBarMetrics();
  RECT client{};
  GetClientRect(parent_, &client);
  const UINT dpi = GetDpiForWindow(parent_);
  const double scale = static_cast<double>(dpi) / 96.0;
  const int shell_height = static_cast<int>(kShellHeight * scale + 0.5);
  const int bookmark_height =
      bookmark_bar_visible_
          ? static_cast<int>(kBookmarkBarHeight * scale + 0.5)
          : 0;
  const int height = (native_page_visible_ || sidebar_visible_)
                         ? client.bottom
                         : shell_height + bookmark_height;
  if (client.right <= 0 || height <= 0) {
    return;
  }
  // Once DesktopWindowXamlSource has created its child HWND, normal window
  // sizing is sufficient. SiteBridge::MoveAndResize during a maximize
  // non-client transaction can asynchronously raise a fatal XAML stowed
  // exception even when the synchronous call succeeds.
  SetWindowPos(island_window_, HWND_TOP, 0, 0, client.right, height,
               SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
  // The window region is not size-dependent. Changing it during maximize can
  // re-enter XAML's non-client transaction and surface as a 0xc000027b stowed
  // exception. Recompute passthrough regions on the XAML dispatcher instead.
  ScheduleTabChromeUpdate();
  if (sidebar_visible_) {
    const auto callback_lifetime = callback_lifetime_;
    root_.DispatcherQueue().TryEnqueue([this, callback_lifetime] {
      if (callback_lifetime->alive.load(std::memory_order_acquire)) {
        UpdateWindowRegion();
      }
    });
  }
}

void Shell::UpdateWindowRegion() {
  if (!island_window_ || !IsWindow(parent_)) {
    return;
  }
  if (!sidebar_visible_ || native_page_visible_) {
    SetWindowRgn(island_window_, nullptr, TRUE);
    return;
  }
  RECT client{};
  GetClientRect(parent_, &client);
  const double scale = static_cast<double>(GetDpiForWindow(parent_)) / 96.0;
  const int top_height = static_cast<int>(
      (kShellHeight + (bookmark_bar_visible_ ? kBookmarkBarHeight : 0)) *
          scale +
      0.5);
  const int sidebar_width =
      static_cast<int>(kSidebarWidth * scale + 0.5);
  HRGN top = CreateRectRgn(0, 0, client.right, top_height);
  HRGN side = CreateRectRgn(
      std::max(0, static_cast<int>(client.right) - sidebar_width), top_height,
      client.right,
      client.bottom);
  CombineRgn(top, top, side, RGN_OR);
  DeleteObject(side);
  // SetWindowRgn takes ownership of `top` on success.
  if (!SetWindowRgn(island_window_, top, TRUE)) {
    DeleteObject(top);
  }
}

void Shell::ApplySystemTheme() {
  try {
    winrt::Windows::UI::ViewManagement::UISettings settings;
    const auto background = settings.GetColorValue(
        winrt::Windows::UI::ViewManagement::UIColorType::Background);
    const bool dark = (static_cast<int>(background.R) + background.G +
                       background.B) < 384;
    root_.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
    const BOOL dark_mode = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(parent_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_mode,
                          sizeof(dark_mode));
    const DWM_SYSTEMBACKDROP_TYPE backdrop_type = DWMSBT_TABBEDWINDOW;
    DwmSetWindowAttribute(parent_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop_type,
                          sizeof(backdrop_type));
    const UINT dpi = GetDpiForWindow(parent_);
    const MARGINS frame_margins{
        0, 0, static_cast<int>(kTabRowHeight * dpi / 96.0 + 0.5), 0};
    DwmExtendFrameIntoClientArea(parent_, &frame_margins);
    if (title_bar_) {
      using winrt::Microsoft::UI::Windowing::TitleBarTheme;
      title_bar_.PreferredTheme(dark ? TitleBarTheme::Dark
                                     : TitleBarTheme::Light);
      // The WinUI caption layer owns the visible glyphs and input. Keep the
      // reserved AppWindow controls transparent so two caption sets can never
      // appear during a presenter/theme transition.
      if (!native_title_bar_suppressed_) {
        KeepNativeCaptionButtonsTransparent(title_bar_);
      }
    }
    // Mica Alt is the base layer. File Explorer puts a Fluent commanding
    // layer over it, then uses that exact same material for the selected tab.
    const auto transparent = SolidColorBrush{Color(0, 0, 0, 0)};
    const auto commanding_layer = ThemeBrush(
        L"LayerOnMicaBaseAltFillColorDefaultBrush",
        dark ? Color(58, 58, 58, 115) : Color(255, 255, 255, 179));
    const auto content_layer = ThemeBrush(
        L"LayerFillColorDefaultBrush",
        dark ? Color(58, 58, 58, 76) : Color(255, 255, 255, 128));
    // Reuse the exact same brush instance for the toolbar and TabView's
    // continuous selected geometry. This keeps the connected surface on one
    // Fluent commanding material rather than compositing separate pieces.
    selected_tab_fill_ = commanding_layer;
    selected_tab_path_.Fill(selected_tab_fill_);
    const auto focus_brush = ThemeBrush(
        L"FocusStrokeColorOuterBrush",
        dark ? Color(255, 255, 255) : Color(0, 0, 0));
    tab_focus_border_.BorderBrush(focus_brush);

    root_.Background(transparent);
    tab_view_.Background(transparent);
    toolbar_.Background(commanding_layer);
    bookmark_bar_.Background(commanding_layer);
    sidebar_.Background(content_layer);
    bookmark_divider_.Background(ThemeBrush(
        L"DividerStrokeColorDefaultBrush",
        dark ? Color(255, 255, 255, 20) : Color(0, 0, 0, 20)));
    native_page_host_.Background(content_layer);
    const auto selected_key =
        winrt::box_value(winrt::hstring{L"TabViewItemHeaderBackgroundSelected"});
    const auto drag_key =
        winrt::box_value(winrt::hstring{L"TabViewItemHeaderDragBackground"});
    const auto border_key =
        winrt::box_value(winrt::hstring{L"TabViewBorderBrush"});
    const auto selected_border_key =
        winrt::box_value(winrt::hstring{L"TabViewSelectedItemBorderBrush"});
    const auto selected_border_thickness_key =
        winrt::box_value(
            winrt::hstring{L"TabViewSelectedItemBorderThickness"});
    // The item-owned path is clipped in a Desktop XAML island. Keep all stock
    // selected fills/strokes transparent and let selected_tab_path_ be the
    // only command-layer sample, preventing double-composited light edges.
    const auto app_resources =
        winrt::Microsoft::UI::Xaml::Application::Current().Resources();
    app_resources.Insert(selected_key, transparent);
    app_resources.Insert(drag_key, commanding_layer);
    app_resources.Insert(border_key, transparent);
    app_resources.Insert(selected_border_key, transparent);
    app_resources.Insert(selected_border_thickness_key,
                         winrt::box_value(UniformThickness(0)));
    tab_view_.Resources().Insert(selected_key, transparent);
    tab_view_.Resources().Insert(drag_key, commanding_layer);
    tab_view_.Resources().Insert(border_key, transparent);
    tab_view_.Resources().Insert(selected_border_key, transparent);
    tab_view_.Resources().Insert(selected_border_thickness_key,
                                 winrt::box_value(UniformThickness(0)));
    ScheduleTabChromeUpdate();
  } catch (...) {
    root_.RequestedTheme(ElementTheme::Default);
  }
}

void Shell::AttachWindowSubclass() {
  winrt::check_bool(SetWindowSubclass(
      parent_, &Shell::ParentSubclassProc, kParentSubclassId,
      reinterpret_cast<DWORD_PTR>(this)) != FALSE);
}

void Shell::DetachWindowSubclass() {
  if (parent_ && IsWindow(parent_)) {
    RemoveWindowSubclass(parent_, &Shell::ParentSubclassProc,
                         kParentSubclassId);
  }
}

LRESULT CALLBACK Shell::ParentSubclassProc(HWND window,
                                           UINT message,
                                           WPARAM wparam,
                                           LPARAM lparam,
                                           UINT_PTR subclass_id,
                                           DWORD_PTR reference_data) {
  auto* shell = reinterpret_cast<Shell*>(reference_data);
  switch (message) {
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0) == SC_MAXIMIZE ||
          ((wparam & 0xFFF0) == SC_RESTORE &&
           shell->work_area_maximized_)) {
        // AppWindow's presenter fail-fasts in Microsoft.UI.Xaml when it tries
        // to maximize this unpackaged Chromium HWND. Apply the monitor work
        // area bounds directly instead. This retains the real caption command
        // and exact restore rectangle without entering the failing presenter.
        shell->ToggleWorkAreaMaximize();
        return 0;
      }
      break;
    case WM_SIZE:
      if (wparam == SIZE_MAXIMIZED && shell->island_window_ &&
          IsWindow(shell->island_window_)) {
        ShowWindow(shell->island_window_, SW_HIDE);
        shell->island_hidden_for_window_transition_ = true;
      }
      [[fallthrough]];
    case WM_DPICHANGED:
      // AppWindow and Chromium both adjust child HWND geometry while handling
      // these messages. Resize the XAML island after that transaction instead
      // of re-entering it from inside the native maximize/DPI path.
      SetTimer(window, kDeferredResizeTimerId, kDeferredResizeDelayMs, nullptr);
      break;
    case WM_TIMER:
      if (wparam == kDeferredResizeTimerId) {
        KillTimer(window, kDeferredResizeTimerId);
        try {
          shell->ResizeIsland();
          if (shell->island_hidden_for_window_transition_ &&
              shell->island_window_ && IsWindow(shell->island_window_)) {
            ShowWindow(shell->island_window_, SW_SHOWNA);
            shell->island_hidden_for_window_transition_ = false;
          }
        } catch (...) {
          // No C++/WinRT exception may escape a Win32 subclass callback. XAML
          // reports such escapes as fatal stowed exceptions (0xc000027b).
          OutputDebugStringW(L"Curve Browser deferred resize failed.\n");
        }
      }
      break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
      shell->ApplySystemTheme();
      break;
    case WM_NCDESTROY:
      KillTimer(window, kDeferredResizeTimerId);
      RemoveWindowSubclass(window, &Shell::ParentSubclassProc, subclass_id);
      shell->parent_ = nullptr;
      break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK Shell::IslandSubclassProc(HWND window,
                                           UINT message,
                                           WPARAM wparam,
                                           LPARAM lparam,
                                           UINT_PTR subclass_id,
                                           DWORD_PTR reference_data) {
  (void)reference_data;
  switch (message) {
    case WM_MOUSEACTIVATE: {
      const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
      if (result != MA_NOACTIVATE && result != MA_NOACTIVATEANDEAT) {
        SetFocus(window);
      }
      return result;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_POINTERDOWN:
      SetFocus(window);
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(window, &Shell::IslandSubclassProc, subclass_id);
      break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace windows_chromium

extern "C" uint32_t __stdcall WcsGetApiVersion(void) {
  return WCS_API_VERSION;
}

extern "C" HRESULT __stdcall WcsCreateShell(
    HWND parent,
    const WcsHostCallbacks* callbacks,
    WcsShellHandle* shell) {
  if (!callbacks || callbacks->size < sizeof(WcsHostCallbacks) ||
      callbacks->api_version != WCS_API_VERSION || !shell) {
    return E_INVALIDARG;
  }
  try {
    *shell = new windows_chromium::Shell(parent, *callbacks);
    return S_OK;
  } catch (...) {
    *shell = nullptr;
    return winrt::to_hresult();
  }
}

extern "C" void __stdcall WcsDestroyShell(WcsShellHandle shell) {
  delete static_cast<windows_chromium::Shell*>(shell);
}

extern "C" HRESULT __stdcall WcsUpdateWindowState(
    WcsShellHandle shell,
    const WcsWindowState* state) {
  if (!shell || !state || state->size < sizeof(WcsWindowState)) {
    return E_INVALIDARG;
  }
  return static_cast<windows_chromium::Shell*>(shell)->Update(*state);
}

extern "C" void __stdcall WcsSetVisible(WcsShellHandle shell, BOOL visible) {
  if (shell) {
    static_cast<windows_chromium::Shell*>(shell)->SetVisible(visible != FALSE);
  }
}

extern "C" void __stdcall WcsShowFind(WcsShellHandle shell) {
  if (shell) {
    static_cast<windows_chromium::Shell*>(shell)->ShowFindFlyout();
  }
}

extern "C" void __stdcall WcsShowRestorePrompt(WcsShellHandle shell) {
  if (shell) {
    static_cast<windows_chromium::Shell*>(shell)->ShowRestorePrompt();
  }
}

extern "C" HRESULT __stdcall WcsCaptureShell(WcsShellHandle shell,
                                               const wchar_t* output_path) {
  if (!shell) {
    return E_INVALIDARG;
  }
  return static_cast<windows_chromium::Shell*>(shell)->Capture(output_path);
}

extern "C" void __stdcall WcsSetVisualStateForTesting(
    WcsShellHandle shell,
    const wchar_t* state) {
  if (shell && state) {
    static_cast<windows_chromium::Shell*>(shell)->SetVisualStateForTesting(
        state);
  }
}

extern "C" int32_t __stdcall WcsShowContextMenu(
    WcsShellHandle shell,
    const WcsContextMenuItem* items,
    size_t item_count,
    int32_t screen_x,
    int32_t screen_y) {
  if (!shell) {
    return -1;
  }
  try {
    return static_cast<windows_chromium::Shell*>(shell)->ShowContextMenu(
        items, item_count, screen_x, screen_y);
  } catch (...) {
    return -1;
  }
}
