#pragma once
// Frame Windows: for every click/release in the loaded macro, measure how many frames (240 TPS ticks)
// it can be moved earlier/later and still beat the level, then show the result live while the macro plays.
//
// Included from main.cpp AFTER the M namespace and inject(): it reuses the macro player's own
// data (M::actions) and its click injection, so analysis and live playback trigger clicks the same way.
//
// How a frame is measured: a baseline run records the player's X at the start of every physics tick.
// "Move click i by k frames" = change its trigger X to the X the player has k ticks later/earlier.
// All other clicks keep their original X. The click passes if the level is still beaten.
#include "framewin_core.hpp"
#include <chrono>
#include <climits>
#include <cmath>
#include <numeric>

namespace FW {

using fwcore::Win;
inline constexpr float kDt = (1.f / 240.f) * 1.0001f;   // just over one tick; ticks are counted by the hook

// ------------------------------------------------------------------ settings (saved values, no mod.json)
inline bool hudOn = false;
inline int cpuMs = 40;          // milliseconds of every frame spent simulating
inline int horizon = 1200;      // a shifted click that survives this many ticks past the change counts as passed (0 = to the end)
inline bool reconverge = true;  // stop a test early once the player is back on the baseline path
inline std::string lengthText;
inline bool pendingAnalysis = false; // Arms the analysis for the next level reset/attempt

inline void loadSettings() {
    auto m = Mod::get();
    hudOn = m->getSavedValue<bool>("fw-hud", false);
    cpuMs = std::clamp(m->getSavedValue<int>("fw-cpu-ms", 40), 5, 200);
    horizon = std::max(0, m->getSavedValue<int>("fw-horizon", 1200));
    reconverge = m->getSavedValue<bool>("fw-reconverge", true);
    lengthText = m->getSavedValue<std::string>("fw-length", std::string());
}

// ------------------------------------------------------------------ results for the loaded macro
inline std::vector<Win> results;
inline uint64_t resultsFp = 0;
inline std::string resultsName;
inline bool resultsOk = false;        // a results file exists for this exact macro
inline bool resultsComplete = false;  // every click analysed
inline long long resultsTicks = 0;

inline uint64_t macroHash() {
    uint64_t h = fwcore::kHashSeed;
    for (auto& a : M::actions) h = fwcore::hashRow(h, a.x, a.down, a.p2);
    return h;
}
inline uint64_t fingerprint() {
    if (M::actions.empty()) return 0;
    uint64_t a, b;
    std::memcpy(&a, &M::actions.front().x, 8);
    std::memcpy(&b, &M::actions.back().x, 8);
    return M::actions.size() * 1000003ULL ^ a ^ (b * 31ULL);
}
inline std::filesystem::path resultsPath() { return M::baseDir() / (M::fileBase() + ".fw"); }

inline size_t analysedCount() {
    size_t n = 0;
    for (auto& w : results) if (w.status != 0) ++n;
    return n;
}

inline void ensureResults(bool force = false) {
    const uint64_t fp = fingerprint();
    const std::string name = M::fileBase();
    if (!force && fp == resultsFp && name == resultsName) return;
    resultsFp = fp; resultsName = name;
    resultsOk = false; resultsComplete = false; resultsTicks = 0;
    results.assign(M::actions.size(), Win{});
    if (M::actions.empty()) return;

    std::ifstream in(resultsPath(), std::ios::binary);
    if (!in) return;
    std::stringstream ss; ss << in.rdbuf();
    fwcore::FileData d;
    if (!fwcore::parse(ss.str(), d)) return;
    if (d.hash != macroHash() || d.wins.size() != M::actions.size()) return;   // macro changed since
    results = d.wins; resultsComplete = d.complete; resultsTicks = d.ticks; resultsOk = true;
}

inline bool saveResults(bool complete, long long ticks) {
    fwcore::FileData d;
    d.macro = M::fileBase(); d.hash = macroHash(); d.complete = complete; d.ticks = ticks; d.wins = results;
    std::ofstream out(resultsPath(), std::ios::binary);
    if (!out) return false;
    out << fwcore::serialize(d);
    resultsOk = true; resultsComplete = complete; resultsTicks = ticks;
    return true;
}

// ------------------------------------------------------------------ simulation state
struct TS { float y = 0, vy = 0, y2 = 0, vy2 = 0; uint16_t flags = 0; };
struct Snap { Ref<CheckpointObject> cp; int tick = 0; };

struct Run {
    enum Mode { Trace, Advance, Test };
    bool active = false;      // the analyzer owns the level: the real game loop is frozen
    bool inSim = false;       // inside our own stepping
    bool done = false, died = false, completed = false, success = false;
    Mode mode = Trace;
    std::vector<double> trig;     // trigger X per action for this run
    std::vector<size_t> order;    // firing order of the actions still to fire
    size_t cur = 0;
    int fullCalls = 0;            // physics ticks started (non-half processCommands calls)
    long long calls = 0, halfCalls = 0;
    std::vector<double> P;        // baseline: player X at the start of each tick
    std::vector<TS> base;         // baseline: player state at the start of each tick
    std::vector<int> fire;        // baseline: tick each action fired on (-1 = never)
    int deathTick = -1, lastChange = 0, successTick = INT_MAX, match = 0;
    int horizon = 0;
    bool reconv = true, usesP2 = false;
};
inline Run R;

inline TS sample(PlayLayer* pl) {
    TS s;
    auto p = pl->m_player1;
    s.y = p->getPositionY(); s.vy = (float)p->m_yVelocity;
    s.flags = (uint16_t)((p->m_isShip ? 1 : 0) | (p->m_isBird ? 2 : 0) | (p->m_isBall ? 4 : 0) | (p->m_isDart ? 8 : 0)
            | (p->m_isRobot ? 16 : 0) | (p->m_isSpider ? 32 : 0) | (p->m_isSwing ? 64 : 0) | (p->m_isUpsideDown ? 128 : 0));
    if (pl->m_player2 && pl->m_player2->isVisible()) {
        s.y2 = pl->m_player2->getPositionY(); s.vy2 = (float)pl->m_player2->m_yVelocity; s.flags |= 256;
    }
    return s;
}
inline bool sameState(TS const& a, TS const& b) {
    return a.flags == b.flags && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.vy - b.vy) < 1e-4f
        && std::fabs(a.y2 - b.y2) < 1e-4f && std::fabs(a.vy2 - b.vy2) < 1e-4f;
}

