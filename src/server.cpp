#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>

#include "HlsManifest.h"
#include "Session.h"
#include "httplib.h"

extern "C" {
#include <curl/curl.h>
#include <libavutil/log.h>
}

namespace {

std::string g_webtorrent_url;
std::unordered_map<std::string, std::shared_ptr<fmp4::Session>> g_sessions;
std::mutex g_sessions_mutex;
std::atomic<int> g_next_session_id{0};

std::pair<int, std::shared_ptr<fmp4::Session>> CreateSession(const std::string& infohash,
                                                             int file_index = 0) {
    int session_id = g_next_session_id.fetch_add(1);
    std::string key = infohash + "_session-" + std::to_string(session_id);

    auto session = std::make_shared<fmp4::Session>(g_webtorrent_url, infohash,
                                                   file_index, session_id);

    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    g_sessions[key] = session;
    return {session_id, session};
}

std::shared_ptr<fmp4::Session> GetSession(const std::string& infohash, int session_id) {
    std::string key = infohash + "_session-" + std::to_string(session_id);
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto it = g_sessions.find(key);
    if (it == g_sessions.end()) return nullptr;
    it->second->Touch();
    return it->second;
}

void CleanupOldSessions(int max_age_minutes = 30) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto it = g_sessions.begin(); it != g_sessions.end(); ) {
        auto age_minutes = std::chrono::duration_cast<std::chrono::minutes>(
            now - it->second->GetLastActivity()).count();
        if (age_minutes > max_age_minutes) {
            std::cout << "[Cleanup] Removing session " << it->first
                      << " (inactive for " << age_minutes << " min)" << std::endl;
            it = g_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Suppress noisy codec-level warnings (truehd substream length mismatches,
    // etc.). Override with FFMPEG_LOG_LEVEL=32 to see AV_LOG_INFO messages.
    const char* log_level_env = std::getenv("FFMPEG_LOG_LEVEL");
    int log_level = log_level_env ? std::atoi(log_level_env) : AV_LOG_FATAL;
    av_log_set_level(log_level);

    g_webtorrent_url = std::getenv("WEBTORRENT_URL")
        ? std::getenv("WEBTORRENT_URL")
        : "http://localhost:8083";
    int port = std::getenv("PORT") ? std::atoi(std::getenv("PORT")) : 8084;

    std::cout << "[fmp4-packager] Starting on port " << port << std::endl;
    std::cout << "[fmp4-packager] WebTorrent URL: " << g_webtorrent_url << std::endl;

    httplib::Server svr;

    // Large segments (50-100 MB raw video bytes) take time to write back.
    svr.set_read_timeout(60, 0);
    svr.set_write_timeout(120, 0);

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("ok", "text/plain");
    });

    // GET /:infohash.m3u8 — manifest, creates a new Session.
    svr.Get(R"(/([a-fA-F0-9]{40})\.m3u8)", [](const httplib::Request& req,
                                              httplib::Response& res) {
        std::string infohash = req.matches[1];
        std::transform(infohash.begin(), infohash.end(), infohash.begin(), ::tolower);

        CleanupOldSessions(30);

        std::cout << "[GET] " << infohash << ".m3u8" << std::endl;

        try {
            auto [session_id, session] = CreateSession(infohash);
            auto keyframes = session->GetKeyframesMs();

            if (keyframes.size() < 2) {
                res.status = 503;
                res.set_content("Not enough keyframes yet", "text/plain");
                return;
            }

            std::string prefix = infohash + "/session-" + std::to_string(session_id) + "/";
            std::string manifest = fmp4::HlsManifest::Generate(keyframes, prefix);

            std::cout << "[GET] Created session-" << session_id << " for " << infohash
                      << std::endl;

            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(manifest, "application/vnd.apple.mpegurl");
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << std::endl;
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // GET /:infohash/session-:id/init.mp4 — fMP4 initialization segment.
    svr.Get(R"(/([a-fA-F0-9]{40})/session-(\d+)/init\.mp4)",
        [](const httplib::Request& req, httplib::Response& res) {
            std::string infohash = req.matches[1];
            std::transform(infohash.begin(), infohash.end(), infohash.begin(), ::tolower);
            int session_id = std::stoi(req.matches[2]);

            std::cout << "[GET] " << infohash << "/session-" << session_id << "/init.mp4"
                      << std::endl;

            try {
                auto session = GetSession(infohash, session_id);
                if (!session) {
                    res.status = 404;
                    res.set_content("Session not found", "text/plain");
                    return;
                }

                auto data = session->GenerateInitSegment();

                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(reinterpret_cast<const char*>(data.data()), data.size(),
                                "video/mp4");
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] " << e.what() << std::endl;
                res.status = 500;
                res.set_content(e.what(), "text/plain");
            }
        });

    // GET /:infohash/session-:id/:segment.m4s — media segment.
    svr.Get(R"(/([a-fA-F0-9]{40})/session-(\d+)/(\d+)\.m4s)",
        [](const httplib::Request& req, httplib::Response& res) {
            std::string infohash = req.matches[1];
            std::transform(infohash.begin(), infohash.end(), infohash.begin(), ::tolower);
            int session_id = std::stoi(req.matches[2]);
            int64_t segment_idx = std::stoll(req.matches[3]);

            // Header line for this segment request is emitted from
            // Session::BuildSegmentLocked along with the indented debug block
            // (see Session.cpp), so we don't print one here.

            try {
                auto session = GetSession(infohash, session_id);
                if (!session) {
                    res.status = 404;
                    res.set_content("Session not found", "text/plain");
                    return;
                }

                auto keyframes = session->GetKeyframesMs();
                if (segment_idx < 0
                    || segment_idx >= static_cast<int64_t>(keyframes.size()) - 1) {
                    res.status = 404;
                    res.set_content("Segment not found", "text/plain");
                    return;
                }

                int64_t start_ms = keyframes[segment_idx];
                int64_t end_ms   = keyframes[segment_idx + 1];

                if (std::getenv("DIODE_SEGMENT_LOG")) {
                    std::cerr << "[DIODE_SEGMENT_LOG] " << infohash << " seg=" << segment_idx
                              << " start_ms=" << start_ms << " end_ms=" << end_ms << std::endl;
                }

                auto data = session->GenerateSegment(segment_idx, start_ms, end_ms);

                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(reinterpret_cast<const char*>(data.data()), data.size(),
                                "video/mp4");
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] " << e.what() << std::endl;
                res.status = 500;
                res.set_content(e.what(), "text/plain");
            }
        });

    // CORS preflight.
    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Range");
        res.status = 204;
    });

    svr.listen("0.0.0.0", port);

    curl_global_cleanup();
    return 0;
}
