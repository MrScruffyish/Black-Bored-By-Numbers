// Chalk-on-blackboard column arithmetic -> mp4.  Handles huge (1000+ digit) operands:
// the board is never rasterised as a whole; each frame draws only strokes visible to the camera.
// By default the camera stays at the board origin and only zooms out, so the view never pans
// back and forth; it fits everything written so far.  Large scenes are sped up automatically.
// Build: g++ -O2 -std=c++17 board.cpp -o board $(pkg-config --cflags --libs freetype2)
#include <ft2build.h>
#include FT_FREETYPE_H
#include <bits/stdc++.h>
#include <unistd.h>
using namespace std;

int W = 1280, H = 720;  // output frame size; 1280x720 is the default
const int FPS = 30, BIG = 110, SMALL = 50, TITLE = 72;
float SCALE = 1;        // scene scale relative to the 1280x720 design
const float RH = 135, GAP = 28, INF = 1e30f, CAM_MARGIN = 100;
const unsigned char CHALK[3] = {238, 242, 236}, GREEN[3] = {34, 72, 50};
int DECIMALS = -1;  // optional cap on quotient decimals; -1 = only the 2*max(A,B) rule

// Camera: "fit" keeps the camera at the origin and only zooms out to fit all
// strokes written so far.  "follow" is the old behaviour: pan to follow the
// last ~2 seconds of activity.  Auto-speed shortens scenes whose final zoom is
// large (i.e. very small writing), since their details are not readable anyway.
enum CamMode { CAM_FIT, CAM_FOLLOW } CAMERA = CAM_FIT;
float ZOOM_LEAD = 0.6f;   // seconds of look-ahead when easing the zoom-out
float AUTO_SPEED = 0.5f;  // speed-up exponent; 0 disables automatic speed-up
float MAX_AUTO_SPEED = 8; // cap for the automatic speed-up factor
FT_Library ft; FT_Face face;

[[noreturn]] void die(const string& m) { fprintf(stderr, "error: %s\n", m.c_str()); exit(1); }

// Video size presets.  A custom "WxH" value is accepted too.
bool parseFormat(const string& f, int& w, int& h) {
    static const map<string, pair<int, int>> presets = {
        {"360p", {640, 360}},    {"480p", {854, 480}},
        {"720p", {1280, 720}},   {"1080p", {1920, 1080}},
        {"1440p", {2560, 1440}}, {"2160p", {3840, 2160}},
        {"4k", {3840, 2160}},
    };
    string key = f;
    for (char& c : key) c = (char)tolower((unsigned char)c);
    auto it = presets.find(key);
    if (it != presets.end()) { w = it->second.first; h = it->second.second; return true; }
    int n = 0;
    if (sscanf(key.c_str(), "%dx%d%n", &w, &h, &n) == 2 && n == (int)key.size() && w > 0 && h > 0) return true;
    return false;
}
void setVideoSize(int w, int h) {
    W = max(2, (w + 1) & ~1);  // yuv420p needs even dimensions
    H = max(2, (h + 1) & ~1);
    SCALE = min(W / 1280.0f, H / 720.0f);
}

// ---------------- glyphs ----------------
struct Glyph { int w, h, left, top; vector<uint8_t> a; };
map<pair<int, char32_t>, Glyph> gcache;
const Glyph& glyph(int size, char32_t c) {
    auto k = make_pair(size, c);
    auto it = gcache.find(k); if (it != gcache.end()) return it->second;
    FT_Set_Pixel_Sizes(face, 0, size);
    if (FT_Load_Char(face, c, FT_LOAD_RENDER)) die("glyph");
    auto* g = face->glyph;
    Glyph G{(int)g->bitmap.width, (int)g->bitmap.rows, g->bitmap_left, g->bitmap_top, {}};
    G.a.resize(G.w * G.h);
    for (int y = 0; y < G.h; y++) memcpy(&G.a[y * G.w], g->bitmap.buffer + y * g->bitmap.pitch, G.w);
    return gcache[k] = G;
}
struct Mip { int w, h; vector<uint8_t> a; };
map<pair<int, char32_t>, vector<Mip>> mipc;
const vector<Mip>& mips(int size, char32_t c) {  // box-filtered pyramid for zoomed-out drawing
    auto k = make_pair(size, c);
    auto it = mipc.find(k); if (it != mipc.end()) return it->second;
    const Glyph& g = glyph(size, c); vector<Mip> v{{max(g.w, 1), max(g.h, 1), g.a}};
    if (v[0].a.empty()) v[0].a.assign(1, 0);
    while (v.back().w > 1 || v.back().h > 1) {
        const Mip& m = v.back(); Mip n{(m.w + 1) / 2, (m.h + 1) / 2, {}}; n.a.resize(n.w * n.h);
        for (int y = 0; y < n.h; y++) for (int x = 0; x < n.w; x++) {
            int s = 0;
            for (int d = 0; d < 4; d++) { int X = 2 * x + d % 2, Y = 2 * y + d / 2; if (X < m.w && Y < m.h) s += m.a[Y * m.w + X]; }
            n.a[y * n.w + x] = s / 4;
        }
        v.push_back(move(n));
    }
    return mipc[k] = v;
}
map<int, pair<float, float>> metr;  // size -> (advance, ascender)
float adv(int s) { if (!metr.count(s)) { FT_Set_Pixel_Sizes(face, 0, s); FT_Load_Char(face, '0', 0);
        metr[s] = {face->glyph->advance.x / 64.f, face->size->metrics.ascender / 64.f}; } return metr[s].first; }
float asc(int s) { adv(s); return metr[s].second; }
float CW;

u32string u32(const string& s) {  // minimal UTF-8 decode
    u32string r;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i]; int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t v = n == 1 ? c : c & (0xFF >> (n + 1));
        for (int k = 1; k < n; k++) v = (v << 6) | (s[i + k] & 0x3F);
        r += v; i += n;
    }
    return r;
}

// ---------------- scene = timeline of strokes ----------------
struct Stroke { float t0, dur, te, x, y, x2, y2; int size; char32_t ch; };  // size==0 -> line
struct Scene {
    vector<Stroke> s; float T = 0, top = 260, titleEnd = 0; string name, value;
    size_t glyphs(const u32string& t, float x, float y, int size, float per, float pause) {
        size_t first = s.size(); float a = adv(size);
        for (char32_t c : t) { if (c != ' ') { s.push_back({T, per, INF, x * SCALE, y * SCALE, 0, 0, max(1, (int)lround(size * SCALE)), c}); T += per; } x += a; }
        T += pause; return first;
    }
    float text(const string& t, float x, float y, int size = TITLE, float pause = 0) {
        glyphs(u32(t), x, y, size, 2, pause); return x + adv(size) * u32(t).size(); }
    size_t big(char32_t c, float x, float y, float pause = 4) { return glyphs(u32string(1, c), x, y, BIG, 6, pause); }
    size_t small(const string& t, float cx, float y, float pause = 3) {
        return glyphs(u32(t), cx - adv(SMALL) * t.size() / 2, y, SMALL, 3, pause); }
    void line(float x1, float y1, float x2, float y2, float pause = 6) {
        float d = min(60.f, max(4.f, fabsf(x2 - x1) / 40)); s.push_back({T, d, INF, x1 * SCALE, y1 * SCALE, x2 * SCALE, y2 * SCALE, 0, 0}); T += d + pause; }
    void erase(size_t from, size_t to) { for (size_t i = from; i < to; i++) s[i].te = T; }
    void pause(float n) { T += n; }
};
array<float, 4> bbox(const Stroke& k) {
    if (!k.size) return {min(k.x, k.x2) - 4 * SCALE, min(k.y, k.y2) - 4 * SCALE, max(k.x, k.x2) + 4 * SCALE, max(k.y, k.y2) + 4 * SCALE};
    const Glyph& g = glyph(k.size, k.ch);
    float x = k.x + g.left, y = k.y + asc(k.size) - g.top;
    return {x, y, x + max(g.w, 1), y + max(g.h, 1)};
}
// Zoom needed to fit the whole scene with the camera anchored at (0, 0).
float fitZoom(const Scene& sc) {
    float mx = 0, my = 0;
    for (auto& k : sc.s) { auto b = bbox(k); mx = max(mx, b[2]); my = max(my, b[3]); }
    return max({1.f, (mx + 120 * SCALE) / (W - CAM_MARGIN * SCALE), (my + 120 * SCALE) / (H - CAM_MARGIN * SCALE)});
}
float autoSpeed(const Scene& sc) {
    if (AUTO_SPEED <= 0 || sc.s.empty()) return 1;
    return min(max(1.f, MAX_AUTO_SPEED), powf(max(1.f, fitZoom(sc)), AUTO_SPEED));
}
// "\begin" / "\begin{name}" and "\end" / "\end{name}" mark a block that is
// always rendered as maths, even when its lines look like arithmetic.
bool latexMarker(const string& s, const string& cmd, string& env) {
    if (s == cmd) { env.clear(); return true; }
    if (s.compare(0, cmd.size() + 1, cmd + "{") == 0 && s.back() == '}') {
        env = s.substr(cmd.size() + 1, s.size() - cmd.size() - 2);
        return true;
    }
    return false;
}
// Trailing "\meta name=value" commands are stripped from the line and apply to
// that line only.
struct Meta {
    int divDigits = -1;   // \meta div_digits=N: significant quotient digits for this line
    int sqrtDigits = -1;  // \meta sqrt_digits=N: significant root digits for this line
};
Meta takeMeta(string& body) {
    static const regex re(R"(\s*\\meta\s+([A-Za-z][A-Za-z0-9_]*)=(\S+)\s*$)");
    Meta meta; smatch m;
    while (regex_search(body, m, re)) {
        string key = m[1], val = m[2];
        if (key == "div_digits" || key == "sqrt_digits") {
            int& dst = key == "div_digits" ? meta.divDigits : meta.sqrtDigits;
            if (dst >= 0) die("duplicate meta command: \\meta " + key);
            char* end = nullptr;
            long n = strtol(val.c_str(), &end, 10);
            if (!end || *end || n < 0 || n > INT_MAX)
                die("meta " + key + " must be a non-negative integer: " + val);
            dst = (int)n;
        } else die("unknown meta command: \\meta " + key);
        body = m.prefix().str();
    }
    return meta;
}
// variable name with LaTeX-style index: b_{i+1}, x_1
float writeName(Scene& sc, const string& n, float x, float y) {
    auto p = n.find('_');
    x = sc.text(n.substr(0, p), x, y);
    if (p != string::npos) {
        string sub = n.substr(p + 1);
        if (!sub.empty() && sub[0] == '{') sub = sub.substr(1, sub.size() - 2);
        x = sc.text(sub, x, y + TITLE * 0.35f, TITLE * 6 / 10);
    }
    return x;
}

