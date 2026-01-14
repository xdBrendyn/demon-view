#include <Geode/Geode.hpp>
#include <Geode/modify/ProfilePage.hpp>
#include <Geode/utils/cocos.hpp>
#include <cocos-ext.h>

#include <optional>
#include <string>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <unordered_map>
#include <cmath>
#include <array>
#include <vector>

#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/GJDifficultySprite.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/LevelSelectLayer.hpp>

using namespace geode::prelude;

namespace {

    static CCNode* findLargestPanelNode(CCNode* root) {
        if (!root) return nullptr;

        CCNode* best = nullptr;
        float bestArea = 0.f;

        std::function<void(CCNode*)> dfs = [&](CCNode* n) {
            if (!n) return;

            // Prefer Scale9 sprites (most GD panels are these)
            bool isPanel = typeinfo_cast<cocos2d::extension::CCScale9Sprite*>(n) != nullptr
                || typeinfo_cast<CCSprite*>(n) != nullptr;

            if (isPanel) {
                auto s = n->getContentSize();
                float area = s.width * s.height;

                // Only consider "real" sized nodes
                if (area > bestArea && s.width > 100.f && s.height > 100.f) {
                    bestArea = area;
                    best = n;
                }
            }

            if (auto children = n->getChildren()) {
                CCObject* obj = nullptr;
                CCARRAY_FOREACH(children, obj) {
                    if (auto child = typeinfo_cast<CCNode*>(obj)) {
                        dfs(child);
                    }
                }
            }
            };

        dfs(root);
        return best;
    }

    // Helper to get string representation of tier for the list row (Yellow text)
    static std::string getDemonTierString(int tier) {
        switch (tier) {
        case 1: return "Easy Demon";
        case 2: return "Medium Demon";
        case 3: return "Hard Demon";
        case 4: return "Insane Demon";
        case 5: return "Extreme Demon";
        default: return "Demon";
        }
    }

    // Helper for the icon label (Stacked text)
    static const char* getDemonTierIconText(int tier) {
        switch (tier) {
        case 1: return "EASY\nDEMON";
        case 2: return "MEDIUM\nDEMON";
        case 3: return "HARD\nDEMON";
        case 4: return "INSANE\nDEMON";
        case 5: return "EXTREME\nDEMON";
        default: return "DEMON";
        }
    }

    static std::vector<GJGameLevel*> collectCompletedDemons() {
        std::vector<GJGameLevel*> out;

        auto glm = GameLevelManager::sharedState();
        if (!glm) return out;

        // 1. Collect Online Levels
        if (auto completed = glm->getCompletedLevels(false)) {
            std::unordered_set<int> ids;
            ids.reserve(completed->count());

            CCObject* obj = nullptr;
            CCARRAY_FOREACH(completed, obj) {
                if (auto s = typeinfo_cast<CCString*>(obj)) {
                    ids.insert(s->intValue());
                }
                else if (auto lvl = typeinfo_cast<GJGameLevel*>(obj)) {
                    ids.insert(lvl->m_levelID);
                }
            }

            for (int id : ids) {
                if (id <= 0) continue;
                auto lvl = glm->getSavedLevel(id);
                if (!lvl) continue;
                glm->verifyLevelState(lvl);
                if (lvl->m_normalPercent < 100) continue;
                if (!lvl->m_demon) continue;
                out.push_back(lvl);
            }
        }

        // 2. Collect RobTop Main Demons
        auto gsm = GameStatsManager::sharedState();
        auto tryAddMain = [&](int mainIndex) {
            auto lvl = glm->getMainLevel(mainIndex, false);
            if (!lvl) return;
            glm->verifyLevelState(lvl);

            bool completedNow = (lvl->m_normalPercent >= 100);
            if (!completedNow && gsm) {
                completedNow = gsm->hasCompletedLevel(lvl);
                if (completedNow) lvl->m_normalPercent = 100;
            }
            if (!completedNow) return;

            // RobTop levels usually have m_demon = true, but let's be safe
            if (!lvl->m_demon) return;

            // Deduplicate (unlikely for main levels, but good practice)
            for (auto* existing : out) {
                if (existing == lvl) return;
                if (existing->m_levelID == lvl->m_levelID) return;
            }
            out.push_back(lvl);
            };

        tryAddMain(14); // Clubstep
        tryAddMain(18); // Theory of Everything 2
        tryAddMain(20); // Deadlocked

        return out;
    }

