#include "MapThumbnail.h"

#include <windows.h>
#include <winhttp.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

namespace RoadTripMovie::MapThumbnail
{
    namespace
    {
        using Handle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

        class GdiPlusRuntime
        {
        public:
            GdiPlusRuntime()
            {
                Gdiplus::GdiplusStartupInput input;
                if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok)
                    throw std::runtime_error("Could not initialize the map image renderer.");
            }

            ~GdiPlusRuntime()
            {
                Gdiplus::GdiplusShutdown(token_);
            }

            GdiPlusRuntime(GdiPlusRuntime const&) = delete;
            GdiPlusRuntime& operator=(GdiPlusRuntime const&) = delete;

        private:
            ULONG_PTR token_{};
        };

        GdiPlusRuntime& GdiPlus()
        {
            static GdiPlusRuntime runtime;
            return runtime;
        }

        void CheckGdi(Gdiplus::Status status, char const* action)
        {
            if (status != Gdiplus::Ok)
                throw std::runtime_error(std::string(action) + " (GDI+ status " +
                    std::to_string(status) + ").");
        }

        std::wstring TilePath(
            std::filesystem::path const& cacheDirectory,
            int zoom,
            int x,
            int y)
        {
            return (cacheDirectory / L"tiles" / std::to_wstring(zoom) /
                std::to_wstring(x) / (std::to_wstring(y) + L".png")).wstring();
        }

