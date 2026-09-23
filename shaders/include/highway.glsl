// Elevated expressways: they run above every 8th side street (x = k*480, y = k*480),
// piers in the street median, deck clear of the facades (street corridor is +-7 m).
// Shared by infra.vert (geometry), traffic.vert (deck traffic) and world.cpp (pier
// collision; keep the constants in sync with apex::kHighway*).
#ifndef APEX_HIGHWAY_GLSL
#define APEX_HIGHWAY_GLSL

const float kHighwayEvery = 480.0;   // metres between expressway lines
const float kDeckHeight[2] = float[2](18.0, 25.0);  // family 0 (along y) / 1 (along x): no clash at crossings
const float kDeckHalfWidth = 5.8;
const float kDeckThickness = 1.3;
const float kSegment = 32.0;         // pier spacing
const float kPierHalf = 0.8;

#endif
