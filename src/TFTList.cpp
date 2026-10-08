#include "TFTList.hpp"
#include <matjson.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

using namespace geode::prelude;

// !!! If the site's data lives somewhere else, change this. It must end in "/".
// The mod expects:  <BASE_URL>_list.json  -> ["level_a", "level_b", ...]  (rank order)
//                   <BASE_URL>level_a.json -> { "id": 123, "name": "...", "author": "...",
//                                               "verifier": "...", "percentToQualify": 100,
//                                               "records": [ { "user": "...", "percent": 100 } ] }
static constexpr auto BASE_URL = "https://thefishtankdemonlist.jjchairy.workers.dev/data/";

namespace {
    struct FetchResult {
        bool ok = false;
        std::vector<ListEntry> entries;
    };

    // Blocking GET. Only ever called from the background thread.
    std::optional<std::string> httpGet(std::string const& url) {
        auto req = web::WebRequest();
        req.userAgent("FishTankDemonListMod");
        req.timeout(std::chrono::seconds(15));
        auto res = req.getSync(url);
        if (!res.ok()) return std::nullopt;
        auto body = res.string();
        if (!body) return std::nullopt;
        return body.unwrap();
    }

    // Looks for a creator in a few likely keys. Accepts a string or an array of strings.
    std::string readCreator(matjson::Value& v) {
        for (auto key : {"author", "creator", "creators", "authors"}) {
            if (!v.contains(key)) continue;
            if (auto s = v[key].asString()) {
                auto str = s.unwrap();
                if (!str.empty()) return str;
            }
            if (auto a = v[key].asArray()) {
                auto vec = a.unwrap();
                std::string joined;
                for (auto& x : vec) {
                    if (auto s = x.asString()) {
                        if (!joined.empty()) joined += ", ";
                        joined += s.unwrap();
                    }
                }
                if (!joined.empty()) return joined;
            }
        }
        return "";
    }

    // Reads a number that may be a JSON number or a numeric string. Returns -1 if absent/null.
    double readNum(matjson::Value& v, std::initializer_list<char const*> keys) {
        for (auto key : keys) {
            if (!v.contains(key)) continue;
            if (auto d = v[key].asDouble()) return d.unwrap();
            if (auto s = v[key].asString()) {
                try { return std::stod(s.unwrap()); } catch (...) {}
            }
        }
        return -1;
    }

    // The website's level page shows "Rank: <tier name>". We don't know which key the
    // data file uses, so look at a few likely ones. A name is any non-numeric string,
    // a number is a numeric value under a tier-like key.
    void readSiteTier(matjson::Value& v, ListEntry& e) {
        for (auto key : {"tier", "gddlTier", "tierNumber"}) {
            if (!v.contains(key)) continue;
            if (auto d = v[key].asDouble()) { e.siteTierNum = static_cast<int>(std::lround(d.unwrap())); break; }
            if (auto s = v[key].asString()) {
                try { e.siteTierNum = static_cast<int>(std::lround(std::stod(s.unwrap()))); break; } catch (...) {}
            }
        }
        for (auto key : {"tierName", "rankName", "rank", "tier", "gddlRank", "gddlTierName"}) {
            if (!v.contains(key)) continue;
            if (auto s = v[key].asString()) {
                auto str = s.unwrap();
                if (str.empty()) continue;
                bool numeric = false;
                try { size_t pos = 0; std::stod(str, &pos); numeric = (pos == str.size()); } catch (...) {}
                if (!numeric) { e.siteTier = str; break; }
            }
        }
        if (e.siteTierNum < 0) e.siteTierNum = 0;
    }

