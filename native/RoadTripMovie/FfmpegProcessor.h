#pragma once

#include "Pipeline.h"

#include <vector>

namespace RoadTripMovie::FfmpegProcessor
{
    struct MediaItem
    {
        std::filesystem::path file;
        bool photo{};
        double duration{};
        bool hasAudio{};
        std::optional<GeoLocation> location;
    };

    std::optional<GeoLocation> ReadLocation(
        std::filesystem::path const& path,
        bool photo);

    void ValidateAudio(std::filesystem::path const& path);

    void RenderSegment(
        MediaItem const& item,
        PipelineSettings const& settings,
        std::filesystem::path const& target,
        std::stop_token cancellation);

    void Concatenate(
        std::vector<std::filesystem::path> const& segments,
        std::filesystem::path const& target,
        std::stop_token cancellation);

    void MixMusic(
        std::filesystem::path const& joinedVideo,
        std::filesystem::path const& music,
        std::filesystem::path const& target,
        double musicVolume,
        double duration,
        std::stop_token cancellation);
}
