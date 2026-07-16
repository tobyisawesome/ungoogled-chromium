// Copyright 2026 The Curve Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "pch.h"

#include "shell.h"

#include "App.xaml.h"

#include <algorithm>
#include <array>
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
constexpr double kToolbarHeight = 48.0;
constexpr double kShellHeight = kTabRowHeight + kToolbarHeight;
constexpr UINT_PTR kParentSubclassId = 0x57435331;  // "WCS1"
constexpr UINT kDeferredResizeMessage = WM_APP + 0x351;

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

FontIcon Glyph(std::wstring_view glyph, double size = 16.0) {
  FontIcon icon;
  icon.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{
      L"Segoe Fluent Icons, Segoe MDL2 Assets"});
  icon.Glyph(glyph);
  icon.FontSize(size);
  return icon;
}

FontIcon ToolbarGlyph(std::wstring_view glyph) {
  // Let WinUI center the glyph inside the 32 px pointer-state surface. A
  // manual baseline shift moves the ink away from the Button's actual center
  // and makes the hover shape appear offset even when its bounds are correct.
  return Glyph(glyph, 16.0);
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

winrt::Microsoft::UI::Xaml::Media::Geometry TabShoulderGeometry(bool left) {
  using namespace winrt::Microsoft::UI::Xaml::Media;
  using winrt::Windows::Foundation::Point;

  PathGeometry geometry;
  PathFigure figure;
  figure.StartPoint(left ? Point{8, 0} : Point{0, 0});
  figure.IsClosed(true);

  BezierSegment curve;
  curve.Point1(left ? Point{8, 4.418f} : Point{0, 4.418f});
  curve.Point2(left ? Point{4.418f, 8} : Point{3.582f, 8});
  curve.Point3(left ? Point{0, 8} : Point{8, 8});
  figure.Segments().Append(curve);

  LineSegment bottom;
  bottom.Point(left ? Point{8, 8} : Point{0, 8});
  figure.Segments().Append(bottom);
  geometry.Figures().Append(figure);
  return geometry.as<Geometry>();
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
  card.CornerRadius(CornerRadius{8});
  card.Padding(Thickness{16, 12, 12, 12});
  card.Margin(Thickness{0, 0, 0, 8});
  card.BorderThickness(Thickness{1});

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
  AttachWindowSubclass();
  ApplySystemTheme();
  ResizeIsland();

  // Mica Alt is the system backdrop intended for windows with tabbed title
  // bars. Older Windows versions safely ignore this window attribute.
  const DWM_SYSTEMBACKDROP_TYPE backdrop_type = DWMSBT_TABBEDWINDOW;
  DwmSetWindowAttribute(parent_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop_type,
                        sizeof(backdrop_type));
  SetPropW(parent_, L"CurveBrowserNativeShellActive",
           reinterpret_cast<HANDLE>(this));
}

Shell::~Shell() {
  if (parent_ && IsWindow(parent_)) {
    RemovePropW(parent_, L"CurveBrowserNativeShellActive");
  }
  DetachWindowSubclass();
  if (non_client_pointer_source_) {
    non_client_pointer_source_.ClearAllRegionRects();
  }
  if (title_bar_) {
    title_bar_.ResetToDefault();
  }
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
  title_bar_ = AppWindow::GetFromWindowId(window_id).TitleBar();
  title_bar_.ExtendsContentIntoTitleBar(true);
  title_bar_.PreferredHeightOption(TitleBarHeightOption::Tall);
  non_client_pointer_source_ =
      winrt::Microsoft::UI::Input::InputNonClientPointerSource::
          GetForWindowId(window_id);
}

void Shell::BuildVisualTree() {
  root_ = Grid{};
  RowDefinition tabs_row;
  tabs_row.Height(GridLength{kTabRowHeight, GridUnitType::Pixel});
  root_.RowDefinitions().Append(tabs_row);
  RowDefinition toolbar_row;
  toolbar_row.Height(GridLength{kToolbarHeight, GridUnitType::Pixel});
  root_.RowDefinitions().Append(toolbar_row);
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
  // The custom title bar uses the 48 px tall system-caption metric so the
  // caption buttons span the complete row. Keep TabView's familiar 40 px
  // strip bottom-aligned, preserving the 8 px Explorer-style top inset while
  // its selected item and shoulders sink directly into the command layer.
  tab_view_.Height(40);
  tab_view_.Padding(Thickness{0});
  const double caption_width = title_bar_
                                   ? title_bar_.RightInset() * 96.0 /
                                         GetDpiForWindow(parent_)
                                   : 138.0;
  tab_view_.Margin(Thickness{0, 0, caption_width, 0});
  tab_view_.VerticalAlignment(VerticalAlignment::Bottom);
  Grid::SetRow(tab_view_, 0);
  Canvas::SetZIndex(tab_view_, 2);
  root_.Children().Append(tab_view_);
  tab_view_.Loaded([this](const auto&, const auto&) {
    tab_view_.ApplyTemplate();
    tab_view_.DispatcherQueue().TryEnqueue([this] {
      if (const auto add_button =
              FindVisualChildByName(tab_view_, L"AddButton")
                  .try_as<Button>()) {
        add_button.Height(32);
        add_button.Width(32);
        add_button.Margin(Thickness{0});
        add_button.Padding(Thickness{0});
        add_button.VerticalAlignment(VerticalAlignment::Center);
        add_button.VerticalContentAlignment(VerticalAlignment::Center);
        add_button.HorizontalContentAlignment(HorizontalAlignment::Center);
      }
    });
  });
  tab_view_.SizeChanged(
      [this](const auto&, const auto&) {
        UpdateTitleBarRegions();
        UpdateTabShoulders();
      });

  tab_shoulder_layer_ = Canvas{};
  tab_shoulder_layer_.IsHitTestVisible(false);
  tab_shoulder_layer_.HorizontalAlignment(HorizontalAlignment::Stretch);
  tab_shoulder_layer_.VerticalAlignment(VerticalAlignment::Stretch);
  Grid::SetRow(tab_shoulder_layer_, 0);
  Grid::SetRowSpan(tab_shoulder_layer_, 2);
  Canvas::SetZIndex(tab_shoulder_layer_, 1);

  left_tab_shoulder_ = winrt::Microsoft::UI::Xaml::Shapes::Path{};
  left_tab_shoulder_.Width(8);
  left_tab_shoulder_.Height(8);
  left_tab_shoulder_.Stretch(
      winrt::Microsoft::UI::Xaml::Media::Stretch::Fill);
  left_tab_shoulder_.Data(TabShoulderGeometry(true));
  left_tab_shoulder_.Visibility(Visibility::Collapsed);
  tab_shoulder_layer_.Children().Append(left_tab_shoulder_);

  right_tab_shoulder_ = winrt::Microsoft::UI::Xaml::Shapes::Path{};
  right_tab_shoulder_.Width(8);
  right_tab_shoulder_.Height(8);
  right_tab_shoulder_.Stretch(
      winrt::Microsoft::UI::Xaml::Media::Stretch::Fill);
  right_tab_shoulder_.Data(TabShoulderGeometry(false));
  right_tab_shoulder_.Visibility(Visibility::Collapsed);
  tab_shoulder_layer_.Children().Append(right_tab_shoulder_);

  // Put the connector below TabView so native text, close buttons, separators,
  // and pointer states remain entirely owned by the control. The material
  // wedges remain visible only in the transparent space beside the selected
  // item and never participate in hit testing.
  root_.Children().InsertAt(0, tab_shoulder_layer_);

  // Draw the caption controls on the same transparent XAML/Mica surface as
  // the tabs. Their rectangles are registered below as true Windows
  // non-client regions, retaining snap layouts and native window commands
  // without exposing Chromium's opaque frame beneath a cutout.
  caption_host_ = Grid{};
  caption_host_.Width(caption_width);
  caption_host_.Height(kTabRowHeight);
  caption_host_.HorizontalAlignment(HorizontalAlignment::Right);
  caption_host_.VerticalAlignment(VerticalAlignment::Top);
  caption_host_.IsHitTestVisible(false);
  Grid::SetRow(caption_host_, 0);
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
    button.BorderThickness(Thickness{0});
    button.CornerRadius(CornerRadius{0});
    button.Padding(Thickness{0});
    button.Background(SolidColorBrush{Color(0, 0, 0, 0)});
    button.IsHitTestVisible(false);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        button, name);
    return button;
  };
  minimize_button_ = make_caption_button(L"\uE921", L"Minimize");
  maximize_button_ = make_caption_button(L"\uE922", L"Maximize");
  close_button_ = make_caption_button(L"\uE8BB", L"Close");
  Grid::SetColumn(minimize_button_, 0);
  Grid::SetColumn(maximize_button_, 1);
  Grid::SetColumn(close_button_, 2);
  caption_host_.Children().Append(minimize_button_);
  caption_host_.Children().Append(maximize_button_);
  caption_host_.Children().Append(close_button_);
  root_.Children().Append(caption_host_);

  if (non_client_pointer_source_) {
    non_client_pointer_source_.PointerEntered(
        [this](const auto&, const auto& args) {
          UpdateCaptionButtonVisual(args.RegionKind(), 1);
        });
    non_client_pointer_source_.PointerExited(
        [this](const auto&, const auto& args) {
          UpdateCaptionButtonVisual(args.RegionKind(), 0);
        });
    non_client_pointer_source_.PointerPressed(
        [this](const auto&, const auto& args) {
          UpdateCaptionButtonVisual(args.RegionKind(), 2);
        });
    non_client_pointer_source_.PointerReleased(
        [this](const auto&, const auto& args) {
          UpdateCaptionButtonVisual(args.RegionKind(),
                                    args.IsPointInRegion() ? 1 : 0);
        });
  }

  tab_view_.AddTabButtonClick([this](const TabView&, const auto&) {
    if (!updating_) {
      Invoke(WCS_COMMAND_NEW_TAB);
    }
  });
  tab_view_.SelectionChanged([this](const auto&, const auto&) {
    UpdateTabShoulders();
    if (updating_) {
      return;
    }
    const auto selected = tab_view_.SelectedItem().try_as<TabViewItem>();
    if (selected && selected.Tag()) {
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
  tab_view_.DoubleTapped([this](const auto&, const auto&) {
    if (IsZoomed(parent_)) {
      ShowWindow(parent_, SW_RESTORE);
    } else {
      ShowWindow(parent_, SW_MAXIMIZE);
    }
  });

  BuildToolbar();
  BuildMenus();

  native_page_host_ = Grid{};
  native_page_host_.Visibility(Visibility::Collapsed);
  Grid::SetRow(native_page_host_, 2);
  root_.Children().Append(native_page_host_);
}

Shell::ToolbarButton Shell::MakeGlyphButton(std::wstring_view glyph,
                                            std::wstring_view tooltip,
                                            WcsCommand command,
                                            bool invoke_on_click) {
  Button button;
  button.Content(ToolbarGlyph(glyph));
  // A native Button with an icon-only surface is the WinUI pattern that
  // matches Explorer's standalone toolbar controls. Its 32 px state shape
  // aligns exactly with the TextBox; the surrounding 40 px grid cell retains
  // the comfortable command spacing without inheriting CommandBar geometry.
  button.Width(32);
  button.Height(32);
  button.HorizontalAlignment(HorizontalAlignment::Center);
  button.VerticalAlignment(VerticalAlignment::Center);
  button.Padding(Thickness{0});
  button.Margin(Thickness{0});
  button.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
      Color(0, 0, 0, 0)});
  button.BorderThickness(Thickness{0});
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
  address_box_.Padding(Thickness{44, 5, 44, 6});
  address_box_.QueryIcon(nullptr);
  address_box_.Loaded([this](const auto&, const auto&) {
    address_box_.ApplyTemplate();
    address_box_.DispatcherQueue().TryEnqueue([this] {
      if (const auto content =
              FindNamedDescendant(address_box_, L"ContentElement")
                  .try_as<ScrollViewer>()) {
        content.Padding(Thickness{44, 5, 44, 6});
      }
      if (const auto placeholder =
              FindNamedDescendant(address_box_,
                                  L"PlaceholderTextContentPresenter")
                  .try_as<ContentControl>()) {
        placeholder.Padding(Thickness{44, 5, 44, 6});
      }
      if (const auto query_button =
              FindNamedDescendant(address_box_, L"QueryButton")
                  .try_as<Button>()) {
        query_button.Visibility(Visibility::Collapsed);
      }
    });
  });
  address_box_.LostFocus([this](const auto&, const auto&) {
    address_box_.IsSuggestionListOpen(false);
  });
  address_host.Children().Append(address_box_);
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
          sender.IsSuggestionListOpen(false);
          sender.ItemsSource(nullptr);
          Invoke(WCS_COMMAND_NAVIGATE, active_tab_id_, active_index_,
                 value.c_str());
          SetFocus(parent_);
          Invoke(WCS_COMMAND_FOCUS_CONTENT, active_tab_id_, active_index_);
          sender.DispatcherQueue().TryEnqueue([sender] {
            sender.IsSuggestionListOpen(false);
            sender.ItemsSource(nullptr);
          });
        }
      });
  address_box_.TextChanged(
      [this](const AutoSuggestBox& sender,
             const AutoSuggestBoxTextChangedEventArgs&) {
        if (suppress_address_suggestions_) {
          suppress_address_suggestions_ = false;
          sender.IsSuggestionListOpen(false);
          return;
        }
        if (!updating_) {
          UpdateAddressSuggestions(sender.Text().c_str());
        }
      });
  security_button_ = MakeGlyphButton(L"\uE72E", L"View site information",
                                     WCS_COMMAND_SHOW_SITE_INFO);
  security_button_.Width(32);
  security_button_.Height(32);
  security_button_.HorizontalAlignment(HorizontalAlignment::Left);
  security_button_.Margin(Thickness{4, 0, 0, 0});
  address_host.Children().Append(security_button_);

  favorite_button_ = MakeGlyphButton(L"\uE734", L"Add this page to favorites",
                                     WCS_COMMAND_BOOKMARK_PAGE);
  favorite_button_.Width(32);
  favorite_button_.Height(32);
  favorite_button_.HorizontalAlignment(HorizontalAlignment::Right);
  favorite_button_.Margin(Thickness{0, 0, 8, 0});
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
  app_menu_.Items().Append(MakeMenuItem(L"Find on page", WCS_COMMAND_FIND));
  app_menu_.Items().Append(MakeMenuItem(L"Print", WCS_COMMAND_PRINT));
  app_menu_.Items().Append(
      MakeMenuItem(L"Save page as", WCS_COMMAND_SAVE_PAGE));
  app_menu_.Items().Append(MenuFlyoutSeparator{});
  app_menu_.Items().Append(
      MakeMenuItem(L"Settings", WCS_COMMAND_OPEN_SETTINGS));
  app_menu_.Items().Append(
      MakeMenuItem(L"About Curve Browser", WCS_COMMAND_OPEN_ABOUT));
  app_menu_.Items().Append(MakeMenuItem(L"Exit", WCS_COMMAND_EXIT));
  menu_button_.Click(
      [this](const auto&, const auto&) { app_menu_.ShowAt(menu_button_); });
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

