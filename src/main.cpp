#include <Geode/Geode.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <algorithm>
#include <cmath>
#include "TFTList.hpp"

using namespace geode::prelude;

namespace {
    constexpr int ROWS_PER_PAGE = 5;

    enum class SortMode { Rank, Name, Creator, Verifier, Tier };

    const ccColor3B CYAN  = {0, 255, 225};
    const ccColor3B GOLD  = {255, 200, 50};
    const ccColor3B WHITE = {255, 255, 255};
    const ccColor3B PANEL = {8, 14, 40};
    const ccColor3B ROW   = {20, 50, 115};
    const ccColor3B ROW_1 = {0, 120, 170};

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    // 175.003 -> "175.003", 200 -> "200"
    std::string trimNum(double v) {
        auto s = fmt::format("{:.3f}", v);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }

    // 2387.689 -> "2,387.689", 200 -> "200.000" (same style as the website's leaderboard)
    std::string commaNum(double v) {
        auto s = fmt::format("{:.3f}", v);
        auto dot = s.find('.');
        std::string ip = s.substr(0, dot), fp = s.substr(dot);
        std::string out;
        for (size_t i = 0; i < ip.size(); i++) {
            if (i > 0 && (ip.size() - i) % 3 == 0) out += ',';
            out += ip[i];
        }
        return out + fp;
    }

    // Places a scrollbar so its visible box is centered on (cx, cy), whatever anchor
    // the Scrollbar uses internally. Measures where it really ends up and corrects it.
    void centerBar(CCNode* bar, float cx, float cy) {
        bar->setPosition({cx, cy});
        auto box = bar->boundingBox();
        bar->setPosition({
            cx + (cx - (box.origin.x + box.size.width / 2)),
            cy + (cy - (box.origin.y + box.size.height / 2))
        });
    }

    // Sort key: harder tier first, GDDL rating breaks ties, unrated last.
    double tierKey(ListEntry const& e) {
        auto t = tierFor(e);
        if (!t.valid) return -1;
        return t.tier + (e.rating >= 0 ? e.rating / 1000.0 : 0.0);
    }

    // The Demon Ladder tier box on the level page: a coloured square with the tier
    // number, to the left of the difficulty face, and the tier name underneath.
    void addTierBox(LevelInfoLayer* layer, TierInfo const& t) {
        auto diff = layer->m_difficultySprite;
        if (!diff || !diff->getParent() || !t.valid) return;
        auto id = "tier-box"_spr;
        if (layer->getChildByIDRecursive(id)) return;

        auto box = CCNode::create();
        box->setID(id);

        auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        bg->setContentSize({28.f, 28.f});
        bg->setColor(t.color);
        box->addChild(bg);

        if (t.exact) {
            auto num = CCLabelBMFont::create(fmt::format("{}", t.tier).c_str(), "bigFont.fnt");
            num->limitLabelWidth(22.f, 0.6f, 0.2f);
            box->addChild(num);
        }

        auto name = CCLabelBMFont::create(t.name.c_str(), "chatFont.fnt");
        name->limitLabelWidth(40.f, 0.5f, 0.2f);
        name->setColor(t.color);
        name->setPosition({0.f, -21.f});
        box->addChild(name);

        box->setPosition(diff->getPosition() + CCPoint{-40.f, 2.f});
        box->setZOrder(diff->getZOrder());
        diff->getParent()->addChild(box);
    }

    CCScale9Sprite* makePanel(CCSize size, ccColor3B color, GLubyte opacity) {
        auto spr = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        spr->setColor(color);
        spr->setOpacity(opacity);
        spr->setContentSize(size);
        return spr;
    }

    CCLabelBMFont* makeLabel(char const* text, char const* font, float maxWidth, float scale, ccColor3B color = {255, 255, 255}) {
        auto lbl = CCLabelBMFont::create(text, font);
        lbl->setColor(color);
        lbl->limitLabelWidth(maxWidth, scale, 0.1f);
        return lbl;
    }
}

