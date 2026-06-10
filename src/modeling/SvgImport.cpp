#include "SvgImport.h"
#include "Sketch.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#define NANOSVG_IMPLEMENTATION
#include "../third_party/nanosvg.h"

namespace materializr {

namespace {

// ─── <use> expansion ────────────────────────────────────────────────────────
// nanosvg ignores SVG's <use> element entirely. Logos that build symmetry by
// cloning one path with rotate transforms (the Aperture Science logo, plenty
// of OpenClipArt files, anything Inkscape exports with "Symbols") then lose
// all the cloned instances and only the original path renders. We inline
// every <use href="#X"> into a <g {use's other attrs}>{clone of element X}</g>
// before nanosvg sees the text, which is what an XML-aware renderer would do
// natively. The text-level rewrite is robust enough for the common case
// (well-formed SVG with simple id attrs) without dragging in a real XML lib.

// Locate a tag-attribute occurrence `name="value"` or `name='value'` where
// the attribute starts at a word boundary. Returns npos if not found.
size_t findAttr(const std::string& s, const std::string& name,
                size_t off, std::string* outValue) {
    for (char quote : {'"', '\''}) {
        std::string pat = name + "=" + quote;
        size_t p = off;
        while (p < s.size()) {
            size_t q = s.find(pat, p);
            if (q == std::string::npos) break;
            // word boundary: must follow whitespace or '<' (so id= doesn't match someid=)
            char prev = q > 0 ? s[q - 1] : ' ';
            if (prev != ' ' && prev != '\t' && prev != '\n' &&
                prev != '\r' && prev != '<') { p = q + 1; continue; }
            size_t vs = q + pat.size();
            size_t ve = s.find(quote, vs);
            if (ve == std::string::npos) return std::string::npos;
            if (outValue) *outValue = s.substr(vs, ve - vs);
            return q;
        }
    }
    return std::string::npos;
}

// Find the element whose id attribute equals `id`. Returns the full span
// (open '<' through matching close '>', inclusive). For self-closing tags
// that's just the single tag.
bool findElementById(const std::string& svg, const std::string& id,
                     size_t& outStart, size_t& outEnd) {
    size_t scan = 0;
    while (scan < svg.size()) {
        std::string val;
        size_t attr = findAttr(svg, "id", scan, &val);
        if (attr == std::string::npos) return false;
        if (val != id) { scan = attr + 1; continue; }

        size_t open = svg.rfind('<', attr);
        size_t close = svg.find('>', attr);
        if (open == std::string::npos || close == std::string::npos) return false;
        if (close > open && svg[close - 1] == '/') {
            outStart = open; outEnd = close + 1; return true;
        }
        // Extract tag name
        size_t ns = open + 1, ne = ns;
        while (ne < close &&
               !std::isspace(static_cast<unsigned char>(svg[ne])) &&
               svg[ne] != '/' && svg[ne] != '>') ne++;
        std::string tag = svg.substr(ns, ne - ns);

        // Walk to matching close tag tracking same-name nesting
        int depth = 1;
        size_t p = close + 1;
        while (p < svg.size()) {
            size_t lt = svg.find('<', p);
            if (lt == std::string::npos) return false;
            size_t gt = svg.find('>', lt);
            if (gt == std::string::npos) return false;
            if (lt + 1 < svg.size() && svg[lt + 1] == '/') {
                size_t cs = lt + 2, ce = cs;
                while (ce < gt &&
                       !std::isspace(static_cast<unsigned char>(svg[ce])) &&
                       svg[ce] != '>') ce++;
                if (svg.substr(cs, ce - cs) == tag) {
                    if (--depth == 0) {
                        outStart = open; outEnd = gt + 1; return true;
                    }
                }
            } else if (lt + 1 < svg.size() &&
                       svg[lt + 1] != '!' && svg[lt + 1] != '?') {
                size_t os = lt + 1, oe = os;
                while (oe < gt &&
                       !std::isspace(static_cast<unsigned char>(svg[oe])) &&
                       svg[oe] != '/' && svg[oe] != '>') oe++;
                if (svg.substr(os, oe - os) == tag &&
                    !(gt > 0 && svg[gt - 1] == '/'))
                    depth++;
            }
            p = gt + 1;
        }
        return false;
    }
    return false;
}

// Locate the first <use ...> element. Returns false when none remain.
bool findFirstUse(const std::string& svg, size_t& outStart, size_t& outEnd,
                  std::string& outHref, std::string& outOtherAttrs) {
    size_t off = 0;
    while (off < svg.size()) {
        size_t lt = svg.find("<use", off);
        if (lt == std::string::npos) return false;
        size_t after = lt + 4;
        if (after >= svg.size()) return false;
        char c = svg[after];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
            c != '/' && c != '>') { off = lt + 1; continue; }
        size_t gt = svg.find('>', lt);
        if (gt == std::string::npos) return false;
        bool selfClosing = (gt > 0 && svg[gt - 1] == '/');
        size_t end = gt + 1;
        if (!selfClosing) {
            size_t et = svg.find("</use>", gt);
            if (et == std::string::npos) return false;
            end = et + 6;
        }
        std::string attrs = svg.substr(after, gt - after);
        if (selfClosing && !attrs.empty() && attrs.back() == '/')
            attrs.pop_back();
        std::string href;
        if (findAttr(attrs, "href", 0, &href) == std::string::npos)
            findAttr(attrs, "xlink:href", 0, &href);
        // Strip href / xlink:href (including their leading whitespace) from
        // the attrs so the <g> we emit doesn't carry them.
        auto strip = [&](std::string s, const std::string& name) {
            for (char quote : {'"', '\''}) {
                std::string pat = name + "=" + quote;
                size_t p = s.find(pat);
                if (p == std::string::npos) continue;
                size_t ve = s.find(quote, p + pat.size());
                if (ve == std::string::npos) continue;
                size_t st = p;
                while (st > 0 && (s[st - 1] == ' ' || s[st - 1] == '\t')) st--;
                return s.substr(0, st) + s.substr(ve + 1);
            }
            return s;
        };
        attrs = strip(attrs, "href");
        attrs = strip(attrs, "xlink:href");

        outStart = lt; outEnd = end;
        outHref = href; outOtherAttrs = attrs;
        return true;
    }
    return false;
}

void expandSvgUses(std::string& svg) {
    const int maxIter = 1024; // termination guard — pathological self-refs only
    int expansions = 0;
    for (int it = 0; it < maxIter; ++it) {
        size_t us, ue;
        std::string href, attrs;
        if (!findFirstUse(svg, us, ue, href, attrs)) {
            if (expansions > 0)
                std::fprintf(stderr, "[SVG] expanded %d <use> reference(s)\n",
                             expansions);
            return;
        }
        if (href.empty() || href[0] != '#') {
            svg.erase(us, ue - us); continue;
        }
        std::string id = href.substr(1);
        size_t es, ee;
        if (!findElementById(svg, id, es, ee)) {
            std::fprintf(stderr, "[SVG] <use href=\"#%s\"> target missing\n",
                         id.c_str());
            svg.erase(us, ue - us); continue;
        }
        // A <use> sitting inside its own referenced element would self-loop.
        if (us >= es && us < ee) {
            svg.erase(us, ue - us); continue;
        }
        std::string clone = svg.substr(es, ee - es);
        std::string repl = "<g " + attrs + ">" + clone + "</g>";
        svg.replace(us, ue - us, repl);
        expansions++;
    }
    std::fprintf(stderr, "[SVG] <use> expansion hit %d-iter cap\n", maxIter);
}

// Sample one cubic segment, point count driven by its control-polygon
// length relative to the whole image (same idea as the glyph sampler's
// deflection: dense enough that the chords read as the curve).
void sampleCubic(std::vector<glm::vec2>& out, const float* p, float ref) {
    float clen = 0.0f;
    for (int i = 0; i < 3; ++i)
        clen += std::hypot(p[(i + 1) * 2] - p[i * 2],
                           p[(i + 1) * 2 + 1] - p[i * 2 + 1]);
    int n = std::max(1, static_cast<int>(std::ceil(clen / (0.005f * ref))));
    n = std::min(n, 64);
    for (int i = 1; i <= n; ++i) {
        float t = static_cast<float>(i) / n, u = 1.0f - t;
        out.push_back(glm::vec2(
            u*u*u*p[0] + 3*u*u*t*p[2] + 3*u*t*t*p[4] + t*t*t*p[6],
            u*u*u*p[1] + 3*u*u*t*p[3] + 3*u*t*t*p[5] + t*t*t*p[7]));
    }
}

} // namespace

