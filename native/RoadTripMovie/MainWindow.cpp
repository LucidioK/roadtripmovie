#include "pch.h"
#include "MainWindow.h"

#include <shobjidl_core.h>
#include <shellapi.h>
#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <system_error>
#include <thread>

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace RoadTripMovie
{
    namespace
    {
        std::filesystem::path GetExecutablePath()
        {
            std::wstring executable(32768, L'\0');
            auto length = GetModuleFileNameW(
                nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (length == 0 || length >= executable.size())
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not locate the installed application.");
            executable.resize(length);
            return executable;
        }

        void AddTextField(StackPanel const& panel, hstring const& header, TextBox const& box)
        {
            box.Header(box_value(header));
            panel.Children().Append(box);
        }

        Button AddBrowseButton(StackPanel const& panel, hstring const& label, auto handler)
        {
            Button button;
            button.Content(box_value(label));
            button.HorizontalAlignment(HorizontalAlignment::Left);
            button.Click(handler);
            panel.Children().Append(button);
            return button;
        }

        NumberBox AddNumberField(
            StackPanel const& panel, hstring const& header,
            double value, double minimum, double maximum)
        {
            auto horizontalPanel = StackPanel();
            horizontalPanel.Orientation(Orientation::Horizontal);
            horizontalPanel.VerticalAlignment(VerticalAlignment::Center);
			TextBlock label;
            label.Width(200);
			label.Text(header);
            label.TextWrapping(TextWrapping::Wrap);
            horizontalPanel.Children().Append(label);
            NumberBox box;
            box.Width(160);
            box.Value(value);
            box.Minimum(minimum);
            box.Maximum(maximum);
            horizontalPanel.Children().Append(box);
			panel.Children().Append(horizontalPanel);
            return box;
        }

		TextBox AddTextFieldWithBrowse(
			StackPanel const& panel,
            hstring const& labelText,
			auto handler,
            hstring const& buttonText = L"Browse")
		{
			auto horizontalPanel = StackPanel();
			horizontalPanel.Orientation(Orientation::Horizontal);
			horizontalPanel.VerticalAlignment(VerticalAlignment::Center);
			TextBlock label;
			label.Width(200);
			label.Text(labelText);
			horizontalPanel.Children().Append(label);
			TextBox box;
			box.Width(200);
			horizontalPanel.Children().Append(box);
			AddBrowseButton(horizontalPanel, buttonText, handler);
			panel.Children().Append(horizontalPanel);
			return box;
		}
    }

    MainWindow::MainWindow()
    {
        window = Window();
        window.AppWindow().SetIcon(
            (GetExecutablePath().parent_path() / L"RoadTripMovie.ico").wstring());
        if (auto overlappedPresenter{ window.AppWindow().Presenter().try_as<OverlappedPresenter>() })
        {
            // Disable the maximize and resize capabilities
            overlappedPresenter.IsMaximizable(false);
            overlappedPresenter.IsResizable(false); // Dialogs shouldn't be resizable
        }
        window.Title(L"RoadTripMovie");

        StackPanel form;
        form.Spacing(12);
        form.HorizontalAlignment(HorizontalAlignment::Stretch);
        form.VerticalAlignment(VerticalAlignment::Stretch);

        AddTitleAndIntroduction(form);

        AddFileInputBoxes(form);

        AddNumberFieldBoxes(form);

        AddActionButtons(form);

        statusText = TextBlock();
        statusText.Text(L"Ready");
        statusText.TextWrapping(TextWrapping::Wrap);
        form.Children().Append(statusText);

        StackPanel footer;
        footer.Orientation(Orientation::Horizontal);
        footer.VerticalAlignment(VerticalAlignment::Center);
		footer.HorizontalAlignment(HorizontalAlignment::Center);
        footer.Spacing(128);
        HyperlinkButton authorLink;
        authorLink.Content(box_value(L"By Lucidio Kuhn"));
        authorLink.NavigateUri(winrt::Windows::Foundation::Uri(
            L"https://github.com/LucidioK/index#about-me"));
        HyperlinkButton projectLink;
        projectLink.Content(box_value(L"Project README"));
        projectLink.NavigateUri(winrt::Windows::Foundation::Uri(
            L"https://github.com/LucidioK/roadtripmovie/blob/master/native/README.md"));
		projectLink.HorizontalAlignment(HorizontalAlignment::Right);
        footer.Children().Append(authorLink);
        footer.Children().Append(projectLink);
        form.Children().Append(footer);

        Border content;
        content.Padding(Thickness{ 24, 24, 24, 24 });
        content.Child(form);
        ScrollViewer scroll;
        scroll.Content(content);
        window.Content(scroll);
        window.AppWindow().Resize(Windows::Graphics::SizeInt32{ 550, 780 });
    }

    void MainWindow::AddTitleAndIntroduction(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form)
    {
        TextBlock title;
        title.Text(L"Create a road-trip movie");
        title.FontSize(28);
        form.Children().Append(title);

        TextBlock introduction;
        introduction.Text(L"Choose a folder of photos and videos, a music track, and an MP4 destination.");
        introduction.TextWrapping(TextWrapping::Wrap);
        form.Children().Append(introduction);
    }

    void MainWindow::AddFileInputBoxes(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form)
    {
        inputFolderBox = AddTextFieldWithBrowse(form, L"Media folder",
            [this](auto&&, auto&&) { BrowseFolderAsync(); });
        musicFileBox = AddTextFieldWithBrowse(form, L"Background music",
            [this](auto&&, auto&&) { BrowseMusicAsync(); });
        outputFileBox = AddTextFieldWithBrowse(form, L"Output video",
            [this](auto&&, auto&&) { BrowseOutputAsync(); }, L"Save as");
    }

    void MainWindow::AddNumberFieldBoxes(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form)
    {
        photoDurationBox = AddNumberField(form, L"Photo duration (seconds)", 5, 0.1, 3600);
        videoLimitBox = AddNumberField(form, L"Video cap (seconds, 0 = full clip)", 0, 0, 86400);
        widthBox = AddNumberField(form, L"Output width", 1920, 16, 7680);
        heightBox = AddNumberField(form, L"Output height", 1080, 16, 4320);
        fpsBox = AddNumberField(form, L"Frames per second", 30, 1, 120);
        videoVolumeBox = AddNumberField(form, L"Video audio volume", 0.25, 0, 10);
        musicVolumeBox = AddNumberField(form, L"Music volume", 1, 0, 10);
    }


    void MainWindow::AddActionButtons(winrt::Microsoft::UI::Xaml::Controls::StackPanel& form)
    {
        openVideoWhenDoneCheckBox = CheckBox();
        openVideoWhenDoneCheckBox.Content(box_value(L"Open video when done"));
        openVideoWhenDoneCheckBox.IsChecked(
            box_value(true).as<winrt::Windows::Foundation::IReference<bool>>());
        form.Children().Append(openVideoWhenDoneCheckBox);

        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.HorizontalAlignment(HorizontalAlignment::Right);
        actions.Spacing(8);
        cancelButton = Button();
        cancelButton.Content(box_value(L"Cancel"));
        cancelButton.IsEnabled(false);
        cancelButton.Click([this](auto&&, auto&&) { Cancel(); });
        actions.Children().Append(cancelButton);
        progressIndicator = ProgressRing();
        progressIndicator.Width(28);
        progressIndicator.Height(28);
        progressIndicator.IsActive(false);
        actions.Children().Append(progressIndicator);
        form.Children().Append(actions);
        startButton = Button();
        startButton.Content(box_value(L"Create movie"));
        startButton.Click([this](auto&&, auto&&) { Start(); });
        actions.Children().Append(startButton);
    }

    MainWindow::~MainWindow()
    {
        cancellation.request_stop();
        if (worker.joinable())
            worker.join();
    }

    void MainWindow::Activate()
    {
        window.Activate();
    }

    void MainWindow::SetRunning(bool value)
    {
        running = value;
        startButton.IsEnabled(!value);
        cancelButton.IsEnabled(value);
        progressIndicator.IsActive(value);
    }

    void MainWindow::SetStatus(std::wstring text)
    {
        statusText.Text(text);
    }

    fire_and_forget MainWindow::BrowseFolderAsync()
    {
        [[maybe_unused]] auto lifetime = shared_from_this();
        FolderPicker picker;
        picker.FileTypeFilter().Append(L"*");
        auto nativeWindow = window.as<IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
        auto folder = co_await picker.PickSingleFolderAsync();
        if (folder)
            inputFolderBox.Text(folder.Path());
    }

    fire_and_forget MainWindow::BrowseMusicAsync()
    {
        [[maybe_unused]] auto lifetime = shared_from_this();
        FileOpenPicker picker;
        for (auto extension : { L".mp3", L".wav", L".aac", L".flac", L".m4a", L".wma" })
            picker.FileTypeFilter().Append(extension);
        auto nativeWindow = window.as<IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
        auto file = co_await picker.PickSingleFileAsync();
        if (file)
            musicFileBox.Text(file.Path());
    }

    fire_and_forget MainWindow::BrowseOutputAsync()
    {
        [[maybe_unused]] auto lifetime = shared_from_this();
        FileSavePicker picker;
        picker.SuggestedFileName(L"roadtrip_movie");
        picker.FileTypeChoices().Insert(
            L"MP4 video", single_threaded_vector<hstring>({ L".mp4" }));
        auto nativeWindow = window.as<IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
        auto file = co_await picker.PickSaveFileAsync();
        if (file)
            outputFileBox.Text(file.Path());
    }

    PipelineSettings MainWindow::ReadSettings() const
    {
        PipelineSettings settings;
        settings.inputFolder = inputFolderBox.Text().c_str();
        settings.musicPath = musicFileBox.Text().c_str();
        settings.outputPath = outputFileBox.Text().c_str();
        settings.ffmpegDirectory = GetExecutablePath().parent_path();
        settings.photoDuration = photoDurationBox.Value();
        auto videoLimit = videoLimitBox.Value();
        if (videoLimit > 0)
            settings.maxVideoDuration = videoLimit;
        settings.width = static_cast<int>(widthBox.Value());
        settings.height = static_cast<int>(heightBox.Value());
        settings.fps = static_cast<int>(fpsBox.Value());
        settings.videoAudioVolume = videoVolumeBox.Value();
        settings.musicVolume = musicVolumeBox.Value();
        auto processors = std::thread::hardware_concurrency();
        settings.maxParallelism = processors > 1 ? processors - 1 : 1;
        return settings;
    }

    void MainWindow::Start()
    {
        if (running)
            return;
        try
        {
            auto settings = ReadSettings();
            cancellation = Pipeline::CancellationSource{};
            SetRunning(true);
            SetStatus(L"Starting");
            auto token = cancellation.get_token();
            auto dispatcher = window.DispatcherQueue();
            std::weak_ptr<MainWindow> weak = shared_from_this();
            auto openVideoWhenDone = openVideoWhenDoneCheckBox.IsChecked().Value();
            worker = std::jthread(
                [settings = std::move(settings), token, dispatcher, weak,
                    openVideoWhenDone](std::stop_token)
            {
                try
                {
                    Pipeline::Run(settings, token, [dispatcher, weak](std::wstring message)
                    {
                        dispatcher.TryEnqueue([weak, message = std::move(message)]
                        {
                            if (auto window = weak.lock())
                                window->SetStatus(message);
                        });
                    });
                    if (openVideoWhenDone)
                    {
                        SHELLEXECUTEINFOW launch{};
                        launch.cbSize = sizeof(launch);
                        launch.fMask = SEE_MASK_FLAG_NO_UI;
                        launch.lpVerb = L"open";
                        auto outputPath = settings.outputPath.wstring();
                        launch.lpFile = outputPath.c_str();
                        launch.nShow = SW_SHOWNORMAL;
                        if (!ShellExecuteExW(&launch))
                            throw std::system_error(
                                static_cast<int>(GetLastError()), std::system_category(),
                                "The movie was created, but Windows could not open it.");
                    }
                    dispatcher.TryEnqueue([weak]
                    {
                        if (auto window = weak.lock())
                            window->SetRunning(false);
                    });
                }
                catch (std::exception const& error)
                {
                    auto message = winrt::to_hstring(error.what());
                    dispatcher.TryEnqueue([weak, message = std::move(message)]
                    {
                        if (auto window = weak.lock())
                        {
                            window->SetStatus(L"Error: " + std::wstring(message.c_str()));
                            window->SetRunning(false);
                        }
                    });
                }
            });
        }
        catch (std::exception const& error)
        {
            auto message = winrt::to_hstring(error.what());
            SetStatus(L"Error: " + std::wstring(message.c_str()));
        }
    }

    void MainWindow::Cancel()
    {
        cancellation.request_stop();
        SetStatus(L"Cancelling after the current FFmpeg operation");
    }
}
