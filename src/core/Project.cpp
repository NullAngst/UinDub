// SPDX-License-Identifier: GPL-3.0-or-later
#include "Project.h"

#include "shared/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ud {

namespace fs = std::filesystem;

std::string Project::projectFile() const { return (fs::path(folder) / "project.json").string(); }
std::string Project::offsetsPath() const { return (fs::path(folder) / "offsets.zst").string(); }
std::string Project::tracksPath() const { return (fs::path(folder) / "tracks.zst").string(); }
std::string Project::cacheDir() const { return (fs::path(folder) / "preview-cache").string(); }

namespace {

const char* refKindName(RefKind k) {
    switch (k) {
    case RefKind::LeftBorder: return "left-border";
    case RefKind::RightBorder: return "right-border";
    default: return "drawn";
    }
}
RefKind refKindFrom(const std::string& s) {
    if (s == "left-border") return RefKind::LeftBorder;
    if (s == "right-border") return RefKind::RightBorder;
    return RefKind::Drawn;
}

JsonValue arr3(const std::array<double, 3>& a) {
    JsonValue v = JsonValue::array();
    for (double x : a) v.push(x);
    return v;
}
std::array<double, 3> arr3From(const JsonValue& v, double def) {
    std::array<double, 3> a{def, def, def};
    for (size_t i = 0; i < 3 && i < v.size(); ++i) a[i] = v[i].num(def);
    return a;
}

JsonValue colorJson(const ColorSettings& c) {
    JsonValue j = JsonValue::object();
    j.set("brightness", c.brightness).set("contrast", c.contrast).set("gamma", c.gamma);
    j.set("black_point", c.blackPoint).set("white_point", c.whitePoint);
    j.set("saturation", c.saturation).set("hue", c.hue);
    j.set("lift", arr3(c.lift)).set("gamma_rgb", arr3(c.gammaRgb)).set("gain", arr3(c.gain));
    j.set("chroma_delay_x", c.chromaDelayX).set("chroma_delay_y", c.chromaDelayY).set("chroma_blur", c.chromaBlur);
    return j;
}

ColorSettings colorFrom(const JsonValue& j) {
    ColorSettings c;
    c.brightness = j["brightness"].num(0);
    c.contrast = j["contrast"].num(1);
    c.gamma = j["gamma"].num(1);
    c.blackPoint = j["black_point"].num(16);
    c.whitePoint = j["white_point"].num(235);
    c.saturation = j["saturation"].num(1);
    c.hue = j["hue"].num(0);
    c.lift = arr3From(j["lift"], 0);
    c.gammaRgb = arr3From(j["gamma_rgb"], 1);
    c.gain = arr3From(j["gain"], 1);
    c.chromaDelayX = j["chroma_delay_x"].num(0);
    c.chromaDelayY = j["chroma_delay_y"].num(0);
    c.chromaBlur = j["chroma_blur"].num(0);
    return c;
}

JsonValue spanJson(const RefSpan& s) {
    JsonValue j = JsonValue::object();
    j.set("start", (long long)s.start).set("end", (long long)s.end).set("def_frame", (long long)s.defFrame).set("template_step", s.templStep);
    JsonValue pts = JsonValue::array();
    for (const PointF& p : s.points) {
        JsonValue q = JsonValue::array();
        q.push(p.x).push(p.y);
        pts.push(q);
    }
    j.set("points", pts);
    JsonValue t = JsonValue::array();
    for (float v : s.templ) t.push(double(v));
    j.set("template", t);
    return j;
}

RefSpan spanFrom(const JsonValue& j) {
    RefSpan s;
    s.start = j["start"].integer(0);
    s.end = j["end"].integer(0);
    s.defFrame = j["def_frame"].integer(0);
    s.templStep = j["template_step"].num(0);
    for (const JsonValue& p : j["points"].a) s.points.push_back({p[size_t(0)].num(0), p[size_t(1)].num(0)});
    for (const JsonValue& v : j["template"].a) s.templ.push_back(float(v.num(0)));
    return s;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

JsonValue edgeSettingsJson(const EdgeLockSettings& s) {
    JsonValue j = JsonValue::object();
    j.set("search_range", s.searchRange).set("top_lines", s.topLines).set("top_range", s.topRange);
    j.set("template_width", s.templateWidth).set("confidence", s.confidence).set("max_gap", s.maxGap);
    j.set("motion_smoothing", s.motionSmoothing).set("strength", s.strength).set("fill", s.fillRepeat ? "repeat" : "black");
    j.set("length_correction", s.lengthCorrection);
    return j;
}

EdgeLockSettings edgeSettingsFromJson(const JsonValue& j) {
    EdgeLockSettings s;
    s.searchRange = int(j["search_range"].integer(12));
    s.topLines = int(j["top_lines"].integer(40));
    s.topRange = int(j["top_range"].integer(24));
    s.templateWidth = int(j["template_width"].integer(24));
    s.confidence = j["confidence"].num(0.6);
    s.maxGap = int(j["max_gap"].integer(24));
    s.motionSmoothing = int(j["motion_smoothing"].integer(15));
    s.strength = j["strength"].num(1.0);
    s.fillRepeat = j["fill"].str("black") == "repeat";
    s.lengthCorrection = j["length_correction"].boolean(false);
    return s;
}

JsonValue Project::settingsJson() const {
    JsonValue j = JsonValue::object();
    j.set("uindub_version", UINDUB_VERSION);
    JsonValue src = JsonValue::object();
    src.set("path", sourcePath).set("join_segments", joinSegments).set("tape_label", tapeLabel);
    src.set("field_order", fieldOrderName(fieldOrder)).set("field_override", fieldOverride);
    j.set("source", src);

    JsonValue el = JsonValue::object();
    el.set("enabled", edgeEnabled).set("settings", edgeSettingsJson(edge)).set("analysis_key", analysisKey);
    JsonValue refsJ = JsonValue::array();
    for (const Reference& r : refs) {
        JsonValue rj = JsonValue::object();
        rj.set("id", r.id).set("kind", refKindName(r.kind)).set("name", r.name).set("enabled", r.enabled);
        JsonValue sp = JsonValue::array();
        for (const RefSpan& s : r.spans) sp.push(spanJson(s));
        rj.set("spans", sp);
        refsJ.push(rj);
    }
    el.set("references", refsJ);
    JsonValue ov = JsonValue::array();
    for (const EdgeOverride& o : overrides) {
        JsonValue oj = JsonValue::object();
        oj.set("kind", o.kind == EdgeOverride::Kind::DontCorrect ? "dont-correct" : "reanalyze");
        oj.set("start", (long long)o.start).set("end", (long long)o.end);
        if (o.kind == EdgeOverride::Kind::Reanalyze) oj.set("settings", edgeSettingsJson(o.settings));
        ov.push(oj);
    }
    el.set("overrides", ov);
    JsonValue cuts = JsonValue::array();
    for (int64_t c : detectedCuts) cuts.push((long long)c);
    el.set("detected_cuts", cuts);
    JsonValue ends = JsonValue::array();
    for (const auto& [slot, f] : autoEnds) {
        JsonValue e = JsonValue::array();
        e.push(slot).push((long long)f);
        ends.push(e);
    }
    el.set("auto_ends", ends);
    j.set("edge_lock", el);

    JsonValue d = JsonValue::object();
    d.set("mode", deintModeName(deint.mode)).set("double_rate", deint.doubleRate).set("qtgmc_preset", deint.qtgmcPreset);
    d.set("source_match", deint.sourceMatch).set("sharpness", deint.sharpness);
    j.set("deinterlace", d);

    JsonValue c = JsonValue::object();
    c.set("clip", colorJson(color)).set("per_chapter", colorPerChapter);
    JsonValue cc = JsonValue::object();
    for (const auto& [k, v] : chapterColors) cc.set(std::to_string(k), colorJson(v));
    c.set("chapters", cc);
    j.set("color", c);

    JsonValue cr = JsonValue::object();
    cr.set("left", crop.left).set("right", crop.right).set("top", crop.top).set("bottom", crop.bottom).set("square_pixels", crop.squarePixels);
    j.set("crop", cr);

    JsonValue ed = JsonValue::object();
    ed.set("in", (long long)edit.in).set("out", (long long)edit.out);
    JsonValue cs = JsonValue::array();
    for (const CutSpan& s : edit.cuts) {
        JsonValue e = JsonValue::array();
        e.push((long long)s.start).push((long long)s.end);
        cs.push(e);
    }
    ed.set("cuts", cs);
    j.set("edit", ed);

    JsonValue o = JsonValue::object();
    o.set("codec", videoCodecName(out.codec)).set("rf", out.rf).set("preset", out.preset).set("chroma_422", out.chroma422);
    o.set("audio", audioCodecName(out.audio)).set("opus_kbps", out.opusKbps).set("aac_kbps", out.aacKbps);
    o.set("container", out.mp4 ? "mp4" : "mkv").set("folder", out.folder).set("name", out.name);
    o.set("chapters", out.chapters).set("captions", out.captions);
    j.set("output", o);

    if (hasTruth) {
        JsonValue t = JsonValue::object();
        t.set("jitter", truth.jitter).set("wobble", truth.wobble).set("wobble_period", truth.wobblePeriod);
        t.set("wobble_crawl", truth.wobbleCrawl).set("hook", truth.hook).set("hook_lines", truth.hookLines);
        t.set("field_offset", truth.fieldOffset).set("seed", (long long)truth.seed);
        j.set("warble_truth", t);
    }
    return j;
}

bool Project::fromJson(const JsonValue& j, std::string& err) {
    if (!j.isObject()) {
        err = "not a project file";
        return false;
    }
    const JsonValue& src = j["source"];
    sourcePath = src["path"].str();
    joinSegments = src["join_segments"].boolean(true);
    tapeLabel = src["tape_label"].str();
    fieldOrder = fieldOrderFromName(src["field_order"].str());
    fieldOverride = src["field_override"].boolean(false);

    const JsonValue& el = j["edge_lock"];
    edgeEnabled = el["enabled"].boolean(false);
    edge = edgeSettingsFromJson(el["settings"]);
    analysisKey = el["analysis_key"].str();
    refs.clear();
    for (const JsonValue& rj : el["references"].a) {
        Reference r;
        r.id = int(rj["id"].integer(0));
        r.kind = refKindFrom(rj["kind"].str());
        r.name = rj["name"].str();
        r.enabled = rj["enabled"].boolean(true);
        for (const JsonValue& s : rj["spans"].a) r.spans.push_back(spanFrom(s));
        refs.push_back(r);
    }
    overrides.clear();
    for (const JsonValue& oj : el["overrides"].a) {
        EdgeOverride o;
        o.kind = oj["kind"].str() == "reanalyze" ? EdgeOverride::Kind::Reanalyze : EdgeOverride::Kind::DontCorrect;
        o.start = oj["start"].integer(0);
        o.end = oj["end"].integer(0);
        if (oj.has("settings")) o.settings = edgeSettingsFromJson(oj["settings"]);
        overrides.push_back(o);
    }
    detectedCuts.clear();
    for (const JsonValue& c : el["detected_cuts"].a) detectedCuts.push_back(c.integer());
    autoEnds.clear();
    for (const JsonValue& e : el["auto_ends"].a) autoEnds.push_back({int(e[size_t(0)].integer()), e[size_t(1)].integer()});

    const JsonValue& d = j["deinterlace"];
    deint.mode = deintModeFromName(d["mode"].str("qtgmc"));
    deint.doubleRate = d["double_rate"].boolean(true);
    deint.qtgmcPreset = d["qtgmc_preset"].str("Slower");
    deint.sourceMatch = int(d["source_match"].integer(0));
    deint.sharpness = d["sharpness"].num(-1);

    const JsonValue& c = j["color"];
    color = c.has("clip") ? colorFrom(c["clip"]) : ColorSettings{};
    colorPerChapter = c["per_chapter"].boolean(false);
    chapterColors.clear();
    for (const auto& [k, v] : c["chapters"].o) chapterColors[std::atoi(k.c_str())] = colorFrom(v);

    const JsonValue& cr = j["crop"];
    crop.left = int(cr["left"].integer(0));
    crop.right = int(cr["right"].integer(0));
    crop.top = int(cr["top"].integer(0));
    crop.bottom = int(cr["bottom"].integer(0));
    crop.squarePixels = cr["square_pixels"].boolean(false);

    const JsonValue& ed = j["edit"];
    edit.in = ed["in"].integer(0);
    edit.out = ed["out"].integer(-1);
    edit.cuts.clear();
    for (const JsonValue& s : ed["cuts"].a) edit.cuts.push_back({s[size_t(0)].integer(), s[size_t(1)].integer()});

    const JsonValue& o = j["output"];
    if (o.isObject()) {
        out.codec = videoCodecFromName(o["codec"].str("x265"));
        out.rf = int(o["rf"].integer(rfDefault(out.codec)));
        out.preset = o["preset"].str(presetDefault(out.codec));
        out.chroma422 = o["chroma_422"].boolean(false);
        out.audio = audioCodecFromName(o["audio"].str("flac"));
        out.opusKbps = int(o["opus_kbps"].integer(192));
        out.aacKbps = int(o["aac_kbps"].integer(256));
        out.mp4 = o["container"].str("mkv") == "mp4";
        out.folder = o["folder"].str();
        out.name = o["name"].str();
        out.chapters = o["chapters"].boolean(true);
        out.captions = o["captions"].boolean(true);
    }
    hasTruth = j.has("warble_truth");
    if (hasTruth) {
        const JsonValue& t = j["warble_truth"];
        truth.jitter = t["jitter"].num(0);
        truth.wobble = t["wobble"].num(0);
        truth.wobblePeriod = t["wobble_period"].num(60);
        truth.wobbleCrawl = t["wobble_crawl"].num(0.12);
        truth.hook = t["hook"].num(0);
        truth.hookLines = t["hook_lines"].num(30);
        truth.fieldOffset = t["field_offset"].num(0);
        truth.seed = uint32_t(t["seed"].integer(1));
    }
    return true;
}

bool Project::save(std::string& err) const {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        err = "cannot create " + folder + ": " + ec.message();
        return false;
    }
    return writeJsonFile(projectFile(), settingsJson(), err);
}

bool Project::load(const std::string& path, Project& out, std::string& err) {
    std::string file = path;
    std::error_code ec;
    if (fs::is_directory(path, ec)) file = (fs::path(path) / "project.json").string();
    JsonValue j;
    if (!readJsonFile(file, j, err)) return false;
    Project p;
    p.folder = fs::path(file).parent_path().string();
    if (!p.fromJson(j, err)) return false;
    out = std::move(p);
    return true;
}

std::string Project::currentAnalysisKey() const {
    std::ostringstream s;
    s << fieldOrderName(renderFieldOrder()) << '|' << edge.searchRange << ',' << edge.topLines << ',' << edge.topRange << ','
      << edge.templateWidth << ',' << edge.confidence << ',' << edge.maxGap << ',' << edge.motionSmoothing << ',' << edge.lengthCorrection;
    for (const Reference& r : refs) {
        if (!r.enabled) continue;
        s << "|r" << int(r.kind);
        for (const RefSpan& sp : r.spans) {
            s << '[' << sp.start << ',' << sp.end;
            for (const PointF& p : sp.points) s << ',' << std::lround(p.x * 1000) << ',' << std::lround(p.y * 1000);
            for (float v : sp.templ) s << ',' << std::lround(v * 1000);
            s << ']';
        }
    }
    char hex[20];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)fnv1a(s.str()));
    return hex;
}