bool SvgImport::load(const std::string& path, SvgPaths& out) {
    out = SvgPaths();
    // Read the file ourselves so we can preprocess <use> references — nanosvg
    // doesn't understand them and would silently drop every cloned shape.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[SVG] cannot open '%s'\n", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    expandSvgUses(text);
    // nsvgParse mutates the buffer in place — give it a null-terminated copy.
    text.push_back('\0');
    NSVGimage* img = nsvgParse(text.data(), "mm", 96.0f);
    if (!img) {
        std::fprintf(stderr, "[SVG] cannot parse '%s'\n", path.c_str());
        return false;
    }
    const float ref = std::max(1.0f, std::max(img->width, img->height));

    bool haveBB = false;
    for (NSVGshape* sh = img->shapes; sh; sh = sh->next) {
        // SVG fills open paths by implicitly closing them to their start (the
        // even-odd / nonzero fill rules act on the implicitly-closed region).
        // The Aperture logo's blades use `d="m... c... l..."` with no `z` and
        // rely on this: visually filled, but nanosvg reports closed=false. We
        // honour the fill semantics so the imported region is closed too.
        const bool filled = (sh->fill.type != NSVG_PAINT_NONE);
        for (NSVGpath* p = sh->paths; p; p = p->next) {
            if (p->npts < 4) continue;
            std::vector<glm::vec2> pts;
            pts.push_back(glm::vec2(p->pts[0], p->pts[1]));
            for (int i = 0; i < p->npts - 1; i += 3)
                sampleCubic(pts, &p->pts[i * 2], ref);
            // Collapse consecutive duplicates — SVG paths routinely carry
            // degenerate (zero-length) cubics at joints, and a zero-length
            // sketch line would sink the whole wire in buildWires.
            {
                std::vector<glm::vec2> ded;
                ded.reserve(pts.size());
                for (const auto& q : pts)
                    if (ded.empty() ||
                        glm::length(q - ded.back()) > 1e-5f * ref)
                        ded.push_back(q);
                pts.swap(ded);
            }
            // drop the closing duplicate; closure is an explicit line later
            if (pts.size() > 2 &&
                glm::length(pts.front() - pts.back()) < 1e-4f * ref)
                pts.pop_back();
            const bool closed = (p->closed != 0) || filled;
            if (pts.size() < (closed ? 3u : 2u)) continue;
            for (const auto& q : pts) {
                if (!haveBB) {
                    out.bbMin = out.bbMax = q;
                    haveBB = true;
                } else {
                    out.bbMin = glm::min(out.bbMin, q);
                    out.bbMax = glm::max(out.bbMax, q);
                }
            }
            out.loops.push_back(std::move(pts));
            out.closed.push_back(closed);
        }
    }
    nsvgDelete(img);

    if (out.empty()) {
        std::fprintf(stderr, "[SVG] '%s' holds no usable paths\n",
                     path.c_str());
        return false;
    }
    std::fprintf(stderr, "[SVG] '%s': %zu paths, %.1f x %.1f units\n",
                 path.c_str(), out.loops.size(), out.size().x, out.size().y);
    return true;
}

