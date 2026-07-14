// Copyright 2026 The Windows Chromium Authors
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

struct ThreadRuntime {
  ThreadRuntime() {
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
          winrt::WindowsChromiumShell::implementation::App>();
    } catch (const winrt::hresult_error& error) {
      std::fwprintf(stderr,
                    L"WinUI Application initialization failed: 0x%08X\n",
                    static_cast<unsigned int>(error.code().value));
      throw;
    }
  }

  winrt::Microsoft::UI::Dispatching::DispatcherQueueController dispatcher{
      nullptr};
  winrt::Windows::Foundation::IInspectable application{nullptr};
};

thread_local std::unique_ptr<ThreadRuntime> g_thread_runtime;

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
  EnsureSelfContainedRuntimeLoaded();
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
  island_window_ = winrt::Microsoft::UI::GetWindowFromWindowId(
      xaml_source_.SiteBridge().WindowId());

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
}

Shell::~Shell() {
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
  tab_view_.CanDragTabs(true);
  tab_view_.CanReorderTabs(true);
  tab_view_.TabWidthMode(TabViewWidthMode::SizeToContent);
  // The custom title bar uses the 48 px tall system-caption metric so the
  // caption buttons span the complete row. Keep TabView's familiar 40 px
  // strip bottom-aligned, preserving the 8 px Explorer-style top inset while
  // its selected item and shoulders sink directly into the command layer.
  tab_view_.Height(40);
  tab_view_.Padding(Thickness{0});
  tab_view_.VerticalAlignment(VerticalAlignment::Bottom);
  Grid::SetRow(tab_view_, 0);
  root_.Children().Append(tab_view_);
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
                                            WcsCommand command) {
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
  button.Click([this, command](const auto&, const auto&) { Invoke(command); });
  return button;
}

void Shell::BuildToolbar() {
  toolbar_ = Grid{};
  // The 40 px controls plus 4 px above and below exactly fill this 48 px
  // commanding row. Keeping that arithmetic exact prevents XAML from
  // compressing or clipping the icon controls.
  toolbar_.Padding(Thickness{8, 4, 8, 4});
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
  reload_button_ =
      MakeGlyphButton(L"\uE72C", L"Reload (Ctrl+R)", WCS_COMMAND_RELOAD);
  Grid::SetColumn(reload_button_, 2);
  toolbar_.Children().Append(reload_button_);

  address_box_ = TextBox{};
  address_box_.PlaceholderText(L"Search or enter an address");
  address_box_.Height(32);
  address_box_.Margin(Thickness{4, 0, 8, 0});
  Grid::SetColumn(address_box_, 3);
  toolbar_.Children().Append(address_box_);
  address_box_.KeyDown(
      [this](const auto&,
             const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
          const std::wstring value = address_box_.Text().c_str();
          Invoke(WCS_COMMAND_NAVIGATE, active_tab_id_, active_index_,
                 value.c_str());
          args.Handled(true);
        }
      });
  address_box_.GotFocus([this](const auto&, const auto&) {
    address_box_.SelectAll();
  });

  profile_button_ = MakeGlyphButton(L"\uE77B", L"Profiles", WCS_COMMAND_OPEN_PROFILES);
  Grid::SetColumn(profile_button_, 4);
  toolbar_.Children().Append(profile_button_);
  menu_button_ = MakeGlyphButton(L"\uE712", L"Settings and more", WCS_COMMAND_OPEN_SETTINGS);
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
      MakeMenuItem(L"About Windows Chromium", WCS_COMMAND_OPEN_ABOUT));
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
    UpdateTabs(state);
    back_button_.IsEnabled(state.can_go_back != 0);
    forward_button_.IsEnabled(state.can_go_forward != 0);
    profile_button_.Content(
        ToolbarGlyph(state.is_incognito ? L"\uE727" : L"\uE77B"));
    ToolTipService::SetToolTip(
        profile_button_,
        winrt::box_value(state.profile_name ? state.profile_name : L"Profiles"));
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
  for (size_t index = 0; index < state.tab_count; ++index) {
    const WcsTabState& tab = state.tabs[index];
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
      context_menu.Items().Append(MakeMenuItem(
          tab.pinned ? L"Unpin tab" : L"Pin tab", WCS_COMMAND_TOGGLE_PIN_TAB,
          tab.tab_id));
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

    const wchar_t* title = tab.title && *tab.title ? tab.title : L"New tab";
    item.Header(winrt::box_value(title));
    item.IsClosable(tab.pinned == 0);
    item.IconSource(nullptr);
    if (tab.loading) {
      SymbolIconSource source;
      source.Symbol(Symbol::Sync);
      item.IconSource(source);
    } else if (tab.audible) {
      FontIconSource source;
      source.Glyph(tab.muted ? L"\uE74F" : L"\uE767");
      source.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{
          L"Segoe Fluent Icons"});
      item.IconSource(source);
    }

    if (static_cast<int32_t>(index) == state.active_index) {
      tab_view_.SelectedItem(item);
      active_index_ = state.active_index;
      active_tab_id_ = tab.tab_id;
      active_url_ = tab.url ? tab.url : L"";
      address_box_.Text(active_url_);
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
    it = tab_items_.erase(it);
  }

  UpdateNativePage(active_url_);
  UpdateTitleBarRegions();
  UpdateTabShoulders();
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

    const auto origin = selected.TransformToVisual(root_).TransformPoint(
        winrt::Windows::Foundation::Point{0, 0});
    const double shoulder_top =
        std::min(kTabRowHeight, origin.Y + selected.ActualHeight()) - 8.0;
    Canvas::SetLeft(left_tab_shoulder_, origin.X - 8.0);
    Canvas::SetTop(left_tab_shoulder_, shoulder_top);
    Canvas::SetLeft(right_tab_shoulder_,
                    origin.X + selected.ActualWidth());
    Canvas::SetTop(right_tab_shoulder_, shoulder_top);
    left_tab_shoulder_.Visibility(Visibility::Visible);
    right_tab_shoulder_.Visibility(Visibility::Visible);
  } catch (...) {
    left_tab_shoulder_.Visibility(Visibility::Collapsed);
    right_tab_shoulder_.Visibility(Visibility::Collapsed);
  }
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
}

