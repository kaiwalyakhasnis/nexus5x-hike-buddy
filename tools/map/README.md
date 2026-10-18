# tools/map/ — offline map builder (runs on the PC)

`build_map.py` turns an OpenStreetMap extract into `hike-buddy.map`, the
file the phone's map page reads from `/share/hike-buddy.map`.

    python3 -m venv out/mapenv && out/mapenv/bin/pip install osmium shapely
    curl -L -o out/map/kanto.osm.pbf https://download.geofabrik.de/asia/japan/kanto-latest.osm.pbf
    out/mapenv/bin/python tools/map/build_map.py out/map/kanto.osm.pbf out/map/hike-buddy.map \
        [west south east north]      # default: Tokyo (without the islands) + Kanagawa

## What it keeps

Trails (`path`, `bridleway`, footways with `sac_scale`), tracks, roads from
motorway down to tertiary, railways, rivers, streams, lakes, coastline,
peaks (with height), huts, springs/drinking water and stations. Residential
streets are left out to keep the file small: the whole boot image must stay
around 15 MB or the bootloader won't start it. Names are kept only when a
Latin-script name exists (the phone's font is ASCII).

## Format

Three levels of detail (tile zoom 13, 11 and 9, simplified to 1.5 m, 6 m and
25 m), each a grid of tiles holding varint-packed lines and points. The
exact layout is documented in `hike-buddy/map.h`.

Map data © OpenStreetMap contributors, ODbL. The map page shows the credit.
