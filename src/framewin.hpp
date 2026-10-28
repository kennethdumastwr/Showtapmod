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
inline bool showWide = true;           // HUD: extra dim line counting clicks whose window is wider than 10
inline bool pendingAnalysis = false;   // armed from the popup; the analysis starts on the next level reset

inline void loadSettings() {
    auto m = Mod::get();
    hudOn = m->getSavedValue<bool>("fw-hud", false);
    cpuMs = std::clamp(m->getSavedValue<int>("fw-cpu-ms", 40), 5, 200);
    horizon = std::max(0, m->getSavedValue<int>("fw-horizon", 1200));
    reconverge = m->getSavedValue<bool>("fw-reconverge", true);
    lengthText = m->getSavedValue<std::string>("fw-length", std::string());
    showWide = m->getSavedValue<bool>("fw-show-wide", true);
}

// ------------------------------------------------------------------ results for the loaded macro
inline std::vector<Win> results;
inline uint64_t resultsFp = 0;
inline std::string resultsName;
inline bool resultsOk = false;        // a results file exists for this exact macro
inline bool resultsComplete = false;  // every click analysed
inline long long resultsTicks = 0;
inline uint64_t resultsSig = 0;       // signature of the baseline run these results belong to

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
    resultsOk = false; resultsComplete = false; resultsTicks = 0; resultsSig = 0;
    results.assign(M::actions.size(), Win{});
    if (M::actions.empty()) return;

    std::ifstream in(resultsPath(), std::ios::binary);
    if (!in) return;
    std::stringstream ss; ss << in.rdbuf();
    fwcore::FileData d;
    if (!fwcore::parse(ss.str(), d)) return;
    if (d.hash != macroHash() || d.wins.size() != M::actions.size()) return;   // macro changed since
    if (!d.hasSig) return;   // written by an older version (could be a broken run saved as 'complete'): not trusted
    results = d.wins; resultsComplete = d.complete; resultsTicks = d.ticks; resultsSig = d.sig; resultsOk = true;
}

