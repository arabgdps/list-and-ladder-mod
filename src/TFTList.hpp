#pragma once
#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>

struct ListRecord {
    std::string user;
    int percent = 0;
};

struct ListEntry {
    int id = 0;
    std::string name;
    std::string verifier;
    std::string creator;            // "" if the site doesn't provide one
    int percentToQualify = 100;
    std::vector<ListRecord> records;

    // The tier shown on the website's level page ("Rank: Aquamarine"). Either may be missing.
    std::string siteTier;     // tier name as written on the site
    int siteTierNum = 0;      // tier number, 0 = not provided

    // Geometry Dash Demon Ladder (gdladder.com) data, -1 = not rated / unknown
    double rating = -1;       // community tier rating, e.g. 12.37
    double enjoyment = -1;
    int ratingCount = 0;
};

// Tier names used by the Fish Tank Demon List. A level's GDDL rating is rounded to a
// tier number, and the tier name is the highest threshold that number reaches:
//   Obsidian 16+, Aquamarine 15, Amethyst 13-14, Emerald 12, Ruby 11, Sapphire 10,
//   Diamond 9, Platinum 7-8, Gold 5-6, Silver 4, Iron 3, Bronze 2, Copper 1
struct TierInfo {
    bool valid = false;
    int tier = 0;           // tier number (approximate if exact == false, only used for sorting)
    bool exact = false;     // true if the tier number is really known
    std::string name;
    cocos2d::ccColor3B color = {255, 255, 255};
};

// rating < 0 (unrated) gives valid = false
TierInfo tierInfoFor(double rating);

// The tier to show for a level: the website's own tier if it has one,
// otherwise the one worked out from the Demon Ladder rating.
TierInfo tierFor(ListEntry const& e);

// One line in a player's profile on the leaderboard.
struct ScoreItem {
    int rank = 0;           // rank of the level on the list
    std::string level;
    int percent = 100;
    double score = 0;
};

struct PlayerScore {
    std::string user;
    double total = 0;
    std::vector<ScoreItem> verified;
    std::vector<ScoreItem> completed;
    std::vector<ScoreItem> progressed;
};

// Same scoring the website uses (rank 1 = 200 points, falling off with rank).
double levelScore(int rank, double percent, double minPercent);

// Builds the player leaderboard from the level list (rank = index + 1).
std::vector<PlayerScore> computeLeaderboard(std::vector<ListEntry> const& entries);

// Downloads the list from the website (on a background thread) and caches it
// in saved values. No coroutines / async templates are used on purpose.
class TFTList {
public:
    static TFTList* get();

    // Cached list (rank = index + 1). Safe to call anytime, works offline.
    std::vector<ListEntry> cached() const;

    // Re-downloads the index and every level file (records can change without the
    // index changing, so we always check everything). Only reports "changed"
    // if the downloaded data differs from the cache.
    void refresh();

    // Looks up the Demon Ladder rating of ANY level id (used on the level page).
    // Cached for the session. cb runs on the main thread with the rating,
    // or -1 if the level isn't on the ladder / couldn't be reached.
    void requestRating(int levelId, std::function<void(double)> cb);

    bool isBusy() const { return m_busy; }

    // Called (on the main thread) when a refresh finishes. Pass nullptr to clear.
    void setOnFinished(std::function<void(bool changed)> cb) { m_onFinished = std::move(cb); }

private:
    void finish(bool changed);

    bool m_busy = false;
    std::unordered_map<int, double> m_ratingCache;
    std::function<void(bool)> m_onFinished;
};
