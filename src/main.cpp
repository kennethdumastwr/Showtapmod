// Percent Macro - click/release macro synced to level position (percent)
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

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

struct MacroInfo {
    std::string name;
    std::string level;
    size_t count = 0;
    double lastPct = 0;
};

namespace M {
    inline Mode mode = Mode::Off;
    inline std::vector<Action> actions;
    inline size_t idx = 0;
    inline bool injecting = false;
    inline bool ready = false;
    inline bool active = true;   // does the macro play on this attempt?
    inline int cycle = 0;        // position in the "play after N attempts" cycle
    inline bool failed = false;  // "die at %" reached: macro stopped clicking
    inline bool held[2] = {false, false};
    inline bool holdP1 = false;
    inline bool holdP2 = false;
    inline std::string levelKey;
    inline std::string levelName;
    inline std::string macroName; // name typed by the user (empty = level id)
    inline std::string nextName;  // macro to play after the skipped attempts
    inline bool sawSkip = false;  // a "play after attempts" skip happened since the last active attempt

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

    // all pop-up notifications follow the "show-status" setting (off = silent, good for showcasing)
    inline void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Success) {
        if (!Mod::get()->getSettingValue<bool>("show-status")) return;
        Notification::create(text, icon, 2.f)->show();
    }

    inline std::string keyFor(GJGameLevel* lvl) {
        int id = lvl->m_levelID.value();
        if (id > 0) return std::to_string(id);
        return "local_" + std::to_string(std::hash<std::string>{}(std::string(lvl->m_levelName)) % 100000000);
    }

    inline std::string sanitize(std::string const& s) {
        std::string o;
        for (char c : s) {
            if (std::isalnum((unsigned char)c) || c == '_' || c == '-') o += c;
            else if (c == ' ') o += '_';
        }
        return o;
    }

    inline std::string fileBase() {
        auto n = sanitize(macroName);
        return n.empty() ? levelKey : n;
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

    inline std::filesystem::path pathFor(std::string const& name) {
        return baseDir() / (name + ".txt");
    }

    // never overwrite an existing macro by accident: add _2, _3, ... if the name is taken
    inline std::string uniqueName(std::string const& base) {
        std::error_code ec;
        if (!std::filesystem::exists(pathFor(base), ec)) return base;
        for (int i = 2; i < 10000; i++) {
            auto c = base + "_" + std::to_string(i);
            if (!std::filesystem::exists(pathFor(c), ec)) return c;
        }
        return base;
    }

    inline bool save(std::string const& name) {
        if (levelKey.empty() || actions.empty() || name.empty()) return false;
        std::ofstream out(pathFor(name));
        if (!out) return false;
        out << "# percent-macro v2: x percent down p2 (25 decimals)\n";
        out << "# level " << levelName << "\n";
        out << std::fixed << std::setprecision(25);
        for (auto& a : actions) {
            out << a.x << ' ' << a.percent << ' ' << (int)a.down << ' ' << (int)a.p2 << '\n';
        }
        Mod::get()->setSavedValue("last-" + levelKey, name);
        return true;
    }
    inline bool save() { return save(fileBase()); }

    // returns true if a macro file was found; current macro is untouched otherwise
    inline bool load(std::string const& name) {
        if (name.empty()) return false;
        std::ifstream in(pathFor(name));
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

    // load from the list: also remembers it as this level's macro
    inline bool loadFile(std::string const& name) {
        if (!load(name)) return false;
        macroName = (name == levelKey) ? "" : name;
        if (!levelKey.empty()) Mod::get()->setSavedValue("last-" + levelKey, name);
        return true;
    }

    inline std::vector<MacroInfo> listMacros() {
        std::vector<MacroInfo> out;
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(baseDir(), ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2) || e.path().extension() != ".txt") continue;
            MacroInfo info;
            info.name = e.path().stem().string();
            std::ifstream in(e.path());
            std::string line;
            while (std::getline(in, line)) {
                if (line.rfind("# level ", 0) == 0) {
                    info.level = line.substr(8);
                } else if (!line.empty() && line[0] != '#') {
                    std::istringstream ss(line);
                    double x, p;
                    if (ss >> x >> p) { info.count++; info.lastPct = p; }
                }
            }
            out.push_back(info);
        }
        std::sort(out.begin(), out.end(), [](MacroInfo const& a, MacroInfo const& b) { return a.name < b.name; });
        return out;
    }

    // only clears the macro in memory (saved files are deleted from the list)
    inline void clearMacro() {
        actions.clear();
        idx = 0;
    }

    inline bool lastDown(bool p2) {
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            if (it->p2 == p2) return it->down;
        }
        return false;
    }

    inline void setMode(Mode m) {
        if (mode == Mode::Record) save(); // leaving (or re-arming) a recording saves it first
        mode = m;
        if (m == Mode::Record) {
            actions.clear();
            // new recording ALWAYS gets its own file - old macros are never overwritten
            std::string base = sanitize(macroName);
            if (base.empty()) base = levelKey;
            auto n = uniqueName(base);
            macroName = (n == levelKey) ? "" : n;
        }
        if (m == Mode::Play) {
            if (actions.empty()) load(fileBase());
            // macro starts on the NEXT attempt (restart the level)
            cycle = period() - 1;
            active = false;
            sawSkip = false;
            idx = 0;
            holdP1 = holdP2 = false;
        }
    }

    // Reaching 100% (or beating the level) ends the recording: save it and switch to Off,
    // so restarting afterwards does NOT cut the recorded actions.
    inline void finishRecording(PlayLayer* pl) {
        if (mode != Mode::Record) return;
        if (pl && pl->m_player1) {
            double x = pl->m_player1->getPositionX();
            double pct = pl->getCurrentPercent();
            for (bool p2 : {false, true}) {
                if (lastDown(p2)) actions.push_back({x, pct, false, p2});
            }
        }
        save();
        mode = Mode::Off;
        M::notify("Recording finished - saved", NotificationIcon::Success);
    }

    // After the skipped attempts: switch to the "then play macro" and turn "die at %" off
    inline void applyNextMacro() {
        auto n = sanitize(nextName);
        if (n.empty()) return;
        if (fileBase() != n || actions.empty()) {
            if (!loadFile(n)) {
                M::notify(fmt::format("Next macro '{}' not found", n), NotificationIcon::Error);
                return;
            }
        }
        if (dieEnabled) {
            dieEnabled = false;
            Mod::get()->setSavedValue("die-enabled", false);
            M::notify("Die at % turned off for the next macro", NotificationIcon::Success);
        }
    }

    // Called after every resetLevel (death, restart, practice-mode respawn)
    inline void onReset(PlayLayer* pl) {
        if (!ready || !pl->m_player1) return;
        double x = pl->m_player1->getPositionX();
        double pct = pl->getCurrentPercent();

        failed = false;
        held[0] = held[1] = false;

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
            if (!active) {
                sawSkip = true;
            } else if (sawSkip) {
                sawSkip = false;
                applyNextMacro();
            }

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
    M::held[p2 ? 1 : 0] = down;
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
        bool playing = M::ready && pl && static_cast<GJBaseGameLayer*>(pl) == self
            && m_player1 && !m_player1->m_isDead
            && M::mode == Mode::Play && M::active;

        if (playing) {
            // "Die at %": release at that percent and never click again,
            // so the macro just fails there naturally.
            if (M::dieEnabled && !M::failed && (double)pl->getCurrentPercent() >= (double)M::diePercent) {
                M::failed = true;
                M::holdP1 = M::holdP2 = false;
                if (M::held[0]) inject(self, false, false);
                if (M::held[1]) inject(self, false, true);
            }

            if (!M::failed) {
                double x = m_player1->getPositionX();

                if (M::holdP1) { M::holdP1 = false; inject(self, true, false); }
                if (M::holdP2) { M::holdP2 = false; inject(self, true, true); }

                while (M::idx < M::actions.size() && x >= M::actions[M::idx].x - M::EPS) {
                    auto a = M::actions[M::idx];
                    inject(self, a.down, a.p2);
                    M::idx++;
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------- PlayLayer
class $modify(MacroPL, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        M::ready = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        M::levelKey = M::keyFor(level);
        M::levelName = M::sanitize(std::string(level->m_levelName));
        M::macroName.clear();

        M::actions.clear();
        if (M::mode != Mode::Record) {
            // load the macro this level used last, else the default one
            std::string last = Mod::get()->getSavedValue<std::string>("last-" + M::levelKey, M::levelKey);
            if (M::load(last)) {
                if (last != M::levelKey) M::macroName = last;
            } else {
                M::load(M::levelKey);
            }
        }

        M::idx = 0;
        M::cycle = 0;
        M::active = true;
        M::sawSkip = false;
        M::failed = false;
        M::held[0] = M::held[1] = false;
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

        // reached 100% while recording -> stop recording automatically
        if (M::ready && M::mode == Mode::Record && m_player1 && !m_player1->m_isDead
            && getCurrentPercent() >= 100.f) {
            M::finishRecording(this);
        }

        auto label = static_cast<CCLabelBMFont*>(m_uiLayer->getChildByID("macro-status"_spr));
        if (!label) return;

        std::string s;
        if (Mod::get()->getSettingValue<bool>("show-status")) {
            if (M::mode == Mode::Record) {
                s = fmt::format("REC - {} actions", M::actions.size());
            } else if (M::mode == Mode::Play) {
                if (M::active) {
                    s = M::failed ? fmt::format("PLAY - stopped at {}% (fail)", M::diePercent)
                                  : fmt::format("PLAY - {}/{}", M::idx, M::actions.size());
                } else {
                    s = fmt::format("MACRO - plays in {} attempt(s)", M::period() - M::cycle);
                }
                if (M::dieEnabled && !M::failed) s += fmt::format("  |  DIE @ {}%", M::diePercent);
            }
        }
        static std::string last;
        if (s != last) {
            last = s;
            label->setString(s.c_str());
        }
    }

    void levelComplete() {
        M::finishRecording(this);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (M::mode == Mode::Record) M::save();
        M::ready = false;
        M::levelKey.clear();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------- macro list popup
class MacroListPopup : public Popup {
protected:
    static constexpr int PER_PAGE = 4;
    std::vector<MacroInfo> m_entries;
    std::vector<CCNode*> m_rows;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    int m_page = 0;

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    static std::string cut(std::string s, size_t n) {
        if (s.size() > n) s = s.substr(0, n - 1) + "~";
        return s;
    }

    void rebuild() {
        for (auto n : m_rows) n->removeFromParent();
        m_rows.clear();

        int pages = std::max(1, (int)((m_entries.size() + PER_PAGE - 1) / PER_PAGE));
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{}/{}", m_page + 1, pages).c_str());

        if (m_entries.empty()) setStatus("No macros saved in this folder yet");

        for (int i = 0; i < PER_PAGE; i++) {
            size_t k = (size_t)m_page * PER_PAGE + i;
            if (k >= m_entries.size()) break;
            auto info = m_entries[k];
            float y = 76.f - 36.f * i;

            auto text = fmt::format("{} | {} | {} acts | {:.0f}%",
                cut(info.name, 12), cut(info.level.empty() ? "?" : info.level, 12), info.count, info.lastPct);
            auto spr = ButtonSprite::create(text.c_str(), 250, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .4f);
            auto name = info.name;
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, name](CCObject*) {
                bool ok = M::loadFile(name);
                this->setStatus(ok ? fmt::format("Loaded '{}' ({} actions)", name, M::actions.size())
                                   : std::string("Could not load that file"));
            });
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {-25.f, y});
            m_rows.push_back(btn);

            auto delSpr = ButtonSprite::create("Del", 40, true, "bigFont.fnt", "GJ_button_06.png", 30.f, .5f);
            auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, name](CCObject* sender) {
                Ref<CCObject> keep = sender; // keep the button alive while we rebuild the list
                std::error_code ec;
                std::filesystem::remove(M::pathFor(name), ec);
                m_entries = M::listMacros();
                this->setStatus(fmt::format("Deleted '{}'", name));
                this->rebuild();
            });
            m_buttonMenu->addChildAtPosition(delBtn, Anchor::Center, {135.f, y});
            m_rows.push_back(delBtn);
        }
    }

    bool init() {
        if (!Popup::init(360.f, 260.f)) return false;
        this->setTitle("Saved Macros");

        m_entries = M::listMacros();

        auto prevSpr = ButtonSprite::create("Prev", 60, true, "bigFont.fnt", "GJ_button_02.png", 26.f, .5f);
        auto prevBtn = CCMenuItemExt::createSpriteExtra(prevSpr, [this](CCObject*) {
            m_page--;
            this->rebuild();
        });
        m_buttonMenu->addChildAtPosition(prevBtn, Anchor::Center, {-100.f, -70.f});

        auto nextSpr = ButtonSprite::create("Next", 60, true, "bigFont.fnt", "GJ_button_02.png", 26.f, .5f);
        auto nextBtn = CCMenuItemExt::createSpriteExtra(nextSpr, [this](CCObject*) {
            m_page++;
            this->rebuild();
        });
        m_buttonMenu->addChildAtPosition(nextBtn, Anchor::Center, {100.f, -70.f});

        m_pageLabel = CCLabelBMFont::create("1/1", "goldFont.fnt");
        m_pageLabel->setScale(.5f);
        m_mainLayer->addChildAtPosition(m_pageLabel, Anchor::Center, {0.f, -70.f});

        m_status = CCLabelBMFont::create("Tap a macro to load it", "chatFont.fnt");
        m_status->setScale(.7f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0.f, -96.f});

        auto folderLabel = CCLabelBMFont::create(fmt::format("Folder: {}", M::baseDir().string()).c_str(), "chatFont.fnt");
        folderLabel->limitLabelWidth(330.f, .5f, .2f);
        folderLabel->setOpacity(160);
        m_mainLayer->addChildAtPosition(folderLabel, Anchor::Center, {0.f, -116.f});

        rebuild();
        return true;
    }

