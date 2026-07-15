// Copyright 2026 The Curve Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "App.xaml.g.h"

#include <winrt/Microsoft.UI.Xaml.Hosting.h>

namespace winrt::CurveBrowserShell::implementation {

struct App : AppT<App> {
  App()
      : xaml_manager_(
            winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager::
                InitializeForCurrentThread()) {}

  void OnLaunched(
      const winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs&) {}

 private:
  winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager xaml_manager_{
      nullptr};
};

}  // namespace winrt::CurveBrowserShell::implementation