// ---------------- math formulas (small LaTeX subset) ----------------
// Supported: \frac{}{}, \sqrt{}, \left...\right, _ and ^ scripts, groups,
// and a handful of symbol commands (\approx, \times, \cdot, \pi, ...).
struct MathAtom {
    enum Kind { CH, GROUP, FRAC, SQRT, DELIM } kind = CH;
    char32_t ch = 0, lo = 0, hi = 0;
    vector<MathAtom> body, num, den, sub, sup;
    bool hasSub = false, hasSup = false;
};
using MathSeq = vector<MathAtom>;
struct MathBox { float w = 0, asc = 0, desc = 0; };

float mAsc(int s) { return s * 0.75f; }
float mDesc(int s) { return s * 0.22f; }
int mScript(int s) { return max(8, (int)lround(s * 0.62f)); }
int mFrac(int s) { return max(8, (int)lround(s * 0.72f)); }

MathBox measureSeq(const MathSeq& v, int size);
MathBox coreBox(const MathAtom& a, int size) {
    switch (a.kind) {
        case MathAtom::CH: return {adv(size), mAsc(size), mDesc(size)};
        case MathAtom::GROUP: return measureSeq(a.body, size);
        case MathAtom::FRAC: {
            int s = mFrac(size);
            MathBox n = measureSeq(a.num, s), d = measureSeq(a.den, s);
            float pad = size * 0.18f, gap = size * 0.12f, bar = -0.30f * size;
            float nb = bar - gap - n.desc, db = bar + gap + d.asc;
            return {max(n.w, d.w) + 2 * pad, n.asc - nb, db + d.desc};
        }
        case MathAtom::SQRT: {
            MathBox b = measureSeq(a.body, size);
            return {adv(size) + b.w + size * 0.08f, max(b.asc + size * 0.10f, mAsc(size)), b.desc};
        }
        default: {  // DELIM
            MathBox b = measureSeq(a.body, size);
            float need = b.asc + b.desc + size * 0.30f;
            int ds = max(size, (int)ceilf(need / 0.92f));
            float w = (a.lo ? adv(ds) : 0.f) + b.w + (a.hi ? adv(ds) : 0.f);
            return {w, max(b.asc, mAsc(ds)), max(b.desc, mDesc(ds))};
        }
    }
}
MathBox measureAtom(const MathAtom& a, int size) {
    MathBox b = coreBox(a, size);
    if (!a.hasSub && !a.hasSup) return b;
    int s = mScript(size); float sw = 0;
    if (a.hasSup) { MathBox u = measureSeq(a.sup, s); sw = max(sw, u.w); b.asc = max(b.asc, 0.45f * size + u.asc); }
    if (a.hasSub) { MathBox u = measureSeq(a.sub, s); sw = max(sw, u.w); b.desc = max(b.desc, 0.36f * size + u.desc); }
    b.w += sw + size * 0.06f;
    return b;
}
MathBox measureSeq(const MathSeq& v, int size) {
    MathBox b;
    for (auto& a : v) { MathBox x = measureAtom(a, size); b.w += x.w; b.asc = max(b.asc, x.asc); b.desc = max(b.desc, x.desc); }
    return b;
}

void emitAtom(Scene& sc, const MathAtom& a, float x, float baseline, int size);
void emitSeq(Scene& sc, const MathSeq& v, float x, float baseline, int size) {
    for (auto& a : v) { emitAtom(sc, a, x, baseline, size); x += measureAtom(a, size).w; }
}
void emitAtom(Scene& sc, const MathAtom& a, float x, float baseline, int size) {
    MathBox core = coreBox(a, size);
    auto put = [&](char32_t c, float gx, float gy, int s) { if (c) sc.glyphs(u32string(1, c), gx, gy - asc(s), s, 2, 0); };
    switch (a.kind) {
        case MathAtom::CH: put(a.ch, x, baseline, size); break;
        case MathAtom::GROUP: emitSeq(sc, a.body, x, baseline, size); break;
        case MathAtom::FRAC: {
            int s = mFrac(size);
            MathBox n = measureSeq(a.num, s), d = measureSeq(a.den, s);
            float pad = size * 0.18f, gap = size * 0.12f, bar = baseline - 0.30f * size;
            emitSeq(sc, a.num, x + (core.w - n.w) / 2, bar - gap - n.desc, s);
            sc.line(x + pad * 0.35f, bar, x + core.w - pad * 0.35f, bar, 3);
            emitSeq(sc, a.den, x + (core.w - d.w) / 2, bar + gap + d.asc, s);
            break;
        }
        case MathAtom::SQRT: {
            MathBox b = measureSeq(a.body, size);
            float top = baseline - b.asc - size * 0.10f;
            put(U'√', x, baseline, size);
            sc.line(x + adv(size) * 0.92f, top, x + core.w, top, 3);
            emitSeq(sc, a.body, x + adv(size), baseline, size);
            break;
        }
        default: {  // DELIM
            MathBox b = measureSeq(a.body, size);
            float need = b.asc + b.desc + size * 0.30f;
            int ds = max(size, (int)ceilf(need / 0.92f));
            float lw = a.lo ? adv(ds) : 0.f;
            float dy = (b.desc - b.asc) / 2 - (mDesc(ds) - mAsc(ds)) / 2;
            if (a.lo) put(a.lo, x, baseline + dy, ds);
            emitSeq(sc, a.body, x + lw, baseline, size);
            if (a.hi) put(a.hi, x + lw + b.w, baseline + dy, ds);
            break;
        }
    }
    if (a.hasSub || a.hasSup) {
        int s = mScript(size);
        float sx = x + core.w + size * 0.06f;
        if (a.hasSub) emitSeq(sc, a.sub, sx, baseline + 0.36f * size, s);
        if (a.hasSup) emitSeq(sc, a.sup, sx, baseline - 0.45f * size, s);
    }
}

struct MathParser {
    string s; size_t i = 0;
    explicit MathParser(string t) : s(move(t)) {}
    bool eof() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    bool at(const char* w) const {
        size_t n = strlen(w);
        return s.compare(i, n, w) == 0 && (i + n >= s.size() || !isalpha((unsigned char)s[i + n]));
    }
    char32_t utf8() {
        unsigned char c = s[i]; int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t v = n == 1 ? c : c & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); k++) v = (v << 6) | (s[i + k] & 0x3F);
        i += n; return v;
    }
    [[noreturn]] void bad(const string& m) { die(m); }
    MathSeq parseGroup() {
        if (peek() != '{') bad("math: expected '{'");
        i++;
        MathSeq v = parseSeq(false);
        if (peek() != '}') bad("math: missing '}'");
        i++;
        return v;
    }
    MathSeq parseArg() {
        while (!eof() && isspace((unsigned char)peek())) i++;  // x_ 2 == x_2
        if (peek() == '{') return parseGroup();
        MathSeq v; v.push_back(parseBase()); return v;
    }
    char32_t parseDelim() {
        if (peek() == '\\') {
            i++;
            if (peek() == '{') { i++; return '{'; }
            if (peek() == '}') { i++; return '}'; }
            if (peek() == '|') { i++; return '|'; }
        }
        if (peek() == '.') { i++; return 0; }
        if (eof()) bad("math: expected delimiter");
        return utf8();
    }
    MathAtom parseBase() {
        if (peek() == '{') { MathAtom g; g.kind = MathAtom::GROUP; g.body = parseGroup(); return g; }
        if (peek() != '\\') { MathAtom c; c.ch = utf8(); return c; }
        i++;
        string cmd;
        if (isalpha((unsigned char)peek())) { while (isalpha((unsigned char)peek())) cmd += s[i++]; }
        else if (!eof()) cmd = string(1, s[i++]);
        if (cmd == "frac") { MathAtom f; f.kind = MathAtom::FRAC; f.num = parseArg(); f.den = parseArg(); return f; }
        if (cmd == "sqrt") {
            if (peek() == '[') { while (!eof() && peek() != ']') i++; if (!eof()) i++; }  // ignore the index
            MathAtom r; r.kind = MathAtom::SQRT; r.body = parseArg(); return r;
        }
        if (cmd == "left") {
            MathAtom d; d.kind = MathAtom::DELIM; d.lo = parseDelim();
            d.body = parseSeq(true);
            if (!at("\\right")) bad("math: missing \\right");
            i += 6; d.hi = parseDelim(); return d;
        }
        if (cmd == "right") bad("math: unexpected \\right");
        static const map<string, char32_t> sym = {
            {"approx", U'≈'}, {"times", U'×'}, {"cdot", U'·'}, {"div", U'÷'}, {"pm", U'±'}, {"mp", U'∓'},
            {"le", U'≤'}, {"leq", U'≤'}, {"ge", U'≥'}, {"geq", U'≥'}, {"neq", U'≠'}, {"ne", U'≠'},
            {"infty", U'∞'}, {"pi", U'π'}, {"alpha", U'α'}, {"beta", U'β'}, {"gamma", U'γ'}, {"delta", U'δ'},
            {"theta", U'θ'}, {"lambda", U'λ'}, {"mu", U'μ'}, {"sigma", U'σ'}, {"phi", U'φ'}, {"omega", U'ω'},
            {"to", U'→'}, {"rightarrow", U'→'}, {"sum", U'∑'}, {"int", U'∫'}, {"partial", U'∂'}, {"nabla", U'∇'}
        };
        auto it = sym.find(cmd);
        if (it != sym.end()) { MathAtom a; a.ch = it->second; return a; }
        if (cmd == "{" || cmd == "}") { MathAtom a; a.ch = cmd[0]; return a; }
        if (cmd == " " || cmd == "," || cmd == ";" || cmd == "quad" || cmd == "qquad") { MathAtom a; a.ch = U' '; return a; }
        if (cmd.empty()) bad("math: lone backslash");
        bad("math: unknown command \\" + cmd);
    }
    MathSeq parseSeq(bool stopRight) {
        MathSeq v;
        while (!eof() && peek() != '}') {
            if (stopRight && at("\\right")) break;
            MathAtom a = parseBase();
            for (;;) {
                size_t save = i;
                while (!eof() && isspace((unsigned char)peek())) i++;  // allow x _ 2
                if (peek() != '_' && peek() != '^') { i = save; break; }
                char c = s[i++]; MathSeq arg = parseArg();
                if (c == '_') { a.sub = arg; a.hasSub = true; }
                else { a.sup = arg; a.hasSup = true; }
            }
            v.push_back(move(a));
        }
        return v;
    }
};