inline void finishRun(bool ok) { R.success = ok; R.done = true; }

// Called from MacroGJ::processCommands (before the original) on every call while analysing.
inline void onCommands(GJBaseGameLayer* self, bool isHalfTick) {
    auto pl = PlayLayer::get();
    if (!R.inSim || R.done || !pl || static_cast<GJBaseGameLayer*>(pl) != self || !pl->m_player1) return;
    ++R.calls;
    int tick;
    if (!isHalfTick) tick = R.fullCalls++;
    else { ++R.halfCalls; tick = std::max(0, R.fullCalls - 1); }
    const double x = pl->m_player1->getPositionX();

    if (!isHalfTick) {
        if (R.mode == Run::Trace) {
            R.P.push_back(x);
            R.base.push_back(sample(pl));
        } else if (R.mode == Run::Test) {
            if (tick >= R.successTick) { finishRun(true); return; }
            if (R.horizon > 0 && tick >= R.lastChange + R.horizon) { finishRun(true); return; }
            if (R.reconv && tick > R.lastChange && tick < (int)R.base.size()) {
                if (sameState(sample(pl), R.base[tick])) { if (++R.match >= 3) { finishRun(true); return; } }
                else R.match = 0;
            }
        }
    }

    // fire every action whose trigger X has been reached, exactly like the macro player does
    while (R.cur < R.order.size() && x >= R.trig[R.order[R.cur]] - M::EPS) {
        const size_t i = R.order[R.cur++];
        inject(self, M::actions[i].down, M::actions[i].p2);
        if (R.mode == Run::Trace) R.fire[i] = tick;
    }
}

// MacroPL::levelComplete: returns true when the completion belongs to a simulated run (swallow it)
inline bool onLevelComplete() {
    if (!R.inSim) return false;
    R.completed = true;
    if (R.mode == Run::Test) finishRun(true); else R.done = true;
    return true;
}