class TFTPopup : public Popup {
protected:
    // layout
    static constexpr float W = 440.f;
    static constexpr float H = 290.f;
    static constexpr float LEFT = 12.f, RIGHT = 425.f;
    static constexpr float LIST_X = 115.f, LIST_W = 310.f;
    static constexpr float PANEL_Y = 38.f, PANEL_H = 176.f;
    static constexpr float ROW_H = 27.f;

    // state
    std::vector<ListEntry> m_all;
    std::vector<std::pair<int, ListEntry>> m_view; // (rank, entry) after filter + sort
    std::vector<PlayerScore> m_players;
    int m_page = 0;
    int m_tab = 0; // 0 = list, 1 = leaderboard
    int m_selPlayer = 0;
    std::string m_search;
    bool m_onlyWithId = false;
    SortMode m_sort = SortMode::Rank;

    // widgets
    CCLabelBMFont* m_subtitle = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    ButtonSprite* m_tabListSpr = nullptr;
    ButtonSprite* m_tabBoardSpr = nullptr;
    ButtonSprite* m_sortSpr = nullptr;
    std::vector<CCNode*> m_listUI;   // everything that belongs to the List tab
    std::vector<CCNode*> m_rowNodes; // rebuilt every page change
    CCNode* m_boardRoot = nullptr;   // built when the Leaderboard tab is opened
    ScrollLayer* m_detailScroll = nullptr;
    std::vector<CCScale9Sprite*> m_playerBgs;

    bool init() {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        this->setTitle("The Fish Tank Demon List & Demon Ladder", "bigFont.fnt", 0.75f, 20.f);
        m_title->limitLabelWidth(W - 100.f, 0.75f, 0.2f);
        m_title->setColor(CYAN);

        m_subtitle = CCLabelBMFont::create("", "chatFont.fnt");
        m_subtitle->setScale(0.7f);
        m_subtitle->setPosition({W / 2, H - 41.f});
        m_mainLayer->addChild(m_subtitle);

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(0.5f);
        m_status->setOpacity(160);
        m_status->setAnchorPoint({1, 0.5f});
        m_status->setPosition({W - 14.f, H - 41.f});
        m_mainLayer->addChild(m_status);

        this->buildTabs();
        this->buildListPanel();
        this->buildFilterPanel();
        this->buildFooter();

        this->reload();
        this->setTab(0);
        this->startRefresh();
        return true;
    }

    // ---------- building ----------

    void buildTabs() {
        float y = H - 61.f;

        m_tabListSpr = ButtonSprite::create("List", 100, true, "bigFont.fnt", "GJ_button_01.png", 22.f, 0.5f);
        auto listBtn = CCMenuItemExt::createSpriteExtra(m_tabListSpr, [this](auto) { this->setTab(0); });
        listBtn->setPosition({W / 2 - 58.f, y});
        m_buttonMenu->addChild(listBtn);

        m_tabBoardSpr = ButtonSprite::create("Leaderboard", 100, true, "bigFont.fnt", "GJ_button_04.png", 22.f, 0.5f);
        auto boardBtn = CCMenuItemExt::createSpriteExtra(m_tabBoardSpr, [this](auto) { this->setTab(1); });
        boardBtn->setPosition({W / 2 + 58.f, y});
        m_buttonMenu->addChild(boardBtn);
    }