public:
    static MacroListPopup* create() {
        auto ret = new MacroListPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- files popup (name / folder / save / list)
class FolderPopup : public Popup {
protected:
    CCLabelBMFont* m_status = nullptr;

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    bool init() {
        if (!Popup::init(340.f, 250.f)) return false;
        this->setTitle("Macro Files");

        // ---- custom folder
        auto toggle = CCMenuItemExt::createToggler(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            [](CCMenuItemToggler* t) {
                M::useFolder = !t->isToggled();
                Mod::get()->setSavedValue("use-folder", M::useFolder);
            });
        toggle->setScale(.7f);
        toggle->toggle(M::useFolder);
        m_buttonMenu->addChildAtPosition(toggle, Anchor::Center, {-140.f, 80.f});

        auto label = CCLabelBMFont::create("Use this folder to save/load macros", "bigFont.fnt");
        label->setScale(.35f);
        label->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {-120.f, 80.f});

        auto input = TextInput::create(300.f, "/storage/emulated/0/Download/macros", "chatFont.fnt");
        input->setMaxCharCount(250);
        input->setString(M::folder, false);
        input->setCallback([](std::string const& s) {
            M::folder = s;
            Mod::get()->setSavedValue("folder", s);
        });
        m_mainLayer->addChildAtPosition(input, Anchor::Center, {0.f, 48.f});

        // ---- macro name
        auto nameLabel = CCLabelBMFont::create("Macro name", "bigFont.fnt");
        nameLabel->setScale(.4f);
        nameLabel->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(nameLabel, Anchor::Center, {-150.f, 12.f});

        auto nameInput = TextInput::create(180.f, "leave empty = level id", "chatFont.fnt");
        nameInput->setMaxCharCount(24);
        nameInput->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ");
        nameInput->setString(M::macroName, false);
        nameInput->setCallback([](std::string const& s) { M::macroName = s; });
        m_mainLayer->addChildAtPosition(nameInput, Anchor::Center, {60.f, 12.f});

        // ---- save / list
        auto saveSpr = ButtonSprite::create("Save now", 110, true, "bigFont.fnt", "GJ_button_01.png", 28.f, .55f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCObject*) {
            if (M::levelKey.empty()) { this->setStatus("Open a level first"); return; }
            auto name = M::fileBase();
            std::error_code ec;
            bool existed = std::filesystem::exists(M::pathFor(name), ec);
            bool ok = M::save(name);
            this->setStatus(ok ? fmt::format("{} '{}' ({} actions)", existed ? "Overwrote" : "Saved as", name, M::actions.size())
                               : std::string("Nothing to save (or folder not writable)"));
        });
        m_buttonMenu->addChildAtPosition(saveBtn, Anchor::Center, {-75.f, -30.f});

        auto listSpr = ButtonSprite::create("Macro list", 110, true, "bigFont.fnt", "GJ_button_02.png", 28.f, .55f);
        auto listBtn = CCMenuItemExt::createSpriteExtra(listSpr, [](CCObject*) {
            if (auto p = MacroListPopup::create()) p->show();
        });
        m_buttonMenu->addChildAtPosition(listBtn, Anchor::Center, {75.f, -30.f});

        m_status = CCLabelBMFont::create(fmt::format("Folder: {}", M::baseDir().string()).c_str(), "chatFont.fnt");
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
            M::notify(fmt::format("Recording as '{}' - restart the level", M::fileBase()), NotificationIcon::Success);
        } else if (m == Mode::Play) {
            M::notify("Macro ready - restart the level", NotificationIcon::Success);
        }
    }

    bool init() {
        if (!Popup::init(340.f, 300.f)) return false;
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

        // ---- macro to play after the skipped attempts (turns "die at %" off)
        auto nextLabel = CCLabelBMFont::create("Then play macro", "bigFont.fnt");
        nextLabel->setScale(.4f);
        nextLabel->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(nextLabel, Anchor::Center, {-150.f, -54.f});

        auto nextInput = TextInput::create(130.f, "macro name", "chatFont.fnt");
        nextInput->setMaxCharCount(24);
        nextInput->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ");
        nextInput->setString(M::nextName, false);
        nextInput->setCallback([](std::string const& s) {
            M::nextName = s;
            Mod::get()->setSavedValue("next-macro", s);
        });
        m_mainLayer->addChildAtPosition(nextInput, Anchor::Center, {85.f, -54.f});

        // ---- button opacity (0 = invisible, it is still clickable at the bottom right)
        auto opLabel = CCLabelBMFont::create("Button opacity", "bigFont.fnt");
        opLabel->setScale(.4f);
        m_mainLayer->addChildAtPosition(opLabel, Anchor::Center, {0.f, -82.f});

        auto slider = Slider::create(this, menu_selector(MacroPopup::onOpacity), .7f);
        slider->setValue(M::opacity());
        m_mainLayer->addChildAtPosition(slider, Anchor::Center, {0.f, -104.f});

        // ---- clear + files + info
        auto clearSpr = ButtonSprite::create("Clear macro", 110, true, "bigFont.fnt", "GJ_button_06.png", 28.f, .5f);
        auto clearBtn = CCMenuItemExt::createSpriteExtra(clearSpr, [this](CCObject*) {
            M::clearMacro();
            this->refresh();
        });
        m_buttonMenu->addChildAtPosition(clearBtn, Anchor::Center, {-115.f, -128.f});

        auto filesSpr = ButtonSprite::create("Files", 80, true, "bigFont.fnt", "GJ_button_02.png", 28.f, .5f);
        auto filesBtn = CCMenuItemExt::createSpriteExtra(filesSpr, [](CCObject*) {
            if (auto p = FolderPopup::create()) p->show();
        });
        m_buttonMenu->addChildAtPosition(filesBtn, Anchor::Center, {-30.f, -128.f});

        m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_infoLabel->setScale(.8f);
        m_mainLayer->addChildAtPosition(m_infoLabel, Anchor::Center, {90.f, -128.f});

        refresh();
        return true;
    }

    void onOpacity(CCObject* sender) {
        float o = static_cast<SliderThumb*>(sender)->getValue();
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

// ---------------------------------------------------------------- bottom-right "Macro" button (pause menu only)
class $modify(MacroPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

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
        this->addChild(menu, 100);
    }
};

$on_mod(Loaded) {
    M::dieEnabled = Mod::get()->getSavedValue<bool>("die-enabled", false);
    M::diePercent = Mod::get()->getSavedValue<float>("die-percent", 50.f);
    M::skipEnabled = Mod::get()->getSavedValue<bool>("skip-enabled", false);
    M::skipN = Mod::get()->getSavedValue<int>("skip-n", 2);
    M::useFolder = Mod::get()->getSavedValue<bool>("use-folder", false);
    M::folder = Mod::get()->getSavedValue<std::string>("folder", std::string());
    M::nextName = Mod::get()->getSavedValue<std::string>("next-macro", std::string());
}
