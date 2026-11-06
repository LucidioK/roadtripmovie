#include "pch.h"
#include "App.h"

namespace winrt::RoadTripMovie::implementation
{
    App::App() = default;

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        window = std::make_shared<::RoadTripMovie::MainWindow>();
        window->Activate();
    }
}
