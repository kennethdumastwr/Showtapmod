// Percent Macro - click/release macro synced to level position (percent)
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

using namespace geode::prelude;

enum class Mode { Off, Record, Play };

// x = exact player X position (this IS the level percent, just without rounding)
// stored as double and saved with 25 decimals
struct Action {
    double x;
    double percent;
    bool down;
    bool p2;
};

namespace M {
    inline Mode mode = Mode::Off;
    inline std::vector<Action> actions;
    inline size_t idx = 0;
    inline bool injecting = false;
    inline bool ready = false;
    inline bool active = true;   // does the macro play on this attempt?
    inline int cycle = 0;        // position in the "play after N attempts" cycle
    inline bool dieDone = false;
    inline int dieTries = 0;
    inline bool holdP1 = false;
    inline bool holdP2 = false;
    inline std::string levelKey;

    // user options
    inline bool dieEnabled = false;
    inline float diePercent = 50.f;
    inline bool skipEnabled = false;
    inline int skipN = 2;
    inline bool useFolder = false;
    inline std::string folder;

    constexpr double EPS = 1e-6;

    inline int period() { return skipEnabled ? std::max(skipN, 0) + 1 : 1; }

    inline float opacity() {
        return (float)Mod::get()->getSettingValue<double>("button-opacity");
    }

    inline std::string keyFor(GJGameLevel* lvl) {
        int id = lvl->m_levelID.value();
        if (id > 0) return std::to_string(id);
        return "local_" + std::to_string(std::hash<std::string>{}(std::string(lvl->m_levelName)) % 100000000);
    }

    // folder macros are saved to / loaded from
    inline std::filesystem::path baseDir() {
        std::error_code ec;
        std::filesystem::path dir;
        if (useFolder && !folder.empty()) {
            dir = std::filesystem::path(folder);
            std::filesystem::create_directories(dir, ec);
            if (!ec) return dir;
        }
        dir = Mod::get()->getSaveDir() / "macros";
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    inline std::filesystem::path macroPath() {
        return baseDir() / (levelKey + ".txt");
    }

    inline bool save() {
        if (levelKey.empty() || actions.empty()) return false;
        std::ofstream out(macroPath());
        if (!out) return false;
        out << "# percent-macro v2: x percent down p2 (25 decimals)\n" << std::fixed << std::setprecision(25);
        for (auto& a : actions) {
            out << a.x << ' ' << a.percent << ' ' << (int)a.down << ' ' << (int)a.p2 << '\n';
        }
        return true;
    }

    // returns true if a macro file was found; current macro is untouched otherwise
    inline bool load() {
        if (levelKey.empty()) return false;
        std::ifstream in(macroPath());
        if (!in) return false;
        std::vector<Action> loaded;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            Action a{};
            int d = 0, p = 0;
            if (ss >> a.x >> a.percent >> d >> p) {
                a.down = d != 0;
                a.p2 = p != 0;
                loaded.push_back(a);
            }
        }
        if (loaded.empty()) return false;
        actions = std::move(loaded);
        idx = 0;
        return true;
    }

    inline void clearMacro() {
        actions.clear();
        idx = 0;
        if (!levelKey.empty()) {
            std::error_code ec;
            std::filesystem::remove(macroPath(), ec);
        }
    }

