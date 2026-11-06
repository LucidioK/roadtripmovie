#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace RoadTripMovie
{
    struct PipelineSettings
    {
        std::filesystem::path inputFolder;
        std::filesystem::path musicPath;
        std::filesystem::path outputPath;
        std::filesystem::path ffmpegDirectory;
        double photoDuration{ 5.0 };
        std::optional<double> maxVideoDuration;
        int width{ 1920 };
        int height{ 1080 };
        int fps{ 30 };
        double videoAudioVolume{ 0.25 };
        double musicVolume{ 1.0 };
        unsigned int maxParallelism{ 8 };
    };

    using ProgressCallback = std::function<void(std::wstring)>;

    struct MediaItem
    {
        std::filesystem::path file;
        bool photo{};
        std::filesystem::file_time_type modifiedAt;
        double duration{};
        bool hasAudio{};
        std::size_t index{};
        bool rendered{};
    };

    class Pipeline
    {
    public:
        using CancellationSource = std::stop_source;

        static void Run(
            PipelineSettings const& settings,
            std::stop_token cancellation,
            ProgressCallback const& progress);
        static void RemoveNotRenderedSegments(
            std::vector<MediaItem>& items,
            std::vector<std::filesystem::path>& segments);
    };
}
