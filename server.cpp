// ============================================================================
//  SmartRoute AI — backend
//  Plain C++17, POSIX sockets, no external dependencies required to build.
//  Optional real Google Maps travel times via libcurl (see README.md).
//
//  Build (fallback mode, no internet calls, works out of the box):
//      g++ -std=c++17 -O2 -o smartroute_server server.cpp -lpthread
//
//  Build (real Google Maps mode):
//      sudo apt install libcurl4-openssl-dev
//      g++ -std=c++17 -O2 -DENABLE_GOOGLE_MAPS -o smartroute_server server.cpp -lpthread -lcurl
//      export GOOGLE_MAPS_API_KEY="your-key-here"
//
//  Run:
//      ./smartroute_server            (listens on http://localhost:8080)
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_GOOGLE_MAPS
#include <curl/curl.h>
#endif

// ---------------------------------------------------------------------------
// 1. DATA MODEL
// ---------------------------------------------------------------------------

struct Hub {
    int id;
    std::string name;
    double lat;
    double lng;
};

// Modes ordered slow -> fast. baseKmh is the open-road fallback speed used
// when no live Google Maps data is available.
struct Mode {
    std::string key;     // "walk" | "cycle" | "bike" | "car"
    std::string label;
    double baseKmh;
    std::string googleTravelMode; // Google Distance Matrix "mode" param
    double googleFactor; // multiplier applied to Google's result (bike has no
                          // native Google mode, so we derive it from driving)
};

static const std::vector<Mode> MODES = {
    {"walk",  "Walk",   5.0,  "walking",   1.0},
    {"cycle", "Cycle",  15.0, "bicycling", 1.0},
    {"bike",  "Bike (Two-wheeler)", 32.0, "driving", 0.72}, // ~28% faster than car in dense traffic
    {"car",   "Car",    22.0, "driving",   1.0},
};

static const Mode* findMode(const std::string& key) {
    for (auto& m : MODES) if (m.key == key) return &m;
    return nullptr;
}

std::mutex g_dataMutex;
std::vector<Hub> g_hubs;
int g_nextHubId = 0;

static void seedChennaiHubs() {
    // Real, well-known Chennai landmarks with approximate real-world coordinates.
    struct Seed { std::string name; double lat, lng; };
    std::vector<Seed> seeds = {
        {"Chennai Central Railway Station", 13.0827, 80.2750},
        {"Chennai International Airport",   12.9941, 80.1709},
        {"T. Nagar",                        13.0418, 80.2341},
        {"Anna Nagar",                      13.0850, 80.2101},
        {"Guindy",                          13.0067, 80.2206},
        {"Velachery",                       12.9791, 80.2183},
        {"Adyar",                           13.0012, 80.2565},
        {"Marina Beach",                    13.0500, 80.2824},
        {"OMR - Sholinganallur",            12.9010, 80.2279},
        {"Tambaram",                        12.9249, 80.1000},
        {"Egmore",                          13.0732, 80.2609},
        {"Porur",                           13.0359, 80.1567},
    };
    for (auto& s : seeds) {
        g_hubs.push_back({g_nextHubId++, s.name, s.lat, s.lng});
    }
}

// ---------------------------------------------------------------------------
// 2. DISTANCE / TIME ESTIMATION
// ---------------------------------------------------------------------------

static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * M_PI / 180.0) * std::cos(lat2 * M_PI / 180.0) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return R * c;
}

// A road-distance fudge factor: straight-line Haversine distance is always
// shorter than actual road distance. Chennai's grid is moderately indirect.
static constexpr double ROAD_FACTOR = 1.35;

#ifdef ENABLE_GOOGLE_MAPS
static size_t curlWriteCb(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Extremely small, targeted JSON scraper: pulls the first
// "duration":{"value":N  and "distance":{"value":N  out of a Google Distance
// Matrix response. Good enough because we control the request and know the
// response shape; not a general JSON parser.
static bool extractNumberAfter(const std::string& body, const std::string& key, double& out) {
    auto pos = body.find(key);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < body.size() && (body[pos] == ' ')) pos++;
    size_t start = pos;
    while (pos < body.size() && (isdigit(body[pos]) || body[pos] == '.' || body[pos] == '-')) pos++;
    if (pos == start) return false;
    out = std::stod(body.substr(start, pos - start));
    return true;
}