// ------------------------------------------------------------------ analyzer
inline void releaseAll(PlayLayer* pl) {
    inject(pl, false, false);
    if (R.usesP2) inject(pl, false, true);
}

inline std::vector<size_t> sortedOrder(std::vector<size_t> v) {
    std::stable_sort(v.begin(), v.end(), [](size_t a, size_t b) { return R.trig[a] < R.trig[b]; });
    return v;
}

struct Analyzer {
    enum class Ph { Init, Trace, NextClick, Advance, Search, Test, Done, Failed };
    Ph ph = Ph::Init;
    PlayLayer* pl = nullptr;
    Snap root, cur;
    size_t i = 0;
    int advanceTo = 0, curK = 0;
    fwcore::WindowSearch ws;
    long long tests = 0, baseTicks = 0;
    size_t doneThisRun = 0;
    std::string msg, note;
    bool stopRequested = false, complete = false;
    std::chrono::steady_clock::time_point t0;
    // macro-player bookkeeping that our level resets must not disturb
    int sCycle = 0; bool sActive = true, sSawSkip = false, sFailed = false, sSkipEnabled = false;
    int lastRow = -1, lastWindow = 0;

    explicit Analyzer(PlayLayer* p) : pl(p) {}

    Snap takeSnap() {
        Snap s;
        s.cp = Ref<CheckpointObject>(pl->createCheckpoint());
        s.tick = R.fullCalls;
        return s;
    }

    void stepOnce() {
        R.inSim = true;
        pl->update(kDt);
        R.inSim = false;
    }

    // restore a snapshot and set up which actions are still to fire (optionally with one shifted)
    void beginFrom(Snap const& s, int shiftIdx, double newTrig) {
        pl->loadFromCheckpoint(s.cp);
        R.fullCalls = s.tick;
        R.done = R.died = R.completed = R.success = false;
        R.match = 0;
        const size_t n = M::actions.size();
        for (size_t j = 0; j < n; ++j) R.trig[j] = M::actions[j].x;
        if (shiftIdx >= 0) R.trig[shiftIdx] = newTrig;

        bool held[2] = {false, false};
        std::vector<size_t> pending;
        for (size_t j = 0; j < n; ++j) {
            if (R.fire[j] >= 0 && R.fire[j] < s.tick) held[M::actions[j].p2 ? 1 : 0] = M::actions[j].down;
            else pending.push_back(j);
        }
        R.order = sortedOrder(std::move(pending));
        R.cur = 0;

        releaseAll(pl);
        if (held[0]) inject(pl, true, false);
        if (held[1] && R.usesP2) inject(pl, true, true);
    }

    void fail(std::string const& why) { msg = why; ph = Ph::Failed; }

    void init() {
        const size_t n = M::actions.size();
        ensureResults();
        if (results.size() != n) results.assign(n, Win{});

        sCycle = M::cycle; sActive = M::active; sSawSkip = M::sawSkip; sFailed = M::failed; sSkipEnabled = M::skipEnabled;
        M::skipEnabled = false; M::sawSkip = false;   // our resets must not count as attempts or swap the macro

        R = Run{};
        R.active = true; R.horizon = horizon; R.reconv = reconverge;
        R.trig.resize(n); R.fire.assign(n, -1);
        for (size_t j = 0; j < n; ++j) { R.trig[j] = M::actions[j].x; if (M::actions[j].p2) R.usesP2 = true; }

        pl->setVisible(false);
        pl->resetLevelFromStart();
        R.fullCalls = 0;
        root = takeSnap();

        std::vector<size_t> all(n);
        std::iota(all.begin(), all.end(), size_t(0));
        R.order = sortedOrder(std::move(all));
        R.cur = 0; R.mode = Run::Trace;
        releaseAll(pl);
        t0 = std::chrono::steady_clock::now();
        ph = Ph::Trace;
    }