bool Project::analysisCurrent() const {
    std::error_code ec;
    return !analysisKey.empty() && analysisKey == currentAnalysisKey() && fs::exists(offsetsPath(), ec);
}

bool Project::edgeActive() const {
    if (!edgeEnabled) return false;
    for (const Reference& r : refs)
        if (r.enabled && !r.spans.empty()) return true;
    return false;
}

const ColorSettings& Project::colorFor(int64_t frame, const std::vector<Chapter>& chapters) const {
    if (!colorPerChapter) return color;
    for (size_t i = 0; i < chapters.size(); ++i)
        if (frame >= chapters[i].start && frame < chapters[i].end) {
            auto it = chapterColors.find(int(i));
            return it == chapterColors.end() ? color : it->second;
        }
    return color;
}

static std::string videosDir() {
    const char* home = std::getenv("HOME");
    const std::string h = home ? home : "/tmp";
    // ~/.config/user-dirs.dirs: XDG_VIDEOS_DIR="$HOME/Videos"
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    std::ifstream f((cfg && *cfg ? std::string(cfg) : h + "/.config") + "/user-dirs.dirs");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("XDG_VIDEOS_DIR=", 0) != 0) continue;
        std::string v = line.substr(15);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        const size_t p = v.find("$HOME");
        if (p != std::string::npos) v.replace(p, 5, h);
        if (!v.empty() && v != h) return v;
    }
    return h + "/Videos";
}

std::string defaultProjectFolder(const std::string& sourcePath) {
    const std::string stem = fs::path(captureBaseFor(sourcePath)).filename().string();
    return (fs::path(videosDir()) / "UinDub" / stem).string();
}

bool moveProject(Project& p, const std::string& newFolder, std::string& err) {
    std::error_code ec;
    if (fs::exists(newFolder, ec) && !fs::is_empty(newFolder, ec)) {
        err = newFolder + " is not empty";
        return false;
    }
    fs::create_directories(fs::path(newFolder).parent_path(), ec);
    fs::rename(p.folder, newFolder, ec);
    if (ec) {
        // Across file systems: copy, then remove the old one.
        ec.clear();
        fs::copy(p.folder, newFolder, fs::copy_options::recursive, ec);
        if (ec) {
            err = "cannot move the project: " + ec.message();
            return false;
        }
        fs::remove_all(p.folder, ec);
    }
    p.folder = newFolder;
    return p.save(err);
}

}  // namespace ud