    inline bool lastDown(bool p2) {
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            if (it->p2 == p2) return it->down;
        }
        return false;
    }

    inline void setMode(Mode m) {
        if (mode == Mode::Record && m != Mode::Record) save();
        mode = m;
        if (m == Mode::Record) {
            actions.clear();
        }
        if (m == Mode::Play) {
            if (actions.empty()) load();
            // macro starts on the NEXT attempt (restart the level)
            cycle = period() - 1;
            active = false;
            idx = 0;
            holdP1 = holdP2 = false;
        }
    }

    // Called after every resetLevel (death, restart, practice-mode respawn)
    inline void onReset(PlayLayer* pl) {
        if (!ready || !pl->m_player1) return;
        double x = pl->m_player1->getPositionX();
        double pct = pl->getCurrentPercent();

        dieTries = 0;
        // only skip the die when respawning from a practice checkpoint that is already past it
        dieDone = pl->m_isPracticeMode && pct >= diePercent;

        if (mode == Mode::Record) {
            // practice mode: throw away everything recorded after the checkpoint we respawned at
            while (!actions.empty() && actions.back().x >= x - EPS) actions.pop_back();
            // the game releases buttons on respawn, so close any open hold
            for (bool p2 : {false, true}) {
                if (lastDown(p2)) actions.push_back({x, pct, false, p2});
            }
        } else if (mode == Mode::Play) {
            cycle = (cycle + 1) % period();
            active = (cycle == 0);

            auto it = std::lower_bound(actions.begin(), actions.end(), x - EPS,
                [](Action const& a, double v) { return a.x < v; });
            idx = it - actions.begin();

            // if we respawn in the middle of a hold, re-press right away
            holdP1 = holdP2 = false;
            bool seen1 = false, seen2 = false;
            for (size_t i = idx; i-- > 0 && !(seen1 && seen2);) {
                auto& a = actions[i];
                if (!a.p2 && !seen1) { holdP1 = a.down; seen1 = true; }
                if (a.p2 && !seen2) { holdP2 = a.down; seen2 = true; }
            }
        }
    }
}

static void inject(GJBaseGameLayer* l, bool down, bool p2) {
    M::injecting = true;
    l->handleButton(down, 1, !p2);
    M::injecting = false;
}