inline bool saveResults(bool complete, long long ticks, uint64_t sig) {
    fwcore::FileData d;
    d.macro = M::fileBase(); d.hash = macroHash(); d.complete = complete; d.ticks = ticks; d.sig = sig; d.wins = results;
    std::ofstream out(resultsPath(), std::ios::binary);
    if (!out) return false;
    out << fwcore::serialize(d);
    resultsOk = true; resultsComplete = complete; resultsTicks = ticks; resultsSig = sig;
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
    int killerId = 0;             // object that killed the player in the last simulated death
    double killerX = 0, killerY = 0, playerY = 0;
    long long ignoredCompletes = 0;   // level-complete calls ignored because the player was nowhere near the end
    bool tickOnAll = false;       // some builds only ever report half ticks: then every processCommands call is a tick
    long long updates = 0;        // update steps we have issued
    bool strict = false;          // k=0 test: the first ticks after a restore must match the baseline exactly
    int strictUntil = 0;
    bool k0Mismatch = false;
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
inline bool sameStateTol(TS const& a, TS const& b, float tol) {
    return a.flags == b.flags && std::fabs(a.y - b.y) < tol && std::fabs(a.vy - b.vy) < tol
        && std::fabs(a.y2 - b.y2) < tol && std::fabs(a.vy2 - b.vy2) < tol;
}
inline bool sameState(TS const& a, TS const& b) { return sameStateTol(a, b, 1e-4f); }

inline void finishRun(bool ok) { R.success = ok; R.done = true; }

// Called from MacroGJ::processCommands (before the original) on every call while analysing.
inline void onCommands(GJBaseGameLayer* self, bool isHalfTick) {
    auto pl = PlayLayer::get();
    if (!R.inSim || R.done || !pl || static_cast<GJBaseGameLayer*>(pl) != self || !pl->m_player1) return;
    ++R.calls;
    const bool isTick = !isHalfTick || R.tickOnAll;
    int tick;
    if (isTick) tick = R.fullCalls++;
    else { ++R.halfCalls; tick = std::max(0, R.fullCalls - 1); }
    const double x = pl->m_player1->getPositionX();

    if (isTick) {
        if (R.mode == Run::Trace) {
            R.P.push_back(x);
            R.base.push_back(sample(pl));
        } else if (R.mode == Run::Test) {
            if (R.strict && tick < R.strictUntil && tick < (int)R.base.size() && !sameStateTol(sample(pl), R.base[tick], 2e-3f)) {
                R.k0Mismatch = true; finishRun(false); return;      // the restore did not reproduce the baseline
            }
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

// MacroPL::levelComplete: returns true when the completion belongs to a simulated run (swallow it).
// A real completion happens at ~100%. A completion reported while the player is nowhere near the end is
// stale state left over from the previous attempt: ignore it and keep running.
inline bool onLevelComplete() {
    if (!R.inSim) return false;
    auto pl = PlayLayer::get();
    if (pl && pl->getCurrentPercent() < 90.f) { ++R.ignoredCompletes; return true; }
    R.completed = true;
    if (R.mode == Run::Test) finishRun(true); else R.done = true;
    return true;
}

// FWPlayLayer::destroyPlayer during a simulated run
inline void onDeath(GameObject* obj) {
    if (R.done) return;
    R.died = true;
    R.deathTick = std::max(0, R.fullCalls - 1);
    R.killerId = obj ? obj->m_objectID : 0;
    if (obj) { R.killerX = obj->getPositionX(); R.killerY = obj->getPositionY(); }
    if (auto pl = PlayLayer::get()) if (pl->m_player1) R.playerY = pl->m_player1->getPositionY();
    R.done = true;                                   // success stays false
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
    enum class Ph { Init, Probe, Trace, NextClick, Advance, Search, Test, Done, Failed };
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
    long long pUpdates = 0, pCalls = 0, pHalf = 0, traceStartUpdates = 0;
    int pFull = 0;
    uint64_t sig = 0;                    // signature of this baseline run (ticks + when every click fired)
    std::vector<char> noop;              // actions that change nothing (release of a button that isn't held)
    bool rootOnly = false, notedFallback = false;
    long long restoreFallbacks = 0;
    std::vector<std::string> attempts;   // how each baseline attempt ended (for the failure message)
    int retries = 0;

    Ref<CheckpointObject> fresh;   // the game's own just-reset state, taken at the end of the player's restart
    explicit Analyzer(PlayLayer* p, CheckpointObject* f = nullptr) : pl(p), fresh(f) {}

    // put the level at its start: the snapshot of the game's own reset if we have one, else reset it ourselves
    void toStart() {
        if (fresh) pl->loadFromCheckpoint(fresh);
        else pl->resetLevelFromStart();
    }

    Snap takeSnap() {
        Snap s;
        s.cp = Ref<CheckpointObject>(pl->createCheckpoint());
        s.tick = R.fullCalls;
        return s;
    }

    void stepOnce() {
        R.inSim = true;
        ++R.updates;
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
        if (resultsComplete) results.assign(n, Win{});    // Analyze on a finished analysis starts it over
        else for (auto& w : results) if (w.status == 3 || w.status == 4 || w.status == 5) w = Win{};   // retry anything that wasn't a real result

        sCycle = M::cycle; sActive = M::active; sSawSkip = M::sawSkip; sFailed = M::failed; sSkipEnabled = M::skipEnabled;
        M::skipEnabled = false; M::sawSkip = false;   // our resets must not count as attempts or swap the macro

        R = Run{};
        R.active = true; R.horizon = horizon; R.reconv = reconverge;
        R.trig.resize(n); R.fire.assign(n, -1);
        for (size_t j = 0; j < n; ++j) { R.trig[j] = M::actions[j].x; if (M::actions[j].p2) R.usesP2 = true; }

        // Probe first, with the level still visible: does the game actually run physics ticks when we step it,
        // and does it report them as full ticks or only half ticks?
        toStart();
        R.fullCalls = 0;
        R.mode = Run::Advance;           // nothing to inject, nothing recorded
        t0 = std::chrono::steady_clock::now();
        ph = Ph::Probe;
    }

    void endProbe() {
        pUpdates = R.updates; pCalls = R.calls; pHalf = R.halfCalls; pFull = R.fullCalls;
        if (pCalls == 0) {
            fail(fmt::format("The game was stepped {} times but never processed a physics tick (processCommands was never called).\n"
                             "Close the pause menu and don't use Practice-mode checkpoints, then try again. "
                             "If it still happens, send me this message.", pUpdates));
            return;
        }
        R.tickOnAll = (pFull == 0);      // only half ticks seen: count every call as a tick
        startBaseline();
    }

    // (re)start the baseline run from a fresh level reset
    void startBaseline() {
        R.done = R.died = R.completed = R.success = false;
        R.fullCalls = 0; R.calls = 0; R.halfCalls = 0;
        R.killerId = 0; R.ignoredCompletes = 0;
        R.P.clear(); R.base.clear();
        std::fill(R.fire.begin(), R.fire.end(), -1);
        for (size_t j = 0; j < M::actions.size(); ++j) R.trig[j] = M::actions[j].x;

        toStart();
        pl->setVisible(false);           // from here on you only see the progress screen
        R.fullCalls = 0;
        root = takeSnap();

        std::vector<size_t> all(M::actions.size());
        std::iota(all.begin(), all.end(), size_t(0));
        R.order = sortedOrder(std::move(all));
        R.cur = 0; R.mode = Run::Trace;
        releaseAll(pl);
        traceStartUpdates = R.updates;
        t0 = std::chrono::steady_clock::now();
        ph = Ph::Trace;
    }

    void endTrace() {
        baseTicks = R.fullCalls;
        const size_t n = M::actions.size();
        size_t fired = 0;
        for (int f : R.fire) if (f >= 0) ++fired;
        const float pct = pl->getCurrentPercent();
        const double endX = R.P.empty() ? 0.0 : R.P.back();

        std::string how = R.completed ? "completed the level"
            : (R.died ? fmt::format("died to object {} at ({:.0f}, {:.0f}) with the player at y={:.0f}", R.killerId, R.killerX, R.killerY, R.playerY) : "never ended");
        attempts.push_back(fmt::format("attempt {}: {} at tick {} (x={:.1f}, {:.1f}%), {} of {} clicks fired, {} bogus level-complete calls ignored",
                                       attempts.size() + 1, how, R.fullCalls, endX, pct, fired, n, R.ignoredCompletes));

        if (!R.completed && !R.died) { fail("The macro run never ended (no death and no level complete).\n" + joinAttempts()); return; }

        // A macro that beats (or nearly beats) the level can't end within the first moments. That is leftover
        // state from the previous attempt: reset again and retry before giving up.
        const bool tooEarly = R.fullCalls < 60 && !(R.completed && pct >= 90.f);
        if (tooEarly) {
            if (retries < 3) { ++retries; startBaseline(); return; }
            fail("The baseline run keeps ending within the first moments after a reset, so there is nothing to analyse.\n"
                 "What ended it:\n" + joinAttempts() +
                 "\nTry restarting the level once yourself and arming the analysis again. If it keeps happening, send me this message.");
            return;
        }
        if (fired == 0) {
            fail("None of the macro's clicks fired in the baseline run, so nothing can be analysed.\n" + joinAttempts());
            return;
        }

        // Signature of this baseline. Saved results only count if they were computed against the same one.
        sig = fwcore::kHashSeed;
        sig = fwcore::hashRow(sig, (double)R.fullCalls, R.completed, false);
        for (size_t j = 0; j < n; ++j) sig = fwcore::hashRow(sig, (double)R.fire[j], false, false);
        if (resultsOk && resultsSig != 0 && resultsSig != sig) {
            results.assign(n, Win{});
            note += "Old results were computed against a different baseline run and were discarded.\n";
        }

        // A release of a button that isn't held (the usual first row of a macro) does nothing: not a click.
        noop.assign(n, 0);
        {
            bool held[2] = {false, false};
            for (size_t idx : R.order) {
                if (R.fire[idx] < 0) continue;
                bool& h = held[M::actions[idx].p2 ? 1 : 0];
                if (!M::actions[idx].down && !h) noop[idx] = 1;
                h = M::actions[idx].down;
            }
        }

        R.successTick = R.completed ? INT_MAX : R.deathTick;
        if (!R.completed) {
            note = fmt::format("Warning: this macro dies in the analyzer at tick {} (x={:.1f}, {:.1f}%, object {}). "
                               "Clicks after that can't be analysed. If it beats the level when you play it, "
                               "the analyzer and the real game disagree (TPS or other mods?).",
                               R.deathTick, endX, pct, R.killerId);
        }
        cur = root;
        i = 0;
        ph = Ph::NextClick;
    }

    std::string joinAttempts() const {
        std::string out;
        for (auto& a : attempts) out += a + "\n";
        return out;
    }

    void nextClick() {
        const size_t n = M::actions.size();
        while (i < n && results[i].status != 0) ++i;                 // resume: skip what is already done
        if (i >= n) { finalize(); return; }
        if (R.fire[i] < 0) { results[i].status = 4; ++i; return; }   // never fires in the baseline run
        if (noop[i]) { results[i] = Win{}; results[i].status = 5; ++i; return; }   // does nothing: not a click
        const int need = std::max(0, R.fire[i] - 12);
        if (rootOnly) { cur = root; ws = fwcore::WindowSearch{}; ph = Ph::Search; return; }
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
        R.strict = (k == 0 && !rootOnly && cur.tick > 0);     // verify the snapshot restore itself on the unshifted test
        R.strictUntil = cur.tick + 4;
        R.k0Mismatch = false;
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
        if (curK == 0 && !R.success && !rootOnly && cur.tick > 0) {
            // The unshifted click failed from a snapshot, so restoring snapshots doesn't reproduce the baseline here.
            // Fall back to starting every test from tick 0: slower, but exact.
            rootOnly = true; ++restoreFallbacks;
            if (!notedFallback) { notedFallback = true; note += "Checkpoint restores did not reproduce the baseline, so every test starts from tick 0 (slower but exact).\n"; }
            cur = root; ws = fwcore::WindowSearch{}; ph = Ph::Search;
            return;
        }
        ws.report(curK, R.success);
        ph = Ph::Search;
    }

    void finishClick() {
        results[i] = ws.result();
        lastRow = (int)i; lastWindow = results[i].status == 1 ? results[i].window : (results[i].status == 2 ? 11 : 0);
        ++i; ++doneThisRun;
        if (doneThisRun % 20 == 0) saveResults(false, baseTicks, sig);
        ph = Ph::NextClick;
    }

    void finalize() {
        complete = true;
        size_t real = 0;
        for (auto& w : results) { if (w.status == 0) complete = false; if (w.status == 1 || w.status == 2) ++real; }
        if (real == 0) {   // nothing was actually measured: never present this as a finished analysis
            complete = false;
            note += "No click could be analysed (see the counts), so this is NOT saved as complete.\n";
        }
        ph = Ph::Done;
    }

    // run for up to `budgetMs`; returns true when finished (Done or Failed)
    bool pump(double budgetMs) {
        const auto start = std::chrono::steady_clock::now();
        while (ph != Ph::Done && ph != Ph::Failed) {
            if (stopRequested) { complete = false; ph = Ph::Done; break; }
            switch (ph) {
                case Ph::Init: init(); break;
                case Ph::Probe:
                    stepOnce();
                    if (R.done || R.updates >= 90) endProbe();
                    break;
                case Ph::Trace:
                    stepOnce();
                    if (R.done) endTrace();
                    else if (R.fullCalls == 0 && R.updates - traceStartUpdates > 600)
                        fail("The probe saw physics ticks, but after hiding the level and restarting, the game stopped producing any. "
                             "Send me this message.");
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
            case Ph::Init: case Ph::Probe: return fmt::format("Checking that the game runs...\nsteps {}  physics calls {}  half-tick calls {}", R.updates, R.calls, R.halfCalls);
            case Ph::Trace: return fmt::format("Recording the baseline run...\ntick {}   steps {}   half-tick calls {}{}", R.fullCalls, R.updates, R.halfCalls, R.tickOnAll ? "   (counting every call)" : "");
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

    bool init(PlayLayer* pl, CheckpointObject* fresh) {
        if (!CCLayer::init()) return false;
        m_pl = pl;
        R.active = true;                              // freeze the real game before anything else
        m_a = std::make_unique<Analyzer>(pl, fresh);

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
            saveResults(m_a->complete, m_a->baseTicks, m_a->sig);
            size_t ok = 0, wide = 0, unstable = 0, never = 0, noeffect = 0;
            for (auto& w : results) {
                if (w.status == 1) ++ok; else if (w.status == 2) ++wide; else if (w.status == 3) ++unstable; else if (w.status == 4) ++never; else if (w.status == 5) ++noeffect;
            }
            const double secs = (double)m_a->baseTicks / fwcore::kTps;
            text = fmt::format("{}\nRun length measured: {} ticks = {:.2f} s ({:.6f}% per frame at constant speed)\n"
                               "Windows 1-10 frames: {}   wider than 10: {}   unstable: {}   never fired: {}   no effect (not clicks): {}\n"
                               "Tests run: {}   half-tick calls: {}   probe: {} steps / {} calls / {} half{}\n{}\nSaved: {}",
                               m_a->complete ? "Done. Turn on \"Show windows\" in the Windows popup, then play the macro."
                                             : "Stopped early. Progress is saved; press Resume to continue.",
                               m_a->baseTicks, secs, secs > 0 ? 100.0 / (double)m_a->baseTicks : 0.0,
                               ok, wide, unstable, never, noeffect, m_a->tests, R.halfCalls, m_a->pUpdates, m_a->pCalls, m_a->pHalf, R.tickOnAll ? " (every call counted as a tick)" : "", m_a->note, resultsPath().string());
        }
        log::info("Frame windows result:\n{}", text);
        m_a->restore(true);
        FLAlertLayer::create("Frame Windows", text, "OK")->show();
        leave();
    }

public:
    static FWOverlay* create(PlayLayer* pl, CheckpointObject* fresh = nullptr) {
        auto ret = new FWOverlay();
        if (ret->init(pl, fresh)) { ret->autorelease(); return ret; }
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
    if (!hudNode || idx >= results.size() || (results[idx].status != 1 && results[idx].status != 2)) return;
    const int c = M::actions[idx].down ? 0 : 1;
    hudFlash[c][fwcore::bucketOf(results[idx].status == 2 ? 11 : results[idx].window)] = .8f;
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
            if (results[j].status != 1 && results[j].status != 2) continue;     // no-effect rows (initial release) never count
            hudCount[M::actions[j].down ? 0 : 1][fwcore::bucketOf(results[j].status == 2 ? 11 : results[j].window)]++;
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
            l->setVisible(b < fwcore::kBuckets - 1 || showWide);
            l->setColor(f > 0.f ? ccColor3B{255, 230, 80} : (b == fwcore::kBuckets - 1 ? ccColor3B{150, 150, 150} : ccColor3B{255, 255, 255}));
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
        pendingAnalysis = true;                // starts on the next level reset (restart or death)
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

        addToggle("Show >10 line on screen", showWide, -128.f, [](bool on) {
            showWide = on;
            Mod::get()->setSavedValue("fw-show-wide", on);
        });

        auto help = CCLabelBMFont::create("Analyze runs the level offscreen at full speed.", "chatFont.fnt");
        help->setScale(.6f);
        m_mainLayer->addChildAtPosition(help, Anchor::Center, {0.f, -148.f});

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

// Freezes the real game loop while the analyzer runs, starts an armed analysis on the next level reset,
// and turns deaths inside simulated runs into a flag.
class $modify(FWPlayLayer, PlayLayer) {
    void update(float dt) {
        if (FW::R.active && !FW::R.inSim) return;            // analyzer owns the level: freeze the real loop
        PlayLayer::update(dt);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (FW::pendingAnalysis && !FW::R.active) {
            FW::pendingAnalysis = false;
            if (auto pl = PlayLayer::get()) {
                auto fresh = pl->createCheckpoint();   // the game's own clean start state, before we touch anything
                if (auto ov = FW::FWOverlay::create(pl, fresh)) CCScene::get()->addChild(ov, 5000);
            }
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (FW::R.inSim) { FW::onDeath(obj); return; }       // simulated death: record it, show nothing
        PlayLayer::destroyPlayer(player, obj);
    }
};