    class CompletedDemonsPopup : public geode::Popup<std::vector<GJGameLevel*>> {
    protected:
        std::vector<GJGameLevel*> m_allLevels;
        std::vector<GJGameLevel*> m_viewLevels;

        int m_page = 0;
        int m_filterTier = 0; // 0=All, 1..5

        std::array<int, 6> m_tierToIconIdx{};
        std::array<std::string, 6> m_tierToFrameName{};
        bool m_tierMapReady = false;

        CCMenu* m_listMenu = nullptr;
        CCLabelBMFont* m_pageLabel = nullptr;
        CCMenu* m_filterMenu = nullptr;
        CCLabelBMFont* m_titleLabel = nullptr;

        // --- Demon Logic ---

        static GJDifficultyName demonDifficultyNameAuto() {
            static std::optional<int> s_found;
            if (s_found) return static_cast<GJDifficultyName>(*s_found);

            auto cache = CCSpriteFrameCache::sharedSpriteFrameCache();
            auto getFrameIfExists = [&](int iconIdx, int nameVal) -> std::string {
                auto frameName = GJDifficultySprite::getDifficultyFrame(iconIdx, static_cast<GJDifficultyName>(nameVal));
                if (frameName.empty()) return "";
                if (!cache->spriteFrameByName(frameName.c_str())) return "";
                return frameName;
                };

            int best = 0;
            int bestScore = -1;

            for (int nameVal = 0; nameVal < 64; ++nameVal) {
                std::vector<std::string> distinct;
                distinct.reserve(16);
                int valid = 0;
                int demonHint = 0;

                for (int enumVal = 0; enumVal <= 20; ++enumVal) {
                    int iconIdx = GJGameLevel::demonIconForDifficulty(static_cast<DemonDifficultyType>(enumVal));
                    auto fr = getFrameIfExists(iconIdx, nameVal);
                    if (fr.empty()) continue;
                    valid++;
                    std::string lower = fr;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (lower.find("demon") != std::string::npos) demonHint++;
                    distinct.push_back(fr);
                }
                std::sort(distinct.begin(), distinct.end());
                distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
                int d = static_cast<int>(distinct.size());
                int score = (d >= 5 ? 100000 : 0) + d * 1000 + valid * 10 + demonHint;
                if (score > bestScore) {
                    bestScore = score;
                    best = nameVal;
                }
            }
            s_found = best;
            return static_cast<GJDifficultyName>(*s_found);
        }

        static std::string frameForEnumVal(int enumVal, GJDifficultyName name) {
            auto cache = CCSpriteFrameCache::sharedSpriteFrameCache();
            int iconIdx = GJGameLevel::demonIconForDifficulty(static_cast<DemonDifficultyType>(enumVal));
            auto frameName = GJDifficultySprite::getDifficultyFrame(iconIdx, name);
            if (frameName.empty()) return "";
            if (!cache->spriteFrameByName(frameName.c_str())) return "";
            return frameName;
        }

        static int iconIdxForEnumVal(int enumVal) {
            return GJGameLevel::demonIconForDifficulty(static_cast<DemonDifficultyType>(enumVal));
        }