        std::vector<uint8_t> DownloadTile(int zoom, int x, int y)
        {
            Handle session(WinHttpOpen(
                L"RoadTripMovie/1.0 (https://github.com/LucidioK/roadtripmovie; map inset)",
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0), WinHttpCloseHandle);
            if (!session)
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not initialize OpenStreetMap tile download");
            WinHttpSetTimeouts(session.get(), 10000, 10000, 10000, 10000);

            Handle connection(WinHttpConnect(
                session.get(), L"tile.openstreetmap.org", INTERNET_DEFAULT_HTTPS_PORT, 0),
                WinHttpCloseHandle);
            if (!connection)
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not connect to OpenStreetMap");

            auto path = L"/" + std::to_wstring(zoom) + L"/" +
                std::to_wstring(x) + L"/" + std::to_wstring(y) + L".png";
            Handle request(WinHttpOpenRequest(
                connection.get(), L"GET", path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_SECURE), WinHttpCloseHandle);
            if (!request)
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not create an OpenStreetMap request");
            if (!WinHttpSendRequest(
                    request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not download an OpenStreetMap tile");

            DWORD status{};
            DWORD statusSize = sizeof(status);
            if (!WinHttpQueryHeaders(
                    request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                    WINHTTP_NO_HEADER_INDEX))
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not read the OpenStreetMap response status");
            if (status != HTTP_STATUS_OK)
                throw std::runtime_error("OpenStreetMap returned HTTP " +
                    std::to_string(status) + " while downloading a map tile.");

            std::vector<uint8_t> bytes;
            for (;;)
            {
                DWORD available{};
                if (!WinHttpQueryDataAvailable(request.get(), &available))
                    throw std::system_error(
                        static_cast<int>(GetLastError()), std::system_category(),
                        "Could not read an OpenStreetMap tile");
                if (available == 0)
                    break;
                if (bytes.size() + available > 10 * 1024 * 1024)
                    throw std::runtime_error("OpenStreetMap returned an oversized tile.");
                auto offset = bytes.size();
                bytes.resize(offset + available);
                DWORD received{};
                if (!WinHttpReadData(
                        request.get(), bytes.data() + offset, available, &received))
                    throw std::system_error(
                        static_cast<int>(GetLastError()), std::system_category(),
                        "Could not read an OpenStreetMap tile");
                bytes.resize(offset + received);
            }
            if (bytes.empty())
                throw std::runtime_error("OpenStreetMap returned an empty map tile.");
            return bytes;
        }

        void EnsureTile(
            std::filesystem::path const& cacheDirectory,
            int zoom,
            int x,
            int y)
        {
            auto tile = std::filesystem::path(TilePath(cacheDirectory, zoom, x, y));
            if (std::filesystem::is_regular_file(tile))
                return;
            std::filesystem::create_directories(tile.parent_path());
            auto bytes = DownloadTile(zoom, x, y);
            auto temporary = tile;
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output || !output.write(
                        reinterpret_cast<char const*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
                    throw std::runtime_error("Could not cache an OpenStreetMap tile.");
            }
            if (!MoveFileExW(
                    temporary.c_str(), tile.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                auto error = GetLastError();
                DeleteFileW(temporary.c_str());
                if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
                    throw std::system_error(
                        static_cast<int>(error), std::system_category(),
                        "Could not write an OpenStreetMap tile cache file");
            }
        }

        CLSID BitmapEncoderClsid()
        {
            UINT count{};
            UINT bytes{};
            CheckGdi(Gdiplus::GetImageEncodersSize(&count, &bytes),
                "Could not query the BMP encoder");
            std::vector<uint8_t> storage(bytes);
            auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
            CheckGdi(Gdiplus::GetImageEncoders(count, bytes, encoders),
                "Could not read image encoders");
            for (UINT index = 0; index < count; ++index)
            {
                if (std::wcscmp(encoders[index].MimeType, L"image/bmp") == 0)
                    return encoders[index].Clsid;
            }
            throw std::runtime_error("The BMP image encoder is unavailable.");
        }
    }

    std::filesystem::path Render(
        GeoLocation const& location,
        std::filesystem::path const& cacheDirectory,
        int width,
        int height,
        int zoom)
    {
        if (!std::isfinite(location.latitude) || !std::isfinite(location.longitude) ||
            location.latitude < -90.0 || location.latitude > 90.0 ||
            location.longitude < -180.0 || location.longitude > 180.0)
            throw std::invalid_argument("The media GPS coordinates are invalid.");
        if (width <= 0 || height <= 0 || zoom < 0 || zoom > 19)
            throw std::invalid_argument("The map inset dimensions or zoom are invalid.");

        std::filesystem::create_directories(cacheDirectory);
        auto key = std::to_wstring(std::llround(location.latitude * 10000.0)) + L"_" +
            std::to_wstring(std::llround(location.longitude * 10000.0)) + L"_" +
            std::to_wstring(zoom) + L"_" + std::to_wstring(width) + L"x" +
            std::to_wstring(height) + L".bmp";
        auto outputPath = cacheDirectory / key;
        if (std::filesystem::is_regular_file(outputPath))
            return outputPath;

        GdiPlus();
        auto worldTiles = 1 << zoom;
        auto worldPixels = static_cast<double>(worldTiles) * 256.0;
        auto latitude = std::clamp(location.latitude, -85.05112878, 85.05112878);
        auto centerX = (location.longitude + 180.0) / 360.0 * worldPixels;
        auto latitudeRadians = latitude * 3.14159265358979323846 / 180.0;
        auto centerY = (1.0 - std::asinh(std::tan(latitudeRadians)) /
            3.14159265358979323846) / 2.0 * worldPixels;
        auto left = centerX - width / 2.0;
        auto top = centerY - height / 2.0;
        auto firstTileX = static_cast<int>(std::floor(left / 256.0));
        auto lastTileX = static_cast<int>(std::floor((left + width - 1) / 256.0));
        auto firstTileY = static_cast<int>(std::floor(top / 256.0));
        auto lastTileY = static_cast<int>(std::floor((top + height - 1) / 256.0));

        Gdiplus::Bitmap map(width, height, PixelFormat24bppRGB);
        CheckGdi(map.GetLastStatus(), "Could not create a map thumbnail");
        Gdiplus::Graphics graphics(&map);
        CheckGdi(graphics.GetLastStatus(), "Could not initialize map drawing");
        graphics.Clear(Gdiplus::Color(255, 242, 240, 232));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        for (auto tileY = firstTileY; tileY <= lastTileY; ++tileY)
        {
            if (tileY < 0 || tileY >= worldTiles)
                continue;
            for (auto tileX = firstTileX; tileX <= lastTileX; ++tileX)
            {
                auto wrappedX = ((tileX % worldTiles) + worldTiles) % worldTiles;
                EnsureTile(cacheDirectory, zoom, wrappedX, tileY);
                auto tilePath = TilePath(cacheDirectory, zoom, wrappedX, tileY);
                Gdiplus::Bitmap tile(tilePath.c_str());
                CheckGdi(tile.GetLastStatus(), "Could not decode an OpenStreetMap tile");
                auto destinationX = static_cast<Gdiplus::REAL>(tileX * 256.0 - left);
                auto destinationY = static_cast<Gdiplus::REAL>(tileY * 256.0 - top);
                CheckGdi(graphics.DrawImage(
                    &tile,
                    Gdiplus::RectF(destinationX, destinationY, 256.0f, 256.0f)),
                    "Could not draw an OpenStreetMap tile");
            }
        }

        auto markerX = static_cast<Gdiplus::REAL>(centerX - left);
        auto markerY = static_cast<Gdiplus::REAL>(centerY - top);
        Gdiplus::SolidBrush marker(Gdiplus::Color(255, 217, 83, 79));
        Gdiplus::Pen markerOutline(Gdiplus::Color(255, 255, 255, 255), 2.0f);
        Gdiplus::RectF markerBounds(markerX - 5, markerY - 5, 10.0f, 10.0f);
        CheckGdi(graphics.FillEllipse(&marker, markerBounds),
            "Could not draw the map location marker");
        CheckGdi(graphics.DrawEllipse(&markerOutline, markerBounds),
            "Could not outline the map location marker");

        Gdiplus::SolidBrush attributionBackground(Gdiplus::Color(200, 255, 255, 255));
        Gdiplus::Font attributionFont(L"Arial", 8.0f);
        Gdiplus::SolidBrush attributionForeground(Gdiplus::Color(255, 35, 35, 35));
        Gdiplus::RectF attributionBounds(
            4.0f, static_cast<Gdiplus::REAL>(height - 16), 210.0f, 12.0f);
        CheckGdi(graphics.FillRectangle(&attributionBackground, attributionBounds),
            "Could not draw map attribution background");
        CheckGdi(graphics.DrawString(
            L"© OpenStreetMap contributors", -1, &attributionFont,
            Gdiplus::PointF(6.0f, static_cast<Gdiplus::REAL>(height - 16)),
            &attributionForeground), "Could not draw map attribution");

        Gdiplus::Pen border(Gdiplus::Color(255, 255, 255, 255), 3.0f);
        CheckGdi(graphics.DrawRectangle(&border, 1, 1, width - 3, height - 3),
            "Could not draw the map border");

        auto temporary = outputPath;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
            L".tmp.bmp";
        auto encoder = BitmapEncoderClsid();
        CheckGdi(map.Save(temporary.c_str(), &encoder, nullptr),
            "Could not save the map thumbnail");
        if (!MoveFileExW(
                temporary.c_str(), outputPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            auto error = GetLastError();
            DeleteFileW(temporary.c_str());
            throw std::system_error(
                static_cast<int>(error), std::system_category(),
                "Could not cache the map thumbnail");
        }
        return outputPath;
    }

    std::filesystem::path RenderCityLabel(
        std::string const& cityName,
        std::filesystem::path const& cacheDirectory,
        int width,
        int height)
    {
        if (cityName.empty() || width <= 0 || height <= 0)
            throw std::invalid_argument("The city label text or dimensions are invalid.");
        auto wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, cityName.data(),
            static_cast<int>(cityName.size()), nullptr, 0);
        if (wideLength <= 0)
            throw std::runtime_error("The nearest city name is not valid UTF-8.");
        std::wstring wideName(static_cast<std::size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, cityName.data(),
                static_cast<int>(cityName.size()), wideName.data(), wideLength) != wideLength)
            throw std::runtime_error("Could not decode the nearest city name.");

        auto labelWidth = std::max(1, width * 8 / 10);
        auto labelHeight = std::max(1, height / 10);
        auto key = std::to_wstring(std::hash<std::string>{}(cityName)) + L"_" +
            std::to_wstring(labelWidth) + L"x" + std::to_wstring(labelHeight) + L".bmp";
        auto labelDirectory = cacheDirectory / L"city_labels";
        auto outputPath = labelDirectory / key;
        if (std::filesystem::is_regular_file(outputPath))
            return outputPath;

        std::filesystem::create_directories(labelDirectory);
        GdiPlus();
        Gdiplus::Bitmap label(labelWidth, labelHeight, PixelFormat32bppARGB);
        CheckGdi(label.GetLastStatus(), "Could not create a city label image");
        Gdiplus::Graphics graphics(&label);
        CheckGdi(graphics.GetLastStatus(), "Could not initialize city label drawing");
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        Gdiplus::Font font(L"Arial", 24.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush foreground(Gdiplus::Color(255, 255, 255, 0));
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        Gdiplus::RectF bounds(
            0.0f, 0.0f, static_cast<Gdiplus::REAL>(labelWidth),
            static_cast<Gdiplus::REAL>(labelHeight));
        CheckGdi(graphics.DrawString(
            wideName.c_str(), static_cast<INT>(wideName.size()), &font,
            bounds, &format, &foreground), "Could not draw the nearest city name");

        auto temporary = outputPath;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
            L".tmp.bmp";
        auto encoder = BitmapEncoderClsid();
        CheckGdi(label.Save(temporary.c_str(), &encoder, nullptr),
            "Could not save the city label image");
        if (!MoveFileExW(
                temporary.c_str(), outputPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            auto error = GetLastError();
            DeleteFileW(temporary.c_str());
            throw std::system_error(
                static_cast<int>(error), std::system_category(),
                "Could not cache the city label image");
        }
        return outputPath;
    }
}