void Shell::UpdateTabs(const WcsWindowState& state) {
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
      item.Tag(winrt::box_value(tab.tab_id));
      item.IsClosable(true);
      item.Height(40);
      item.SizeChanged(
          [this](const auto&, const auto&) { UpdateTabShoulders(); });

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

    // SelectedBackgroundPath is a shape inside TabViewItem's control
    // template, so Control::Background does not drive it. Override the
    // ThemeResource at the item itself; this is the nearest resource scope
    // and reliably updates both the body and native lower shoulder geometry.
    const auto selected_background_key = winrt::box_value(
        winrt::hstring{L"TabViewItemHeaderBackgroundSelected"});
    const auto drag_background_key = winrt::box_value(
        winrt::hstring{L"TabViewItemHeaderDragBackground"});
    item.Resources().Insert(selected_background_key, toolbar_.Background());
    item.Resources().Insert(drag_background_key, toolbar_.Background());

    const wchar_t* title = tab.title && *tab.title ? tab.title : L"New tab";
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        item, winrt::hstring{title});
    item.IsClosable(tab.pinned == 0);
    item.IconSource(nullptr);
    if (tab.favicon_url && *tab.favicon_url) {
      try {
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap{
            winrt::Windows::Foundation::Uri{tab.favicon_url}};
        ImageIconSource source;
        source.ImageSource(bitmap);
        item.IconSource(source);
      } catch (...) {
        // Invalid or unsupported favicon URLs simply retain TabView's default
        // no-icon layout.
      }
    }
    if (tab.loading) {
      StackPanel loading_header;
      loading_header.Orientation(Orientation::Horizontal);
      ProgressRing progress;
      progress.Width(14);
      progress.Height(14);
      progress.Margin(Thickness{0, 0, 6, 0});
      progress.IsActive(true);
      TextBlock label;
      label.Text(title);
      loading_header.Children().Append(progress);
      loading_header.Children().Append(label);
      item.Header(loading_header);
    } else if (tab.audible) {
      item.Header(winrt::box_value(title));
      FontIconSource source;
      source.Glyph(tab.muted ? L"\uE74F" : L"\uE767");
      source.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{
          L"Segoe Fluent Icons"});
      item.IconSource(source);
    } else {
      item.Header(winrt::box_value(title));
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
      active_url_ = tab.url ? tab.url : L"";
      if (address_box_.Text() != active_url_) {
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

  UpdateNativePage(active_url_);
  UpdateTitleBarRegions();
  UpdateTabShoulders();
  // Selection activates SelectedBackgroundPath through x:Load during the
  // next layout pass. Re-apply after that pass so the realized WinUI template
  // receives the shared command-layer material and exposes its shoulders.
  root_.DispatcherQueue().TryEnqueue([this] { UpdateTabShoulders(); });
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
  security_button_.Content(ToolbarGlyph(secure ? L"\uE72E" : L"\uE946"));
  const wchar_t* label = secure ? L"Connection is secure"
                                : L"View site information";
  ToolTipService::SetToolTip(security_button_, winrt::box_value(label));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      security_button_, label);
}