bool Shell::IsNativePage(std::wstring_view url) const {
  constexpr std::wstring_view prefixes[] = {
      L"chrome://settings",   L"chrome://downloads", L"chrome://history",
      L"chrome://bookmarks",  L"chrome://extensions",
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
    return L"About Windows Chromium";
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
        L"Update checks use the Windows Chromium release channel and never require a Google account.",
        updates));
  } else if (StartsWithInsensitive(url, L"chrome://settings")) {
    ToggleSwitch privacy;
    privacy.IsOn(true);
    page.Children().Append(MakeSettingsCard(
        L"Tracking protection",
        L"Use Chromium's protection lists and uBlock Origin to reduce cross-site tracking.",
        privacy));
    ToggleSwitch startup;
    startup.IsOn(false);
    page.Children().Append(MakeSettingsCard(
        L"Continue where you left off",
        L"Restore your local windows and tabs when Windows Chromium starts.",
        startup));
    ComboBox search;
    search.MinWidth(180);
    search.Items().Append(winrt::box_value(L"DuckDuckGo"));
    search.Items().Append(winrt::box_value(L"Brave Search"));
    search.Items().Append(winrt::box_value(L"Google"));
    search.SelectedIndex(0);
    page.Children().Append(MakeSettingsCard(
        L"Search engine", L"Choose the service used for address-bar searches.",
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
    std::wstring message = L"Windows Chromium shell capture failed: ";
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
}

void Shell::UpdateWindowRegion() {
  if (!island_window_) {
    return;
  }
  RECT bounds{};
  GetClientRect(island_window_, &bounds);
  const UINT dpi = GetDpiForWindow(parent_);
  const double scale = static_cast<double>(dpi) / 96.0;
  const int tab_height = static_cast<int>(kTabRowHeight * scale + 0.5);
  const int caption_width = title_bar_
                                ? title_bar_.RightInset()
                                : GetSystemMetricsForDpi(SM_CXSIZE, dpi) * 3;
  const int top_width =
      std::max(0, static_cast<int>(bounds.right) - caption_width);
  HRGN top = CreateRectRgn(0, 0, top_width, tab_height);
  HRGN body = CreateRectRgn(0, tab_height, bounds.right, bounds.bottom);
  CombineRgn(top, top, body, RGN_OR);
  DeleteObject(body);
  SetWindowRgn(island_window_, top, TRUE);  // The system owns `top` now.
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
    tab_view_.Resources().Insert(selected_key, commanding_layer);
    tab_view_.Resources().Insert(drag_key, commanding_layer);
    // TabView uses this brush for its 4 px lower radius paths. Matching the
    // commanding layer turns those native paths into visible material
    // shoulders instead of a low-contrast stroke at the tab/toolbar join.
    tab_view_.Resources().Insert(shoulder_key, commanding_layer);
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