int SvgImport::place(Sketch* sketch, const SvgPaths& svg, glm::vec2 pos,
                     float widthMm, float angleDeg) {
    if (!sketch || svg.empty() || widthMm <= 0.01f) return 0;
    glm::vec2 size = svg.size();
    float rawW = (size.x > 1e-6f) ? size.x : size.y;
    if (rawW <= 1e-6f) return 0;
    const float scale = widthMm / rawW;
    const glm::vec2 center = 0.5f * (svg.bbMin + svg.bbMax);
    const float a = glm::radians(angleDeg);
    const float ca = std::cos(a), sa = std::sin(a);
    auto map = [&](glm::vec2 p) {
        glm::vec2 l = (p - center) * scale;
        l.y = -l.y; // SVG is Y-down; sketches are Y-up
        return pos + glm::vec2(l.x * ca - l.y * sa, l.x * sa + l.y * ca);
    };

    int placed = 0;
    for (size_t li = 0; li < svg.loops.size(); ++li) {
        const auto& loop = svg.loops[li];
        // ids only — SketchPoint* would dangle across reallocations
        std::vector<int> ids;
        ids.reserve(loop.size());
        for (const auto& p : loop)
            ids.push_back(sketch->addPoint(map(p), /*fromText=*/true));
        for (size_t i = 0; i + 1 < ids.size(); ++i)
            sketch->addLine(ids[i], ids[i + 1], /*fromText=*/true);
        if (svg.closed[li] && ids.size() >= 3)
            sketch->addLine(ids.back(), ids.front(), /*fromText=*/true);
        placed++;
    }
    std::fprintf(stderr, "[SVG] placed %d loops at %.1f mm wide\n", placed,
                 widthMm);
    return placed;
}

} // namespace materializr
