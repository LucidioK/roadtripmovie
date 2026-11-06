#include "pch.h"
#include "Pipeline.h"
#include "FfmpegProcessor.h"

#include <Demuxer.h>
#include <libHandling/FFmpegLibrariesBuilder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace RoadTripMovie
{
    namespace
    {
        using namespace std::filesystem;

        std::wstring ToWide(std::string_view text)
        {
            if (text.empty())
                return {};
            auto size = MultiByteToWideChar(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (size <= 0)
                return L"An error occurred.";
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        class TemporaryDirectory
        {
        public:
            TemporaryDirectory()
            {
                auto root = temp_directory_path();
                for (int attempt = 0; attempt < 20; ++attempt)
                {
                    auto id = std::chrono::steady_clock::now().time_since_epoch().count() + attempt;
                    path_ = root / (L"RoadTripMovie_" + std::to_wstring(id));
                    std::error_code error;
                    if (create_directory(path_, error))
                        return;
                }
                throw std::runtime_error("Unable to create a temporary working directory.");
            }
            ~TemporaryDirectory()
            {
                std::error_code error;
                remove_all(path_, error);
            }
            TemporaryDirectory(TemporaryDirectory const&) = delete;
            TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;
            path const& Path() const noexcept { return path_; }

        private:
            path path_;
        };


        struct VideoInfo
        {
            double duration{};
            bool hasAudio{};
        };

        std::shared_ptr<libffmpeg::IFFmpegLibraries> LoadLibFFmpeg(path const& ffmpegDirectory)
        {
            auto result = libffmpeg::FFmpegLibrariesBuilder()
                .withAdditionalSearchPaths(std::vector<libffmpeg::Path>{ ffmpegDirectory })
                .tryLoadingOfLibraries();
            if (!result.ffmpegLibraries)
                throw std::runtime_error(
                    "LibFFmpeg could not load the packaged avcodec/avformat/avutil/swresample DLLs.");
            return std::move(result.ffmpegLibraries);
        }

        VideoInfo ProbeVideo(path const& file, std::shared_ptr<libffmpeg::IFFmpegLibraries> const& libraries)
        {
            libffmpeg::Demuxer demuxer(libraries);
            if (!demuxer.openFile(file))
                throw std::runtime_error("LibFFmpeg could not read a video file.");

            auto context = demuxer.getFormatContext();
            auto duration = static_cast<double>(context->getDuration()) / 1'000'000.0;
            bool hasAudio = false;
            for (auto const& stream : context->getStreams())
            {
                if (stream.getCodecType() == libffmpeg::avutil::MediaType::Audio)
                {
                    hasAudio = true;
                    break;
                }
            }
            if (!std::isfinite(duration) || duration <= 0.0)
                throw std::runtime_error("Video has no usable duration: " + file.string());
            return { duration, hasAudio };
        }

        bool IsPhoto(path const& file)
        {
            static const std::vector<std::wstring> extensions{
                L".jpg", L".jpeg", L".png", L".heic", L".heif"
            };
            auto extension = file.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            return std::ranges::find(extensions, extension) != extensions.end();
        }

        bool IsVideo(path const& file)
        {
            static const std::vector<std::wstring> extensions{
                L".mp4", L".mov", L".avi", L".mkv", L".m4v"
            };
            auto extension = file.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            return std::ranges::find(extensions, extension) != extensions.end();
        }

        void ValidateSettings(PipelineSettings const& settings)
        {
            if (!is_directory(settings.inputFolder))
                throw std::invalid_argument("Select an existing media folder.");
            if (!is_regular_file(settings.musicPath))
                throw std::invalid_argument("Select an existing background music file.");
            if (settings.outputPath.empty() || settings.outputPath.extension() != L".mp4")
                throw std::invalid_argument("The output file must have an .mp4 extension.");
            if (settings.width <= 0 || settings.height <= 0 || settings.fps <= 0 ||
                settings.photoDuration <= 0.0 || settings.videoAudioVolume < 0.0 ||
                settings.musicVolume < 0.0 ||
                (settings.maxVideoDuration && *settings.maxVideoDuration <= 0.0))
                throw std::invalid_argument("One or more movie settings are outside their valid range.");
        }

        std::vector<MediaItem> ScanMedia(
            PipelineSettings const& settings,
            std::shared_ptr<libffmpeg::IFFmpegLibraries> const& libraries,
            std::stop_token cancellation,
            ProgressCallback const& progress)
        {
            std::vector<MediaItem> items;
            for (auto const& entry : directory_iterator(settings.inputFolder))
            {
                if (cancellation.stop_requested())
                    throw std::runtime_error("Movie creation was cancelled.");
                if (!entry.is_regular_file())
                    continue;

                auto file = entry.path();
                bool photo = IsPhoto(file);
                if (!photo && !IsVideo(file))
                    continue;
                try
                {
                    auto modified = last_write_time(file);
                    double duration = settings.photoDuration;
                    bool hasAudio = false;
                    if (!photo)
                    {
                        auto video = ProbeVideo(file, libraries);
                        duration = video.duration;
                        hasAudio = video.hasAudio;
                        if (settings.maxVideoDuration)
                            duration = std::min(duration, *settings.maxVideoDuration);
                    }
                    items.push_back({ file, photo, modified, duration, hasAudio, 0, false });
                }
                catch (std::exception const& error)
                {
                    if (progress)
                        progress(L"Skipping unreadable file: " + file.filename().wstring() +
                                 L" (" + ToWide(error.what()) + L")");
                }
            }

            std::ranges::sort(items, {}, &MediaItem::modifiedAt);
            for (std::size_t index = 0; index < items.size(); ++index)
                items[index].index = index;
            return items;
        }

    }

    void Pipeline::Run(
        PipelineSettings const& settings,
        std::stop_token cancellation,
        ProgressCallback const& progress)
    {
        ValidateSettings(settings);
        auto libraries = LoadLibFFmpeg(settings.ffmpegDirectory);
        auto items = ScanMedia(settings, libraries, cancellation, progress);
        if (items.empty())
            throw std::runtime_error("No supported photos or videos could be read from the selected folder.");
        if (progress)
            progress(L"Found " + std::to_wstring(items.size()) + L" items. Rendering segments…");

        TemporaryDirectory temporary;
        std::vector<path> segments(items.size());
        std::atomic_size_t next{ 0 };
        std::atomic_size_t completed{ 0 };
        std::stop_source localStop;
        std::mutex errorMutex;
        std::exception_ptr firstError;
        auto workerCount = std::clamp(settings.maxParallelism, 1u,
            std::min(4u, static_cast<unsigned int>(items.size())));

        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (unsigned int worker = 0; worker < workerCount; ++worker)
        {
            workers.emplace_back([&](std::stop_token threadStop)
            {
                while (!threadStop.stop_requested() && !cancellation.stop_requested() &&
                       !localStop.stop_requested())
                {
                    auto index = next.fetch_add(1);
                    if (index >= items.size())
                        return;
                    items[index].rendered = false;
                    try
                    {
                        auto segment = temporary.Path() /
                            (L"segment_" + std::to_wstring(index) + L".mp4");
                        FfmpegProcessor::RenderSegment(
                            { items[index].file, items[index].photo,
                                items[index].duration, items[index].hasAudio },
                            settings, segment, cancellation);
                        segments[index] = segment;
                        auto count = completed.fetch_add(1) + 1;
                        if (progress)
                            progress(L"Rendered " + std::to_wstring(count) + L" of " +
                                     std::to_wstring(items.size()) + L" segments.");
                        items[index].rendered = true;
                    }
                    catch (...)
                    {
                        {
                            std::scoped_lock lock(errorMutex);
                            if (!firstError)
                                firstError = std::current_exception();
                        }
                        items[index].rendered = false;
                        //localStop.request_stop();
                        return;
                    }
                }
            });
        }
        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }
        if (cancellation.stop_requested())
            throw std::runtime_error("Movie creation was cancelled.");

        RemoveNotRenderedSegments(items, segments);
        if (progress)
            progress(L"Joining segments and mixing background music…");
        auto joined = temporary.Path() / L"joined.mp4";

        FfmpegProcessor::Concatenate(segments, joined, cancellation);

        auto output = settings.outputPath;
        if (output.has_parent_path())
            create_directories(output.parent_path());
        double duration = 0.0;
        for (auto const& item : items)
            duration += item.duration;
        FfmpegProcessor::MixMusic(
            joined, settings.musicPath, output, settings.musicVolume, duration, cancellation);
        if (progress)
            progress(L"Finished: " + output.wstring());
    }

    void Pipeline::RemoveNotRenderedSegments(std::vector<MediaItem>& items, std::vector<std::filesystem::path>& segments)
    {
        int renderedCount = count_if(items.begin(), items.end(), [](MediaItem const& item) { return item.rendered; });
        std::vector<MediaItem> itemsActuallyRendered(renderedCount);
        std::vector<std::filesystem::path> segmentsActuallyRendered(renderedCount);
        for (int i = 0, j = 0; i < items.size(); i++)
        {
            if (items[i].rendered)
            {
                itemsActuallyRendered[j] = items[i];
                segmentsActuallyRendered[j] = segments[i];
                j++;
            }
        }
        items.clear();
        segments.clear();
        for (int i = 0; i < renderedCount; i++) {
            items.emplace_back(itemsActuallyRendered[i]);
            segments.emplace_back(segmentsActuallyRendered[i]);
        }
        itemsActuallyRendered.clear();
        segmentsActuallyRendered.clear();
    }
}