    // Asks gdladder.com for this level's community rating. Never fails the whole refresh:
    // on a network error the previously cached rating is kept.
    void fetchLadder(ListEntry& e, std::vector<ListEntry> const& old) {
        if (e.id <= 0) return;

        auto req = web::WebRequest();
        req.userAgent("FishTankDemonListMod");
        req.timeout(std::chrono::seconds(10));
        auto res = req.getSync(fmt::format("https://gdladder.com/api/level/{}", e.id));

        if (res.ok()) {
            if (auto body = res.string()) {
                if (auto j = matjson::parse(body.unwrap())) {
                    auto v = j.unwrap();
                    e.rating = readNum(v, {"Rating", "rating"});
                    e.enjoyment = readNum(v, {"Enjoyment", "enjoyment"});
                    e.ratingCount = static_cast<int>(std::max(0.0, readNum(v, {"RatingCount", "ratingCount"})));
                    return;
                }
            }
        }
        if (res.code() == 404) return; // level isn't on the ladder (yet)

        for (auto& o : old) { // keep what we had
            if (o.id == e.id) {
                e.rating = o.rating;
                e.enjoyment = o.enjoyment;
                e.ratingCount = o.ratingCount;
                return;
            }
        }
    }

    matjson::Value toJson(std::vector<ListEntry> const& entries) {
        matjson::Value arr = matjson::Value::array();
        for (auto& e : entries) {
            matjson::Value recs = matjson::Value::array();
            for (auto& r : e.records) {
                recs.push(matjson::makeObject({
                    {"user", r.user},
                    {"percent", r.percent},
                }));
            }
            arr.push(matjson::makeObject({
                {"id", e.id},
                {"name", e.name},
                {"verifier", e.verifier},
                {"creator", e.creator},
                {"qualify", e.percentToQualify},
                {"records", recs},
                {"siteTier", e.siteTier},
                {"siteTierNum", e.siteTierNum},
                {"rating", e.rating},
                {"enjoyment", e.enjoyment},
                {"votes", e.ratingCount},
            }));
        }
        return arr;
    }

    // Runs on the background thread. Must not touch GD / Mod saved values.
    FetchResult fetchAll(std::vector<ListEntry> const& old) {
        FetchResult out;

        auto indexBody = httpGet(std::string(BASE_URL) + "_list.json");
        if (!indexBody) return out;

        auto parsed = matjson::parse(*indexBody);
        if (!parsed) return out;
        auto arr = parsed.unwrap().asArray();
        if (!arr) return out;

        std::vector<std::string> files;
        for (auto& v : arr.unwrap()) {
            if (auto s = v.asString()) files.push_back(s.unwrap());
        }
        if (files.empty()) return out;

        for (auto& file : files) {
            // If any level fails to download we give up, so a flaky connection
            // can never overwrite a good cache with a half-empty list.
            auto body = httpGet(std::string(BASE_URL) + file + ".json");
            if (!body) return out;
            auto j = matjson::parse(*body);
            if (!j) return out;

            auto v = j.unwrap();
            ListEntry e;
            e.name = file; // fallback name
            e.id = static_cast<int>(v["id"].asInt().unwrapOr(0));
            auto name = v["name"].asString().unwrapOr("");
            if (!name.empty()) e.name = name;
            e.verifier = v["verifier"].asString().unwrapOr("");
            e.creator = readCreator(v);
            readSiteTier(v, e);
            e.percentToQualify = static_cast<int>(v["percentToQualify"].asDouble().unwrapOr(100));

            if (v.contains("records")) {
                if (auto recs = v["records"].asArray()) {
                    auto vec = recs.unwrap();
                    for (auto& r : vec) {
                        ListRecord rec;
                        rec.user = r["user"].asString().unwrapOr("");
                        rec.percent = static_cast<int>(r["percent"].asDouble().unwrapOr(0));
                        if (!rec.user.empty()) e.records.push_back(std::move(rec));
                    }
                }
            }
            fetchLadder(e, old);
            out.entries.push_back(std::move(e));
        }

        out.ok = true;
        return out;
    }