// ---------------------------------------------------------------- recording / playback
class $modify(MacroGJ, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (M::ready && pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this)
            && button == 1 && !M::injecting) {
            // while the macro is playing, ignore real clicks so nothing desyncs
            if (M::mode == Mode::Play && M::active) return;

            if (M::mode == Mode::Record && m_player1 && !m_player1->m_isDead) {
                M::actions.push_back({
                    (double)m_player1->getPositionX(),
                    (double)pl->getCurrentPercent(),
                    down,
                    !isPlayer1
                });
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        auto self = static_cast<GJBaseGameLayer*>(this);
        bool inLevel = M::ready && pl && static_cast<GJBaseGameLayer*>(pl) == self
            && m_player1 && !m_player1->m_isDead;
        bool playing = inLevel && M::mode == Mode::Play && M::active;

        if (playing) {
            double x = m_player1->getPositionX();

            if (M::holdP1) { M::holdP1 = false; inject(self, true, false); }
            if (M::holdP2) { M::holdP2 = false; inject(self, true, true); }

            while (M::idx < M::actions.size() && x >= M::actions[M::idx].x - M::EPS) {
                auto a = M::actions[M::idx];
                inject(self, a.down, a.p2);
                M::idx++;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        // "Die at %": works while the macro plays AND when the mode is Off.
        // Not during Record, and not on the attempts you play yourself (play-after-attempts).
        bool dieMode = M::mode == Mode::Off || (M::mode == Mode::Play && M::active);
        if (inLevel && dieMode && M::dieEnabled && !M::dieDone
            && m_player1 && !m_player1->m_isDead
            && (double)pl->getCurrentPercent() >= (double)M::diePercent) {
            pl->destroyPlayer(pl->m_player1, pl->m_anticheatSpike);
            if (m_player1->m_isDead) {
                M::dieDone = true;
            } else if (++M::dieTries >= 3) {
                // kill got blocked somehow - force a restart so the attempt still fails
                M::dieDone = true;
                pl->resetLevel();
            }
        }
    }
};

// ---------------------------------------------------------------- PlayLayer
class $modify(MacroPL, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        M::ready = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        M::levelKey = M::keyFor(level);
        if (M::mode == Mode::Record) M::actions.clear();
        else if (!M::load()) M::actions.clear();

        M::idx = 0;
        M::cycle = 0;
        M::active = true;
        M::dieDone = false;
        M::dieTries = 0;
        M::holdP1 = M::holdP2 = false;
        M::ready = true;

        auto win = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID("macro-status"_spr);
        label->setScale(.35f);
        label->setOpacity(140);
        label->setAnchorPoint({0.f, 1.f});
        label->setPosition({6.f, win.height - 6.f});
        m_uiLayer->addChild(label, 50);
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        M::onReset(this);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto label = static_cast<CCLabelBMFont*>(m_uiLayer->getChildByID("macro-status"_spr));
        if (!label) return;

        std::string s;
        if (Mod::get()->getSettingValue<bool>("show-status")) {
            if (M::mode == Mode::Record) {
                s = fmt::format("REC - {} actions", M::actions.size());
            } else if (M::mode == Mode::Play) {
                if (M::active) s = fmt::format("PLAY - {}/{}", M::idx, M::actions.size());
                else s = fmt::format("MACRO - plays in {} attempt(s)", M::period() - M::cycle);
            }
            if (M::dieEnabled) {
                if (!s.empty()) s += "  |  ";
                s += fmt::format("DIE @ {}%", M::diePercent);
            }
        }
        static std::string last;
        if (s != last) {
            last = s;
            label->setString(s.c_str());
        }
    }

    void levelComplete() {
        if (M::mode == Mode::Record) M::save();
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (M::mode == Mode::Record) M::save();
        M::ready = false;
        M::levelKey.clear();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------- folder popup
class FolderPopup : public Popup {
protected:
    CCLabelBMFont* m_status = nullptr;

    void setStatus(std::string const& s) {
        m_status->setString(s.c_str());
    }

    bool init() {
        if (!Popup::init(340.f, 210.f)) return false;
        this->setTitle("Macro Folder");

        auto toggle = CCMenuItemExt::createToggler(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            [](CCMenuItemToggler* t) {
                M::useFolder = !t->isToggled();
                Mod::get()->setSavedValue("use-folder", M::useFolder);
            });
        toggle->setScale(.7f);
        toggle->toggle(M::useFolder);
        m_buttonMenu->addChildAtPosition(toggle, Anchor::Center, {-140.f, 48.f});

        auto label = CCLabelBMFont::create("Use this folder to save/load macros", "bigFont.fnt");
        label->setScale(.35f);
        label->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {-120.f, 48.f});

        auto input = TextInput::create(300.f, "/storage/emulated/0/Download/macros", "chatFont.fnt");
        input->setMaxCharCount(250);
        input->setString(M::folder, false);
        input->setCallback([](std::string const& s) {
            M::folder = s;
            Mod::get()->setSavedValue("folder", s);
        });
        m_mainLayer->addChildAtPosition(input, Anchor::Center, {0.f, 12.f});

        auto saveSpr = ButtonSprite::create("Save now", 100, true, "bigFont.fnt", "GJ_button_01.png", 28.f, .55f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCObject*) {
            if (M::levelKey.empty()) { this->setStatus("Open a level first"); return; }
            bool ok = M::save();
            this->setStatus(ok ? fmt::format("Saved {} actions", M::actions.size())
                               : std::string("Nothing to save (or folder not writable)"));
        });
        m_buttonMenu->addChildAtPosition(saveBtn, Anchor::Center, {-70.f, -32.f});

        auto loadSpr = ButtonSprite::create("Load now", 100, true, "bigFont.fnt", "GJ_button_02.png", 28.f, .55f);
        auto loadBtn = CCMenuItemExt::createSpriteExtra(loadSpr, [this](CCObject*) {
            if (M::levelKey.empty()) { this->setStatus("Open a level first"); return; }
            bool ok = M::load();
            this->setStatus(ok ? fmt::format("Loaded {} actions", M::actions.size())
                               : std::string("No macro found for this level in that folder"));
        });
        m_buttonMenu->addChildAtPosition(loadBtn, Anchor::Center, {70.f, -32.f});

        m_status = CCLabelBMFont::create(fmt::format("Folder: {}", M::baseDir().string()).c_str(), "chatFont.fnt");
        m_status->setScale(.6f);
        m_status->limitLabelWidth(310.f, .6f, .2f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0.f, -72.f});
        return true;
    }

public:
    static FolderPopup* create() {
        auto ret = new FolderPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- main popup
class MacroPopup : public Popup {
protected:
    CCLabelBMFont* m_modeLabel = nullptr;
    CCLabelBMFont* m_infoLabel = nullptr;

    void refresh() {
        char const* names[] = {"Off", "Recording", "Playing"};
        m_modeLabel->setString(fmt::format("Mode: {}", names[(int)M::mode]).c_str());
        m_infoLabel->setString(fmt::format("{} actions", M::actions.size()).c_str());
    }

    void applyMode(Mode m) {
        M::setMode(m);
        refresh();
        if (m == Mode::Record) {
            Notification::create("Recording armed - restart the level", NotificationIcon::Success, 2.f)->show();
        } else if (m == Mode::Play) {
            Notification::create("Macro ready - restart the level", NotificationIcon::Success, 2.f)->show();
        }
    }

    bool init() {
        if (!Popup::init(340.f, 290.f)) return false;
        this->setTitle("Percent Macro");

        // ---- mode buttons
        auto addMode = [this](char const* text, Mode m, float x) {
            auto spr = ButtonSprite::create(text, 80, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .6f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, m](CCObject*) { this->applyMode(m); });
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, 90.f});
        };
        addMode("Off", Mode::Off, -100.f);
        addMode("Record", Mode::Record, 0.f);
        addMode("Play", Mode::Play, 100.f);

        m_modeLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_modeLabel->setScale(.6f);
        m_mainLayer->addChildAtPosition(m_modeLabel, Anchor::Center, {0.f, 58.f});

        // ---- die at percent
        auto dieToggle = CCMenuItemExt::createToggler(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            [](CCMenuItemToggler* t) {
                M::dieEnabled = !t->isToggled();
                Mod::get()->setSavedValue("die-enabled", M::dieEnabled);
            });
        dieToggle->setScale(.7f);
        dieToggle->toggle(M::dieEnabled);
        m_buttonMenu->addChildAtPosition(dieToggle, Anchor::Center, {-140.f, 22.f});

        auto dieLabel = CCLabelBMFont::create("Die at percent", "bigFont.fnt");
        dieLabel->setScale(.4f);
        dieLabel->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(dieLabel, Anchor::Center, {-120.f, 22.f});

        auto dieInput = TextInput::create(70.f, "50", "bigFont.fnt");
        dieInput->setFilter("0123456789.");
        dieInput->setMaxCharCount(6);
        dieInput->setString(fmt::format("{}", M::diePercent), false);
        dieInput->setCallback([](std::string const& s) {
            auto r = utils::numFromString<float>(s);
            if (r.isOk()) {
                M::diePercent = std::clamp(r.unwrap(), 0.f, 100.f);
                Mod::get()->setSavedValue("die-percent", M::diePercent);
            }
        });
        m_mainLayer->addChildAtPosition(dieInput, Anchor::Center, {120.f, 22.f});

        // ---- play after N attempts
        auto skipToggle = CCMenuItemExt::createToggler(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            [](CCMenuItemToggler* t) {
                M::skipEnabled = !t->isToggled();
                Mod::get()->setSavedValue("skip-enabled", M::skipEnabled);
            });
        skipToggle->setScale(.7f);
        skipToggle->toggle(M::skipEnabled);
        m_buttonMenu->addChildAtPosition(skipToggle, Anchor::Center, {-140.f, -18.f});

        auto skipLabel = CCLabelBMFont::create("Play after attempts", "bigFont.fnt");
        skipLabel->setScale(.4f);
        skipLabel->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(skipLabel, Anchor::Center, {-120.f, -18.f});

        auto skipInput = TextInput::create(50.f, "2", "bigFont.fnt");
        skipInput->setFilter("0123456789");
        skipInput->setMaxCharCount(3);
        skipInput->setString(std::to_string(M::skipN), false);
        skipInput->setCallback([](std::string const& s) {
            auto r = utils::numFromString<int>(s);
            if (r.isOk()) {
                M::skipN = std::max(0, r.unwrap());
                Mod::get()->setSavedValue("skip-n", M::skipN);
            }
        });
        m_mainLayer->addChildAtPosition(skipInput, Anchor::Center, {130.f, -18.f});

        // ---- button opacity
        auto opLabel = CCLabelBMFont::create("Button opacity", "bigFont.fnt");
        opLabel->setScale(.4f);
        m_mainLayer->addChildAtPosition(opLabel, Anchor::Center, {0.f, -55.f});

        auto slider = Slider::create(this, menu_selector(MacroPopup::onOpacity), .7f);
        slider->setValue((M::opacity() - 0.1f) / 0.9f);
        m_mainLayer->addChildAtPosition(slider, Anchor::Center, {0.f, -80.f});

        // ---- clear + folder + info
        auto clearSpr = ButtonSprite::create("Clear macro", 110, true, "bigFont.fnt", "GJ_button_06.png", 28.f, .5f);
        auto clearBtn = CCMenuItemExt::createSpriteExtra(clearSpr, [this](CCObject*) {
            M::clearMacro();
            this->refresh();
        });
        m_buttonMenu->addChildAtPosition(clearBtn, Anchor::Center, {-115.f, -118.f});

        auto folderSpr = ButtonSprite::create("Folder", 80, true, "bigFont.fnt", "GJ_button_02.png", 28.f, .5f);
        auto folderBtn = CCMenuItemExt::createSpriteExtra(folderSpr, [](CCObject*) {
            if (auto p = FolderPopup::create()) p->show();
        });
        m_buttonMenu->addChildAtPosition(folderBtn, Anchor::Center, {-30.f, -118.f});

        m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_infoLabel->setScale(.8f);
        m_mainLayer->addChildAtPosition(m_infoLabel, Anchor::Center, {90.f, -118.f});

        refresh();
        return true;
    }

    void onOpacity(CCObject* sender) {
        float v = static_cast<SliderThumb*>(sender)->getValue();
        float o = 0.1f + v * 0.9f;
        Mod::get()->setSettingValue<double>("button-opacity", o);
        if (auto n = CCScene::get()->getChildByIDRecursive("macro-button"_spr)) {
            if (auto b = typeinfo_cast<CCMenuItemSpriteExtra*>(n)) {
                if (auto rgba = typeinfo_cast<CCRGBAProtocol*>(b->getNormalImage())) {
                    rgba->setOpacity((GLubyte)(o * 255.f));
                }
            }
        }
    }