Scene formula(const string& tex) {
    MathParser p(tex);
    MathSeq v = p.parseSeq(false);
    if (!p.eof()) p.bad("math: unexpected '}'");
    MathBox b = measureSeq(v, TITLE);
    Scene sc;
    emitSeq(sc, v, 60, 30 + b.asc, TITLE);
    sc.pause(90);
    return sc;
}

// ---------------- numbers ----------------
struct Num { vector<int> d; int dec; string txt; };  // d[k] = digit of exponent k-dec
typedef vector<int> Big;  // little-endian digits
void trim(Big& a) { while (a.size() > 1 && !a.back()) a.pop_back(); }
int cmp(const Big& a, const Big& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (int i = a.size() - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
Big subB(Big a, const Big& b) {
    int br = 0;
    for (size_t i = 0; i < a.size(); i++) { int v = a[i] - br - (i < b.size() ? b[i] : 0); br = v < 0; a[i] = v + 10 * br; }
    trim(a); return a;
}
Big mulS(const Big& a, int q) { Big r; int c = 0; for (int x : a) { int v = x * q + c; r.push_back(v % 10); c = v / 10; } while (c) { r.push_back(c % 10); c /= 10; } trim(r); return r; }
Big addInt(Big a, int v) {  // a + v, v small
    int c = v;
    for (size_t i = 0; c; i++) {
        if (i == a.size()) a.push_back(0);
        int s = a[i] + c % 10;
        a[i] = s % 10; c = c / 10 + s / 10;
    }
    trim(a); return a;
}
bool isZero(const Big& a) { return a.size() == 1 && !a[0]; }
int sigDigits(const Num& n) { Big t = n.d; trim(t); return t.size(); }  // digits ignoring leading zeros


Num parse(string s) {
    Num n; s.erase(remove_if(s.begin(), s.end(), [](unsigned char c) { return isspace(c); }), s.end());
    size_t dot = s.find('.'), com = s.find(',');
    if (dot != string::npos && com != string::npos)
        die("ambiguous number (use either a decimal comma or a decimal point): " + s);
    size_t p = dot != string::npos ? dot : com;  // decimal separator, '.' or ','
    for (size_t k = 0; k < s.size(); k++)
        if (k != p && !isdigit((unsigned char)s[k])) die("invalid number: " + s);
    if (p == 0 && s.size() == 1) die("invalid number: " + s);
    string i = s.substr(0, p), f = p == string::npos ? "" : s.substr(p + 1);
    i.erase(0, min(i.find_first_not_of('0'), i.size())); if (i.empty()) i = "0";
    n.dec = f.size(); string all = i + f;
    for (auto it = all.rbegin(); it != all.rend(); ++it) n.d.push_back(*it - '0');
    return n;
}
int get(const Num& n, int e) { int k = e + n.dec; return k >= 0 && k < (int)n.d.size() ? n.d[k] : 0; }
int hiE(const Num& n) { return (int)n.d.size() - n.dec - 1; }
// ---------------- square root ----------------
// Digits of N split into pairs, with the decimal point between the two lists.
// A pair is {tens, units}; the leading integer pair has tens = -1 when it has
// only one digit, and an odd fractional tail is padded with a zero.
struct Pairs {
    vector<vector<int>> ints, frac;
    int count() const { return ints.size() + frac.size(); }
    int value(int i) const {
        if (i < (int)ints.size()) { const auto& d = ints[i]; return (d[0] < 0 ? 0 : d[0]) * 10 + d[1]; }
        i -= ints.size();
        if (i < (int)frac.size()) { const auto& d = frac[i]; return (d[0] < 0 ? 0 : d[0]) * 10 + d[1]; }
        return 0;  // implicit zero pairs continue the decimal expansion
    }
};
Pairs splitPairs(const Num& n) {
    Pairs p; vector<int> ints, frac;
    for (int e = hiE(n); e >= 0; e--) ints.push_back(get(n, e));
    for (int e = -1; e >= -n.dec; e--) frac.push_back(get(n, e));
    p.ints.assign((ints.size() + 1) / 2, vector<int>(2, -1));
    int k = (int)ints.size() - 1;
    for (int i = (int)p.ints.size() - 1; i >= 0; i--) {
        p.ints[i][1] = ints[k--];
        if (k >= 0) p.ints[i][0] = ints[k--];
    }
    for (size_t j = 0; j < frac.size(); j += 2) {
        vector<int> d(2, -1);
        d[0] = frac[j];
        d[1] = j + 1 < frac.size() ? frac[j + 1] : 0;
        p.frac.push_back(d);
    }
    return p;
}
struct SqrtStep { int digit; Big divisor, product, remainder; };
struct SqrtResult {
    int intDigits = 0;  // root digits before the comma
    Big root;
    vector<SqrtStep> steps;
    int dec() const { return (int)steps.size() - intDigits; }
};
SqrtResult sqrtResult(const Num& n, int sqrtDigits) {
    Pairs p = splitPairs(n);
    int P_int = (int)p.ints.size(), P_given = p.count();
    int cap = sqrtDigits >= 0 ? sqrtDigits : 2 * sigDigits(n);
    SqrtResult r; r.intDigits = P_int;
    Big root{0}, rem{0};
    int sig = 0; bool started = false;
    for (int i = 0; ; i++) {
        Big cur = addInt(mulS(rem, 100), p.value(i));
        Big trial = mulS(root, 20);
        int d = 9;
        while (d > 0 && cmp(mulS(addInt(trial, d), d), cur) > 0) d--;
        Big divisor = addInt(trial, d), product = mulS(divisor, d);
        rem = subB(cur, product);
        root = addInt(mulS(root, 10), d);
        r.steps.push_back({d, divisor, product, rem});
        if (d) started = true;
        if (started) sig++;
        if (i + 1 >= P_int) {
            bool exact = isZero(rem);
            for (int j = i + 1; exact && j < P_given; j++) if (p.value(j)) exact = false;
            if (exact || sig >= cap) break;  // integer digits are always kept whole
        }
    }
    r.root = root;
    return r;
}

string norm(string s) {
    while (!s.empty() && isspace(s.back())) s.pop_back();
    if (s.empty() || s[0] == '.') s = "0" + s;
    if (s.find('.') != string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
    size_t z = 0; while (z + 1 < s.size() && s[z] == '0' && s[z + 1] != '.') z++;
    return s.substr(z);
}
string digits2str(const vector<int>& res, int dec) {  // res[k] = digit of exponent k-dec
    string s; vector<int> r = res; while ((int)r.size() <= dec) r.push_back(0);
    for (int k = r.size() - 1; k >= 0; k--) { s += char('0' + r[k]); if (k == dec && dec) s += '.'; }
    return norm(s);
}
string bcform(const Num& n) { return digits2str(n.d, n.dec); }
string fmt(const string& s) {  // "12345.6789" -> "12 345,678 9"
    auto p = s.find('.'); string i = s.substr(0, p), r;
    for (size_t k = 0; k < i.size(); k++) { if (k && (i.size() - k) % 3 == 0) r += ' '; r += i[k]; }
    if (p == string::npos) return r;
    r += ','; string f = s.substr(p + 1);
    for (size_t k = 0; k < f.size(); k++) { if (k && k % 3 == 0) r += ' '; r += f[k]; }
    return r;
}
string fmtNum(const Num& n) { return fmt(bcform(n)); }
string bcRaw(const string& expr) {  // bc output without norm(), so errors are distinguishable from 0
    char path[] = "/tmp/board_bcXXXXXX"; int fd = mkstemp(path); if (fd < 0) die("mkstemp");
    string in = "scale=10000\n" + expr + "\nquit\n"; if (write(fd, in.data(), in.size()) < 0) die("write"); close(fd);
    FILE* p = popen(("BC_LINE_LENGTH=0 bc -q " + string(path)).c_str(), "r"); string out; char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p); unlink(path);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    return out;
}
string bc(const string& expr) { return norm(bcRaw(expr)); }

// column grid: exponent e -> x of the digit cell; comma slot between e=0 and e=-1; gap every 3 digits
struct Grid {
    float CX, slot = 0.5f;  // slot: width (in CW) reserved for the comma; 0 -> commas squeezed in
    float X(int e) const { return e >= 0 ? CX - (e + 1) * CW - (e / 3) * GAP : CX + CW * slot + (-e - 1) * CW + ((-e - 1) / 3) * GAP; }
    static Grid leftAt(float x, int hi, float slot = 0.5f) { return {x + (hi + 1) * CW + (hi / 3) * GAP, slot}; }  // X(hi) == x
};
void commaSlot(Scene& sc, const Grid& g, float y) { sc.glyphs(U",", g.CX - CW / 4, y, BIG, 3, 4); }
void commaAfter(Scene& sc, const Grid& g, int eLeft, float y) {  // comma right of column eLeft
    if (eLeft == 0 && g.slot > 0) commaSlot(sc, g, y); else sc.glyphs(U",", g.X(eLeft) + CW / 2 + 4, y + 18, BIG, 3, 4);
}

Scene start(const string& name, const string& expr) {
    Scene sc; sc.name = name; float x = 60;
    if (!name.empty()) { x = writeName(sc, name, x, 30); x = sc.text(" = ", x, 30); sc.top += 100; }
    sc.titleEnd = sc.text(expr, x, 30, TITLE, 30); return sc;
}
void finish(Scene& sc, const vector<int>& res, int dec, const string& expr) {
    string mine = digits2str(res, dec), ref = bc(expr);
    if (mine != ref) die("algorithm/bc mismatch for " + expr + "\n mine: " + mine + "\n bc:   " + ref);
    sc.value = fmt(ref); sc.pause(20);
    if (sc.name.empty()) sc.text(" = " + sc.value, sc.titleEnd, 30, TITLE, 75);
    else { float x = writeName(sc, sc.name, 60, 130); sc.text(" = " + sc.value, x, 130, TITLE, 75); }
}

// ---------------- algorithms ----------------
Scene addsub(Num a, Num b, bool plus, const string& name) {
    Scene sc = start(name, fmtNum(a) + (plus ? " + " : " − ") + fmtNum(b));
    int lo = -max(a.dec, b.dec), hi = max(hiE(a), hiE(b)) + (plus ? 1 : 0), hiA = hiE(a);
    string expr = bcform(a) + (plus ? " + " : " - ") + bcform(b);
    if (!plus) for (Num* n : {&a, &b}) { int k = -lo - n->dec; n->d.insert(n->d.begin(), k, 0); n->dec += k; }
    Grid g = Grid::leftAt(200 + CW, hi); auto X = [&](int e) { return g.X(e); };
    float y0 = sc.top, y1 = y0 + RH, yl = y1 + 125, yr = y1 + RH + 15;
    for (auto [n, y] : {make_pair(&a, y0), make_pair(&b, y1)})
        for (int e = hiE(*n); e >= -n->dec; e--) { if (e == -1) commaSlot(sc, g, y); sc.big('0' + get(*n, e), X(e), y); }
    float sx = X(hi) - CW * 1.2f;
    sc.big(plus ? U'+' : U'−', sx, y1); sc.line(sx, yl, X(lo) + CW, yl, 10);
    vector<int> res;  // res[e-lo]
    if (plus) {
        int carry = 0;
        for (int e = lo; e <= hi; e++) {
            if (e == hi && !carry) break;
            int v = get(a, e) + get(b, e) + carry; res.push_back(v % 10); carry = v / 10;
            if (e == 0 && lo < 0) commaSlot(sc, g, yr);
            sc.big('0' + v % 10, X(e), yr, 6);
            if (carry) sc.small("1", X(e + 1) + CW / 2, y0 - 40, 8);
        }
    } else {
        int n = hiA - lo + 1; vector<int> top(n), level(n, 0);
        for (int e = lo; e <= hiA; e++) top[e - lo] = get(a, e);
        float SW = adv(SMALL);
        auto above = [&](int e, int v) {  // strike current value of column e, write v above it
            int& L = level[e - lo]; float yb = y0 - 40 - 48 * (L - 1);
            if (!L) sc.line(X(e) + 8, y0 + 105, X(e) + CW - 8, y0 + 35, 3);
            else sc.line(X(e) + CW / 2 - SW / 2, yb + 48, X(e) + CW / 2 + SW / 2, yb + 15, 3);
            L++; top[e - lo] = v; sc.small(to_string(v), X(e) + CW / 2, y0 - 40 - 48 * (L - 1), 6);
        };
        for (int e = lo; e <= hiA; e++) {
            if (top[e - lo] < get(b, e)) {
                int k = e + 1; while (k <= hiA && top[k - lo] == 0) k++;
                if (k > hiA) die("negative result not supported: " + expr);
                above(k, top[k - lo] - 1);
                for (int j = k - 1; j > e; j--) above(j, 9);
                sc.small("1", X(e) + 6, y0 + 8, 6); top[e - lo] += 10;
            }
            res.push_back(top[e - lo] - get(b, e));
        }
        int hn = 0; for (int e = lo; e <= hiA; e++) if (res[e - lo]) hn = max(hn, e);
        for (int e = lo; e <= min(hiA, hn); e++) { if (e == 0 && lo < 0) commaSlot(sc, g, yr); sc.big('0' + res[e - lo], X(e), yr, 8); }
    }
    finish(sc, res, -lo, expr);
    return sc;
}

Scene mul(const Num& A, const Num& B, const string& name) {
    Scene sc = start(name, fmtNum(A) + " × " + fmtNum(B));
    int nA = A.d.size(), nB = B.d.size(), nres = nA + nB, D = A.dec + B.dec;
    Grid g = Grid::leftAt(200 + 2 * CW, nres - 1 - D, 0);
    auto X = [&](int i) { return g.X(i - D); };  // i = integer digit position (result units = D)
    auto ys = [&](int r) { return sc.top + r * RH; };
    for (auto [n, r] : {make_pair(&A, 0), make_pair(&B, 1)})
        for (int i = n->d.size() - 1; i >= 0; i--) { sc.big('0' + n->d[i], X(i), ys(r)); if (i == n->dec && i) commaAfter(sc, g, i - D, ys(r)); }
    float sx = X(max(nA, nB)) - CW * 0.6f, right = X(0) + CW;
    sc.big(U'×', sx, ys(1)); sc.line(sx, ys(1) + 125, right, ys(1) + 125, 10);
    vector<vector<int>> part(nB, vector<int>(nres, 0));
    for (int j = 0; j < nB; j++) {
        int carry = 0; float y = ys(2 + j) + 15; size_t c0 = 0, c1 = 0;
        for (int i = 0; i < nA; i++) {
            int v = A.d[i] * B.d[j] + carry; part[j][i + j] = v % 10; carry = v / 10;
            sc.big('0' + v % 10, X(i + j), y, 6);
            sc.erase(c0, c1); c0 = c1 = 0;  // carry used -> wipe it
            if (carry && i < nA - 1) { c0 = sc.small(to_string(carry), X(i + j + 1) + CW / 2, y - 30, 8); c1 = sc.s.size(); }
        }
        if (carry) { part[j][nA + j] = carry; sc.big('0' + carry, X(nA + j), y, 6); }
    }
    vector<int> res; float yr = ys(2) + 15;
    if (nB > 1) {
        float yl = ys(2 + nB) + 10; yr = yl + 20;
        sc.line(sx, yl, right, yl, 10);
        long carry = 0; size_t c0 = 0, c1 = 0;
        for (int e = 0; e < nres || carry; e++) {
            long v = carry; for (int j = 0; j < nB; j++) v += e < nres ? part[j][e] : 0;
            if (e >= nres - 1 && v == 0 && !carry) break;
            res.push_back(v % 10); carry = v / 10;
            sc.big('0' + v % 10, X(e), yr, 6); sc.erase(c0, c1); c0 = c1 = 0;
            if (carry) { c0 = sc.small(to_string(carry), X(e + 1) + CW / 2, yr + 115, 8); c1 = sc.s.size(); }
        }
    } else {
        res = part[0]; while (res.size() > 1 && !res.back()) res.pop_back();
    }
    while ((int)res.size() <= D) { sc.big('0', X(res.size()), yr, 6); res.push_back(0); }  // 0,0x
    if (D) commaAfter(sc, g, 0, yr);
    finish(sc, res, D, bcform(A) + " * " + bcform(B));
    return sc;
}

// Number of dividend digits consumed by long division, i.e. the number of
// quotient digits.  Shared by the renderer and --only-show-results so both
// stop the quotient at exactly the same place.  cap counts fractional digits
// by default; with totalDigits it counts all significant digits of the
// quotient (leading zeros do not count, integer digits are always kept whole).
int divisionLength(const Num& A, const Big& d, int nInt, int cap, bool totalDigits) {
    int nD = A.d.size();
    auto digit = [&](int c) { int i = nD - 1 - c; return i >= 0 ? A.d[i] : 0; };
    auto qdig = [&](Big& r) { int q = 9; while (cmp(mulS(d, q), r) > 0) q--; return q; };
    auto push10 = [&](Big& r, int dg) { r.insert(r.begin(), dg); trim(r); };
    int cEnd = 0; Big r{0}; int sigFrac = 0, sigTotal = 0; bool started = false;
    for (;;) {
        int e = nInt - 1 - cEnd;
        if (e < 0) {
            if (totalDigits ? sigTotal >= cap : sigFrac >= cap) break;  // reached the digit budget
            if (cEnd >= nD && isZero(r)) break;                         // exact, nothing left to bring down
        }
        push10(r, digit(cEnd));
        int q = qdig(r);
        r = subB(r, mulS(d, q));
        if (q) started = true;
        if (started) sigTotal++;                                   // leading zeros do not count
        if (e < 0 && (sigFrac > 0 || q)) sigFrac++;                // leading zeros after the comma are free
        cEnd++;
    }
    return cEnd;
}

// long division, Swedish "trappan": divisor left of the bar, quotient above the dividend
Scene divide(const Num& a, const Num& b, const string& op, const string& name, int divDigits = -1) {
    Scene sc = start(name, fmtNum(a) + " " + op + " " + fmtNum(b));
    Big d = b.d; trim(d); if (isZero(d)) die("division by zero");
    Num A = a; int k = b.dec;  // move the comma k steps right in both numbers
    if (A.dec < k) { A.d.insert(A.d.begin(), k - A.dec, 0); A.dec = 0; } else A.dec -= k;
    while ((int)A.d.size() > A.dec + 1 && !A.d.back()) A.d.pop_back();
    Num Dv{d, 0, ""};
    if (k) sc.titleEnd = sc.text(" = " + fmtNum(A) + " " + op + " " + fmtNum(Dv), sc.titleEnd, 30, TITLE, 30);
    int nInt = A.d.size() - A.dec, nD = A.d.size();
    int N = max(sigDigits(a), sigDigits(b));  // max significant digits in the operands
    int decCap = divDigits >= 0 ? divDigits      // \meta div_digits=N overrides everything
               : 2 * N;                          // significant quotient decimals: 2 * max(A, B)
    if (divDigits < 0 && DECIMALS >= 0) decCap = min(decCap, DECIMALS);
    auto digit = [&](int c) { int i = nD - 1 - c; return i >= 0 ? A.d[i] : 0; };
    auto qdig = [&](Big& r) { int q = 9; while (cmp(mulS(d, q), r) > 0) q--; return q; };
    auto push10 = [&](Big& r, int dg) { r.insert(r.begin(), dg); trim(r); };
    int cEnd = divisionLength(A, d, nInt, decCap, divDigits >= 0);
    int nd = d.size(); float divW = nd * CW + ((nd - 1) / 3) * GAP, barX = 200 + divW + 25;
    Grid g = Grid::leftAt(barX + 30, nInt - 1); auto X = [&](int e) { return g.X(e); };
    Grid gd = Grid::leftAt(200, nd - 1);
    float yQ = sc.top, yD = yQ + RH + 10;
    for (int i = nd - 1; i >= 0; i--) sc.big('0' + d[i], gd.X(i), yD);
    sc.line(barX, yD + 120, barX, yD + 12, 4);
    sc.line(barX, yD + 12, X(nInt - cEnd) + CW + 10, yD + 12, 6);
    for (int c = 0; c < cEnd; c++) { int e = nInt - 1 - c; if (e == -1) commaSlot(sc, g, yD); sc.big('0' + digit(c), X(e), yD); }
    Big r{0}; float ycur = yD; bool sep = false, started = false; vector<int> qd;
    for (int c = 0; c < cEnd; c++) {
        int e = nInt - 1 - c, dg = digit(c); push10(r, dg);
        if (sep) sc.big('0' + dg, X(e), ycur, 6);  // bring down
        int q = qdig(r); qd.push_back(q);
        if (q || started || e == 0) { started = true; sc.big('0' + q, X(e), yQ, 6); if (e == 0 && cEnd > nInt) commaSlot(sc, g, yQ); }
        if (q) {
            Big p = mulS(d, q); float py = ycur + RH - 10;
            for (int t = p.size() - 1; t >= 0; t--) sc.big('0' + p[t], X(e + t), py, 3);
            sc.line(X(e + p.size() - 1) - 10, py + 120, X(e) + CW + 10, py + 120, 6);
            r = subB(r, p); ycur = py + RH + 5; sep = true;
            if (!isZero(r) || c + 1 == cEnd) for (int t = r.size() - 1; t >= 0; t--) sc.big('0' + r[t], X(e + t), ycur, 4);
        }
    }
    vector<int> res(qd.rbegin(), qd.rend());
    int dec = cEnd - nInt;
    finish(sc, res, dec, "scale=" + to_string(dec) + "\n" + bcform(a) + " / " + bcform(b));
    return sc;
}

// Square root by the longhand method, laid out like long division: the root
// sits above the radicand as the quotient, each step's trial divisor is written
// on the left, and the product is subtracted underneath.
Scene sqrtScene(const Num& n, const string& name, int sqrtDigits) {
    Pairs p = splitPairs(n);
    SqrtResult sr = sqrtResult(n, sqrtDigits);
    Scene sc = start(name, "√" + fmtNum(n));
    int P_int = sr.intDigits, P = (int)sr.steps.size();
    int maxDiv = 1;
    for (auto& st : sr.steps) maxDiv = max(maxDiv, (int)st.divisor.size());
    float divRight = 200 + maxDiv * CW + ((maxDiv - 1) / 3) * GAP, barX = divRight + CW;
    float x0 = barX + CW, PGAP = GAP * 0.6f;
    auto pairX = [&](int i) { return x0 + i * (2 * CW + PGAP); };
    auto unitExp = [&](int i) { return i < P_int ? 2 * (P_int - 1 - i) : -2 * (i - P_int) - 2; };
    auto cellX = [&](int e) {  // exponent -> x, relative to the radicand's units digit
        int pi, within;
        if (e >= 0) { pi = P_int - 1 - e / 2; within = 1 - e % 2; }
        else { int q = -e - 1; pi = P_int + q / 2; within = q % 2; }
        return pairX(pi) + within * CW;
    };
    auto pairDigits = [&](int i, int& tens, int& units) {
        if (i < (int)p.ints.size()) { tens = p.ints[i][0]; units = p.ints[i][1]; }
        else {
            int j = i - (int)p.ints.size();
            if (j < (int)p.frac.size()) { tens = p.frac[j][0]; units = p.frac[j][1]; }
            else { tens = 0; units = 0; }  // implicit zero pair
        }
    };
    float yRoot = sc.top, yRad = yRoot + RH + 10;
    sc.line(barX, yRad + 12, pairX(P - 1) + 2 * CW, yRad + 12, 6);  // radical overline
    sc.line(barX, yRad + 120, barX, yRad + 12, 4);                  // short division bar
    for (int i = 0; i < P; i++) {
        int t, u; pairDigits(i, t, u);
        if (t >= 0) sc.big('0' + t, pairX(i), yRad);
        sc.big('0' + u, pairX(i) + CW, yRad);
    }
    if (P > P_int) {
        float cx = pairX(P_int) - PGAP * 0.5f;
        sc.glyphs(U",", cx, yRad, BIG, 3, 4);
        sc.glyphs(U",", cx, yRoot, BIG, 3, 4);
    }
    float ycur = yRad;
    for (int i = 0; i < P; i++) {
        const SqrtStep& st = sr.steps[i];
        sc.big('0' + st.digit, pairX(i) + CW, yRoot, 6);
        float py = ycur + RH - 10;
        if (st.digit) {
            Grid gd = Grid::leftAt(divRight, (int)st.divisor.size() - 1);
            for (int k = (int)st.divisor.size() - 1; k >= 0; k--) sc.big('0' + st.divisor[k], gd.X(k), py, 3);
            for (int k = (int)st.product.size() - 1; k >= 0; k--) sc.big('0' + st.product[k], cellX(unitExp(i) + k), py, 3);
            sc.line(cellX(unitExp(i) + (int)st.product.size() - 1) - 10, py + 120, pairX(i) + CW + 10, py + 120, 6);
        }
        float yrem = py + RH + 5;
        if (!isZero(st.remainder) || i + 1 == P)
            for (int k = (int)st.remainder.size() - 1; k >= 0; k--) sc.big('0' + st.remainder[k], cellX(unitExp(i) + k), yrem, 4);
        if (i + 1 < P) {
            int t, u; pairDigits(i + 1, t, u);
            if (t >= 0) sc.big('0' + t, pairX(i + 1), yrem, 4);
            sc.big('0' + u, pairX(i + 1) + CW, yrem, 4);
        }
        ycur = yrem;
    }
    vector<int> res = sr.root;
    int dec = sr.dec();
    finish(sc, res, dec, "scale=" + to_string(dec) + "\nsqrt(" + bcform(n) + ")");
    return sc;
}

// Numeric result of one arithmetic line, formatted exactly as the video shows
// it.  Used by --only-show-results, which skips all drawing.
string computeValue(const Num& a, const Num& b, const string& op, int divDigits = -1) {
    string expr;
    if (op == "/" || op == ":" || op == "÷") {
        Big d = b.d; trim(d); if (isZero(d)) die("division by zero");
        Num A = a; int k = b.dec;  // move the comma k steps right in both numbers
        if (A.dec < k) { A.d.insert(A.d.begin(), k - A.dec, 0); A.dec = 0; } else A.dec -= k;
        while ((int)A.d.size() > A.dec + 1 && !A.d.back()) A.d.pop_back();
        int nInt = A.d.size() - A.dec;
        int decCap = divDigits >= 0 ? divDigits : 2 * max(sigDigits(a), sigDigits(b));
        if (divDigits < 0 && DECIMALS >= 0) decCap = min(decCap, DECIMALS);
        int dec = divisionLength(A, d, nInt, decCap, divDigits >= 0) - nInt;
        expr = "scale=" + to_string(dec) + "\n" + bcform(a) + " / " + bcform(b);
    } else if (op == "+" || op == "-" || op == "−") {
        expr = bcform(a) + (op == "+" ? " + " : " - ") + bcform(b);
    } else {
        expr = bcform(a) + " * " + bcform(b);
    }
    string v = bc(expr);
    if (!v.empty() && v[0] == '-') die("negative result not supported: " + expr);
    for (char& c : v) if (c == '.') c = ',';  // no thousand separators, keep the decimal comma
    return v;
}

// Numeric square root for --only-show-results, using the same digit extraction
// as the rendered scene and cross-checked against bc.
string computeSqrtValue(const Num& n, int sqrtDigits) {
    SqrtResult r = sqrtResult(n, sqrtDigits);
    int dec = r.dec();
    string mine = digits2str(r.root, dec);
    string ref = bc("scale=" + to_string(dec) + "\nsqrt(" + bcform(n) + ")");
    if (mine != ref) die("algorithm/bc mismatch for sqrt(" + bcform(n) + ")\n mine: " + mine + "\n bc:   " + ref);
    for (char& c : mine) if (c == '.') c = ',';
    return mine;
}

// ---------------- input expander ----------------
// The optional --expand preprocessor turns a few small directives into ordinary
// input lines.  It is deliberately textual: \let stores a number, ${NAME}
// substitutes it, \calc renders one arithmetic line while remembering its
// displayed result, \repeat ... \endrepeat repeats a block (optionally
// applying a per-iteration div_digits), \if{...} ... \else ... \endif
// selects a block after evaluating a numeric comparison with bc, and
// \begin{algorithm}{title} ... \end{algorithm} adds a formula title and a
// variable scope.
const regex ARITH_RE(R"(^\s*(?:([A-Za-z][A-Za-z0-9]*(?:\s*_\s*(?:\{[^}]*\}|[A-Za-z0-9]+))?)\s*=\s*)?([0-9 .,]*[0-9,.])\s*(\+|-|\*|x|×|−|/|:|÷)\s*([0-9 .,]*[0-9,.])\s*$)");
const regex SQRT_RE(R"(^\s*(?:([A-Za-z][A-Za-z0-9]*(?:\s*_\s*(?:\{[^}]*\}|[A-Za-z0-9]+))?)\s*=\s*)?sqrt\s*\{\s*([0-9 .,]*[0-9,.])\s*\}\s*$)");
const regex VAR_RE(R"([A-Za-z][A-Za-z0-9_]*)");
const regex LABEL_RE(R"(\s+label\s*=\s*([A-Za-z][A-Za-z0-9_]*)\s*$)");

string trimStr(const string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}
bool startsWithWord(const string& s, const string& w) {
    return s.compare(0, w.size(), w) == 0 && (s.size() == w.size() || isspace((unsigned char)s[w.size()]));
}
string displayNumber(const Num& n) {
    string s = bcform(n);
    for (char& c : s) if (c == '.') c = ',';  // no thousand separators, keep the decimal comma
    return s;
}
string substVars(const string& s, const map<string, string>& vars, int lineNo) {
    string r; size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            int depth = 1; size_t j = i + 2;
            while (j < s.size() && depth) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') depth--;
                if (depth) j++;
            }
            if (depth) die("line " + to_string(lineNo) + ": missing '}' in ${...}");
            string name = trimStr(s.substr(i + 2, j - (i + 2)));
            auto it = vars.find(name);
            if (it == vars.end()) die("line " + to_string(lineNo) + ": undefined variable: " + name);
            r += it->second;
            i = j + 1;
        } else r += s[i++];
    }
    return r;
}
int parseCount(const string& s, int lineNo, const string& what) {
    if (s.empty()) die("line " + to_string(lineNo) + ": expected " + what);
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (!end || *end || v < 0 || v > INT_MAX)
        die("line " + to_string(lineNo) + ": invalid " + what + ": " + s);
    return (int)v;
}
struct ExpandLine { string text; int no; };
struct RepeatHeader {
    string var;
    int start = 0, end = 0;
    vector<int> digits;  // optional per-iteration div_digits; one value repeats for all
};
RepeatHeader parseRepeat(const string& body, int lineNo, const map<string, string>& vars) {
    string rest = substVars(trimStr(body.substr(7)), vars, lineNo);  // \repeat
    static const regex re(R"(^([A-Za-z][A-Za-z0-9_]*)\s*=\s*([0-9]+)\s*\.\.\s*([0-9]+)\s*(.*)$)");
    smatch m;
    if (!regex_match(rest, m, re))
        die("line " + to_string(lineNo) + ": \\repeat needs NAME = START..END [digits ...]");
    RepeatHeader h; h.var = m[1];
    h.start = parseCount(m[2], lineNo, "repeat start");
    h.end = parseCount(m[3], lineNo, "repeat end");
    if (h.start > h.end) die("line " + to_string(lineNo) + ": \\repeat start is after its end");
    string tail = trimStr(m[4]);
    if (!tail.empty()) {
        if (startsWithWord(tail, "digits")) tail = trimStr(tail.substr(6));
        if (!tail.empty() && tail.front() == '[') {
            if (tail.back() != ']') die("line " + to_string(lineNo) + ": missing ']' in \\repeat digits");
            tail = trimStr(tail.substr(1, tail.size() - 2));
        }
        string cur;
        auto flush = [&] {
            if (!cur.empty()) { h.digits.push_back(parseCount(cur, lineNo, "repeat digit count")); cur.clear(); }
        };
        for (char c : tail) {
            if (c == ' ' || c == '\t' || c == ',') flush();
            else cur += c;
        }
        flush();
    }
    int count = h.end - h.start + 1;
    if (!h.digits.empty() && h.digits.size() != 1 && (int)h.digits.size() != count)
        die("line " + to_string(lineNo) + ": \\repeat has " + to_string(h.digits.size()) +
            " digit counts for " + to_string(count) + " iterations");
    return h;
}
struct Expander {
    map<string, string> vars;
    vector<string> out;
    string applyLoopDiv(const string& body, int loopDiv) {
        if (loopDiv < 0) return body;
        smatch m;
        if (!regex_match(body, m, ARITH_RE)) return body;
        string op = m[3];
        if (op != "/" && op != ":" && op != "÷") return body;
        string copy = body;
        if (takeMeta(copy).divDigits >= 0) return body;  // an explicit \meta wins
        return body + " \\meta div_digits=" + to_string(loopDiv);
    }
    void processLine(const string& body, int lineNo, int loopDiv) {
        if (startsWithWord(body, "\\let")) {
            string rest = trimStr(body.substr(4));
            size_t eq = rest.find('=');
            if (eq == string::npos) die("line " + to_string(lineNo) + ": \\let needs NAME = VALUE");
            string name = trimStr(rest.substr(0, eq)), val = trimStr(rest.substr(eq + 1));
            if (!regex_match(name, VAR_RE))
                die("line " + to_string(lineNo) + ": invalid variable name: " + name);
            if (val.empty()) die("line " + to_string(lineNo) + ": \\let needs a value");
            Num n = parse(substVars(val, vars, lineNo));
            vars[name] = displayNumber(n);
            return;
        }
        if (startsWithWord(body, "\\calc")) {
            string work = substVars(trimStr(body.substr(5)), vars, lineNo);
            Meta meta; string label;
            for (;;) {  // accept \meta and label= in either order
                bool changed = false;
                Meta m = takeMeta(work);
                if (m.divDigits >= 0) {
                    if (meta.divDigits >= 0) die("line " + to_string(lineNo) + ": duplicate meta command: \\meta div_digits");
                    meta.divDigits = m.divDigits; changed = true;
                }
                if (m.sqrtDigits >= 0) {
                    if (meta.sqrtDigits >= 0) die("line " + to_string(lineNo) + ": duplicate meta command: \\meta sqrt_digits");
                    meta.sqrtDigits = m.sqrtDigits; changed = true;
                }
                smatch lm;
                if (regex_search(work, lm, LABEL_RE)) {
                    if (!label.empty()) die("line " + to_string(lineNo) + ": duplicate label");
                    label = lm[1]; work = lm.prefix().str(); changed = true;
                }
                if (!changed) break;
            }
            work = trimStr(work);
            smatch am;
            if (!regex_match(work, am, ARITH_RE))
                die("line " + to_string(lineNo) + ": cannot parse \\calc: " + work);
            string shown = am[1], op = am[3];
            Num a = parse(am[2]), b = parse(am[4]);
            bool isDiv = op == "/" || op == ":" || op == "÷";
            if (isDiv && meta.divDigits < 0 && loopDiv >= 0) meta.divDigits = loopDiv;
            shown.erase(remove_if(shown.begin(), shown.end(), [](unsigned char c) { return isspace(c); }), shown.end());
            string store = label.empty() ? shown : label;
            if (!store.empty()) vars[store] = computeValue(a, b, op, meta.divDigits);
            string metaSuffix;
            if (meta.divDigits >= 0) metaSuffix += " \\meta div_digits=" + to_string(meta.divDigits);
            if (meta.sqrtDigits >= 0) metaSuffix += " \\meta sqrt_digits=" + to_string(meta.sqrtDigits);
            out.push_back(work + metaSuffix);
            return;
        }
        out.push_back(applyLoopDiv(substVars(body, vars, lineNo), loopDiv));
    }
    size_t matchingEnd(const vector<ExpandLine>& in, size_t start, int lineNo) {
        int depth = 1;
        for (size_t j = start; j < in.size(); j++) {
            string b = trimStr(in[j].text);
            if (b.empty() || b[0] == '#') continue;
            if (startsWithWord(b, "\\repeat")) depth++;
            else if (startsWithWord(b, "\\endrepeat") && --depth == 0) return j;
        }
        die("line " + to_string(lineNo) + ": \\repeat without matching \\endrepeat");
        return in.size();
    }
    string parseIf(const string& body, int lineNo) {
        if (body.size() < 5 || body.compare(0, 4, "\\if{") != 0)
            die("line " + to_string(lineNo) + ": \\if needs a condition: \\if{...}");
        int depth = 0; size_t close = string::npos;
        for (size_t j = 3; j < body.size(); j++) {
            if (body[j] == '{') depth++;
            else if (body[j] == '}' && --depth == 0) { close = j; break; }
        }
        if (close == string::npos) die("line " + to_string(lineNo) + ": missing '}' in \\if{...}");
        if (!trimStr(body.substr(close + 1)).empty())
            die("line " + to_string(lineNo) + ": unexpected text after \\if{...}");
        string cond = trimStr(body.substr(4, close - 4));
        if (cond.empty()) die("line " + to_string(lineNo) + ": empty \\if condition");
        return cond;
    }
    bool evalCondition(const string& cond, int lineNo) {
        string c = substVars(cond, vars, lineNo);
        for (char& ch : c) if (ch == ',') ch = '.';
        string n;  // accept "=" and "<>" as comparisons too
        for (size_t i = 0; i < c.size(); i++) {
            if (c[i] == '<' && i + 1 < c.size() && c[i + 1] == '>') { n += "!="; i++; }
            else if (c[i] == '=' &&
                     (i == 0 || (c[i - 1] != '<' && c[i - 1] != '>' && c[i - 1] != '!' && c[i - 1] != '=')) &&
                     (i + 1 >= c.size() || c[i + 1] != '=')) n += "==";
            else n += c[i];
        }
        string r = bcRaw("(" + n + ")");
        if (r == "1") return true;
        if (r == "0") return false;
        die("line " + to_string(lineNo) + ": cannot evaluate \\if condition: " + c);
        return false;
    }
    struct IfParts { size_t elsePos = string::npos, endPos = string::npos; };
    IfParts matchingIf(const vector<ExpandLine>& in, size_t start, int lineNo) {
        int depth = 1; IfParts p;
        for (size_t j = start; j < in.size(); j++) {
            string b = trimStr(in[j].text);
            if (b.empty() || b[0] == '#') continue;
            if (b.compare(0, 3, "\\if") == 0) depth++;
            else if (b == "\\endif") {
                if (--depth == 0) { p.endPos = j; return p; }
            } else if (b == "\\else" && depth == 1) {
                if (p.elsePos != string::npos) die("line " + to_string(lineNo) + ": duplicate \\else");
                p.elsePos = j;
            }
        }
        die("line " + to_string(lineNo) + ": \\if without matching \\endif");
        return p;
    }
    string parseAlgorithm(const string& body, int lineNo) {
        string rest = trimStr(body.substr(17));  // \begin{algorithm}
        if (rest.empty()) return "";
        if (rest.front() != '{') die("line " + to_string(lineNo) + ": expected {title} after \\begin{algorithm}");
        int depth = 0; size_t close = string::npos;
        for (size_t j = 0; j < rest.size(); j++) {
            if (rest[j] == '{') depth++;
            else if (rest[j] == '}' && --depth == 0) { close = j; break; }
        }
        if (close == string::npos) die("line " + to_string(lineNo) + ": missing '}' in algorithm title");
        if (!trimStr(rest.substr(close + 1)).empty())
            die("line " + to_string(lineNo) + ": unexpected text after algorithm title");
        return trimStr(rest.substr(1, close - 1));
    }
    size_t matchingAlgorithm(const vector<ExpandLine>& in, size_t start, int lineNo) {
        int depth = 1;
        for (size_t j = start; j < in.size(); j++) {
            string b = trimStr(in[j].text);
            if (b.empty() || b[0] == '#') continue;
            if (b.compare(0, 17, "\\begin{algorithm}") == 0) depth++;
            else if (b == "\\end{algorithm}" && --depth == 0) return j;
        }
        die("line " + to_string(lineNo) + ": \\begin{algorithm} without matching \\end{algorithm}");
        return in.size();
    }
    void block(const vector<ExpandLine>& in, size_t& i, int loopDiv) {
        while (i < in.size()) {
            string l = in[i].text;
            int lineNo = in[i].no;
            size_t p = l.find_first_not_of(" \t");
            if (p == string::npos || l[p] == '#') { out.push_back(l); i++; continue; }
            string body = trimStr(l);
            if (body == "\\else") die("line " + to_string(lineNo) + ": unexpected \\else");
            if (body == "\\endif") die("line " + to_string(lineNo) + ": unexpected \\endif");
            if (body == "\\end{algorithm}") die("line " + to_string(lineNo) + ": unexpected \\end{algorithm}");
            if (startsWithWord(body, "\\endrepeat")) die("line " + to_string(lineNo) + ": unexpected \\endrepeat");
            if (body.compare(0, 17, "\\begin{algorithm}") == 0) {
                string title = substVars(parseAlgorithm(body, lineNo), vars, lineNo);
                size_t bodyStart = i + 1, bodyEnd = matchingAlgorithm(in, bodyStart, lineNo);
                vector<ExpandLine> bodyLines(in.begin() + bodyStart, in.begin() + bodyEnd);
                if (!title.empty()) {  // a formula title, not arithmetic
                    out.push_back("\\begin{latex}");
                    out.push_back(title);
                    out.push_back("\\end{latex}");
                }
                auto saved = vars;
                size_t bi = 0;
                block(bodyLines, bi, loopDiv);
                vars = saved;  // an algorithm gets its own variable scope
                i = bodyEnd + 1;
                continue;
            }
            if (body.compare(0, 3, "\\if") == 0) {
                string cond = parseIf(body, lineNo);
                IfParts parts = matchingIf(in, i + 1, lineNo);
                size_t thenBegin = i + 1, thenEnd = parts.elsePos != string::npos ? parts.elsePos : parts.endPos;
                size_t elseBegin = parts.elsePos != string::npos ? parts.elsePos + 1 : parts.endPos;
                bool truth = evalCondition(cond, lineNo);
                vector<ExpandLine> branch;
                if (truth) branch.assign(in.begin() + thenBegin, in.begin() + thenEnd);
                else branch.assign(in.begin() + elseBegin, in.begin() + parts.endPos);
                size_t bi = 0;
                block(branch, bi, loopDiv);
                i = parts.endPos + 1;
                continue;
            }
            if (startsWithWord(body, "\\repeat")) {
                RepeatHeader h = parseRepeat(body, lineNo, vars);
                size_t bodyStart = i + 1, bodyEnd = matchingEnd(in, bodyStart, lineNo);
                vector<ExpandLine> bodyLines(in.begin() + bodyStart, in.begin() + bodyEnd);
                auto old = vars.find(h.var);
                bool had = old != vars.end();
                string saved = had ? old->second : "";
                for (int v = h.start; v <= h.end; v++) {
                    vars[h.var] = to_string(v);
                    int d = h.digits.empty() ? loopDiv : (h.digits.size() == 1 ? h.digits[0] : h.digits[v - h.start]);
                    size_t bi = 0;
                    block(bodyLines, bi, d);
                }
                if (had) vars[h.var] = saved; else vars.erase(h.var);
                i = bodyEnd + 1;
                continue;
            }
            processLine(body, lineNo, loopDiv);
            i++;
        }
    }
    vector<string> run(const vector<string>& in) {
        vector<ExpandLine> lines;
        for (size_t k = 0; k < in.size(); k++) lines.push_back({in[k], (int)k + 1});
        size_t i = 0;
        block(lines, i, -1);
        return out;
    }
};
vector<string> expandLines(const vector<string>& in) { Expander e; return e.run(in); }

