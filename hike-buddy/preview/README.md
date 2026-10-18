# hike-buddy/preview/ — screen preview on the PC

Builds the real `main.cpp` drawing code with the host compiler and renders
one screen to a PPM image, using made-up sensor values. Used to check a
layout before booting the phone.

    make -C hike-buddy/preview
    out/preview/preview <heading> <steps> out.ppm [page] [map] [lat lon [zoom]]

    # main page, compass at 247 deg, 8432 steps
    out/preview/preview 247 8432 out/main.ppm
    # map page around Mt. Takao, zoom level 1
    out/preview/preview 0 0 out/map.ppm 1 out/map/hike-buddy.map 35.6251 139.2436 1

| File | What it does |
|---|---|
| `preview.cpp` | Includes `../main.cpp` (its `main` renamed), fakes the battery, a 3-hour altitude profile and a GPS fix, and replaces the fbdev backend with one that writes the PPM. |
| `gps_stub.cpp` | Stands in for `gps.cpp` (no modem on the PC). |
| `Makefile` | Host `g++` build into `out/preview/`. |
