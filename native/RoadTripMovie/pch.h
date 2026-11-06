#pragma once

#include <unknwn.h>
#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <ranges>
#include <system_error>
#include <utility>

// Ensure C++/WinRT headers are available and included correctly
// Fix for E1696: cannot open source file "winrt/Windows.Foundation.h"

// 1. Make sure you have installed the "Microsoft.Windows.CppWinRT" NuGet package in your project.
// 2. Ensure your project is targeting Windows 10 or later and has C++/WinRT enabled.
// 3. The include path for C++/WinRT headers must be set in your project settings:
//    - Right-click project > Properties > VC++ Directories > Include Directories
//    - Add: $(ProjectDir)Generated Files\winrt;$(WindowsSdkDir)Include\<WindowsTargetPlatformVersion>\cppwinrt

// 4. If you are missing the NuGet package, add it via:
//    - Right-click project > Manage NuGet Packages > Browse > "Microsoft.Windows.CppWinRT" > Install

// 5. If you are using precompiled headers, ensure pch.h is the first include in your .cpp files.

// No code changes are needed in this file if the above steps are followed.
// If you still encounter the error, try cleaning and rebuilding the project.