Scene summary(const vector<Scene>& scenes) {
    Scene sc; sc.text("Summary", 60, 30, TITLE, 20); float y = 140;
    for (auto& s : scenes) if (!s.name.empty()) { float x = writeName(sc, s.name, 100, y); sc.text(" = " + s.value, x, y, TITLE, 15); y += 100; }
    sc.pause(90); return sc;
}

// ---------------- renderer ----------------
vector<uint8_t> noiseTile; const int NT = 1024;
void textures() {
    mt19937 rng(1); uniform_real_distribution<float> u(0, 1);
    noiseTile.resize(NT * NT);
    for (auto& v : noiseTile) v = 255 * (0.55f + 0.45f * u(rng)) * (u(rng) < 0.08f ? 0.3f : 1);
}

void render(Scene& sc, FILE* out, long& frameNo, long totalFrames) {
    auto& S = sc.s; float BW = W, BH = H; const int BK = 512;
    vector<array<float, 4>> BB(S.size());
    unordered_map<uint64_t, vector<uint32_t>> buckets;
    for (uint32_t i = 0; i < S.size(); i++) {
        auto b = BB[i] = bbox(S[i]); BW = max(BW, b[2] + 120 * SCALE); BH = max(BH, b[3] + 120 * SCALE);
        for (int bx = max(0.f, b[0]) / BK; bx <= (int)max(0.f, b[2]) / BK; bx++)
            for (int by = max(0.f, b[1]) / BK; by <= (int)max(0.f, b[3]) / BK; by++) buckets[(uint64_t)bx << 32 | by].push_back(i);
    }
    vector<long> stamp(S.size(), -1); vector<float> acc(W * H); vector<uint8_t> fr(W * H * 3);
    const float M = CAM_MARGIN * SCALE, WIN = 60, zmax = max({1.f, BW / W, BH / H}) * 1.05f;
    float zc = 1, cam[2] = {0, 0}, tgt[2] = {0, 0}, bnd[2] = {BW, BH}; bool first = true;
    array<float, 4> act{0, 0, (float)W, (float)H}; size_t wa = 0, wb = 0, nextB = 0;
    auto pen = [&](const Stroke& k, float f) { float p = k.dur < 1e-6f ? 1 : clamp((f - k.t0 + 1) / k.dur, 0.f, 1.f);
        return make_pair(k.x + (k.x2 - k.x) * p, k.y + (k.y2 - k.y) * p); };
    long nF = (long)ceil(sc.T) + 1; auto t0 = chrono::steady_clock::now();
    bool fitCam = CAMERA == CAM_FIT;
    vector<float> zfit;
    if (fitCam) {
        // Required zoom for every frame, based on everything written up to then.
        zfit.resize(nF); float mx = 0, my = 0;
        for (long f = 0; f < nF; f++) {
            while (nextB < S.size() && S[nextB].t0 <= f) {
                mx = max(mx, BB[nextB][2]); my = max(my, BB[nextB][3]); nextB++;
            }
            zfit[f] = max({1.f, (mx + 120 * SCALE) / (W - M), (my + 120 * SCALE) / (H - M)});
        }
        // Ease towards the future requirement (a backwards exponential average).
        // The result is monotone, always at least the current requirement, and
        // starts zooming out before new strokes reach the edge.  It never pans
        // and never zooms back in, so the view cannot jump around.
        float a = expf(-1.f / (FPS * max(0.05f, ZOOM_LEAD)));
        float z = zfit.back();
        for (long f = nF - 1; f >= 0; f--) { z = a * z + (1 - a) * zfit[f]; zfit[f] = max(z, zfit[f]); }
    }
    for (long f = 0; f < nF; f++, frameNo++) {
        float vw[2] = {(float)W, (float)H};
        if (fitCam) {
            // --- camera: fixed at the origin, zoom out only ---
            zc = zfit[f]; cam[0] = cam[1] = 0;
            vw[0] = W * zc; vw[1] = H * zc;
        } else {
            // --- camera: follow everything written during the last WIN frames ---
            while (wb < S.size() && S[wb].t0 <= f) wb++;
            while (wa < wb && S[wa].t0 < f - WIN) wa++;
            if (wa < wb) {
                act = {INF, INF, -INF, -INF};
                for (size_t i = wa; i < wb; i++) {
                    array<float, 4> b = BB[i];
                    if (!S[i].size) { auto [px, py] = pen(S[i], f); b = {px - 4 * SCALE, py - 4 * SCALE, px + 4 * SCALE, py + 4 * SCALE}; }
                    act = {min(act[0], b[0]), min(act[1], b[1]), max(act[2], b[2]), max(act[3], b[3])};
                }
            }
            float zt = clamp(max((act[2] - act[0]) / (W - 2 * M), (act[3] - act[1]) / (H - 2 * M)), 1.f, zmax);
            zc = first ? zt : zc * powf(zt / zc, 0.04f);
            vw[0] = W * zc; vw[1] = H * zc;
            float mb = M * zc;
            for (int ax = 0; ax < 2; ax++) {
                float lo = act[ax], hi = act[ax + 2];
                if (hi - lo > vw[ax] - 2 * mb) tgt[ax] = (lo + hi - vw[ax]) / 2;
                else { if (lo < tgt[ax] + mb) tgt[ax] = lo - mb; if (hi > tgt[ax] + vw[ax] - mb) tgt[ax] = hi - vw[ax] + mb; }
                tgt[ax] = clamp(tgt[ax], 0.f, max(0.f, bnd[ax] - vw[ax]));
                cam[ax] = first ? tgt[ax] : cam[ax] + (tgt[ax] - cam[ax]) * 0.08f;
            }
            first = false;
        }
        // --- draw visible strokes into a coverage buffer ---
        fill(acc.begin(), acc.end(), 0.f);
        float iz = 1 / zc; int L = max(0, (int)floor(log2f(zc) + 1e-4f));
        for (int bx = max(0, (int)(cam[0] / BK)); bx <= (int)((cam[0] + vw[0]) / BK); bx++)
            for (int by = max(0, (int)(cam[1] / BK)); by <= (int)((cam[1] + vw[1]) / BK); by++) {
                auto it = buckets.find((uint64_t)bx << 32 | by); if (it == buckets.end()) continue;
                for (uint32_t id : it->second) {
                    const Stroke& k = S[id];
                    if (k.t0 > f) break;
                    if (stamp[id] == f || k.te <= f) continue;
                    stamp[id] = f;
                    float p = k.dur < 1e-6f ? 1 : min(1.f, (f - k.t0 + 1) / k.dur);
                    if (k.size) {
                        const auto& mv = mips(k.size, k.ch); int l = min(L, (int)mv.size() - 1), s = 1 << l; const Mip& m = mv[l];
                        float gx = BB[id][0], gy = BB[id][1], wgt = (s * iz) * (s * iz) / 65025.f; int cols = ceil(m.w * p);
                        for (int j = 0; j < m.h; j++) {
                            int sy = floor((gy + (j + 0.5f) * s - cam[1]) * iz); if (sy < 0 || sy >= H) continue;
                            int ny = ((int)gy + j * s) & (NT - 1);
                            for (int i = 0; i < cols; i++) {
                                int a = m.a[j * m.w + i]; if (!a) continue;
                                int sx = floor((gx + (i + 0.5f) * s - cam[0]) * iz); if (sx < 0 || sx >= W) continue;
                                acc[sy * W + sx] += a * noiseTile[ny * NT + (((int)gx + i * s) & (NT - 1))] * wgt;
                            }
                        }
                    } else {
                        auto [ex, ey] = pen(k, f);
                        float x1 = (k.x - cam[0]) * iz, y1 = (k.y - cam[1]) * iz, x2 = (ex - cam[0]) * iz, y2 = (ey - cam[1]) * iz;
                        float hw = 3 * SCALE * iz, al = 1; if (hw < 0.5f) { al = hw * 2; hw = 0.5f; }
                        float dx = x2 - x1, dy = y2 - y1, L2 = max(dx * dx + dy * dy, 1e-6f);
                        for (int sy = max(0, (int)floor(min(y1, y2) - hw)); sy <= min(H - 1, (int)ceil(max(y1, y2) + hw)); sy++)
                            for (int sx = max(0, (int)floor(min(x1, x2) - hw)); sx <= min(W - 1, (int)ceil(max(x1, x2) + hw)); sx++) {
                                float t = clamp(((sx + .5f - x1) * dx + (sy + .5f - y1) * dy) / L2, 0.f, 1.f);
                                float qx = x1 + t * dx - sx - .5f, qy = y1 + t * dy - sy - .5f;
                                if (qx * qx + qy * qy > hw * hw) continue;
                                int nx = (int)(cam[0] + sx * zc) & (NT - 1), ny = (int)(cam[1] + sy * zc) & (NT - 1);
                                acc[sy * W + sx] += al * noiseTile[ny * NT + nx] / 255.f;
                            }
                    }
                }
            }
        for (int i = 0; i < W * H; i++) { float a = min(1.f, acc[i]); for (int c = 0; c < 3; c++) fr[i * 3 + c] = GREEN[c] + (CHALK[c] - GREEN[c]) * a; }
        fwrite(fr.data(), 1, fr.size(), out);
        if (frameNo % 600 == 0) {
            double s = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "\rframe %ld / %ld  (%.0f fps)   ", frameNo, totalFrames, f / max(s, 1e-3)); fflush(stderr);
        }
    }
}