    void endTrace() {
        baseTicks = R.fullCalls;
        if (!R.completed && !R.died) { fail("The macro run never ended (no death and no level complete)."); return; }
        R.successTick = R.completed ? INT_MAX : R.deathTick;
        if (!R.completed) {
            note = fmt::format("Warning: this macro dies in the analyzer at tick {} (x={:.1f}). "
                               "Clicks after that can't be analysed. If it beats the level when you play it, "
                               "the analyzer and the real game disagree (TPS or other mods?).",
                               R.deathTick, R.P.empty() ? 0.0 : R.P.back());
        }
        cur = root;
        i = 0;
        ph = Ph::NextClick;
    }

    void nextClick() {
        const size_t n = M::actions.size();
        while (i < n && results[i].status != 0) ++i;                 // resume: skip what is already done
        if (i >= n) { finalize(); return; }
        if (R.fire[i] < 0) { results[i].status = 4; ++i; return; }   // never fires in the baseline run
        const int need = std::max(0, R.fire[i] - 12);
        if (cur.tick + 30 < need) {                                  // move the baseline closer to this click
            beginFrom(cur, -1, 0.0);
            R.mode = Run::Advance;
            advanceTo = need;
            ph = Ph::Advance;
        } else {
            ws = fwcore::WindowSearch{};
            ph = Ph::Search;
        }
    }

    void startTest(int k) {
        const double nt = (k == 0) ? M::actions[i].x : R.P[R.fire[i] + k];
        beginFrom(cur, (int)i, nt);
        R.mode = Run::Test;
        R.lastChange = R.fire[i] + std::max(k, 0);
        R.match = 0;
        curK = k;
        ph = Ph::Test;
    }

    void searchStep() {
        int k = 0;
        auto can = [&](int kk) {
            const int ft = R.fire[i] + kk;
            return ft >= cur.tick && ft >= 0 && ft < (int)R.P.size();
        };
        if (ws.next(k, can)) startTest(k);
        else finishClick();
    }

    void testDone() {
        ++tests;
        ws.report(curK, R.success);
        ph = Ph::Search;
    }

    void finishClick() {
        results[i] = ws.result();
        lastRow = (int)i; lastWindow = results[i].status == 1 ? results[i].window : (results[i].status == 2 ? 11 : 0);
        ++i; ++doneThisRun;
        if (doneThisRun % 20 == 0) saveResults(false, baseTicks);
        ph = Ph::NextClick;
    }

    void finalize() {
        complete = true;
        for (auto& w : results) if (w.status == 0) complete = false;
        ph = Ph::Done;
    }

    // run for up to `budgetMs`; returns true when finished (Done or Failed)
    bool pump(double budgetMs) {
        const auto start = std::chrono::steady_clock::now();
        while (ph != Ph::Done && ph != Ph::Failed) {
            if (stopRequested) { complete = false; ph = Ph::Done; break; }
            switch (ph) {
                case Ph::Init: init(); break;
                case Ph::Trace:
                    stepOnce();
                    if (R.done) endTrace();
                    else if (R.fullCalls > 240 * 60 * 30) fail("The macro run is longer than 30 minutes of ticks; stopped.");
                    break;
                case Ph::NextClick: nextClick(); break;
                case Ph::Advance:
                    stepOnce();
                    if (R.done) fail("The baseline run died while moving to a click: restoring checkpoints isn't reproducing the macro on this build.");
                    else if (R.fullCalls >= advanceTo) { cur = takeSnap(); ws = fwcore::WindowSearch{}; ph = Ph::Search; }
                    break;
                case Ph::Search: searchStep(); break;
                case Ph::Test:
                    stepOnce();
                    if (R.done) testDone();
                    else if (R.fullCalls > (int)R.P.size() + 600) { R.success = false; R.done = true; testDone(); }  // ran past the baseline end without finishing
                    break;
                default: break;
            }
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs) break;
        }
        return ph == Ph::Done || ph == Ph::Failed;
    }

    // give the level back and save
    void restore(bool alive) {
        R.inSim = false;
        R.active = false;
        if (alive) {
            pl->setVisible(true);
            pl->resetLevelFromStart();
        }
        M::cycle = sCycle; M::active = sActive; M::sawSkip = sSawSkip; M::failed = sFailed; M::skipEnabled = sSkipEnabled;
    }

    std::string progress() const {
        const size_t n = M::actions.size();
        size_t done = 0;
        for (auto& w : results) if (w.status != 0) ++done;
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::string eta = "...";
        if (doneThisRun > 0 && done < n) {
            const double left = el / (double)doneThisRun * (double)(n - done);
            eta = left >= 90 ? fmt::format("{:.0f} min", left / 60.0) : fmt::format("{:.0f} s", left);
        }
        switch (ph) {
            case Ph::Init: case Ph::Trace: return fmt::format("Recording the baseline run... tick {}", R.fullCalls);
            default: break;
        }
        return fmt::format("{} / {} clicks   tests {}   elapsed {:.0f} s   left ~{}{}", done, n, tests, el, eta,
                           lastRow >= 0 ? fmt::format("\nlast: row {} = {} frame{}", lastRow, lastWindow >= 11 ? std::string(">10") : std::to_string(lastWindow), lastWindow == 1 ? "" : "s") : std::string());
    }
};