public:
    static MacroPopup* create() {
        auto ret = new MacroPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- bottom-right "Macro" button
static void addMacroButton(CCNode* parent) {
    auto win = CCDirector::get()->getWinSize();

    auto spr = ButtonSprite::create("Macro", 70, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .7f);
    spr->setCascadeOpacityEnabled(true);
    spr->setOpacity((GLubyte)(M::opacity() * 255.f));

    auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCObject*) {
        if (auto p = MacroPopup::create()) p->show();
    });
    btn->setID("macro-button"_spr);

    auto menu = CCMenu::create();
    menu->setID("macro-menu"_spr);
    menu->setPosition({win.width - 48.f, 24.f});
    menu->addChild(btn);
    parent->addChild(menu, 100);
}

class $modify(MacroMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        addMacroButton(this);
        return true;
    }
};

class $modify(MacroPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        addMacroButton(this);
    }
};

$on_mod(Loaded) {
    M::dieEnabled = Mod::get()->getSavedValue<bool>("die-enabled", false);
    M::diePercent = Mod::get()->getSavedValue<float>("die-percent", 50.f);
    M::skipEnabled = Mod::get()->getSavedValue<bool>("skip-enabled", false);
    M::skipN = Mod::get()->getSavedValue<int>("skip-n", 2);
    M::useFolder = Mod::get()->getSavedValue<bool>("use-folder", false);
    M::folder = Mod::get()->getSavedValue<std::string>("folder", std::string());
}