int main(int argc, char** argv) {
    string in, out, enc = "libx264", font = "/usr/share/fonts/truetype/msttcorefonts/cour.ttf";
    bool fontGiven = false;
    double minutes = 0, speed = 1; bool dry = false, onlyResults = false, expand = false, showExpanded = false;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "-o") { if (++i == argc) die("-o requires an output file"); out = argv[i]; }
        else if (a == "--minutes") minutes = atof(argv[++i]);
        else if (a == "--speed") speed = atof(argv[++i]); else if (a == "--encoder") enc = argv[++i];
        else if (a == "--decimals") DECIMALS = atoi(argv[++i]); else if (a == "--dry") dry = true;
        else if (a == "--format") {
            if (++i == argc) die("--format requires a preset (360p, 480p, 720p, 1080p, 1440p, 2160p) or WxH");
            int w, h;
            if (!parseFormat(argv[i], w, h))
                die("unknown video format: " + string(argv[i]) + " (try 360p, 480p, 720p, 1080p, 1440p, 2160p or WxH)");
            setVideoSize(w, h);
        }
        else if (a == "--expand") expand = true;
        else if (a == "--show-expanded") showExpanded = true;
        else if (a == "--only-show-results") onlyResults = true;
        else if (a == "--font") { if (++i == argc) die("--font requires a path"); font = argv[i]; fontGiven = true; }
        else if (a == "--camera") {
            if (++i == argc) die("--camera requires fit or follow");
            string v = argv[i];
            if (v == "fit") CAMERA = CAM_FIT;
            else if (v == "follow") CAMERA = CAM_FOLLOW;
            else die("--camera must be fit or follow");
        }
        else if (a == "--zoom-lead") { if (++i == argc) die("--zoom-lead requires seconds"); ZOOM_LEAD = atof(argv[i]); }
        else if (a == "--auto-speed") { if (++i == argc) die("--auto-speed requires a value"); AUTO_SPEED = atof(argv[i]); }
        else if (a == "--max-auto-speed") { if (++i == argc) die("--max-auto-speed requires a value"); MAX_AUTO_SPEED = atof(argv[i]); }
        else if (in.empty()) in = a;
        else if (out.empty()) out = a;
        else die("unexpected argument: " + a);
    }
    if (showExpanded && onlyResults) die("--show-expanded and --only-show-results are mutually exclusive");
    if (showExpanded) expand = true;
    if (in.empty() || (!onlyResults && !showExpanded && out.empty())) die("usage: board ops.txt out.mp4 [--minutes M | --speed S] [--decimals N] [--font PATH] [--encoder h264_nvenc] [--format 360p|480p|720p|1080p|1440p|2160p|WxH] [--camera fit|follow] [--zoom-lead S] [--auto-speed R] [--max-auto-speed X] [--expand] [--show-expanded] [--dry] [--only-show-results]\n       out.mp4 may be omitted with --only-show-results or --show-expanded");
    AUTO_SPEED = max(0.f, AUTO_SPEED); MAX_AUTO_SPEED = max(1.f, MAX_AUTO_SPEED); ZOOM_LEAD = max(0.05f, ZOOM_LEAD);
    if (!onlyResults && !showExpanded) {  // results/expanded only: no font, no rendering
        if (FT_Init_FreeType(&ft)) die("freetype");
        vector<string> fonts = {font};
        if (!fontGiven)
            for (const char* p : {"/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Regular.otf",
                                  "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
                                  "/usr/share/fonts/truetype/noto/NotoMono-Regular.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}) fonts.push_back(p);
        bool loaded = false;
        for (auto& p : fonts) if (!FT_New_Face(ft, p.c_str(), 0, &face)) { font = p; loaded = true; break; }
        if (!loaded) die("cannot load font: " + font);
        CW = adv(BIG);
    }
    ifstream f(in); if (!f) die("cannot open " + in);
    vector<string> inputLines; string line;
    while (getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        inputLines.push_back(line);
    }
    if (expand) inputLines = expandLines(inputLines);
    if (showExpanded) {
        for (auto& l : inputLines) printf("%s\n", l.c_str());
        return 0;
    }
    vector<Scene> scenes; vector<string> results; smatch m;
    bool latexBlock = false; string latexEnv;
    for (string l : inputLines) {
        auto p = l.find_first_not_of(" \t"); if (p == string::npos || l[p] == '#') continue;
        string body = l.substr(p, l.find_last_not_of(" \t") - p + 1);
        if (!expand && (startsWithWord(body, "\\let") || startsWithWord(body, "\\calc") ||
                        startsWithWord(body, "\\repeat") || startsWithWord(body, "\\endrepeat") ||
                        body.compare(0, 3, "\\if") == 0 || body == "\\else" || body == "\\endif" ||
                        body.compare(0, 17, "\\begin{algorithm}") == 0 || body == "\\end{algorithm}"))
            die("this directive requires --expand: " + body);
        Meta meta = takeMeta(body);
        if (body.empty()) die("meta command needs an operation on the same line: " + l);
        string env;
        if (latexBlock) {
            if (latexMarker(body, "\\end", env)) {
                if (!latexEnv.empty() && !env.empty() && env != latexEnv)
                    die("mismatched \\end{" + env + "}, expected \\end{" + latexEnv + "}");
                latexBlock = false; latexEnv.clear(); continue;
            }
            if (onlyResults) { results.push_back(body); continue; }  // latex as-is
            if (body.size() >= 2 && body.front() == '$' && body.back() == '$') body = body.substr(1, body.size() - 2);
            scenes.push_back(formula(body));
            fprintf(stderr, "ok (latex): %.100s\n", body.c_str());
            continue;
        }
        if (latexMarker(body, "\\begin", env)) { latexBlock = true; latexEnv = env; continue; }
        if (latexMarker(body, "\\end", env)) die("unexpected \\end without matching \\begin");
        bool math = body.find('\\') != string::npos || body.front() == '$' ||
                    body.find("≈") != string::npos || body.find("≤") != string::npos || body.find("≥") != string::npos;
        if (math) {
            if (onlyResults) { results.push_back(body); continue; }  // latex as-is
            if (body.size() >= 2 && body.front() == '$' && body.back() == '$') body = body.substr(1, body.size() - 2);
            scenes.push_back(formula(body));
            fprintf(stderr, "ok (math): %.100s\n", body.c_str());
            continue;
        }
        if (regex_match(body, m, SQRT_RE)) {
            Num n = parse(m[2]); string name = m[1];
            name.erase(remove_if(name.begin(), name.end(), [](unsigned char c) { return isspace(c); }), name.end());
            if (onlyResults) {
                results.push_back(computeSqrtValue(n, meta.sqrtDigits));
                continue;
            }
            scenes.push_back(sqrtScene(n, name, meta.sqrtDigits));
            fprintf(stderr, "ok (sqrt): %.100s\n", body.c_str());
            continue;
        }
        if (!regex_match(body, m, ARITH_RE))
            die("cannot parse: " + body + "\n  (wrap it in \\begin{latex} ... \\end{latex} to render it as maths)");
        Num a = parse(m[2]), b = parse(m[4]); string op = m[3], name = m[1];
        name.erase(remove_if(name.begin(), name.end(), [](unsigned char c) { return isspace(c); }), name.end());
        if (onlyResults) {
            results.push_back(computeValue(a, b, op, meta.divDigits));
            continue;
        }
        scenes.push_back(op == "+" ? addsub(a, b, true, name) : op == "-" || op == "−" ? addsub(a, b, false, name)
                         : op == "/" || op == ":" || op == "÷" ? divide(a, b, op, name, meta.divDigits) : mul(a, b, name));
        fprintf(stderr, "ok (bc agrees): %.100s\n", l.c_str());
    }
    if (latexBlock) die("unterminated \\begin" + (latexEnv.empty() ? string() : "{" + latexEnv + "}") + " block");
    if (onlyResults) {
        for (auto& r : results) printf("%s\n", r.c_str());
        return 0;
    }
    if (any_of(scenes.begin(), scenes.end(), [](auto& s) { return !s.name.empty(); })) scenes.push_back(summary(scenes));
    vector<double> boost(scenes.size(), 1);
    double weighted = 0, maxBoost = 1;
    for (size_t i = 0; i < scenes.size(); i++) {
        boost[i] = autoSpeed(scenes[i]);
        maxBoost = max(maxBoost, boost[i]);
        weighted += scenes[i].T / boost[i];
    }
    double k = minutes > 0 ? minutes * 60 * FPS / max(weighted, 1e-9) : 1 / max(speed, 1e-9);
    long frames = 0;
    for (size_t i = 0; i < scenes.size(); i++) {
        double ks = k / boost[i];
        for (auto& st : scenes[i].s) { st.t0 *= ks; st.dur *= ks; if (st.te < INF) st.te *= ks; }
        scenes[i].T *= ks; frames += (long)ceil(scenes[i].T) + 1;
    }
    fprintf(stderr, "video length %.1f min (%ld frames, time scale %.4g", frames / (60.0 * FPS), frames, k);
    if (maxBoost > 1.001) fprintf(stderr, ", auto-speed up to %.2fx", maxBoost);
    fprintf(stderr, ")\n");
    if (dry) return 0;
    textures();
    string cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s " + to_string(W) + "x" + to_string(H) + " -r " + to_string(FPS) + " -i - -c:v " + enc +
                 (enc == "libx264" ? " -preset veryfast -crf 20" : "") + " -pix_fmt yuv420p '" + out + "'";
    FILE* ff = popen(cmd.c_str(), "w"); if (!ff) die("ffmpeg");
    long n = 0; for (auto& s : scenes) { render(s, ff, n, frames); fprintf(stderr, "\nscene ends at %.1f s\n", n / (double)FPS); }
    if (pclose(ff)) die("ffmpeg failed");
    fprintf(stderr, "\nwrote %s\n", out.c_str());
}
