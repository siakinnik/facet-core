// Time zone selection for the panel process (and, via the protocol, plugins).
// Facet sets TZ for itself instead of changing the system zone: no root
// needed and no side effects on the rest of the device.
#pragma once

#include <string>
#include <vector>

namespace facet::tz {

// IANA zone ids from tzdata (zone1970.tab / zone.tab), sorted. Cached.
const std::vector<std::string>& zones();
// The system zone ("Europe/Berlin"), or "" if it cannot be determined.
std::string system_zone();
// "" = system zone. Returns false (and keeps the system zone) if unknown.
bool apply(const std::string& zone);
// "Europe/Berlin" -> "Europe"; "UTC" -> "UTC".
std::string region_of(const std::string& zone);
// "America/Argentina/Buenos_Aires" -> "Argentina/Buenos Aires".
std::string city_label(const std::string& zone);

}  // namespace facet::tz
