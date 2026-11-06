#include "pch.h"
#include "App.h"

#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>

using namespace winrt;

namespace
{
    class BootstrapLifetime
    {
    public:
        BootstrapLifetime()
        {
            UINT32 packageNameLength = 0;
            const auto packageResult = GetCurrentPackageFullName(&packageNameLength, nullptr);
            if (packageResult == ERROR_SUCCESS || packageResult == ERROR_INSUFFICIENT_BUFFER)
                return;
            if (packageResult != APPMODEL_ERROR_NO_PACKAGE)
                winrt::throw_hresult(HRESULT_FROM_WIN32(packageResult));

            const auto result = MddBootstrapInitialize2(
                WINDOWSAPPSDK_RELEASE_MAJORMINOR,
                WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
                PACKAGE_VERSION{ WINDOWSAPPSDK_RUNTIME_VERSION_UINT64 },
                MddBootstrapInitializeOptions_None);
            if (FAILED(result))
            {
                throw winrt::hresult_error(
                    result,
                    L"Windows App Runtime 1.6 (x64) is not available. Install the Windows App Runtime 1.6, or run RoadTripMovie through the MSIX package project.");
            }
            initialized = true;
        }

        ~BootstrapLifetime()
        {
            if (initialized)
                MddBootstrapShutdown();
        }

        BootstrapLifetime(BootstrapLifetime const&) = delete;
        BootstrapLifetime& operator=(BootstrapLifetime const&) = delete;

    private:
        bool initialized{ false };
    };
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        BootstrapLifetime bootstrap;
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
        {
            winrt::make<winrt::RoadTripMovie::implementation::App>();
        });
        return 0;
    }
    catch (winrt::hresult_error const& error)
    {
        MessageBoxW(nullptr, error.message().c_str(), L"RoadTripMovie startup failed",
            MB_OK | MB_ICONERROR);
        return static_cast<int>(error.code());
    }
}