        void buildTierMap() {
            m_tierToIconIdx.fill(0);
            m_tierToFrameName.fill("");
            m_tierMapReady = false;

            auto name = demonDifficultyNameAuto();
            struct Cand { int enumVal; int iconIdx; std::string frame; };
            std::vector<Cand> cands;
            cands.reserve(32);

            for (int enumVal = 0; enumVal <= 20; ++enumVal) {
                int iconIdx = iconIdxForEnumVal(enumVal);
                auto fr = frameForEnumVal(enumVal, name);
                if (fr.empty()) continue;
                cands.push_back({ enumVal, iconIdx, fr });
            }

            std::unordered_map<std::string, Cand> bestByFrame;
            for (auto const& c : cands) {
                auto it = bestByFrame.find(c.frame);
                if (it == bestByFrame.end() || c.iconIdx < it->second.iconIdx) {
                    bestByFrame[c.frame] = c;
                }
            }

            std::vector<Cand> unique;
            for (auto& kv : bestByFrame) unique.push_back(kv.second);

            std::sort(unique.begin(), unique.end(), [](auto const& a, auto const& b) {
                return a.iconIdx < b.iconIdx;
                });

            if (unique.size() >= 3) {
                std::rotate(unique.begin(), unique.begin() + 1, unique.begin() + 3);
            }

            int count = std::min<int>(5, unique.size());
            for (int i = 0; i < count; ++i) {
                int tier = i + 1;
                m_tierToIconIdx[tier] = unique[i].iconIdx;
                m_tierToFrameName[tier] = unique[i].frame;
            }
            m_tierMapReady = (count == 5);
        }

        int tierForLevel(GJGameLevel* lvl) const {
            // FIX: Explicitly handle RobTop Main Demons
            if (lvl->m_levelID == 14) return 1; // Clubstep -> Easy
            if (lvl->m_levelID == 18) return 1; // ToE2 -> Easy
            if (lvl->m_levelID == 20) return 1; // Deadlocked -> Medium

            int raw = static_cast<int>(lvl->m_demonDifficulty);
            auto name = demonDifficultyNameAuto();
            auto fr = frameForEnumVal(raw, name);
            if (!fr.empty()) {
                for (int tier = 1; tier <= 5; ++tier) {
                    if (!m_tierToFrameName[tier].empty() && fr == m_tierToFrameName[tier]) return tier;
                }
            }
            if (raw >= 0 && raw <= 4) return raw + 1;
            if (raw >= 1 && raw <= 5) return raw;
            return 3;
        }

        static void applyTierLabelToIcon(CCNode* icon, int tier) {
            if (!icon) return;
            CCObject* obj = nullptr;
            CCARRAY_FOREACH(icon->getChildren(), obj) {
                if (auto lbl = typeinfo_cast<CCLabelBMFont*>(obj)) {
                    lbl->setString(getDemonTierIconText(tier));
                    lbl->setAlignment(CCTextAlignment::kCCTextAlignmentCenter);
                    lbl->setScale(0.60f);
                    return;
                }
            }
        }

        CCNode* makeDemonIconNodeForTier(int tier, float scale) const {
            tier = std::clamp(tier, 1, 5);
            int iconIdx = (m_tierMapReady && m_tierToIconIdx[tier] != 0)
                ? m_tierToIconIdx[tier]
                : GJGameLevel::demonIconForDifficulty(static_cast<DemonDifficultyType>(tier));

            auto icon = GJDifficultySprite::create(iconIdx, demonDifficultyNameAuto());
            if (!icon) {
                auto node = CCNode::create();
                node->setContentSize({ 22.f, 22.f });
                return node;
            }

            applyTierLabelToIcon(icon, tier);

            icon->setScale(scale);
            return icon;
        }

        CCNode* makeDemonIconNode(GJGameLevel* lvl) const {
            return makeDemonIconNodeForTier(tierForLevel(lvl), 0.42f);
        }

        // --- UI Construction ---

