#include "CityLookup.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace RoadTripMovie::CityLookup
{
    namespace
    {
        struct City
        {
            std::string name;
            std::array<double, 3> point;
        };

        struct Node
        {
            City city;
            std::size_t left{ std::numeric_limits<std::size_t>::max() };
            std::size_t right{ std::numeric_limits<std::size_t>::max() };
            std::size_t axis{};
        };

        std::filesystem::path CitiesFilePath()
        {
            std::wstring executable(32768, L'\0');
            auto length = GetModuleFileNameW(
                nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (length == 0 || length >= executable.size())
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "Could not locate the installed application to load city data.");
            executable.resize(length);
            return std::filesystem::path(executable).parent_path() /
                L"worldcities-geo-simp.csv";
        }

        std::vector<std::string> ParseCsvRow(std::string_view line)
        {
            std::vector<std::string> fields;
            std::string field;
            bool quoted{};
            for (std::size_t index = 0; index < line.size(); ++index)
            {
                auto character = line[index];
                if (quoted)
                {
                    if (character == '"')
                    {
                        if (index + 1 < line.size() && line[index + 1] == '"')
                        {
                            field.push_back('"');
                            ++index;
                        }
                        else
                        {
                            quoted = false;
                        }
                    }
                    else
                    {
                        field.push_back(character);
                    }
                }
                else if (character == ',' && fields.size() < 2)
                {
                    fields.push_back(std::move(field));
                    field.clear();
                }
                else if (character == '"' && field.empty())
                {
                    quoted = true;
                }
                else
                {
                    field.push_back(character);
                }
            }
            if (quoted)
                throw std::runtime_error("The city data CSV contains an unterminated quoted field.");
            fields.push_back(std::move(field));
            if (fields.size() != 3)
                throw std::runtime_error("The city data CSV must have three columns.");
            return fields;
        }

        double ParseCoordinate(std::string const& value, double minimum, double maximum)
        {
            char* end{};
            auto result = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(result) ||
                result < minimum || result > maximum)
                throw std::runtime_error("The city data CSV contains an invalid coordinate.");
            return result;
        }

        std::array<double, 3> ToUnitSphere(double latitude, double longitude)
        {
            constexpr double radiansPerDegree = 3.14159265358979323846 / 180.0;
            auto latitudeRadians = latitude * radiansPerDegree;
            auto longitudeRadians = longitude * radiansPerDegree;
            auto latitudeCosine = std::cos(latitudeRadians);
            return {
                latitudeCosine * std::cos(longitudeRadians),
                latitudeCosine * std::sin(longitudeRadians),
                std::sin(latitudeRadians)
            };
        }

        std::vector<City> LoadCities()
        {
            auto path = CitiesFilePath();
            std::ifstream input(path, std::ios::binary);
            if (!input)
                throw std::runtime_error(
                    "Could not open the packaged city data file: " + path.string());

            std::string line;
            if (!std::getline(input, line))
                throw std::runtime_error("The packaged city data file is empty.");
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xef &&
                static_cast<unsigned char>(line[1]) == 0xbb &&
                static_cast<unsigned char>(line[2]) == 0xbf)
                line.erase(0, 3);
            if (line != "cityname,latitude,longitude")
                throw std::runtime_error("The packaged city data file has an invalid header.");

            std::vector<City> cities;
            while (std::getline(input, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                auto fields = ParseCsvRow(line);
                auto latitude = ParseCoordinate(fields[1], -90.0, 90.0);
                auto longitude = ParseCoordinate(fields[2], -180.0, 180.0);
                if (fields[0].empty())
                    throw std::runtime_error("The city data CSV contains an unnamed city.");
                cities.push_back({ std::move(fields[0]), ToUnitSphere(latitude, longitude) });
            }
            if (!input.eof())
                throw std::runtime_error("Could not read the packaged city data file.");
            if (cities.empty())
                throw std::runtime_error("The packaged city data file contains no cities.");
            return cities;
        }

        class CityIndex
        {
        public:
            CityIndex()
            {
                auto cities = LoadCities();
                nodes_.resize(cities.size());
                root_ = Build(cities, 0, cities.size(), 0);
            }

            std::string const& Nearest(std::array<double, 3> const& point) const
            {
                auto bestDistance = std::numeric_limits<double>::infinity();
                std::size_t best = std::numeric_limits<std::size_t>::max();
                Search(root_, point, bestDistance, best);
                if (best == std::numeric_limits<std::size_t>::max())
                    throw std::runtime_error("Could not find a city near the media location.");
                return nodes_[best].city.name;
            }

        private:
            std::size_t Build(
                std::vector<City>& cities,
                std::size_t begin,
                std::size_t end,
                std::size_t axis)
            {
                if (begin == end)
                    return std::numeric_limits<std::size_t>::max();
                auto middle = begin + (end - begin) / 2;
                std::nth_element(
                    cities.begin() + begin, cities.begin() + middle, cities.begin() + end,
                    [axis](City const& left, City const& right)
                    {
                        return left.point[axis] < right.point[axis];
                    });
                nodes_[middle].city = std::move(cities[middle]);
                nodes_[middle].axis = axis;
                nodes_[middle].left = Build(cities, begin, middle, (axis + 1) % 3);
                nodes_[middle].right = Build(cities, middle + 1, end, (axis + 1) % 3);
                return middle;
            }

            void Search(
                std::size_t nodeIndex,
                std::array<double, 3> const& point,
                double& bestDistance,
                std::size_t& best) const
            {
                if (nodeIndex == std::numeric_limits<std::size_t>::max())
                    return;
                auto const& node = nodes_[nodeIndex];
                double distance{};
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    auto delta = point[axis] - node.city.point[axis];
                    distance += delta * delta;
                }
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = nodeIndex;
                }

                auto axis = node.axis;
                auto delta = point[axis] - node.city.point[axis];
                auto nearChild = delta < 0.0 ? node.left : node.right;
                auto farChild = delta < 0.0 ? node.right : node.left;
                Search(nearChild, point, bestDistance, best);
                if (delta * delta < bestDistance)
                    Search(farChild, point, bestDistance, best);
            }

            std::vector<Node> nodes_;
            std::size_t root_{ std::numeric_limits<std::size_t>::max() };
        };

        CityIndex& Index()
        {
            static CityIndex index;
            return index;
        }
    }

    std::string NearestCity(GeoLocation const& location)
    {
        if (!std::isfinite(location.latitude) || !std::isfinite(location.longitude) ||
            location.latitude < -90.0 || location.latitude > 90.0 ||
            location.longitude < -180.0 || location.longitude > 180.0)
            throw std::invalid_argument("The media GPS coordinates are invalid.");
        if (-0.1 <= location.latitude && location.latitude <= 0.1 &&
            -0.1 <= location.longitude && location.longitude <= 0.1)
            return "_";
        return Index().Nearest(ToUnitSphere(location.latitude, location.longitude));
    }
}
