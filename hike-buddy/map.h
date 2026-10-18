// Offline OpenStreetMap: vector tiles written by tools/map/build_map.py,
// rasterised straight into a surface.
//
// File format (little-endian):
//   "HBMAP1\0\0", u32 nlevels,
//   per level: u8 tile zoom, u8 shift, u16 0, u32 tx0, ty0, nx, ny, index offset
//   per level index: nx*ny x {u32 offset, u32 length}, row-major from tx0,ty0
//   tile: varint count, then features
//     line  (kind < 20): u8 kind, varint npts, zigzag-varint dx,dy from the
//                        previous point (the first from 0,0)
//     point (kind >= 20): u8 kind, varint x, y, ele (m, 0 = none), u8 name length, name
//   Tile-local coordinates are global units >> shift from the tile's corner.
// Global units: Web Mercator, 2^28 around the world.
#pragma once

#include <stdint.h>

struct GRSurface;

bool map_open(const char* path);
bool map_loaded();

void map_from_lonlat(double lon, double lat, double* gx, double* gy);
// Global units per metre on the ground at global y.
double map_units_per_metre(double gy);

struct MapView {
  double cx, cy;  // centre, global units
  float mpp;      // metres per pixel
};

// Draws the map for `v` into rows [top, top + height) of s (cleared first),
// centred in that band.
void map_render(GRSurface* s, int top, int height, const MapView& v);