        static std::string toLowerCopy(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        void rebuildView() {
            m_viewLevels.clear();

            if (m_filterTier == 0) {
                m_viewLevels = m_allLevels;
                std::sort(m_viewLevels.begin(), m_viewLevels.end(), [&](auto a, auto b) {
                    int ta = tierForLevel(a);
                    int tb = tierForLevel(b);
                    if (ta != tb) return ta < tb;
                    auto an = toLowerCopy(a->m_levelName);
                    auto bn = toLowerCopy(b->m_levelName);
                    if (an != bn) return an < bn;
                    return a->m_levelID < b->m_levelID;
                    });
            }
            else {
                for (auto* lvl : m_allLevels) {
                    if (tierForLevel(lvl) == m_filterTier) m_viewLevels.push_back(lvl);
                }
                std::sort(m_viewLevels.begin(), m_viewLevels.end(), [&](auto a, auto b) {
                    auto an = toLowerCopy(a->m_levelName);
                    auto bn = toLowerCopy(b->m_levelName);
                    if (an != bn) return an < bn;
                    return a->m_levelID < b->m_levelID;
                    });
            }

            if (m_titleLabel) {
                std::string mode = (m_filterTier == 0) ? "All" : getDemonTierString(m_filterTier);
                m_titleLabel->setString(fmt::format("Completed Demons - {} ({})", mode, m_viewLevels.size()).c_str());
            }
        }

        void setFilter(int tier) {
            m_filterTier = std::clamp(tier, 0, 5);
            m_page = 0;
            rebuildView();
            updateFilterButtonHighlight();
            rebuildPage();
        }

        CCNode* makeFilterIconButtonNode(int tier) {
            auto btn = ButtonSprite::create("", 32, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);

            auto icon = makeDemonIconNodeForTier(tier, 0.52f);
            auto size = btn->getContentSize();
            icon->setPosition({ size.width / 2.f, size.height / 2.f + 1.f });
            btn->addChild(icon);
            return btn;
        }

        CCNode* makeAllButtonNode() {
            return ButtonSprite::create("ALL", 32, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
        }

        void updateFilterButtonHighlight() {
            if (!m_filterMenu) return;
            CCObject* obj = nullptr;
            CCARRAY_FOREACH(m_filterMenu->getChildren(), obj) {
                auto mi = static_cast<CCMenuItemSpriteExtra*>(obj);
                if (!mi) continue;
                mi->setColor((mi->getTag() == m_filterTier) ? ccColor3B{ 255, 255, 255 } : ccColor3B{ 120, 120, 120 });
            }
        }

        void onFilter(CCObject* sender) {
            auto n = typeinfo_cast<CCNode*>(sender);
            if (!n) return;
            setFilter(n->getTag());
        }

        CCMenuItemSpriteExtra* makeRowItem(int idx, GJGameLevel* lvl) {
            constexpr float rowW = 380.f;
            constexpr float rowH = 26.f;

            auto row = CCNode::create();
            row->setContentSize({ rowW, rowH });

            if (auto bg = cocos2d::extension::CCScale9Sprite::createWithSpriteFrameName("GJ_square01.png")) {
                bg->setContentSize({ rowW, rowH });
                bg->setOpacity(35);
                bg->setPosition({ rowW / 2.f, rowH / 2.f });
                row->addChild(bg);
            }

            auto icon = makeDemonIconNode(lvl);
            icon->setPosition({ 16.f, rowH / 2.f });
            row->addChild(icon);

            auto name = CCLabelBMFont::create(lvl->m_levelName.c_str(), "bigFont.fnt");
            name->setAnchorPoint({ 0.f, 0.5f });
            name->setPosition({ 36.f, rowH / 2.f + 5.f });
            name->setScale(0.32f);
            row->addChild(name);

            int tier = tierForLevel(lvl);
            auto diffText = getDemonTierString(tier);
            auto diffLabel = CCLabelBMFont::create(diffText.c_str(), "chatFont.fnt");
            diffLabel->setAnchorPoint({ 0.f, 0.5f });
            diffLabel->setPosition({ 36.f, rowH / 2.f - 7.f });
            diffLabel->setScale(0.45f);
            diffLabel->setColor({ 255, 200, 0 });
            row->addChild(diffLabel);

            float maxNameW = rowW - 36.f - 60.f;
            float nameW = name->getContentSize().width * name->getScale();
            if (nameW > maxNameW && nameW > 0.f) {
                name->setScale(name->getScale() * (maxNameW / nameW));
            }

            auto idStr = fmt::format("{}", lvl->m_levelID);
            auto id = CCLabelBMFont::create(idStr.c_str(), "bigFont.fnt");
            id->setAnchorPoint({ 1.f, 0.5f });
            id->setPosition({ rowW - 10.f, rowH / 2.f });
            id->setScale(0.28f);
            row->addChild(id);

            auto item = CCMenuItemSpriteExtra::create(row, this, menu_selector(CompletedDemonsPopup::onOpenLevel));
            item->setTag(idx);
            return item;
        }

        void rebuildPage() {
            if (!m_listMenu) return;
            m_listMenu->removeAllChildren();

            int total = static_cast<int>(m_viewLevels.size());
            if (total <= 0) {
                if (m_pageLabel) m_pageLabel->setString("0/0");
                return;
            }

            constexpr float yStep = 28.f;
            float topY = m_size.height - 95.f;
            float bottomY = 60.f;

            int perPage = std::max(1, static_cast<int>(std::floor((topY - bottomY) / yStep)) + 1);
            int maxPage = std::max(0, (total - 1) / perPage);
            m_page = std::clamp(m_page, 0, maxPage);

            int start = m_page * perPage;
            int end = std::min(start + perPage, total);

            float y = topY;
            for (int i = start; i < end; i++) {
                auto item = makeRowItem(i, m_viewLevels[i]);
                item->setPosition(ccp(0.f, y));
                m_listMenu->addChild(item);
                y -= yStep;
            }

            if (m_pageLabel) {
                m_pageLabel->setString(fmt::format("{}/{}", m_page + 1, maxPage + 1).c_str());
            }
        }

        void onPrev(CCObject*) { m_page--; rebuildPage(); }
        void onNext(CCObject*) { m_page++; rebuildPage(); }

        void onOpenLevel(CCObject* sender) {
            auto node = typeinfo_cast<CCNode*>(sender);
            if (!node) return;
            int idx = node->getTag();
            if (idx < 0 || idx >= (int)m_viewLevels.size()) return;

            auto lvl = m_viewLevels[idx];

            // Close popup first
            this->onClose(nullptr);

            // FIX: If it's a Main Level (Clubstep, ToE2, Deadlocked), open the Main Level Screen (LevelSelectLayer)
            if (lvl->m_levelType == GJLevelType::Main || lvl->m_levelID < 128) {
                // ID 14 (Clubstep), 18 (ToE2), 20 (Deadlocked).
                // LevelSelectLayer expects index = ID - 1.
                auto scene = LevelSelectLayer::scene(lvl->m_levelID - 1);
                CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
            }
            else {
                // Online levels use LevelInfoLayer
                auto info = LevelInfoLayer::create(lvl, false);
                CCDirector::sharedDirector()->getRunningScene()->addChild(info, 200);
            }
        }

        bool setup(std::vector<GJGameLevel*> levels) override {
            m_allLevels = std::move(levels);
            buildTierMap();

            m_titleLabel = CCLabelBMFont::create("", "goldFont.fnt");
            m_titleLabel->setPosition({ m_size.width / 2.f, m_size.height - 25.f });
            m_titleLabel->setScale(0.55f);
            m_mainLayer->addChild(m_titleLabel);

            m_listMenu = CCMenu::create();
            m_listMenu->setPosition({ m_size.width / 2.f, 0.f });
            m_mainLayer->addChild(m_listMenu);

            m_filterMenu = CCMenu::create();
            m_filterMenu->setPosition(ccp(m_size.width / 2.f, m_size.height - 58.f));
            m_mainLayer->addChild(m_filterMenu);

            // Spacing
            float spacing = 60.f;
            float startX = -spacing * 2.5f;

            for (int tier = 1; tier <= 5; tier++) {
                auto node = makeFilterIconButtonNode(tier);
                auto btn = CCMenuItemSpriteExtra::create(node, this, menu_selector(CompletedDemonsPopup::onFilter));
                btn->setTag(tier);
                btn->setPosition(ccp(startX + (tier - 1) * spacing, 0.f));
                m_filterMenu->addChild(btn);
            }
            {
                auto node = makeAllButtonNode();
                auto btn = CCMenuItemSpriteExtra::create(node, this, menu_selector(CompletedDemonsPopup::onFilter));
                btn->setTag(0);
                btn->setPosition(ccp(startX + 5 * spacing, 0.f));
                m_filterMenu->addChild(btn);
            }

            auto navMenu = CCMenu::create();
            navMenu->setPosition(ccp(m_size.width / 2.f, 22.f));
            m_mainLayer->addChild(navMenu);

            auto prevSpr = ButtonSprite::create("Prev", 45, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
            prevSpr->setScale(0.8f);
            auto nextSpr = ButtonSprite::create("Next", 45, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
            nextSpr->setScale(0.8f);

            auto prevBtn = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(CompletedDemonsPopup::onPrev));
            auto nextBtn = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(CompletedDemonsPopup::onNext));

            prevBtn->setPosition(ccp(-95.f, 0.f));
            nextBtn->setPosition(ccp(95.f, 0.f));

            navMenu->addChild(prevBtn);
            navMenu->addChild(nextBtn);

            m_pageLabel = CCLabelBMFont::create("", "bigFont.fnt");
            m_pageLabel->setScale(0.45f);
            m_pageLabel->setPosition({ 0.f, 0.f });
            navMenu->addChild(m_pageLabel);

            setFilter(0);
            return true;
        }

    public:
        static CompletedDemonsPopup* create(std::vector<GJGameLevel*> levels) {
            auto p = new CompletedDemonsPopup();
            if (p && p->initAnchored(440.f, 300.f, std::move(levels))) {
                p->autorelease();
                return p;
            }
            CC_SAFE_DELETE(p);
            return nullptr;
        }
    };

}

class $modify(CompletedDemonsProfilePage, ProfilePage) {
    bool init(int accountID, bool ownProfile) {
        if (!ProfilePage::init(accountID, ownProfile)) return false;

        // Find the big profile panel so we can place the button inside it
        CCNode* panel = findLargestPanelNode(this);

        // Fallback: old method (near close button) if panel wasn't found
        CCNode* anchorParent = this;
        CCPoint anchorPos = ccp(0.f, 0.f);

        if (panel && panel->getParent()) {
            anchorParent = panel->getParent();

            // Convert "top-right inside panel" into the parent coordinate space
            auto sz = panel->getContentSize();
            auto ap = panel->getAnchorPoint();
            float sx = panel->getScaleX();
            float sy = panel->getScaleY();

            float left = panel->getPositionX() - ap.x * sz.width * sx;
            float bottom = panel->getPositionY() - ap.y * sz.height * sy;

            float right = left + sz.width * sx;
            float top = bottom + sz.height * sy;

            // Tweak these offsets to taste (this is the blue-box region)
            // Move left (-X) and down (-Y) from the panel's top-right corner.
            anchorPos = ccp(right - 38.f, top - 20.f);

        }
        else {
            // Old approach if needed
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            anchorPos = ccp(winSize.width - 30.f, winSize.height - 25.f);

            if (auto close = geode::cocos::getChildBySpriteFrameName(this, "GJ_closeBtn_001.png")) {
                anchorParent = close->getParent();
                anchorPos = ccp(close->getPositionX() - 55.f, close->getPositionY());
            }
        }

        auto menu = CCMenu::create();
        menu->setPosition(ccp(0.f, 0.f));
        menu->setZOrder(999);
        anchorParent->addChild(menu);

        auto spr = ButtonSprite::create("Demons", 55, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
        spr->setScale(0.7f);

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(CompletedDemonsProfilePage::onCompletedDemons)
        );

        btn->setPosition(anchorPos);
        menu->addChild(btn);

        return true;
    }

    void onCompletedDemons(CCObject*) {
        auto demons = collectCompletedDemons();
        if (auto popup = CompletedDemonsPopup::create(std::move(demons))) {
            popup->show();
        }
    }
};