    std::string lowerStr(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    double round3(double x) { return std::round(x * 1000.0) / 1000.0; }
}

double levelScore(int rank, double percent, double minPercent) {
    if (rank > 150) return 0;
    if (rank > 75 && percent < 100) return 0;
    double s = (-24.9975 * std::pow(rank - 1, 0.4) + 200.0)
             * ((percent - (minPercent - 1)) / (100.0 - (minPercent - 1)));
    s = std::max(0.0, s);
    if (percent != 100) return round3(s - s / 3.0);
    return std::max(round3(s), 0.0);
}

std::vector<PlayerScore> computeLeaderboard(std::vector<ListEntry> const& entries) {
    std::vector<PlayerScore> players;

    // Names are matched case-insensitively, the first spelling seen is kept.
    auto indexOf = [&](std::string const& user) -> size_t {
        auto key = lowerStr(user);
        for (size_t i = 0; i < players.size(); i++) {
            if (lowerStr(players[i].user) == key) return i;
        }
        PlayerScore p;
        p.user = user;
        players.push_back(std::move(p));
        return players.size() - 1;
    };

    for (size_t i = 0; i < entries.size(); i++) {
        auto& e = entries[i];
        int rank = static_cast<int>(i) + 1;

        if (!e.verifier.empty()) {
            auto idx = indexOf(e.verifier);
            players[idx].verified.push_back({rank, e.name, 100, levelScore(rank, 100, e.percentToQualify)});
        }
        for (auto& r : e.records) {
            auto idx = indexOf(r.user);
            ScoreItem item{rank, e.name, r.percent, levelScore(rank, r.percent, e.percentToQualify)};
            if (r.percent == 100) players[idx].completed.push_back(item);
            else players[idx].progressed.push_back(item);
        }
    }

    for (auto& p : players) {
        double total = 0;
        for (auto& s : p.verified) total += s.score;
        for (auto& s : p.completed) total += s.score;
        for (auto& s : p.progressed) total += s.score;
        p.total = round3(total);
    }
    std::stable_sort(players.begin(), players.end(), [](auto& a, auto& b) { return a.total > b.total; });
    return players;
}

namespace {
    struct Band { int min; char const* name; cocos2d::ccColor3B color; };
    const Band BANDS[] = { // hardest -> easiest
        {16, "Obsidian",   {150, 110, 210}},
        {15, "Aquamarine", {127, 255, 212}},
        {13, "Amethyst",   {190, 110, 240}},
        {12, "Emerald",    {60, 225, 110}},
        {11, "Ruby",       {240, 50, 70}},
        {10, "Sapphire",   {70, 120, 255}},
        {9,  "Diamond",    {150, 235, 255}},
        {7,  "Platinum",   {215, 235, 240}},
        {5,  "Gold",       {255, 200, 50}},
        {4,  "Silver",     {205, 205, 215}},
        {3,  "Iron",       {150, 155, 165}},
        {2,  "Bronze",     {190, 125, 55}},
        {1,  "Copper",     {215, 105, 60}},
    };
}

TierInfo tierInfoFor(double rating) {
    TierInfo t;
    if (rating < 0) return t;
    t.tier = std::max(1, static_cast<int>(std::lround(rating)));
    t.valid = true;
    t.exact = true;
    for (auto& b : BANDS) {
        if (t.tier >= b.min) {
            t.name = b.name;
            t.color = b.color;
            break;
        }
    }
    return t;
}

TierInfo tierFor(ListEntry const& e) {
    // 1. the website's own tier name (what its level page shows as "Rank")
    if (!e.siteTier.empty()) {
        TierInfo t;
        t.valid = true;
        t.name = e.siteTier;
        auto key = lowerStr(e.siteTier);
        for (auto& b : BANDS) {
            if (lowerStr(b.name) == key) {
                t.name = b.name;
                t.color = b.color;
                t.tier = b.min;
                break;
            }
        }
        if (e.siteTierNum > 0) {
            t.tier = e.siteTierNum;
            t.exact = true;
        }
        return t;
    }
    // 2. the website's tier number
    if (e.siteTierNum > 0) return tierInfoFor(e.siteTierNum);
    // 3. fall back to the Demon Ladder rating
    return tierInfoFor(e.rating);
}

TFTList* TFTList::get() {
    static TFTList instance;
    return &instance;
}

std::vector<ListEntry> TFTList::cached() const {
    std::vector<ListEntry> out;
    auto saved = Mod::get()->getSavedValue<std::string>("cached-list-v2", "[]");
    auto parsed = matjson::parse(saved);
    if (!parsed) return out;
    auto arr = parsed.unwrap().asArray();
    if (!arr) return out;
    for (auto& v : arr.unwrap()) {
        ListEntry e;
        e.id = static_cast<int>(v["id"].asInt().unwrapOr(0));
        e.name = v["name"].asString().unwrapOr("?");
        e.verifier = v["verifier"].asString().unwrapOr("");
        e.creator = v["creator"].asString().unwrapOr("");
        e.percentToQualify = static_cast<int>(v["qualify"].asInt().unwrapOr(100));
        e.siteTier = v["siteTier"].asString().unwrapOr("");
        e.siteTierNum = static_cast<int>(v["siteTierNum"].asInt().unwrapOr(0));
        e.rating = v["rating"].asDouble().unwrapOr(-1);
        e.enjoyment = v["enjoyment"].asDouble().unwrapOr(-1);
        e.ratingCount = static_cast<int>(v["votes"].asInt().unwrapOr(0));
        if (auto recs = v["records"].asArray()) {
            auto vec = recs.unwrap();
            for (auto& r : vec) {
                ListRecord rec;
                rec.user = r["user"].asString().unwrapOr("");
                rec.percent = static_cast<int>(r["percent"].asInt().unwrapOr(0));
                if (!rec.user.empty()) e.records.push_back(std::move(rec));
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

void TFTList::refresh() {
    if (m_busy) return;
    m_busy = true;

    auto old = this->cached(); // read on the main thread, used to keep ratings if gdladder is unreachable
    std::thread([this, old = std::move(old)]() {
        auto result = fetchAll(old);

        // Back to the main thread to save + update the UI.
        queueInMainThread([this, result = std::move(result)]() {
            bool changed = false;
            if (result.ok) {
                auto dumped = toJson(result.entries).dump();
                auto old = Mod::get()->getSavedValue<std::string>("cached-list-v2", "");
                if (dumped != old) {
                    Mod::get()->setSavedValue<std::string>("cached-list-v2", dumped);
                    changed = true;
                }
            }
            this->finish(changed);
        });
    }).detach();
}

void TFTList::requestRating(int id, std::function<void(double)> cb) {
    if (id <= 0) { cb(-1); return; }
    auto it = m_ratingCache.find(id);
    if (it != m_ratingCache.end()) { cb(it->second); return; }

    std::thread([this, id, cb]() {
        double rating = -1;
        bool definitive = false; // false = network problem, don't remember the answer

        auto req = web::WebRequest();
        req.userAgent("FishTankDemonListMod");
        req.timeout(std::chrono::seconds(10));
        auto res = req.getSync(fmt::format("https://gdladder.com/api/level/{}", id));

        if (res.ok()) {
            if (auto body = res.string()) {
                if (auto j = matjson::parse(body.unwrap())) {
                    auto v = j.unwrap();
                    rating = readNum(v, {"Rating", "rating"});
                    definitive = true;
                }
            }
        } else if (res.code() == 404) {
            definitive = true; // not a ladder level
        }

        queueInMainThread([this, id, rating, definitive, cb]() {
            if (definitive) m_ratingCache[id] = rating;
            cb(rating);
        });
    }).detach();
}

void TFTList::finish(bool changed) {
    m_busy = false;
    if (m_onFinished) m_onFinished(changed);
}
