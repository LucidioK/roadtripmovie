#pragma once

#include "Pipeline.h"

#include <thread>

namespace RoadTripMovie
{
    class MainWindow : public std::enable_shared_from_this<MainWindow>
    {
    public:
        MainWindow();
        ~MainWindow();

        void Activate();

    private:
        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBox inputFolderBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBox musicFileBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBox outputFileBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox photoDurationBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox videoLimitBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox widthBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox heightBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox fpsBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox videoVolumeBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox musicVolumeBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button startButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button cancelButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ProgressRing progressIndicator{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText{ nullptr };
        ::RoadTripMovie::Pipeline::CancellationSource cancellation;
        std::jthread worker;
        bool running{ false };

        winrt::fire_and_forget BrowseFolderAsync();
        winrt::fire_and_forget BrowseMusicAsync();
        winrt::fire_and_forget BrowseOutputAsync();
        void Start();
        void Cancel();
        void SetRunning(bool value);
        void SetStatus(std::wstring text);
        ::RoadTripMovie::PipelineSettings ReadSettings() const;
        void AddActionButtons(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form);
        void AddTitleAndIntroduction(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form);
        void AddNumberFieldBoxes(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form);
        void AddFileInputBoxes(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form);
    };
}
