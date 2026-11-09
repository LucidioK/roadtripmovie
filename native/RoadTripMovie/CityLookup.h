#pragma once

#include "Pipeline.h"

#include <string>

namespace RoadTripMovie::CityLookup
{
    std::string NearestCity(GeoLocation const& location);
}