// ------------------------------------------------------------------ progress overlay (hides the level)
class FWOverlay : public CCLayer {
protected:
    PlayLayer* m_pl = nullptr;
    std::unique_ptr<Analyzer> m_a;
    CCLabelBMFont* m_title = nullptr;
    CCLabelBMFont* m_info = nullptr;
    ButtonSprite* m_stopBtn = nullptr;
    bool m_finished = false;
    float m_labelTimer = 0.f;

    bool init(PlayLayer* pl) {
        if (!CCLayer::init()) return false;
        m_pl = pl;
        R.active = true;                              // freeze the real game before anything else
        m_a = std::make_unique<Analyzer>(pl);

        this->setTouchEnabled(true);
        this->setTouchPriority(-1000);                // swallow every touch: the popup/pause menu below must not react
        this->setKeypadEnabled(true);

        auto win = CCDirector::get()->getWinSize();
        this->addChild(CCLayerColor::create({8, 8, 14, 255}, win.width, win.height), -1);

        m_title = CCLabelBMFont::create("Counting frame windows...", "bigFont.fnt");
        m_title->setScale(.55f);
        m_title->setPosition({win.width / 2, win.height / 2 + 40.f});
        this->addChild(m_title);

        m_info = CCLabelBMFont::create("starting...", "chatFont.fnt");
        m_info->setScale(.9f);
        m_info->setPosition({win.width / 2, win.height / 2 - 4.f});
        this->addChild(m_info);

        m_stopBtn = ButtonSprite::create("Stop & save", 110, true, "bigFont.fnt", "GJ_button_06.png", 30.f, .6f);
        m_stopBtn->setPosition({win.width / 2, win.height / 2 - 58.f});
        this->addChild(m_stopBtn);

        this->scheduleUpdate();
        return true;
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (m_stopBtn && m_stopBtn->boundingBox().containsPoint(this->convertToNodeSpace(touch->getLocation()))) {
            m_a->stopRequested = true;
        }
        return true;
    }
    void keyBackClicked() override { m_a->stopRequested = true; }

    void leave() {
        m_finished = true;
        this->removeFromParent();
    }