    void buildListPanel() {
        auto panel = makePanel({LIST_W, PANEL_H}, PANEL, 150);
        panel->setPosition({LIST_X + LIST_W / 2, PANEL_Y + PANEL_H / 2});
        m_mainLayer->addChild(panel);
        m_listUI.push_back(panel);

        float hy = PANEL_Y + PANEL_H - 11.f;
        auto head = makePanel({LIST_W - 8.f, 16.f}, {40, 20, 90}, 200);
        head->setPosition({LIST_X + LIST_W / 2, hy});
        m_mainLayer->addChild(head);
        m_listUI.push_back(head);

        struct Col { char const* text; float x; float w; bool left; };
        for (auto c : { Col{"Rank", 20.f, 24.f, false}, Col{"Level Name", 32.f, 70.f, true}, Col{"Creator", 150.f, 42.f, false},
                        Col{"Tier", 194.f, 40.f, false}, Col{"Points", 238.f, 34.f, false}, Col{"Verifier", 280.f, 38.f, false} }) {
            auto lbl = makeLabel(c.text, "goldFont.fnt", c.w, 0.4f);
            if (c.left) lbl->setAnchorPoint({0, 0.5f});
            lbl->setPosition({LIST_X + c.x, hy});
            m_mainLayer->addChild(lbl);
            m_listUI.push_back(lbl);
        }
    }

    void buildFilterPanel() {
        float px = LEFT, pw = 92.f;
        float cx = px + pw / 2;

        auto panel = makePanel({pw, PANEL_H}, PANEL, 150);
        panel->setPosition({cx, PANEL_Y + PANEL_H / 2});
        m_mainLayer->addChild(panel);
        m_listUI.push_back(panel);

        auto addTitle = [&](char const* text, float y) {
            auto lbl = makeLabel(text, "goldFont.fnt", pw - 10.f, 0.45f);
            lbl->setPosition({cx, y});
            m_mainLayer->addChild(lbl);
            m_listUI.push_back(lbl);
        };

        // Filter by
        addTitle("Filter By", 198.f);
        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(TFTPopup::onFilterToggle), 0.45f);
        toggler->setPosition({px + 17.f, 177.f});
        m_buttonMenu->addChild(toggler);
        m_listUI.push_back(toggler);
        auto idLbl = makeLabel("Has ID", "bigFont.fnt", 48.f, 0.35f);
        idLbl->setAnchorPoint({0, 0.5f});
        idLbl->setPosition({px + 32.f, 177.f});
        m_mainLayer->addChild(idLbl);
        m_listUI.push_back(idLbl);

        // Search
        addTitle("Search", 148.f);
        auto input = TextInput::create(46.f, "Name", "chatFont.fnt");
        input->setPosition({px + 29.f, 127.f});
        input->setMaxCharCount(24);
        input->setCallback([this](std::string const& text) {
            m_search = text;
            m_page = 0;
            this->rebuildView();
        });
        m_mainLayer->addChild(input);
        m_listUI.push_back(input);

        auto findSpr = ButtonSprite::create("Find", 32, true, "goldFont.fnt", "GJ_button_01.png", 18.f, 0.4f);
        auto findBtn = CCMenuItemExt::createSpriteExtra(findSpr, [this](auto) { this->rebuildView(); });
        findBtn->setPosition({px + 73.f, 127.f});
        m_buttonMenu->addChild(findBtn);
        m_listUI.push_back(findBtn);