// Returns travel time in minutes via Google Distance Matrix, or -1 on failure.
static double googleTravelMinutes(double lat1, double lon1, double lat2, double lon2,
                                   const std::string& googleMode) {
    const char* key = std::getenv("GOOGLE_MAPS_API_KEY");
    if (!key || std::strlen(key) == 0) return -1;

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    std::ostringstream url;
    url << "https://maps.googleapis.com/maps/api/distancematrix/json"
        << "?origins=" << lat1 << "," << lon1
        << "&destinations=" << lat2 << "," << lon2
        << "&mode=" << googleMode
        << "&departure_time=now"
        << "&key=" << key;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return -1;

    // Prefer duration_in_traffic (live) if present, else duration.
    double seconds;
    if (extractNumberAfter(response, "\"duration_in_traffic\"", seconds) ||
        extractNumberAfter(response, "\"duration\"", seconds)) {
        return seconds / 60.0;
    }
    return -1;
}
#endif

// Travel time in minutes between two hubs for a given mode. Tries Google
// Maps first (if compiled in and a key is set), otherwise falls back to a
// Haversine-distance estimate using the mode's base speed.
static double travelMinutes(const Hub& a, const Hub& b, const Mode& mode, double& outDistKm, bool& usedLive) {
    outDistKm = haversineKm(a.lat, a.lng, b.lat, b.lng) * ROAD_FACTOR;
    usedLive = false;

#ifdef ENABLE_GOOGLE_MAPS
    double liveMin = googleTravelMinutes(a.lat, a.lng, b.lat, b.lng, mode.googleTravelMode);
    if (liveMin > 0) {
        usedLive = true;
        return liveMin * mode.googleFactor;
    }
#endif
    return (outDistKm / mode.baseKmh) * 60.0;
}

// ---------------------------------------------------------------------------
// 3. DIJKSTRA over the complete graph of hubs (edge weight = travelMinutes)
// ---------------------------------------------------------------------------

struct RouteResult {
    bool found = false;
    std::vector<int> pathHubIds;
    double totalMinutes = 0;
    double totalKm = 0;
    bool usedLiveData = false;
};

static RouteResult computeRoute(int startId, int endId, const Mode& mode) {
    RouteResult result;
    std::lock_guard<std::mutex> lock(g_dataMutex);

    // Guard against unknown hub ids up front. Without this, std::map's
    // operator[] silently default-constructs missing entries (0.0 for
    // dist, 0 for prev), which previously let path reconstruction spin
    // forever while holding this lock — deadlocking every future request.
    bool startExists = false, endExists = false;
    for (auto& h : g_hubs) {
        if (h.id == startId) startExists = true;
        if (h.id == endId) endExists = true;
    }
    if (!startExists || !endExists) return result; // found = false

    std::map<int, double> dist;
    std::map<int, int> prev;
    for (auto& h : g_hubs) dist[h.id] = std::numeric_limits<double>::infinity();
    dist[startId] = 0;

    using PQItem = std::pair<double, int>;
    std::priority_queue<PQItem, std::vector<PQItem>, std::greater<>> pq;
    pq.push({0, startId});
    std::map<int, bool> visited;
    bool anyLive = false;

    while (!pq.empty()) {
        auto [d, u] = pq.top(); pq.pop();
        if (visited[u]) continue;
        visited[u] = true;
        if (u == endId) break;

        for (auto& v : g_hubs) {
            if (v.id == u || visited[v.id]) continue;
            const Hub* hu = nullptr; const Hub* hv = &v;
            for (auto& h : g_hubs) if (h.id == u) hu = &h;
            if (!hu) continue;

            double distKm; bool live;
            double w = travelMinutes(*hu, *hv, mode, distKm, live);
            if (live) anyLive = true;

            double nd = d + w;
            if (nd < dist[v.id]) {
                dist[v.id] = nd;
                prev[v.id] = u;
                pq.push({nd, v.id});
            }
        }
    }

    if (dist[endId] == std::numeric_limits<double>::infinity()) return result;

    std::vector<int> path;
    int cur = endId;
    path.push_back(cur);
    size_t safety = g_hubs.size() + 2; // a real path can't be longer than this
    while (cur != startId) {
        auto it = prev.find(cur);
        if (it == prev.end() || safety-- == 0) return result; // corrupt/unreachable state: bail out, don't hang
        cur = it->second;
        path.push_back(cur);
    }
    std::reverse(path.begin(), path.end());

    result.found = true;
    result.pathHubIds = path;
    result.totalMinutes = dist[endId];
    result.usedLiveData = anyLive;

    double km = 0;
    for (size_t i = 0; i + 1 < path.size(); i++) {
        const Hub *a = nullptr, *b = nullptr;
        for (auto& h : g_hubs) { if (h.id == path[i]) a = &h; if (h.id == path[i+1]) b = &h; }
        double d; bool live;
        travelMinutes(*a, *b, mode, d, live);
        km += d;
    }
    result.totalKm = km;
    return result;
}