    void update(float dt) override {
        if (m_finished) return;
        if (PlayLayer::get() != m_pl) {               // left the level: nothing to restore
            m_a->restore(false);
            leave();
            return;
        }

        const bool finished = m_a->pump((double)cpuMs);

        m_labelTimer += dt;
        if (m_labelTimer > 0.2f || finished) {
            m_labelTimer = 0.f;
            m_info->setString(m_a->progress().c_str());
        }
        if (!finished) return;

        // ---- finished: save, restore, report
        const bool failed = m_a->ph == Analyzer::Ph::Failed;
        std::string text;
        if (failed) {
            text = m_a->msg;
        } else {
            saveResults(m_a->complete, m_a->baseTicks);
            size_t ok = 0, wide = 0, unstable = 0, never = 0;
            for (auto& w : results) {
                if (w.status == 1) ++ok; else if (w.status == 2) ++wide; else if (w.status == 3) ++unstable; else if (w.status == 4) ++never;
            }
            const double secs = (double)m_a->baseTicks / fwcore::kTps;
            text = fmt::format("{}\nRun length measured: {} ticks = {:.2f} s ({:.6f}% per frame at constant speed)\n"
                               "Windows 1-10 frames: {}   wider than 10: {}   unstable: {}   never fired: {}\n"
                               "Tests run: {}   half-tick calls: {}\n{}\nSaved: {}",
                               m_a->complete ? "Done. Turn on \"Show windows\" in the Windows popup, then play the macro."
                                             : "Stopped early. Progress is saved; press Resume to continue.",
                               m_a->baseTicks, secs, secs > 0 ? 100.0 / (double)m_a->baseTicks : 0.0,
                               ok, wide, unstable, never, m_a->tests, R.halfCalls, m_a->note, resultsPath().string());
        }
        log::info("Frame windows result:\n{}", text);
        m_a->restore(true);
        FLAlertLayer::create("Frame Windows", text, "OK")->show();
        leave();
    }

public:
    static FWOverlay* create(PlayLayer* pl) {
        auto ret = new FWOverlay();
        if (ret->init(pl)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }

    ~FWOverlay() override {
        m_a.reset();
        R.active = false;
        R.inSim = false;
    }
};

// ------------------------------------------------------------------ live HUD (top-left while a macro plays)
inline CCNode* hudNode = nullptr;
inline CCLabelBMFont* hudLabel[2][fwcore::kBuckets] = {};
inline int hudCount[2][fwcore::kBuckets] = {};
inline int hudShown[2][fwcore::kBuckets];
inline float hudFlash[2][fwcore::kBuckets] = {};
inline size_t hudIdx = SIZE_MAX;
inline uint64_t hudFp = 0;

inline void attachHUD(PlayLayer* pl) {
    hudNode = CCNode::create();
    hudNode->setID("fw-hud"_spr);
    auto win = CCDirector::get()->getWinSize();
    const char* heads[2] = {"CLICK", "RELEASE"};
    for (int c = 0; c < 2; ++c) {
        const float x = 6.f + c * 62.f;
        auto h = CCLabelBMFont::create(heads[c], "goldFont.fnt");
        h->setScale(.3f); h->setAnchorPoint({0.f, 1.f}); h->setPosition({x, win.height - 22.f});
        hudNode->addChild(h);
        for (int b = 0; b < fwcore::kBuckets; ++b) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setScale(.27f); l->setAnchorPoint({0.f, 1.f}); l->setPosition({x, win.height - 34.f - b * 10.f});
            hudNode->addChild(l);
            hudLabel[c][b] = l;
            hudShown[c][b] = -1; hudCount[c][b] = 0; hudFlash[c][b] = 0.f;
        }
    }
    hudNode->setVisible(false);
    hudIdx = SIZE_MAX;
    pl->m_uiLayer->addChild(hudNode, 50);
}

// the macro player just fired action `idx`
inline void onFire(size_t idx) {
    if (!hudNode || idx >= results.size() || results[idx].status != 1) return;
    const int c = M::actions[idx].down ? 0 : 1;
    hudFlash[c][fwcore::bucketOf(results[idx].window)] = .8f;
}

inline void updateHUD(PlayLayer*, float dt) {
    if (R.inSim || !hudNode) return;
    bool show = hudOn && M::ready && M::mode == Mode::Play && M::active && !R.active;
    if (show) { ensureResults(); show = resultsOk && resultsComplete; }
    hudNode->setVisible(show);
    if (!show) { hudIdx = SIZE_MAX; return; }

    const uint64_t fp = fingerprint();
    if (M::idx != hudIdx || fp != hudFp) {            // recount: covers restarts and practice-mode respawns too
        hudIdx = M::idx; hudFp = fp;
        for (auto& row : hudCount) for (auto& v : row) v = 0;
        for (size_t j = 0; j < M::idx && j < results.size(); ++j) {
            if (results[j].status != 1) continue;
            hudCount[M::actions[j].down ? 0 : 1][fwcore::bucketOf(results[j].window)]++;
        }
    }
    for (int c = 0; c < 2; ++c) {
        for (int b = 0; b < fwcore::kBuckets; ++b) {
            auto l = hudLabel[c][b];
            if (!l) continue;
            if (hudShown[c][b] != hudCount[c][b]) {
                hudShown[c][b] = hudCount[c][b];
                l->setString(fmt::format("{}: {}", fwcore::bucketName(b), hudCount[c][b]).c_str());
            }
            float& f = hudFlash[c][b];
            if (f > 0.f) f = std::max(0.f, f - dt);
            const float k = std::min(1.f, f / .8f);
            l->setScale(.27f * (1.f + .45f * k));
            l->setColor(f > 0.f ? ccColor3B{255, 230, 80} : ccColor3B{255, 255, 255});
        }
    }
}

