#pragma once

#include "App.xaml.g.h"
#include "MainWindow.h"

namespace winrt::RoadTripMovie::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        std::shared_ptr<::RoadTripMovie::MainWindow> window;
    };
}
