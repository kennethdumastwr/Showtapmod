#pragma once
// Pure C++ (no Geode): the parts of the frame-window counter that don't need the game.
#include <cstdint>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace fwcore {

constexpr int kMaxWin = 10;       // only windows of 10 frames or fewer are reported
constexpr double kTps = 240.0;

// One macro row's result.
struct Win {
    int8_t lo = 0, hi = 0;        // how many frames earlier / later it still works
    int8_t window = 0;            // 1..10 frames (11 = more than 10)
    uint8_t status = 0;           // 0 not analysed, 1 ok, 2 wider than 10, 3 unstable, 4 never fired, 5 no effect (release of a button that wasn't held)
};

// HUD buckets: 1, 2, 3, 4, 5-6, 7-8, 9, 10, and a last one for 'wider than 10'
constexpr int kBuckets = 9;
inline const char* bucketName(int b) {
    static const char* n[kBuckets] = {"1", "2", "3", "4", "5-6", "7-8", "9", "10", ">10"};
    return n[b];
}
inline int bucketOf(int w) {          // w in 1..10, or 11 for 'wider than 10'
    if (w > kMaxWin) return 8;
    if (w <= 4) return w - 1;
    if (w <= 6) return 4;
    if (w <= 8) return 5;
    return w == 9 ? 6 : 7;
}

// Decides which frame shift to test next for ONE click, and when to stop.
// Shifts are tried outward from the original frame (+1, -1, +2, -2, ...). Each side stops at its
// first failure, and the whole search stops as soon as the window is known to be wider than 10.
struct WindowSearch {
    int hi = 0, lo = 0;
    bool upOk = true, downOk = true, turnUp = true;
    bool zeroDone = false, unstable = false, finished = false;

    // returns false when finished. canTest(k) says whether shift k is possible (inside the run).
    bool next(int& k, const std::function<bool(int)>& canTest) {
        if (finished) return false;
        if (!zeroDone) { k = 0; return true; }
        for (;;) {
            if (1 + hi + lo > kMaxWin) { finished = true; return false; }
            const bool wantUp = upOk && (turnUp || !downOk);
            const bool wantDown = !wantUp && downOk;
            if (!wantUp && !wantDown) { finished = true; return false; }
            const int kk = wantUp ? hi + 1 : -(lo + 1);
            turnUp = !wantUp;
            if (!canTest(kk)) { (wantUp ? upOk : downOk) = false; continue; }
            k = kk;
            return true;
        }
    }
    void report(int k, bool ok) {
        if (k == 0) { zeroDone = true; if (!ok) { unstable = true; finished = true; } return; }
        if (k > 0) { if (ok) ++hi; else upOk = false; }
        else       { if (ok) ++lo; else downOk = false; }
    }
    Win result() const {
        Win w; w.lo = (int8_t)lo; w.hi = (int8_t)hi;
        if (unstable) { w.status = 3; return w; }
        const int total = 1 + hi + lo;
        w.window = (int8_t)(total > kMaxWin ? kMaxWin + 1 : total);
        w.status = total > kMaxWin ? 2 : 1;
        return w;
    }
};

// ---- length input "1:28", "88", "1:28.5" -> seconds
inline bool parseLength(const std::string& s, double& secs) {
    if (s.empty()) return false;
    double m = 0, sec = 0;
    const auto c = s.find(':');
    try {
        if (c == std::string::npos) { sec = std::stod(s); }
        else { m = std::stod(s.substr(0, c)); sec = std::stod(s.substr(c + 1)); }
    } catch (...) { return false; }
    secs = m * 60.0 + sec;
    return secs > 0;
}
inline double ticksFor(double secs) { return secs * kTps; }
inline double percentPerFrame(double secs) { return 100.0 / ticksFor(secs); }

// ---- results file (.fw)
struct FileData {
    std::string macro;
    uint64_t hash = 0;
    bool complete = false;
    long long ticks = 0;
    uint64_t sig = 0;             // signature of the baseline run the results were computed against
    bool hasSig = false;          // files from older versions have none and are not trusted
    std::vector<Win> wins;
};

inline std::string serialize(const FileData& d) {
    std::ostringstream o;
    o << "# frame-windows v1\n# macro " << d.macro << "\n";
    o << "# meta actions " << d.wins.size() << " hash " << d.hash << " complete " << (d.complete ? 1 : 0)
      << " ticks " << d.ticks << " sig " << d.sig << "\n# idx status window lo hi\n";
    for (size_t i = 0; i < d.wins.size(); ++i) {
        const auto& w = d.wins[i];
        o << i << ' ' << (int)w.status << ' ' << (int)w.window << ' ' << (int)w.lo << ' ' << (int)w.hi << '\n';
    }
    return o.str();
}

inline bool parse(const std::string& text, FileData& d) {
    d = FileData{};
    std::istringstream is(text);
    std::string line;
    size_t n = 0; bool gotMeta = false;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.rfind("# macro ", 0) == 0) d.macro = line.substr(8);
            else if (line.rfind("# meta ", 0) == 0) {
                std::istringstream m(line.substr(7));
                std::string k; int c = 0;
                while (m >> k) {
                    if (k == "actions") m >> n;
                    else if (k == "hash") m >> d.hash;
                    else if (k == "complete") { m >> c; d.complete = c != 0; }
                    else if (k == "ticks") m >> d.ticks;
                    else if (k == "sig") { m >> d.sig; d.hasSig = true; }
                }
                gotMeta = true;
            }
            continue;
        }
        std::istringstream r(line);
        size_t idx; int st, w, lo, hi;
        if (!(r >> idx >> st >> w >> lo >> hi)) return false;
        if (d.wins.size() <= idx) d.wins.resize(idx + 1);
        d.wins[idx] = Win{(int8_t)lo, (int8_t)hi, (int8_t)w, (uint8_t)st};
    }
    if (!gotMeta) return false;
    if (d.wins.size() < n) d.wins.resize(n);
    return d.wins.size() == n;
}

// FNV-1a over (x, down, p2): ties a results file to the exact macro it was computed for.
inline uint64_t hashRow(uint64_t h, double x, bool down, bool p2) {
    uint64_t bits; std::memcpy(&bits, &x, sizeof bits);
    h ^= bits; h *= 1099511628211ULL;
    h ^= (uint64_t)((down ? 1 : 0) | (p2 ? 2 : 0)); h *= 1099511628211ULL;
    return h;
}
constexpr uint64_t kHashSeed = 1469598103934665603ULL;

} // namespace fwcore