// ---------------------------------------------------------------------------
// 4. TINY JSON HELPERS
// ---------------------------------------------------------------------------

static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// Extract a string value for "key":"value" from a flat JSON object body.
static bool jsonGetString(const std::string& body, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    pos = body.find('"', pos);
    if (pos == std::string::npos) return false;
    pos++;
    size_t end = pos;
    std::string val;
    while (end < body.size() && body[end] != '"') {
        if (body[end] == '\\' && end + 1 < body.size()) { val += body[end + 1]; end += 2; continue; }
        val += body[end]; end++;
    }
    out = val;
    return true;
}

// Extract a numeric value for "key":number
static bool jsonGetNumber(const std::string& body, const std::string& key, double& out) {
    std::string pat = "\"" + key + "\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < body.size() && body[pos] == ' ') pos++;
    size_t start = pos;
    while (pos < body.size() && (isdigit(body[pos]) || body[pos] == '.' || body[pos] == '-')) pos++;
    if (pos == start) return false;
    out = std::stod(body.substr(start, pos - start));
    return true;
}

// ---------------------------------------------------------------------------
// 5. HTTP SERVER (minimal, single-purpose)
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

static const char* CORS_HEADERS =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n";

static void sendResponse(int clientFd, int statusCode, const std::string& statusText,
                          const std::string& contentType, const std::string& body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << CORS_HEADERS
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    std::string out = resp.str();
    send(clientFd, out.c_str(), out.size(), 0);
}

static void sendJson(int clientFd, int statusCode, const std::string& statusText, const std::string& json) {
    sendResponse(clientFd, statusCode, statusText, "application/json", json);
}

static std::string hubsToJson() {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < g_hubs.size(); i++) {
        auto& h = g_hubs[i];
        out << "{\"id\":" << h.id << ",\"name\":\"" << jsonEscape(h.name) << "\","
            << "\"lat\":" << h.lat << ",\"lng\":" << h.lng << "}";
        if (i + 1 < g_hubs.size()) out << ",";
    }
    out << "]";
    return out.str();
}

static std::string modesToJson() {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < MODES.size(); i++) {
        auto& m = MODES[i];
        out << "{\"key\":\"" << m.key << "\",\"label\":\"" << m.label << "\",\"baseKmh\":" << m.baseKmh << "}";
        if (i + 1 < MODES.size()) out << ",";
    }
    out << "]";
    return out.str();
}