// ------------------------------------------------------------------ popup
class FramePopup : public Popup {
protected:
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_lenInfo = nullptr;
    CCMenuItemToggler* m_hudToggle = nullptr;
    CCMenuItemSpriteExtra* m_runBtn = nullptr;
    ButtonSprite* m_runSpr = nullptr;

    static void err(std::string const& t) { Notification::create(t, NotificationIcon::Error, 2.5f)->show(); }

    void refresh() {
        ensureResults();
        const size_t n = M::actions.size(), done = analysedCount();
        std::string s;
        if (n == 0) s = "No macro loaded. Load one from the Macro popup > Files.";
        else if (resultsComplete) s = fmt::format("'{}': {} actions, all analysed.\nMeasured run: {} ticks = {:.2f} s", M::fileBase(), n, resultsTicks, (double)resultsTicks / fwcore::kTps);
        else if (resultsOk && done > 0) s = fmt::format("'{}': {} actions, {} analysed (partial).", M::fileBase(), n, done);
        else s = fmt::format("'{}': {} actions, not analysed yet.", M::fileBase(), n);
        m_status->setString(s.c_str());

        if (m_hudToggle) {
            m_hudToggle->setEnabled(resultsComplete);
            if (!resultsComplete && hudOn) { hudOn = false; m_hudToggle->toggle(false); }
        }

        double secs = 0;
        if (fwcore::parseLength(lengthText, secs)) {
            m_lenInfo->setString(fmt::format("= {:.0f} frames, {:.6f}% per frame", fwcore::ticksFor(secs), fwcore::percentPerFrame(secs)).c_str());
        } else {
            m_lenInfo->setString("optional cross-check, e.g. 1:28");
        }
    }

    void onAnalyze() {
        auto pl = PlayLayer::get();
        if (!pl) return err("Open this from the pause menu inside the level.");
        if (M::actions.empty()) return err("Load a macro first.");
        if (M::mode == Mode::Record) return err("Turn recording off first (Macro popup > Off).");
        if (R.active) return;

        // Arm analysis for the next attempt/restart
        pendingAnalysis = true;
        Notification::create("Analysis armed! Restart the level to begin.", NotificationIcon::Success, 2.5f)->show();
        this->onClose(nullptr);
    }

    void onDelete() {
        std::error_code ec;
        std::filesystem::remove(resultsPath(), ec);
        ensureResults(true);
        hudOn = false;
        Mod::get()->setSavedValue("fw-hud", false);
        refresh();
    }

    void addToggle(char const* text, bool on, float y, std::function<void(bool)> cb, CCMenuItemToggler** out = nullptr) {
        auto t = CCMenuItemExt::createToggler(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            [cb](CCMenuItemToggler* tg) { cb(!tg->isToggled()); });
        t->setScale(.7f);
        t->toggle(on);
        m_buttonMenu->addChildAtPosition(t, Anchor::Center, {-140.f, y});
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(.38f); l->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(l, Anchor::Center, {-120.f, y});
        if (out) *out = t;
    }

