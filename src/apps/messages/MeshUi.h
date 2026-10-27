#pragma once

// Small helpers shared by the mesh unit's screens (Meldinger, Kart, SOS).

#include <MeshPacket.h>

#include <cstdint>
#include <string>

class GfxRenderer;

namespace meshui {

// Title bar with the screen's name. Returns the y where content starts.
int drawTitle(const GfxRenderer& renderer, const char* title);

// "14:21", "i går 14:21" or "3. okt" for a unix time; "" when unknown.
std::string timeText(uint32_t unixTime);
// "5 min siden", "2 t siden", "3 d siden".
std::string agoText(uint32_t unixTime);
// Unix seconds now, or 0 when the clock is not set.
uint32_t nowUnix();

// Distance in metres and bearing in degrees (0 = north) from a to b.
double distanceM(const mesh::Position& a, const mesh::Position& b);
double bearingDeg(const mesh::Position& a, const mesh::Position& b);
// "1,2 km" or "350 m".
std::string distanceText(double metres);
// "nord", "nordøst", ...
const char* directionText(double degrees);
// "59,9139 N  10,7522 Ø".
std::string coordinatesText(const mesh::Position& p);

}  // namespace meshui