static void handleRequest(int clientFd, const HttpRequest& req) {
    if (req.method == "OPTIONS") {
        sendResponse(clientFd, 204, "No Content", "text/plain", "");
        return;
    }

    if (req.method == "GET" && req.path == "/api/hubs") {
        sendJson(clientFd, 200, "OK", hubsToJson());
        return;
    }

    if (req.method == "GET" && req.path == "/api/modes") {
        sendJson(clientFd, 200, "OK", modesToJson());
        return;
    }

    if (req.method == "POST" && req.path == "/api/hubs") {
        std::string name; double lat = 0, lng = 0;
        bool okName = jsonGetString(req.body, "name", name);
        bool okLat = jsonGetNumber(req.body, "lat", lat);
        bool okLng = jsonGetNumber(req.body, "lng", lng);
        if (!okName || !okLat || !okLng) {
            sendJson(clientFd, 400, "Bad Request", "{\"error\":\"name, lat, lng are required\"}");
            return;
        }
        Hub h;
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            h = {g_nextHubId++, name, lat, lng};
            g_hubs.push_back(h);
        }
        std::ostringstream out;
        out << "{\"id\":" << h.id << ",\"name\":\"" << jsonEscape(h.name) << "\","
            << "\"lat\":" << h.lat << ",\"lng\":" << h.lng << "}";
        sendJson(clientFd, 201, "Created", out.str());
        return;
    }

    if (req.method == "POST" && req.path == "/api/route") {
        double originId = 0, destId = 0;
        std::string modeKey;
        if (!jsonGetNumber(req.body, "originId", originId) ||
            !jsonGetNumber(req.body, "destId", destId) ||
            !jsonGetString(req.body, "mode", modeKey)) {
            sendJson(clientFd, 400, "Bad Request", "{\"error\":\"originId, destId, mode are required\"}");
            return;
        }
        const Mode* mode = findMode(modeKey);
        if (!mode) {
            sendJson(clientFd, 400, "Bad Request", "{\"error\":\"unknown mode\"}");
            return;
        }
        RouteResult r = computeRoute((int)originId, (int)destId, *mode);
        if (!r.found) {
            sendJson(clientFd, 200, "OK", "{\"found\":false}");
            return;
        }
        std::ostringstream out;
        out << "{\"found\":true,\"totalMinutes\":" << r.totalMinutes
            << ",\"totalKm\":" << r.totalKm
            << ",\"usedLiveData\":" << (r.usedLiveData ? "true" : "false")
            << ",\"path\":[";
        for (size_t i = 0; i < r.pathHubIds.size(); i++) {
            out << r.pathHubIds[i];
            if (i + 1 < r.pathHubIds.size()) out << ",";
        }
        out << "]}";
        sendJson(clientFd, 200, "OK", out.str());
        return;
    }

    sendJson(clientFd, 404, "Not Found", "{\"error\":\"no such route\"}");
}

static HttpRequest parseRequest(const std::string& raw) {
    HttpRequest req;
    std::istringstream stream(raw);
    std::string requestLine;
    std::getline(stream, requestLine);
    std::istringstream rl(requestLine);
    std::string httpVersion;
    rl >> req.method >> req.path;

    // Strip query string for routing purposes (not used currently).
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) req.path = req.path.substr(0, qpos);

    auto bodyPos = raw.find("\r\n\r\n");
    if (bodyPos != std::string::npos) req.body = raw.substr(bodyPos + 4);
    return req;
}

static void clientThread(int clientFd) {
    char buf[8192];
    std::string raw;
    // Read until we've got the headers; then check Content-Length for body.
    ssize_t n;
    while ((n = recv(clientFd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, n);
        if (raw.find("\r\n\r\n") != std::string::npos) {
            // crude but sufficient: check if we likely have the whole body
            auto headerEnd = raw.find("\r\n\r\n") + 4;
            auto clPos = raw.find("Content-Length:");
            if (clPos == std::string::npos || clPos > headerEnd) break;
            size_t len = std::stoul(raw.substr(clPos + 15));
            if (raw.size() - headerEnd >= len) break;
        }
        if (raw.size() > 65536) break; // safety cap
    }
    if (!raw.empty()) {
        HttpRequest req = parseRequest(raw);
        handleRequest(clientFd, req);
    }
    close(clientFd);
}

int main() {
    seedChennaiHubs();

#ifdef ENABLE_GOOGLE_MAPS
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const char* key = std::getenv("GOOGLE_MAPS_API_KEY");
    std::cout << "[SmartRoute AI] Google Maps mode compiled in. API key "
              << (key && std::strlen(key) ? "found." : "NOT set — falling back to estimates.") << "\n";
#else
    std::cout << "[SmartRoute AI] Running in estimate mode (Haversine distance + mode speed).\n"
              << "[SmartRoute AI] Rebuild with -DENABLE_GOOGLE_MAPS -lcurl for live Google Maps travel times.\n";
#endif

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(serverFd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(serverFd, 32) < 0) { perror("listen"); return 1; }

    std::cout << "[SmartRoute AI] Backend listening on http://localhost:8080\n";

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) continue;
        std::thread(clientThread, clientFd).detach();
    }
    return 0;
}