void Shell::UpdateTabShoulders() {
  if (!left_tab_shoulder_ || !right_tab_shoulder_ || !root_) {
    return;
  }
  try {
    const auto selected = tab_view_.SelectedItem().try_as<TabViewItem>();
    if (!selected || selected.ActualWidth() <= 0 ||
        selected.ActualHeight() <= 0) {
      left_tab_shoulder_.Visibility(Visibility::Collapsed);
      right_tab_shoulder_.Visibility(Visibility::Collapsed);
      return;
    }

    // WinUI 3 2.2 resolves this ThemeResource from the control theme rather
    // than the item's logical resource ancestry in an unpackaged XAML island.
    // Set the template shape directly after realization so the selected tab,
    // including TabView's built-in lower arcs, uses the command-layer brush.
    if (const auto selected_background =
            FindVisualChildByName(selected, L"SelectedBackgroundPath")
                .try_as<winrt::Microsoft::UI::Xaml::Shapes::Shape>()) {
      selected_background.Fill(toolbar_.Background());
    }

    const auto origin = selected.TransformToVisual(root_).TransformPoint(
        winrt::Windows::Foundation::Point{0, 0});
    // The unpackaged-island TabView template does not expose its lower arcs.
    // Draw exactly one pair of 8-DIP shoulders in the bottom of the tab row.
    // They curve into the command surface and terminate at its boundary;
    // nothing is painted over the toolbar itself.
    Canvas::SetLeft(left_tab_shoulder_, origin.X - 8.0);
    Canvas::SetTop(left_tab_shoulder_, kTabRowHeight - 8.0);
    Canvas::SetLeft(right_tab_shoulder_,
                    origin.X + selected.ActualWidth());
    Canvas::SetTop(right_tab_shoulder_, kTabRowHeight - 8.0);
    left_tab_shoulder_.Visibility(Visibility::Visible);
    right_tab_shoulder_.Visibility(Visibility::Visible);
  } catch (...) {
    left_tab_shoulder_.Visibility(Visibility::Collapsed);
    right_tab_shoulder_.Visibility(Visibility::Collapsed);
  }
}