        // Sort by
        addTitle("Sort By", 98.f);
        m_sortSpr = ButtonSprite::create("Rank", 70, true, "bigFont.fnt", "GJ_button_02.png", 20.f, 0.4f);
        auto sortBtn = CCMenuItemExt::createSpriteExtra(m_sortSpr, [this](auto) {
            m_sort = static_cast<SortMode>((static_cast<int>(m_sort) + 1) % 5);
            m_page = 0;
            this->updateSortLabel();
            this->rebuildView();
        });
        sortBtn->setPosition({cx, 77.f});
        m_buttonMenu->addChild(sortBtn);
        m_listUI.push_back(sortBtn);
    }

    void buildFooter() {
        float y = 20.f;

        // Close (bottom-left, under the filter panel)
        auto closeSpr = ButtonSprite::create("Close", 70, true, "bigFont.fnt", "GJ_button_05.png", 20.f, 0.45f);
        auto closeBtn = CCMenuItemExt::createSpriteExtra(closeSpr, [this](auto) { this->onClose(nullptr); });
        closeBtn->setPosition({LEFT + 46.f, y});
        m_buttonMenu->addChild(closeBtn);

        // Pagination (List tab only)
        float pageCx = LIST_X + LIST_W / 2 - 25.f;
        auto leftSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        leftSpr->setScale(0.5f);
        auto leftBtn = CCMenuItemExt::createSpriteExtra(leftSpr, [this](auto) { this->changePage(-1); });
        leftBtn->setPosition({pageCx - 50.f, y});
        m_buttonMenu->addChild(leftBtn);
        m_listUI.push_back(leftBtn);

        m_pageLabel = CCLabelBMFont::create("Page 1/1", "bigFont.fnt");
        m_pageLabel->setScale(0.4f);
        m_pageLabel->setPosition({pageCx, y});
        m_mainLayer->addChild(m_pageLabel);
        m_listUI.push_back(m_pageLabel);

        auto rightSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        rightSpr->setScale(0.5f);
        rightSpr->setFlipX(true);
        auto rightBtn = CCMenuItemExt::createSpriteExtra(rightSpr, [this](auto) { this->changePage(1); });
        rightBtn->setPosition({pageCx + 50.f, y});
        m_buttonMenu->addChild(rightBtn);
        m_listUI.push_back(rightBtn);

        // Refresh (both tabs)
        auto refreshSpr = ButtonSprite::create("Refresh", 80, true, "bigFont.fnt", "GJ_button_02.png", 20.f, 0.45f);
        auto refreshBtn = CCMenuItemExt::createSpriteExtra(refreshSpr, [this](auto) { this->startRefresh(); });
        refreshBtn->setPosition({RIGHT - 50.f, y});
        m_buttonMenu->addChild(refreshBtn);
    }

    // ---------- leaderboard tab ----------

    void buildBoard() {
        this->closeBoard();
        m_boardRoot = CCNode::create();
        m_mainLayer->addChild(m_boardRoot);

        m_players = computeLeaderboard(m_all);
        m_selPlayer = std::clamp(m_selPlayer, 0, std::max(0, static_cast<int>(m_players.size()) - 1));

        float leftW = 160.f, gap = 6.f;
        float rightX = LEFT + leftW + gap;
        float rightW = RIGHT - rightX;

        auto leftPanel = makePanel({leftW, PANEL_H}, PANEL, 150);
        leftPanel->setPosition({LEFT + leftW / 2, PANEL_Y + PANEL_H / 2});
        m_boardRoot->addChild(leftPanel);

        auto rightPanel = makePanel({rightW, PANEL_H}, PANEL, 150);
        rightPanel->setPosition({rightX + rightW / 2, PANEL_Y + PANEL_H / 2});
        m_boardRoot->addChild(rightPanel);

        if (m_players.empty()) {
            auto lbl = CCLabelBMFont::create("No data yet - hit Refresh", "bigFont.fnt");
            lbl->setScale(0.45f);
            lbl->setPosition({W / 2, PANEL_Y + PANEL_H / 2});
            m_boardRoot->addChild(lbl);
            return;
        }

        // players (left)
        auto scroll = ScrollLayer::create({leftW - 18.f, PANEL_H - 8.f});
        scroll->setPosition({LEFT + 4.f, PANEL_Y + 4.f});
        m_boardRoot->addChild(scroll);

        auto bar = Scrollbar::create(scroll);
        m_boardRoot->addChild(bar);
        centerBar(bar, LEFT + leftW - 11.f, PANEL_Y + PANEL_H / 2);

        auto content = scroll->m_contentLayer;
        float cw = scroll->getContentSize().width;
        float rowH = 24.f;
        float ch = std::max(scroll->getContentSize().height, m_players.size() * rowH);
        content->setContentSize({cw, ch});

        m_playerBgs.clear();
        for (size_t i = 0; i < m_players.size(); i++) {
            auto& p = m_players[i];
            auto node = CCNode::create();
            node->setContentSize({cw - 4.f, rowH - 3.f});
            node->setAnchorPoint({0.5f, 0.5f});

            auto bg = makePanel({cw - 4.f, rowH - 3.f}, ROW_1, static_cast<int>(i) == m_selPlayer ? 220 : 0);
            bg->setPosition({(cw - 4.f) / 2, (rowH - 3.f) / 2});
            node->addChild(bg);
            m_playerBgs.push_back(bg);

            float cy = (rowH - 3.f) / 2;
            auto rank = makeLabel(fmt::format("#{}", i + 1).c_str(), "chatFont.fnt", 22.f, 0.6f);
            rank->setAnchorPoint({1, 0.5f});
            rank->setPosition({26.f, cy});
            node->addChild(rank);

            auto score = makeLabel(commaNum(p.total).c_str(), "chatFont.fnt", 52.f, 0.6f);
            score->setAnchorPoint({1, 0.5f});
            score->setPosition({84.f, cy});
            node->addChild(score);

            auto name = makeLabel(p.user.c_str(), "chatFont.fnt", cw - 4.f - 94.f, 0.65f);
            name->setAnchorPoint({0, 0.5f});
            name->setPosition({92.f, cy});
            node->addChild(name);

            auto btn = CCMenuItemExt::createSpriteExtra(node, [this, i](auto) {
                m_selPlayer = static_cast<int>(i);
                for (size_t k = 0; k < m_playerBgs.size(); k++) {
                    m_playerBgs[k]->setOpacity(k == i ? 220 : 0);
                }
                this->showPlayerDetail();
            });
            btn->m_scaleMultiplier = 1.0f;
            auto menu = CCMenu::create();
            menu->addChild(btn);
            menu->setPosition({cw / 2, ch - rowH / 2 - i * rowH});
            content->addChild(menu);
        }
        scroll->moveToTop();

        // details (right)
        m_detailScroll = ScrollLayer::create({rightW - 18.f, PANEL_H - 8.f});
        m_detailScroll->setPosition({rightX + 4.f, PANEL_Y + 4.f});
        m_boardRoot->addChild(m_detailScroll);

        auto detailBar = Scrollbar::create(m_detailScroll);
        m_boardRoot->addChild(detailBar);
        centerBar(detailBar, rightX + rightW - 11.f, PANEL_Y + PANEL_H / 2);
        this->showPlayerDetail();
    }

    void showPlayerDetail() {
        if (!m_detailScroll || m_players.empty()) return;
        auto content = m_detailScroll->m_contentLayer;
        content->removeAllChildren();

        auto& p = m_players[m_selPlayer];
        float w = m_detailScroll->getContentSize().width;
        float viewH = m_detailScroll->getContentSize().height;

        int sections = (p.verified.empty() ? 0 : 1) + (p.completed.empty() ? 0 : 1) + (p.progressed.empty() ? 0 : 1);
        int items = static_cast<int>(p.verified.size() + p.completed.size() + p.progressed.size());
        float h = std::max(viewH, 34.f + 22.f + sections * 26.f + items * 17.f + 8.f);
        content->setContentSize({w, h});

        float y = h - 4.f;

        auto title = CCLabelBMFont::create(fmt::format("#{} {}", m_selPlayer + 1, p.user).c_str(), "chatFont.fnt");
        title->limitLabelWidth(w - 12.f, 1.4f, 0.5f);
        title->setAnchorPoint({0, 1});
        title->setPosition({6.f, y});
        content->addChild(title);
        y -= 34.f;

        auto total = CCLabelBMFont::create(trimNum(p.total).c_str(), "chatFont.fnt");
        total->setScale(0.8f);
        total->setAnchorPoint({0, 1});
        total->setPosition({6.f, y});
        content->addChild(total);
        y -= 22.f;

        auto addSection = [&](std::string const& heading, std::vector<ScoreItem> const& list, bool showPercent) {
            if (list.empty()) return;
            auto head = CCLabelBMFont::create(fmt::format("{} ({})", heading, list.size()).c_str(), "chatFont.fnt");
            head->setScale(1.0f);
            head->setColor(GOLD);
            head->setAnchorPoint({0, 1});
            head->setPosition({6.f, y});
            content->addChild(head);
            y -= 26.f;

            for (auto& s : list) {
                float cy = y - 8.5f;
                auto rank = CCLabelBMFont::create(fmt::format("#{}", s.rank).c_str(), "chatFont.fnt");
                rank->setScale(0.65f);
                rank->setAnchorPoint({0, 0.5f});
                rank->setPosition({6.f, cy});
                content->addChild(rank);

                auto text = showPercent ? fmt::format("{} ({}%)", s.level, s.percent) : s.level;
                auto lvl = makeLabel(text.c_str(), "chatFont.fnt", w - 100.f, 0.65f);
                lvl->setAnchorPoint({0, 0.5f});
                lvl->setPosition({34.f, cy});
                content->addChild(lvl);

                auto score = CCLabelBMFont::create(fmt::format("+{}", trimNum(s.score)).c_str(), "chatFont.fnt");
                score->setScale(0.65f);
                score->setAnchorPoint({1, 0.5f});
                score->setPosition({w - 8.f, cy});
                content->addChild(score);
                y -= 17.f;
            }
        };
        addSection("Verified", p.verified, false);
        addSection("Completed", p.completed, false);
        addSection("Progressed", p.progressed, true);

        m_detailScroll->moveToTop();
    }

    void closeBoard() {
        if (m_boardRoot) {
            m_boardRoot->removeFromParent();
            m_boardRoot = nullptr;
        }
        m_detailScroll = nullptr;
        m_playerBgs.clear();
    }

    // ---------- logic ----------

    static void setShown(CCNode* n, bool shown) {
        n->setVisible(shown);
        if (auto item = typeinfo_cast<CCMenuItem*>(n)) item->setEnabled(shown);
        if (auto input = typeinfo_cast<TextInput*>(n)) input->setEnabled(shown);
    }

    void setTab(int tab) {
        m_tab = tab;
        m_tabListSpr->updateBGImage(tab == 0 ? "GJ_button_01.png" : "GJ_button_04.png");
        m_tabBoardSpr->updateBGImage(tab == 1 ? "GJ_button_01.png" : "GJ_button_04.png");

        for (auto n : m_listUI) setShown(n, tab == 0);
        for (auto n : m_rowNodes) setShown(n, tab == 0);

        if (tab == 1) this->buildBoard();

        else this->closeBoard();
    }

    void onFilterToggle(CCObject* sender) {
        // the toggler flips *after* the callback fires, so isToggled() is the old value here
        m_onlyWithId = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        m_page = 0;
        this->rebuildView();
    }

    void updateSortLabel() {
        char const* names[] = {"Rank", "Name", "Creator", "Verifier", "Tier"};
        m_sortSpr->setString(names[static_cast<int>(m_sort)]);
    }

    int pageCount() const {
        return std::max(1, (static_cast<int>(m_view.size()) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
    }

    void changePage(int delta) {
        int next = std::clamp(m_page + delta, 0, this->pageCount() - 1);
        if (next == m_page) return;
        m_page = next;
        this->showPage();
    }

    void reload() {
        m_all = TFTList::get()->cached();
        m_subtitle->setString(fmt::format("Top {} Demons", m_all.size()).c_str());
        this->rebuildView();
        if (m_tab == 1) this->buildBoard();
    }

    void rebuildView() {
        m_view.clear();
        auto needle = lower(m_search);
        int rank = 1;
        for (auto& e : m_all) {
            bool ok = true;
            if (m_onlyWithId && e.id <= 0) ok = false;
            if (ok && !needle.empty()) {
                ok = lower(e.name).find(needle) != std::string::npos
                  || lower(e.creator).find(needle) != std::string::npos
                  || lower(e.verifier).find(needle) != std::string::npos;
            }
            if (ok) m_view.emplace_back(rank, e);
            rank++;
        }

        switch (m_sort) {
            case SortMode::Name:
                std::stable_sort(m_view.begin(), m_view.end(), [](auto& a, auto& b) { return lower(a.second.name) < lower(b.second.name); });
                break;
            case SortMode::Creator:
                std::stable_sort(m_view.begin(), m_view.end(), [](auto& a, auto& b) { return lower(a.second.creator) < lower(b.second.creator); });
                break;
            case SortMode::Tier: // hardest first, unrated last
                std::stable_sort(m_view.begin(), m_view.end(), [](auto& a, auto& b) { return tierKey(a.second) > tierKey(b.second); });
                break;
            case SortMode::Verifier:
                std::stable_sort(m_view.begin(), m_view.end(), [](auto& a, auto& b) { return lower(a.second.verifier) < lower(b.second.verifier); });
                break;
            default: break;
        }

        m_page = std::clamp(m_page, 0, this->pageCount() - 1);
        this->showPage();
    }

    void showPage() {
        for (auto n : m_rowNodes) n->removeFromParent();
        m_rowNodes.clear();

        m_pageLabel->setString(fmt::format("Page {}/{}", m_page + 1, this->pageCount()).c_str());

        if (m_view.empty()) {
            auto lbl = CCLabelBMFont::create(m_all.empty() ? "No data yet - hit Refresh" : "No results", "bigFont.fnt");
            lbl->setScale(0.45f);
            lbl->setPosition({LIST_X + LIST_W / 2, PANEL_Y + PANEL_H / 2 - 10.f});
            m_mainLayer->addChild(lbl);
            lbl->setVisible(m_tab == 0);
            m_rowNodes.push_back(lbl);
            return;
        }

        float firstY = PANEL_Y + PANEL_H - 22.f - ROW_H / 2 - 3.f;
        int start = m_page * ROWS_PER_PAGE;
        for (int i = 0; i < ROWS_PER_PAGE && start + i < static_cast<int>(m_view.size()); i++) {
            auto& [rank, entry] = m_view[start + i];
            auto row = this->makeRow(rank, entry);
            row->setPosition({LIST_X + LIST_W / 2, firstY - i * (ROW_H + 3.f)});
            if (entry.id > 0) {
                m_buttonMenu->addChild(row); // clickable
            } else {
                m_mainLayer->addChild(row);
            }
            setShown(row, m_tab == 0);
            m_rowNodes.push_back(row);
        }
    }

    CCNode* makeRow(int rank, ListEntry const& e) {
        float w = LIST_W - 8.f;
        float ox = 4.f; // row is a little narrower than the list panel; keeps columns lined up with the header
        float cy = ROW_H / 2;

        auto node = CCNode::create();
        node->setContentSize({w, ROW_H});
        node->setAnchorPoint({0.5f, 0.5f});

        auto bg = makePanel({w, ROW_H}, rank == 1 ? ROW_1 : ROW, rank == 1 ? 210 : 175);
        bg->setPosition({w / 2, cy});
        node->addChild(bg);

        // rank is shown as a plain number (the "#" glyph in bigFont looks like "No")
        auto rankLbl = makeLabel(fmt::format("{}", rank).c_str(), "bigFont.fnt", 22.f, 0.5f, rank <= 3 ? GOLD : WHITE);
        rankLbl->setPosition({20.f - ox, cy});
        node->addChild(rankLbl);

        auto name = makeLabel(e.name.c_str(), "bigFont.fnt", 92.f, 0.45f);
        name->setAnchorPoint({0, 0.5f});
        name->setPosition({32.f - ox, cy});
        node->addChild(name);

        auto creator = makeLabel(e.creator.empty() ? "N/A" : e.creator.c_str(), "bigFont.fnt", 42.f, 0.35f);
        creator->setPosition({150.f - ox, cy});
        node->addChild(creator);

        // Demon Ladder tier: name on top, number underneath
        auto tier = tierFor(e);
        if (tier.valid) {
            auto tName = makeLabel(tier.name.c_str(), "chatFont.fnt", 42.f, 0.55f, tier.color);
            tName->setPosition({194.f - ox, cy + 5.f});
            node->addChild(tName);
            if (tier.exact) {
                auto tNum = makeLabel(fmt::format("Tier {}", tier.tier).c_str(), "chatFont.fnt", 42.f, 0.5f);
                tNum->setPosition({194.f - ox, cy - 6.f});
                node->addChild(tNum);
            } else {
                tName->setPositionY(cy); // no number to show underneath
            }
        } else {
            auto none = makeLabel("-", "chatFont.fnt", 42.f, 0.6f);
            none->setPosition({194.f - ox, cy});
            node->addChild(none);
        }

        // points = what the level is worth on the website's scoring
        auto points = makeLabel(trimNum(levelScore(rank, 100, e.percentToQualify)).c_str(), "bigFont.fnt", 34.f, 0.4f, GOLD);
        points->setPosition({238.f - ox, cy});
        node->addChild(points);

        auto verifier = makeLabel(e.verifier.empty() ? "N/A" : e.verifier.c_str(), "bigFont.fnt", 38.f, 0.35f);
        verifier->setPosition({280.f - ox, cy});
        node->addChild(verifier);

        if (e.id <= 0) return node;

        // Clickable row: open the level by ID
        auto btn = CCMenuItemExt::createSpriteExtra(node, [id = e.id](auto) {
            auto search = GJSearchObject::create(SearchType::Search, std::to_string(id));
            CCDirector::get()->pushScene(
                CCTransitionFade::create(0.5f, LevelBrowserLayer::scene(search))
            );
        });
        btn->m_scaleMultiplier = 1.03f;
        return btn;
    }

    void startRefresh() {
        auto list = TFTList::get();
        if (list->isBusy()) return;
        m_status->setString("Checking for updates...");
        list->setOnFinished([this](bool changed) {
            m_status->setString(changed ? "Updated!" : "Up to date");
            if (changed) this->reload();
        });
        list->refresh();
    }

    ~TFTPopup() override {
        TFTList::get()->setOnFinished(nullptr);
    }

public:
    static TFTPopup* create() {
        auto ret = new TFTPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(TFTMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // Check for a new version of the list once per game launch.
        static bool checked = false;
        if (!checked) {
            checked = true;
            TFTList::get()->refresh();
        }

        auto spr = CircleButtonSprite::createWithSprite(
            "tft-icon.png"_spr, 1.f, CircleBaseColor::Green, CircleBaseSize::MediumAlt
        );
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](auto) {
            TFTPopup::create()->show();
        });
        btn->setID("demonlist-button"_spr);

        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        }
        return true;
    }
};

// Shows the Demon Ladder tier next to the difficulty face on every demon's level page.
class $modify(TFTLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        int id = level->m_levelID;
        if (id <= 0) return true;

        // Levels on the Fish Tank list: use the list's own tier.
        for (auto& e : TFTList::get()->cached()) {
            if (e.id != id) continue;
            auto tier = tierFor(e);
            if (tier.valid) {
                addTierBox(this, tier);
                return true;
            }
            break;
        }

        // Any other demon: ask gdladder.com.
        Ref<LevelInfoLayer> self = this;
        TFTList::get()->requestRating(id, [self](double rating) {
            if (rating < 0) return;
            addTierBox(self, tierInfoFor(rating));
        });
        return true;
    }
};
