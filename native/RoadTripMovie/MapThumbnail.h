#pragma once

#include "Pipeline.h"

#include <filesystem>

namespace RoadTripMovie::MapThumbnail
{
    std::filesystem::path Render(
        GeoLocation const& location,
        std::filesystem::path const& cacheDirectory,
        int width,
        int height,
        int zoom);
}