void Shell::UpdateCaptionButtonVisual(
    winrt::Microsoft::UI::Input::NonClientRegionKind kind,
    int state) {
  using winrt::Microsoft::UI::Input::NonClientRegionKind;
  ToolbarButton button{nullptr};
  if (kind == NonClientRegionKind::Minimize) {
    button = minimize_button_;
  } else if (kind == NonClientRegionKind::Maximize) {
    button = maximize_button_;
  } else if (kind == NonClientRegionKind::Close) {
    button = close_button_;
  }
  if (!button) {
    return;
  }

  auto color = Color(0, 0, 0, 0);
  if (state > 0 && kind == NonClientRegionKind::Close) {
    color = state == 2 ? Color(162, 29, 17) : Color(196, 43, 28);
  } else if (state == 2) {
    color = Color(255, 255, 255, 32);
  } else if (state == 1) {
    color = Color(255, 255, 255, 20);
  }
  button.Background(SolidColorBrush{color});
}

void Shell::UpdateMaximizeGlyph() {
  if (!maximize_button_) {
    return;
  }
  maximize_button_.Content(Glyph(IsZoomed(parent_) ? L"\uE923" : L"\uE922",
                                  10));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      maximize_button_, IsZoomed(parent_) ? L"Restore" : L"Maximize");
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

  const int right_inset = title_bar_ ? title_bar_.RightInset() : 0;
  const int minimum_drag_width = static_cast<int>(48.0 * scale + 0.5);
  const int maximum_passthrough =
      std::max(0, static_cast<int>(client.right) - right_inset -
                      minimum_drag_width);
  const int passthrough_width = std::clamp(
      static_cast<int>(std::ceil(interactive_width_dip * scale)), 0,
      maximum_passthrough);
  const int title_height = static_cast<int>(kTabRowHeight * scale + 0.5);

  non_client_pointer_source_.ClearRegionRects(
      winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough);
  if (passthrough_width > 0) {
    const std::array rectangles = {winrt::Windows::Graphics::RectInt32{
        0, 0, passthrough_width, title_height}};
    non_client_pointer_source_.SetRegionRects(
        winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough,
        rectangles);
  }
  non_client_pointer_source_.ClearRegionRects(
      winrt::Microsoft::UI::Input::NonClientRegionKind::Caption);
  const int caption_left = passthrough_width;
  const int caption_right = std::max(caption_left,
                                     static_cast<int>(client.right) -
                                         right_inset);
  if (caption_right > caption_left) {
    const std::array rectangles = {winrt::Windows::Graphics::RectInt32{
        caption_left, 0, caption_right - caption_left, title_height}};
    non_client_pointer_source_.SetRegionRects(
        winrt::Microsoft::UI::Input::NonClientRegionKind::Caption,
        rectangles);
  }

  using winrt::Microsoft::UI::Input::NonClientRegionKind;
  non_client_pointer_source_.ClearRegionRects(NonClientRegionKind::Minimize);
  non_client_pointer_source_.ClearRegionRects(NonClientRegionKind::Maximize);
  non_client_pointer_source_.ClearRegionRects(NonClientRegionKind::Close);
  if (right_inset > 0) {
    const int button_left = static_cast<int>(client.right) - right_inset;
    const int first_width = right_inset / 3;
    const int second_width = right_inset / 3;
    const int third_width = right_inset - first_width - second_width;
    const std::array minimize_rect = {winrt::Windows::Graphics::RectInt32{
        button_left, 0, first_width, title_height}};
    const std::array maximize_rect = {winrt::Windows::Graphics::RectInt32{
        button_left + first_width, 0, second_width, title_height}};
    const std::array close_rect = {winrt::Windows::Graphics::RectInt32{
        button_left + first_width + second_width, 0, third_width,
        title_height}};
    non_client_pointer_source_.SetRegionRects(NonClientRegionKind::Minimize,
                                               minimize_rect);
    non_client_pointer_source_.SetRegionRects(NonClientRegionKind::Maximize,
                                               maximize_rect);
    non_client_pointer_source_.SetRegionRects(NonClientRegionKind::Close,
                                               close_rect);
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
  StackPanel page;
  page.MaxWidth(920);
  page.HorizontalAlignment(HorizontalAlignment::Stretch);
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

HRESULT Shell::Capture(const wchar_t* output_path) {
  if (!output_path || !*output_path) {
    return E_INVALIDARG;
  }
  try {
    CaptureAsync(output_path);
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
      submenu.Text(source.label ? source.label : L"");
      submenu.IsEnabled(source.enabled != 0);
      submenus.emplace(static_cast<int32_t>(index), submenu);
      append_item(source.parent_index, submenu);
      continue;
    }

    MenuFlyoutItem item;
    item.Text(source.label ? source.label : L"");
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

winrt::fire_and_forget Shell::CaptureAsync(std::wstring output_path) {
  try {
    winrt::Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
    co_await bitmap.RenderAsync(root_);
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

void Shell::ResizeIsland() {
  if (!island_window_ || !IsWindow(parent_)) {
    return;
  }
  RECT client{};
  GetClientRect(parent_, &client);
  const UINT dpi = GetDpiForWindow(parent_);
  const double scale = static_cast<double>(dpi) / 96.0;
  const int shell_height = static_cast<int>(kShellHeight * scale + 0.5);
  const int height = native_page_visible_ ? client.bottom : shell_height;
  xaml_source_.SiteBridge().MoveAndResize(
      winrt::Windows::Graphics::RectInt32{0, 0, client.right, height});
  UpdateWindowRegion();
  UpdateTitleBarRegions();
  UpdateMaximizeGlyph();
}

void Shell::UpdateWindowRegion() {
  if (!island_window_) {
    return;
  }
  // The caption glyphs are now XAML visuals and their rectangles are native
  // non-client regions, so the island can retain Mica across the full width.
  // Removing the old cutout eliminates Chromium's opaque black/gray surface.
  SetWindowRgn(island_window_, nullptr, TRUE);
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
      const auto color_reference = [](winrt::Windows::UI::Color value) {
        return winrt::box_value(value)
            .as<winrt::Windows::Foundation::IReference<
                winrt::Windows::UI::Color>>();
      };
      title_bar_.ButtonBackgroundColor(
          color_reference(Color(0, 0, 0, 0)));
      title_bar_.ButtonInactiveBackgroundColor(
          color_reference(Color(0, 0, 0, 0)));
      title_bar_.ButtonHoverBackgroundColor(color_reference(
          dark ? Color(255, 255, 255, 20) : Color(0, 0, 0, 16)));
      title_bar_.ButtonPressedBackgroundColor(color_reference(
          dark ? Color(255, 255, 255, 32) : Color(0, 0, 0, 28)));
      title_bar_.ButtonForegroundColor(color_reference(
          dark ? Color(255, 255, 255) : Color(0, 0, 0)));
      title_bar_.ButtonInactiveForegroundColor(color_reference(
          dark ? Color(255, 255, 255, 154) : Color(0, 0, 0, 154)));
    }
    // Mica Alt is the base layer. File Explorer puts a Fluent commanding
    // layer over it, then uses that exact same material for the selected tab.
    // Sharing one brush instance also lets TabView's native lower arcs merge
    // into the toolbar instead of reading as a flat seam between two colors.
    const auto transparent = SolidColorBrush{Color(0, 0, 0, 0)};
    const auto commanding_layer = ThemeBrush(
        L"LayerOnMicaBaseAltFillColorDefaultBrush",
        dark ? Color(58, 58, 58, 115) : Color(255, 255, 255, 179));
    const auto content_layer = ThemeBrush(
        L"LayerFillColorDefaultBrush",
        dark ? Color(58, 58, 58, 76) : Color(255, 255, 255, 128));

    root_.Background(transparent);
    tab_view_.Background(transparent);
    toolbar_.Background(commanding_layer);
    native_page_host_.Background(content_layer);
    left_tab_shoulder_.Fill(commanding_layer);
    right_tab_shoulder_.Fill(commanding_layer);

    const auto selected_key =
        winrt::box_value(winrt::hstring{L"TabViewItemHeaderBackgroundSelected"});
    const auto drag_key =
        winrt::box_value(winrt::hstring{L"TabViewItemHeaderDragBackground"});
    const auto shoulder_key =
        winrt::box_value(winrt::hstring{L"TabViewBorderBrush"});
    // The WinUI control theme owns SelectedBackgroundPath in an unpackaged
    // island, so publish the override at the application resource scope as
    // well as the TabView/item scopes. ThemeResource then updates the loaded
    // path and its native shoulder geometry immediately.
    const auto app_resources =
        winrt::Microsoft::UI::Xaml::Application::Current().Resources();
    app_resources.Insert(selected_key, commanding_layer);
    app_resources.Insert(drag_key, commanding_layer);
    tab_view_.Resources().Insert(selected_key, commanding_layer);
    tab_view_.Resources().Insert(drag_key, commanding_layer);
    // The separate shoulder paths provide the complete lower connector. Keep
    // TabView's own translucent border clear so it cannot alpha-stack with a
    // shoulder and leave a dark nib where the two geometries meet.
    tab_view_.Resources().Insert(shoulder_key, transparent);
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
    case WM_SIZE:
    case WM_DPICHANGED:
      // AppWindow and Chromium both adjust child HWND geometry while handling
      // these messages. Resize the XAML island after that transaction instead
      // of re-entering it from inside the native maximize/DPI path.
      PostMessageW(window, kDeferredResizeMessage, 0, 0);
      break;
    case kDeferredResizeMessage:
      shell->ResizeIsland();
      break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
      shell->ApplySystemTheme();
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(window, &Shell::ParentSubclassProc, subclass_id);
      shell->parent_ = nullptr;
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

extern "C" HRESULT __stdcall WcsCaptureShell(WcsShellHandle shell,
                                               const wchar_t* output_path) {
  if (!shell) {
    return E_INVALIDARG;
  }
  return static_cast<windows_chromium::Shell*>(shell)->Capture(output_path);
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
