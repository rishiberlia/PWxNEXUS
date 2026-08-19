# SmartRoute AI

A menu-first route planner for Chennai: add your own hubs, pick a transport
mode (walk → cycle → bike → car), and get a fastest-route calculation between
any two points on the map.

**Architecture — two real, separate programs:**

- **`backend/server.cpp`** — a C++17 HTTP JSON API. Holds the hub list,
  runs Dijkstra's algorithm over the hubs, and computes travel times either
  from real Google Maps data (if you enable it) or from a Haversine-distance
  estimate. No frameworks — raw POSIX sockets and a hand-rolled JSON layer,
  so it builds with just `g++`.
- **`frontend/index.html`** — the menu screen ("SmartRoute AI") and the
  planner UI (map, hub selector, mode picker, route stats). Talks to the
  backend over `fetch()` at `http://localhost:8080`. Map tiles are OpenStreetMap
  via Leaflet (free, no key needed) — only the *travel-time* numbers use
  Google Maps, not the map rendering.

---

## 1. Run it (works immediately, no API key required)

```bash
cd backend
g++ -std=c++17 -O2 -o smartroute_server server.cpp -lpthread
./smartroute_server
```

You should see:
```
[SmartRoute AI] Backend listening on http://localhost:8080
```

Then just open `frontend/index.html` in a browser (double-click it, or
`open frontend/index.html` / drag it into a tab). Click **Launch planner**.

In this mode, travel times are computed from real Chennai coordinates using
the Haversine formula (great-circle distance × a 1.35 road-indirectness
factor) and each mode's typical speed. It's a solid estimate, not live
traffic.

---

## 2. Enable real Google Maps travel times

The backend can call Google's **Distance Matrix API** server-side (so your
API key never touches the browser). This requires two things your machine
needs internet for, which this build environment did not have — so this path
is written and ready, but untested end-to-end here. Steps on your own machine:

```bash
# 1. Install libcurl's dev headers
sudo apt install libcurl4-openssl-dev      # Debian/Ubuntu
# (brew install curl on macOS, vcpkg install curl on Windows)

# 2. Get a Google Maps API key with "Distance Matrix API" enabled:
#    https://console.cloud.google.com/google/maps-apis/credentials

# 3. Build with Google Maps support turned on
cd backend
g++ -std=c++17 -O2 -DENABLE_GOOGLE_MAPS -o smartroute_server server.cpp -lpthread -lcurl

# 4. Set your key and run
export GOOGLE_MAPS_API_KEY="your-key-here"
./smartroute_server
```

When a key is present and reachable, `/api/route` returns
`"usedLiveData": true` and the sidebar shows a **green "Live Google Maps
data"** badge instead of "Estimated." If the API call fails for any reason
(no internet, bad key, rate limit), it silently falls back to the Haversine
estimate — the app never breaks.

**Mode → Google Maps mapping** (Google has no native "motorbike" mode):
| SmartRoute mode | Google Distance Matrix mode | Notes |
|---|---|---|
| Walk | `walking` | direct |
| Cycle | `bicycling` | direct |
| Bike (two-wheeler) | `driving` × 0.72 | two-wheelers thread traffic faster than cars in Chennai; tune the factor in `MODES` in `server.cpp` |
| Car | `driving` | direct, includes `duration_in_traffic` when available |

---

## 3. Using the app

- **Menu screen** — title screen, click "Launch planner."
- **Origin / Destination** — dropdowns populated from the backend's hub list.
  Twelve real Chennai landmarks are pre-seeded (Chennai Central, T. Nagar,
  Anna Nagar, the airport, Marina Beach, OMR, Tambaram, etc.).
- **+ Add a hub / location** — type a name and either enter lat/lng directly
  or click anywhere on the map to fill them in, then Save. It's POSTed to the
  backend and immediately available for routing.
- **Mode of transport** — Walk, Cycle, Bike, Car, ordered slow → fast, each
  showing its baseline speed.
- **Find fastest route** — runs Dijkstra across every hub (so it can hop
  through an intermediate hub if that's faster for the selected mode), draws
  the path on the map, and shows ETA, distance, and whether the number came
  from live data or an estimate.

---

## 4. API reference

| Method | Path | Body | Returns |
|---|---|---|---|
| GET | `/api/hubs` | – | `[{id, name, lat, lng}, ...]` |
| POST | `/api/hubs` | `{name, lat, lng}` | the created hub |
| GET | `/api/modes` | – | `[{key, label, baseKmh}, ...]` |
| POST | `/api/route` | `{originId, destId, mode}` | `{found, totalMinutes, totalKm, usedLiveData, path:[hubIds]}` |

---

## 5. Known limitations / next steps

- The backend is single-process, in-memory — hubs you add are lost on
  restart. Add a `hubs.json` load/save if you need persistence.
- Dijkstra runs over a *complete graph* of all hub pairs (there's no real
  Chennai road-segment graph), so each "hop" is a direct point-to-point leg,
  not a turn-by-turn road route. That's intentional for a hub-routing tool;
  swap in the Google **Directions API** (polyline + steps) instead of
  Distance Matrix if you want actual road-following routes drawn on the map.
- The HTTP server is intentionally minimal (single-purpose, no HTTPS, no
  auth) — fine for local/demo use, not for exposing on the open internet as-is.
