// Chalk-on-blackboard column arithmetic -> mp4.  Handles huge (1000+ digit) operands:
// the board is never rasterised as a whole; each frame draws only strokes visible to the camera,
// and the camera zooms out to fit whatever was written during the last ~2 seconds.
// Build: g++ -O2 -std=c++17 board.cpp -o board $(pkg-config --cflags --libs freetype2)
#include <ft2build.h>
#include FT_FREETYPE_H
#include <bits/stdc++.h>
#include <unistd.h>
using namespace std;

const int W = 1280, H = 720, FPS = 30, BIG = 110, SMALL = 50, TITLE = 72;
const float RH = 135, GAP = 28, INF = 1e30f;
const unsigned char CHALK[3] = {238, 242, 236}, GREEN[3] = {34, 72, 50};
int DECIMALS = -1;  // optional cap on quotient decimals; -1 = only the 2*max(A,B) rule
FT_Library ft; FT_Face face;

[[noreturn]] void die(const string& m) { fprintf(stderr, "error: %s\n", m.c_str()); exit(1); }

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
        for (char32_t c : t) { if (c != ' ') { s.push_back({T, per, INF, x, y, 0, 0, size, c}); T += per; } x += a; }
        T += pause; return first;
    }
    float text(const string& t, float x, float y, int size = TITLE, float pause = 0) {
        glyphs(u32(t), x, y, size, 2, pause); return x + adv(size) * u32(t).size(); }
    size_t big(char32_t c, float x, float y, float pause = 4) { return glyphs(u32string(1, c), x, y, BIG, 6, pause); }
    size_t small(const string& t, float cx, float y, float pause = 3) {
        return glyphs(u32(t), cx - adv(SMALL) * t.size() / 2, y, SMALL, 3, pause); }
    void line(float x1, float y1, float x2, float y2, float pause = 6) {
        float d = min(60.f, max(4.f, fabsf(x2 - x1) / 40)); s.push_back({T, d, INF, x1, y1, x2, y2, 0, 0}); T += d + pause; }
    void erase(size_t from, size_t to) { for (size_t i = from; i < to; i++) s[i].te = T; }
    void pause(float n) { T += n; }
};
array<float, 4> bbox(const Stroke& k) {
    if (!k.size) return {min(k.x, k.x2) - 4, min(k.y, k.y2) - 4, max(k.x, k.x2) + 4, max(k.y, k.y2) + 4};
    const Glyph& g = glyph(k.size, k.ch);
    float x = k.x + g.left, y = k.y + asc(k.size) - g.top;
    return {x, y, x + max(g.w, 1), y + max(g.h, 1)};
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
            while (peek() == '_' || peek() == '^') {
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
Big mulS(const Big& a, int q) { Big r; int c = 0; for (int x : a) { int v = x * q + c; r.push_back(v % 10); c = v / 10; } if (c) r.push_back(c); trim(r); return r; }
bool isZero(const Big& a) { return a.size() == 1 && !a[0]; }
int sigDigits(const Num& n) { Big t = n.d; trim(t); return t.size(); }  // digits ignoring leading zeros

Num parse(string s) {
    Num n; s.erase(remove(s.begin(), s.end(), ' '), s.end());
    auto p = s.find(','); string i = s.substr(0, p), f = p == string::npos ? "" : s.substr(p + 1);
    i.erase(0, min(i.find_first_not_of('0'), i.size())); if (i.empty()) i = "0";
    n.dec = f.size(); string all = i + f;
    for (auto it = all.rbegin(); it != all.rend(); ++it) n.d.push_back(*it - '0');
    return n;
}
int get(const Num& n, int e) { int k = e + n.dec; return k >= 0 && k < (int)n.d.size() ? n.d[k] : 0; }
int hiE(const Num& n) { return (int)n.d.size() - n.dec - 1; }
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
string bc(const string& expr) {
    char path[] = "/tmp/board_bcXXXXXX"; int fd = mkstemp(path); if (fd < 0) die("mkstemp");
    string in = "scale=10000\n" + expr + "\nquit\n"; if (write(fd, in.data(), in.size()) < 0) die("write"); close(fd);
    FILE* p = popen(("BC_LINE_LENGTH=0 bc -q " + string(path)).c_str(), "r"); string out; char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p); unlink(path); return norm(out);
}

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

// long division, Swedish "trappan": divisor left of the bar, quotient above the dividend
Scene divide(const Num& a, const Num& b, const string& op, const string& name) {
    Scene sc = start(name, fmtNum(a) + " " + op + " " + fmtNum(b));
    Big d = b.d; trim(d); if (isZero(d)) die("division by zero");
    Num A = a; int k = b.dec;  // move the comma k steps right in both numbers
    if (A.dec < k) { A.d.insert(A.d.begin(), k - A.dec, 0); A.dec = 0; } else A.dec -= k;
    while ((int)A.d.size() > A.dec + 1 && !A.d.back()) A.d.pop_back();
    Num Dv{d, 0, ""};
    if (k) sc.titleEnd = sc.text(" = " + fmtNum(A) + " " + op + " " + fmtNum(Dv), sc.titleEnd, 30, TITLE, 30);
    int nInt = A.d.size() - A.dec, nD = A.d.size();
    int N = max(sigDigits(a), sigDigits(b));  // max significant digits in the operands
    int decCap = 2 * N;                       // significant quotient decimals: 2 * max(A, B)
    if (DECIMALS >= 0) decCap = min(decCap, DECIMALS);
    auto digit = [&](int c) { int i = nD - 1 - c; return i >= 0 ? A.d[i] : 0; };
    auto qdig = [&](Big& r) { int q = 9; while (cmp(mulS(d, q), r) > 0) q--; return q; };
    auto push10 = [&](Big& r, int dg) { r.insert(r.begin(), dg); trim(r); };
    int cEnd = 0;
    { Big r{0}; int sigFrac = 0;
      for (;;) {
        int e = nInt - 1 - cEnd;
        if (e < 0) {
            if (sigFrac >= decCap) break;                 // reached the decimal budget
            if (cEnd >= nD && isZero(r)) break;           // exact, nothing left to bring down
        }
        push10(r, digit(cEnd));
        int q = qdig(r);
        r = subB(r, mulS(d, q));
        if (e < 0 && (sigFrac > 0 || q)) sigFrac++;       // leading zeros after the comma are free
        cEnd++;
      } }
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
        auto b = BB[i] = bbox(S[i]); BW = max(BW, b[2] + 120); BH = max(BH, b[3] + 120);
        for (int bx = max(0.f, b[0]) / BK; bx <= (int)max(0.f, b[2]) / BK; bx++)
            for (int by = max(0.f, b[1]) / BK; by <= (int)max(0.f, b[3]) / BK; by++) buckets[(uint64_t)bx << 32 | by].push_back(i);
    }
    vector<long> stamp(S.size(), -1); vector<float> acc(W * H); vector<uint8_t> fr(W * H * 3);
    const float M = 100, WIN = 60, zmax = max({1.f, BW / W, BH / H}) * 1.05f;
    float zc = 1, cam[2] = {0, 0}, tgt[2] = {0, 0}, bnd[2] = {BW, BH}; bool first = true;
    array<float, 4> act{0, 0, (float)W, (float)H}; size_t wa = 0, wb = 0;
    auto pen = [&](const Stroke& k, float f) { float p = k.dur < 1e-6f ? 1 : clamp((f - k.t0 + 1) / k.dur, 0.f, 1.f);
        return make_pair(k.x + (k.x2 - k.x) * p, k.y + (k.y2 - k.y) * p); };
    long nF = (long)ceil(sc.T) + 1; auto t0 = chrono::steady_clock::now();
    for (long f = 0; f < nF; f++, frameNo++) {
        // --- camera: fit everything written during the last WIN frames ---
        while (wb < S.size() && S[wb].t0 <= f) wb++;
        while (wa < wb && S[wa].t0 < f - WIN) wa++;
        if (wa < wb) {
            act = {INF, INF, -INF, -INF};
            for (size_t i = wa; i < wb; i++) {
                array<float, 4> b = BB[i];
                if (!S[i].size) { auto [px, py] = pen(S[i], f); b = {px - 4, py - 4, px + 4, py + 4}; }
                act = {min(act[0], b[0]), min(act[1], b[1]), max(act[2], b[2]), max(act[3], b[3])};
            }
        }
        float zt = clamp(max((act[2] - act[0]) / (W - 2 * M), (act[3] - act[1]) / (H - 2 * M)), 1.f, zmax);
        zc = first ? zt : zc * powf(zt / zc, 0.04f);
        float vw[2] = {W * zc, H * zc}, mb = M * zc;
        for (int ax = 0; ax < 2; ax++) {
            float lo = act[ax], hi = act[ax + 2];
            if (hi - lo > vw[ax] - 2 * mb) tgt[ax] = (lo + hi - vw[ax]) / 2;
            else { if (lo < tgt[ax] + mb) tgt[ax] = lo - mb; if (hi > tgt[ax] + vw[ax] - mb) tgt[ax] = hi - vw[ax] + mb; }
            tgt[ax] = clamp(tgt[ax], 0.f, max(0.f, bnd[ax] - vw[ax]));
            cam[ax] = first ? tgt[ax] : cam[ax] + (tgt[ax] - cam[ax]) * 0.08f;
        }
        first = false;
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
                        float hw = 3 * iz, al = 1; if (hw < 0.5f) { al = hw * 2; hw = 0.5f; }
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
    double minutes = 0, speed = 1; bool dry = false;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "-o") { if (++i == argc) die("-o requires an output file"); out = argv[i]; }
        else if (a == "--minutes") minutes = atof(argv[++i]);
        else if (a == "--speed") speed = atof(argv[++i]); else if (a == "--encoder") enc = argv[++i];
        else if (a == "--decimals") DECIMALS = atoi(argv[++i]); else if (a == "--dry") dry = true;
        else if (a == "--font") { if (++i == argc) die("--font requires a path"); font = argv[i]; fontGiven = true; }
        else if (in.empty()) in = a;
        else if (out.empty()) out = a;
        else die("unexpected argument: " + a);
    }
    if (in.empty() || out.empty()) die("usage: board ops.txt out.mp4 [--minutes M | --speed S] [--decimals N] [--font PATH] [--encoder h264_nvenc] [--dry]");
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
    ifstream f(in); if (!f) die("cannot open " + in);
    regex re(R"(^\s*(?:([A-Za-z][A-Za-z0-9]*(?:_(?:\{[^}]*\}|[A-Za-z0-9]))?)\s*=\s*)?([0-9 ,]*[0-9,])\s*(\+|-|\*|x|×|−|/|:|÷)\s*([0-9 ,]*[0-9,])\s*$)");
    vector<Scene> scenes; string l; smatch m;
    while (getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        auto p = l.find_first_not_of(" \t"); if (p == string::npos || l[p] == '#') continue;
        string body = l.substr(p, l.find_last_not_of(" \t") - p + 1);
        bool math = body.find('\\') != string::npos || body.front() == '$' ||
                    body.find("≈") != string::npos || body.find("≤") != string::npos || body.find("≥") != string::npos;
        if (math) {
            if (body.size() >= 2 && body.front() == '$' && body.back() == '$') body = body.substr(1, body.size() - 2);
            scenes.push_back(formula(body));
            fprintf(stderr, "ok (math): %.100s\n", body.c_str());
            continue;
        }
        if (!regex_match(l, m, re)) die("cannot parse: " + l);
        Num a = parse(m[2]), b = parse(m[4]); string op = m[3], name = m[1];
        scenes.push_back(op == "+" ? addsub(a, b, true, name) : op == "-" || op == "−" ? addsub(a, b, false, name)
                         : op == "/" || op == ":" || op == "÷" ? divide(a, b, op, name) : mul(a, b, name));
        fprintf(stderr, "ok (bc agrees): %.100s\n", l.c_str());
    }
    if (any_of(scenes.begin(), scenes.end(), [](auto& s) { return !s.name.empty(); })) scenes.push_back(summary(scenes));
    double total = 0; for (auto& s : scenes) total += s.T;
    double k = minutes > 0 ? minutes * 60 * FPS / total : 1 / speed;
    long frames = 0;
    for (auto& s : scenes) { for (auto& st : s.s) { st.t0 *= k; st.dur *= k; if (st.te < INF) st.te *= k; } s.T *= k; frames += (long)ceil(s.T) + 1; }
    fprintf(stderr, "video length %.1f min (%ld frames, time scale %.4g)\n", frames / (60.0 * FPS), frames, k);
    if (dry) return 0;
    textures();
    string cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i - -c:v " + enc +
                 (enc == "libx264" ? " -preset veryfast -crf 20" : "") + " -pix_fmt yuv420p '" + out + "'";
    FILE* ff = popen(cmd.c_str(), "w"); if (!ff) die("ffmpeg");
    long n = 0; for (auto& s : scenes) { render(s, ff, n, frames); fprintf(stderr, "\nscene ends at %.1f s\n", n / (double)FPS); }
    if (pclose(ff)) die("ffmpeg failed");
    fprintf(stderr, "\nwrote %s\n", out.c_str());
}
