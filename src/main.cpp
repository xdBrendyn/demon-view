#include <Geode/Geode.hpp>

#include <Geode/modify/ProfilePage.hpp>
#include <Geode/utils/cocos.hpp>

#include <cocos-ext.h>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/GJDifficultySprite.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

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

    static cocos2d::extension::CCScale9Sprite* findLargestPanel(CCNode* root) {
        if (!root) return nullptr;

        cocos2d::extension::CCScale9Sprite* best = nullptr;
        float bestArea = 0.f;

        std::function<void(CCNode*)> walk = [&](CCNode* n) {
            if (!n) return;

            if (auto s9 = typeinfo_cast<cocos2d::extension::CCScale9Sprite*>(n)) {
                auto rect = s9->boundingBox();
                float area = rect.size.width * rect.size.height;

                if (area > bestArea) {
                    bestArea = area;
                    best = s9;
                }
            }

            auto children = n->getChildren();
            if (!children) return;

            CCObject* obj = nullptr;
            CCARRAY_FOREACH(children, obj) {
                if (auto c = typeinfo_cast<CCNode*>(obj)) walk(c);
            }
            };

        walk(root);
        return best;
    }

    static std::vector<GJGameLevel*> collectCompletedDemons() {
        std::vector<GJGameLevel*> out;

        auto glm = GameLevelManager::sharedState();
        if (!glm) return out;

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

                if (!lvl->m_demon) continue;
                glm->verifyLevelState(lvl);
                if (lvl->m_normalPercent < 100) continue;

                out.push_back(lvl);
            }
        }

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
            if (!lvl->m_demon) return;

            lvl->m_demonDifficulty = 3;

            for (auto* existing : out) {
                if (existing == lvl) return;
                if (existing->m_levelID == lvl->m_levelID && existing->m_levelName == lvl->m_levelName) return;
            }
            out.push_back(lvl);
            };

        tryAddMain(14); // Clubstep
        tryAddMain(18); // ToE2
        tryAddMain(20); // Deadlocked

        return out;
    }

    class CompletedDemonsPopup : public geode::Popup<std::vector<GJGameLevel*>> {
    protected:
        std::vector<GJGameLevel*> m_allLevels;
        std::vector<GJGameLevel*> m_viewLevels;

        int m_page = 0;
        int m_filterTier = 0;

        CCMenu* m_listMenu = nullptr;
        CCLabelBMFont* m_pageLabel = nullptr;
        CCMenu* m_filterMenu = nullptr;
        CCLabelBMFont* m_titleLabel = nullptr;

        int tierForLevel(GJGameLevel* lvl) const {
            int raw = static_cast<int>(lvl->m_demonDifficulty);

            switch (raw) {
            case 3: return 1;
            case 4: return 2;
            case 0: return 3;
            case 5: return 4;
            case 6: return 5;
            default: return 3;
            }
        }

        CCNode* makeDemonIconNodeForTier(int tier, float scale) const {
            tier = std::clamp(tier, 1, 5);

            int diffVal = 0;
            switch (tier) {
            case 1: diffVal = 3; break;
            case 2: diffVal = 4; break;
            case 3: diffVal = 0; break;
            case 4: diffVal = 5; break;
            case 5: diffVal = 6; break;
            }

            int iconIdx = GJGameLevel::demonIconForDifficulty(static_cast<DemonDifficultyType>(diffVal));

            auto icon = GJDifficultySprite::create(iconIdx, (GJDifficultyName)0);

            if (!icon) {
                auto node = CCNode::create();
                node->setContentSize({ 22.f, 22.f });
                return node;
            }

            applyTierLabelToIcon(icon, tier);
            icon->setScale(scale);
            return icon;
        }

        static const char* tierIconLabelText(int tier) {
            switch (tier) {
            case 1: return "EASY\nDEMON";
            case 2: return "MEDIUM\nDEMON";
            case 3: return "HARD\nDEMON";
            case 4: return "INSANE\nDEMON";
            case 5: return "EXTREME\nDEMON";
            default: return "DEMON";
            }
        }

        static void applyTierLabelToIcon(CCNode* icon, int tier) {
            if (!icon) return;
            CCObject* obj = nullptr;
            CCARRAY_FOREACH(icon->getChildren(), obj) {
                if (auto lbl = typeinfo_cast<CCLabelBMFont*>(obj)) {
                    lbl->setString(tierIconLabelText(tier));
                    lbl->setScale(0.65f);
                    lbl->setAlignment(CCTextAlignment::kCCTextAlignmentCenter);
                    return;
                }
            }
        }

        CCNode* makeDemonIconNode(GJGameLevel* lvl) const {
            return makeDemonIconNodeForTier(tierForLevel(lvl), 0.42f);
        }

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
                m_titleLabel->setString(
                    fmt::format("Completed Demons - {} ({})", mode, m_viewLevels.size()).c_str()
                );
            }
        }

        void updateFilterButtonHighlight() {
            if (!m_filterMenu) return;

            CCObject* obj = nullptr;
            CCARRAY_FOREACH(m_filterMenu->getChildren(), obj) {
                auto mi = static_cast<CCMenuItemSpriteExtra*>(obj);
                if (!mi) continue;

                mi->setColor((mi->getTag() == m_filterTier)
                    ? ccColor3B{ 255, 255, 255 }
                : ccColor3B{ 120, 120, 120 });
            }
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

        void setFilter(int tier) {
            m_filterTier = std::clamp(tier, 0, 5);
            m_page = 0;
            rebuildView();
            updateFilterButtonHighlight();
            rebuildPage();
        }

        void onFilter(CCObject* sender) {
            auto n = typeinfo_cast<CCNode*>(sender);
            if (!n) return;
            setFilter(n->getTag());
        }

        void onPrev(CCObject*) {
            m_page--;
            rebuildPage();
        }

        void onNext(CCObject*) {
            m_page++;
            rebuildPage();
        }

        void onOpenLevel(CCObject* sender) {
            auto node = typeinfo_cast<CCNode*>(sender);
            if (!node) return;

            int idx = node->getTag();
            if (idx < 0 || idx >= (int)m_viewLevels.size()) return;

            auto lvl = m_viewLevels[idx];

            this->onClose(nullptr);

            if (auto scene = CCDirector::sharedDirector()->getRunningScene()) {
                if (auto info = LevelInfoLayer::create(lvl, false)) {
                    scene->addChild(info, 200);
                }
            }
        }

        CCNode* makeFilterIconButtonNode(int tier) {
            auto btn = ButtonSprite::create(
                "", 32, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f
            );

            auto icon = makeDemonIconNodeForTier(tier, 0.52f);
            auto size = btn->getContentSize();
            icon->setPosition({ size.width / 2.f, size.height / 2.f + 1.f });
            btn->addChild(icon);

            return btn;
        }

        CCNode* makeAllButtonNode() {
            return ButtonSprite::create(
                "ALL", 32, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f
            );
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

            auto item = CCMenuItemSpriteExtra::create(
                row, this, menu_selector(CompletedDemonsPopup::onOpenLevel)
            );
            item->setTag(idx);
            return item;
        }

        bool setup(std::vector<GJGameLevel*> levels) override {
            m_allLevels = std::move(levels);

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

            float spacing = 60.f;
            float startX = -spacing * 2.5f;

            for (int tier = 1; tier <= 5; tier++) {
                auto node = makeFilterIconButtonNode(tier);
                auto btn = CCMenuItemSpriteExtra::create(
                    node, this, menu_selector(CompletedDemonsPopup::onFilter)
                );
                btn->setTag(tier);
                btn->setPosition(ccp(startX + (tier - 1) * spacing, 0.f));
                m_filterMenu->addChild(btn);
            }

            {
                auto node = makeAllButtonNode();
                auto btn = CCMenuItemSpriteExtra::create(
                    node, this, menu_selector(CompletedDemonsPopup::onFilter)
                );
                btn->setTag(0);
                btn->setPosition(ccp(startX + 5 * spacing, 0.f));
                m_filterMenu->addChild(btn);
            }

            auto navMenu = CCMenu::create();
            navMenu->setPosition(ccp(m_size.width / 2.f, 22.f));
            m_mainLayer->addChild(navMenu);

            auto prevSpr = ButtonSprite::create(
                "Prev", 45, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f
            );
            prevSpr->setScale(0.8f);

            auto nextSpr = ButtonSprite::create(
                "Next", 45, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f
            );
            nextSpr->setScale(0.8f);

            auto prevBtn = CCMenuItemSpriteExtra::create(
                prevSpr, this, menu_selector(CompletedDemonsPopup::onPrev)
            );
            auto nextBtn = CCMenuItemSpriteExtra::create(
                nextSpr, this, menu_selector(CompletedDemonsPopup::onNext)
            );

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

        if (ownProfile) {
            auto panel = findLargestPanel(this);
            CCNode* anchorParent = panel ? panel->getParent() : this;

            auto menu = CCMenu::create();
            menu->setPosition({ 0.f, 0.f });
            menu->setZOrder(999);
            anchorParent->addChild(menu);

            auto icon = CCSprite::create("demon_view_icon_512.png"_spr);
            if (!icon) {
                log::error("[DemonView] Failed to load demon_view_icon.png");
                return true;
            }
            icon->setScale(0.28f);

            auto circleSpr = CircleButtonSprite::create(
                icon,
                CircleBaseColor::Green,
                CircleBaseSize::MediumAlt
            );
            circleSpr->setScale(0.85f);

            auto btn = CCMenuItemSpriteExtra::create(
                circleSpr,
                this,
                menu_selector(CompletedDemonsProfilePage::onCompletedDemons)
            );
            menu->addChild(btn);

            if (panel) {
                auto r = panel->boundingBox();
                float pad = 10;

                btn->setPosition({
                    r.getMaxX() - pad,
                    r.getMaxY() - pad
                    });
            }
            else {
                auto win = CCDirector::sharedDirector()->getWinSize();
                btn->setPosition({ win.width, win.height });
            }
        }

        return true;
    }

    void onCompletedDemons(CCObject*) {
        auto demons = collectCompletedDemons();
        if (auto popup = CompletedDemonsPopup::create(std::move(demons))) {
            popup->show();
        }
    }
};