    void addNumber(char const* text, int value, float y, int maxChars, std::function<void(int)> cb) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(.38f); l->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(l, Anchor::Center, {-150.f, y});
        auto in = TextInput::create(70.f, "0", "bigFont.fnt");
        in->setFilter("0123456789");
        in->setMaxCharCount(maxChars);
        in->setString(std::to_string(value), false);
        in->setCallback([cb](std::string const& s) {
            auto r = utils::numFromString<int>(s);
            if (r.isOk()) cb(r.unwrap());
        });
        m_mainLayer->addChildAtPosition(in, Anchor::Center, {120.f, y});
    }

    bool init() {
        if (!Popup::init(340.f, 300.f)) return false;
        this->setTitle("Frame Windows");

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(.75f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0.f, 96.f});

        // level length cross-check
        auto lenLabel = CCLabelBMFont::create("Level length", "bigFont.fnt");
        lenLabel->setScale(.38f); lenLabel->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(lenLabel, Anchor::Center, {-150.f, 62.f});
        auto lenInput = TextInput::create(70.f, "1:28", "bigFont.fnt");
        lenInput->setFilter("0123456789:.");
        lenInput->setMaxCharCount(8);
        lenInput->setString(lengthText, false);
        lenInput->setCallback([this](std::string const& s) {
            lengthText = s;
            Mod::get()->setSavedValue("fw-length", s);
            refresh();
        });
        m_mainLayer->addChildAtPosition(lenInput, Anchor::Center, {-30.f, 62.f});
        m_lenInfo = CCLabelBMFont::create("", "chatFont.fnt");
        m_lenInfo->setScale(.65f);
        m_lenInfo->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(m_lenInfo, Anchor::Center, {15.f, 62.f});

        // analyze / resume + delete
        m_runSpr = ButtonSprite::create("Analyze / Resume", 140, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .6f);
        m_runBtn = CCMenuItemExt::createSpriteExtra(m_runSpr, [this](CCObject*) { this->onAnalyze(); });
        m_buttonMenu->addChildAtPosition(m_runBtn, Anchor::Center, {-70.f, 24.f});
        auto delSpr = ButtonSprite::create("Delete results", 120, true, "bigFont.fnt", "GJ_button_06.png", 30.f, .5f);
        auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this](CCObject*) { this->onDelete(); });
        m_buttonMenu->addChildAtPosition(delBtn, Anchor::Center, {70.f, 24.f});

        // show windows (only available once the counting is finished)
        addToggle("Show windows (after analysis)", hudOn, -12.f, [](bool on) {
            hudOn = on;
            Mod::get()->setSavedValue("fw-hud", on);
        }, &m_hudToggle);

        addNumber("CPU ms per frame", cpuMs, -44.f, 3, [](int v) {
            cpuMs = std::clamp(v, 5, 200);
            Mod::get()->setSavedValue("fw-cpu-ms", cpuMs);
        });
        addNumber("Check ticks (0=all)", horizon, -74.f, 5, [](int v) {
            horizon = std::max(0, v);
            Mod::get()->setSavedValue("fw-horizon", horizon);
        });
        addToggle("Stop test when back on path", reconverge, -104.f, [](bool on) {
            reconverge = on;
            Mod::get()->setSavedValue("fw-reconverge", on);
        });

        auto help = CCLabelBMFont::create("Analyze runs the level offscreen at full speed.", "chatFont.fnt");
        help->setScale(.6f);
        m_mainLayer->addChildAtPosition(help, Anchor::Center, {0.f, -132.f});

        refresh();
        return true;
    }

public:
    static FramePopup* create() {
        auto ret = new FramePopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

inline CCMenuItemSpriteExtra* makePauseButton() {
    auto spr = ButtonSprite::create("Windows", 70, true, "bigFont.fnt", "GJ_button_02.png", 30.f, .7f);
    auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCObject*) {
        if (auto p = FramePopup::create()) p->show();
    });
    btn->setPosition({0.f, 34.f});
    return btn;
}

} // namespace FW

// Freezes the real game loop while the analyzer runs, and turns deaths inside simulated runs into a flag.
class $modify(FWPlayLayer, PlayLayer) {
    void update(float dt) {
        if (FW::R.active && !FW::R.inSim) return;            // analyzer owns the level: freeze the real loop
        PlayLayer::update(dt);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        
        // Trigger the armed analysis on the fresh attempt reset (using FW:: namespace prefix)
        if (FW::pendingAnalysis) {
            FW::pendingAnalysis = false;
            if (auto pl = PlayLayer::get()) {
                if (auto ov = FW::FWOverlay::create(pl)) {
                    CCScene::get()->addChild(ov, 5000);
                }
            }
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (FW::R.inSim) {                               // simulated death: record it, show nothing
            if (!FW::R.done) {
                FW::R.died = true;
                FW::R.deathTick = std::max(0, FW::R.fullCalls - 1);
                FW::R.done = true;                       // success stays false
            }
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }
};
