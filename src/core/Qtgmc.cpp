// SPDX-License-Identifier: GPL-3.0-or-later
#include "Qtgmc.h"

#include <VSScript4.h>
#include <VapourSynth4.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace ud {

namespace fs = std::filesystem;

const std::vector<std::string>& qtgmcPresets() {
    static const std::vector<std::string> p = {"Placebo", "Very Slow", "Slower", "Slow", "Medium", "Fast",
                                               "Faster", "Very Fast", "Super Fast", "Ultra Fast", "Draft"};
    return p;
}

namespace {

const VSSCRIPTAPI* g_vss = nullptr;
const VSAPI* g_vs = nullptr;
VsRuntimeInfo g_info;
std::once_flag g_once;

std::string selfDir() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return fs::path(buf).parent_path().string();
}

std::string firstGlob(const fs::path& dir, const std::string& prefix, const std::string& contains) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    std::vector<std::string> hits;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && n.find(contains) != std::string::npos) hits.push_back(e.path().string());
    }
    std::sort(hits.begin(), hits.end());
    return hits.empty() ? std::string() : hits.back();
}

// The stack the AppImage carries: usr/lib/uindub/python next to usr/bin.
bool findBundled(VsRuntimeInfo& r, std::string& home) {
    const std::string bin = selfDir();
    if (bin.empty()) return false;
    const fs::path base = fs::path(bin).parent_path() / "lib" / "uindub" / "python";
    std::error_code ec;
    const fs::path py = base / "bin" / "python3";
    if (!fs::exists(py, ec)) return false;
    // The real library, libpython3.N.so.1.0, not the libpython3.so stable-ABI stub.
    std::string lib;
    {
        const std::regex re(R"(libpython3\.[0-9]+\.so(\.[0-9.]+)?)");
        std::error_code e2;
        for (const auto& e : fs::directory_iterator(base / "lib", e2)) {
            const std::string n = e.path().filename().string();
            if (std::regex_match(n, re) && n.size() > fs::path(lib).filename().string().size()) lib = e.path().string();
        }
    }
    const std::string pydir = firstGlob(base / "lib", "python3.", "");
    if (lib.empty() || pydir.empty()) return false;
    const fs::path vss = fs::path(pydir) / "site-packages" / "vapoursynth" / "libvsscript.so";
    if (!fs::exists(vss, ec)) return false;
    r.python = py.lexically_normal().string();
    r.pythonLib = fs::path(lib).lexically_normal().string();
    r.vsscript = vss.lexically_normal().string();
    home = base.lexically_normal().string();
    return true;
}

std::string shellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

// A development build: ask the Python named by UINDUB_PYTHON where things are.
bool findDev(VsRuntimeInfo& r, std::string& err) {
    const char* py = std::getenv("UINDUB_PYTHON");
    if (!py || !*py) return false;
    const std::string cmd = shellQuote(py) +
                            " -c \"import sys, os, sysconfig, vapoursynth;"
                            "d = sysconfig.get_config_var('LIBDIR') or '';"
                            "n = sysconfig.get_config_var('INSTSONAME') or sysconfig.get_config_var('LDLIBRARY') or '';"
                            "print(sys.executable); print(os.path.join(d, n));"
                            "print(os.path.join(os.path.dirname(vapoursynth.__file__), 'libvsscript.so'))\" 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        err = "cannot run " + std::string(py);
        return false;
    }
    std::vector<std::string> lines;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        lines.push_back(l);
    }
    const int rc = pclose(p);
    if (rc != 0 || lines.size() < 3) {
        err = "UINDUB_PYTHON could not import vapoursynth: " + (lines.empty() ? std::string("no output") : lines.back());
        return false;
    }
    r.python = lines[lines.size() - 3];
    r.pythonLib = fs::path(lines[lines.size() - 2]).lexically_normal().string();
    r.vsscript = fs::path(lines[lines.size() - 1]).lexically_normal().string();
    return true;
}

std::string tomlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    return o;
}

void initRuntime() {
    VsRuntimeInfo& r = g_info;
    std::string home, err;
    const bool bundled = findBundled(r, home);
    if (!bundled && !findDev(r, err)) {
        r.error = err.empty() ? "The QTGMC stack (VapourSynth and its Python) is not part of this build. Use bwdif, or run the AppImage."
                              : err;
        return;
    }
    // VSScript finds its Python through vapoursynth.toml, keyed by the
    // library's own path. A private copy keeps the user's config untouched.
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* homeEnv = std::getenv("HOME");
    const fs::path cfgRoot = (cache && *cache ? fs::path(cache) : fs::path(homeEnv ? homeEnv : "/tmp") / ".cache") / "uindub" / "vsconfig";
    std::error_code ec;
    fs::create_directories(cfgRoot / "vapoursynth", ec);
    struct stat st {};
    if (::stat(r.python.c_str(), &st) != 0) {
        r.error = "cannot stat " + r.python;
        return;
    }
    {
        std::ofstream toml(cfgRoot / "vapoursynth" / "vapoursynth.toml", std::ios::trunc);
        std::vector<std::string> keys = {r.vsscript};
        std::string alt = r.vsscript;
        const size_t pos = alt.find("/lib64/");
        if (pos != std::string::npos) keys.push_back(alt.replace(pos, 7, "/lib/"));
        for (const std::string& k : keys)
            toml << '"' << tomlEscape(k) << "\" = [\"" << tomlEscape(r.python) << "\",\"" << tomlEscape(r.pythonLib) << "\",\""
                 << (long long)st.st_mtime << "\"]\n";
    }
    // Never let the user's own site-packages shadow the bundled ones.
    setenv("PYTHONNOUSERSITE", "1", 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    if (bundled) setenv("PYTHONHOME", home.c_str(), 1);
    const char* oldCfg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = oldCfg ? oldCfg : "";
    setenv("XDG_CONFIG_HOME", cfgRoot.c_str(), 1);
    void* h = dlopen(r.vsscript.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        r.error = std::string("cannot load VapourSynth: ") + dlerror();
    } else {
        using GetApi = const VSSCRIPTAPI* (*)(int);
        using LastError = const char* (*)();
        auto get = reinterpret_cast<GetApi>(dlsym(h, "getVSScriptAPI"));
        auto last = reinterpret_cast<LastError>(dlsym(h, "getVSScriptAPILastError"));
        if (!get) {
            r.error = "the VapourSynth library has no getVSScriptAPI";
        } else {
            g_vss = get(VSSCRIPT_API_VERSION);
            if (!g_vss) {
                r.error = std::string("VapourSynth would not start: ") + (last && last() ? last() : "unknown error");
            } else {
                g_vs = g_vss->getVSAPI(VAPOURSYNTH_API_VERSION);
                if (!g_vs) r.error = "VapourSynth API version mismatch";
            }
        }
    }
    if (saved.empty()) unsetenv("XDG_CONFIG_HOME");
    else setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    if (!g_vs) {
        g_vss = nullptr;
        return;
    }
    VSCore* core = g_vs->createCore(0);
    VSCoreInfo ci;
    g_vs->getCoreInfo(core, &ci);
    r.version = "R" + std::to_string(ci.core);
    g_vs->freeCore(core);
    r.ok = true;
}

// --- our source filter ---------------------------------------------------------

struct SrcData {
    VSVideoInfo vi{};
    QtgmcSource source;
    int fieldBased = 2;
    std::mutex m;
};

const VSFrame* VS_CC srcGetFrame(int n, int activationReason, void* instanceData, void**, VSFrameContext* frameCtx, VSCore* core,
                                 const VSAPI* vsapi) {
    if (activationReason != arInitial) return nullptr;
    SrcData* d = static_cast<SrcData*>(instanceData);
    FramePtr f;
    {
        std::lock_guard<std::mutex> lk(d->m);
        f = d->source(n);
    }
    if (!f) {
        const std::string msg = "UinDub source: frame " + std::to_string(n) + " is not available";
        vsapi->setFilterError(msg.c_str(), frameCtx);
        return nullptr;
    }
    VSFrame* dst = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, nullptr, core);
    for (int p = 0; p < d->vi.format.numPlanes; ++p) {
        uint8_t* w = vsapi->getWritePtr(dst, p);
        const ptrdiff_t stride = vsapi->getStride(dst, p);
        const int pw = vsapi->getFrameWidth(dst, p), ph = vsapi->getFrameHeight(dst, p);
        for (int y = 0; y < ph; ++y) std::memcpy(w + stride * y, f->data[p] + size_t(y) * size_t(f->linesize[p]), size_t(pw) * 2);
    }
    VSMap* props = vsapi->getFramePropertiesRW(dst);
    vsapi->mapSetInt(props, "_FieldBased", d->fieldBased, maReplace);
    vsapi->mapSetInt(props, "_DurationNum", d->vi.fpsDen, maReplace);
    vsapi->mapSetInt(props, "_DurationDen", d->vi.fpsNum, maReplace);
    return dst;
}

void VS_CC srcFree(void* instanceData, VSCore*, const VSAPI*) { delete static_cast<SrcData*>(instanceData); }

std::string pyNumber(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

}  // namespace

const VsRuntimeInfo& vapoursynthRuntime() {
    std::call_once(g_once, initRuntime);
    return g_info;
}

struct QtgmcRunner::Impl {
    VSScript* script = nullptr;
    VSNode* out = nullptr;
    int64_t outFrames = 0;
    bool gray = false;
    int width = 0, height = 0;
    int window = 8;
    std::mutex m;
    std::condition_variable cv;
    std::map<int64_t, const VSFrame*> done;
    std::map<int64_t, std::string> errors;
    std::set<int64_t> pending;
    int64_t requested = 0;
    int inflight = 0;

    static void VS_CC onFrame(void* user, const VSFrame* f, int n, VSNode*, const char* errorMsg) {
        Impl* self = static_cast<Impl*>(user);
        std::lock_guard<std::mutex> lk(self->m);
        if (f) self->done[n] = f;
        else self->errors[n] = errorMsg ? errorMsg : "QTGMC failed";
        self->pending.erase(n);
        --self->inflight;
        self->cv.notify_all();
    }
};

QtgmcRunner::QtgmcRunner() = default;
QtgmcRunner::~QtgmcRunner() { close(); }

bool QtgmcRunner::open(const QtgmcConfig& c, int64_t inputFrames, QtgmcSource source, std::string& err) {
    close();
    const VsRuntimeInfo& rt = vapoursynthRuntime();
    if (!rt.ok) {
        err = rt.error;
        return false;
    }
    if (inputFrames <= 0) {
        err = "nothing to deinterlace";
        return false;
    }
    m_i = std::make_unique<Impl>();
    m_i->gray = c.gray;
    m_i->width = c.width;
    m_i->height = c.height;
    VSCore* core = g_vs->createCore(0);
    if (c.threads > 0) g_vs->setThreadCount(c.threads, core);
    VSCoreInfo ci;
    g_vs->getCoreInfo(core, &ci);
    m_i->window = std::max(4, ci.numThreads * 2);
    auto* d = new SrcData;
    d->source = std::move(source);
    d->fieldBased = c.tff ? 2 : 1;
    if (!g_vs->queryVideoFormat(&d->vi.format, c.gray ? cfGray : cfYUV, stInteger, 16, 0, 0, core)) {
        delete d;
        g_vs->freeCore(core);
        err = "VapourSynth has no 16-bit format";
        return false;
    }
    d->vi.fpsNum = c.fps.num;
    d->vi.fpsDen = c.fps.den;
    d->vi.width = c.width;
    d->vi.height = c.height;
    d->vi.numFrames = int(std::min<int64_t>(inputFrames, 0x7FFFFFFF));
    VSNode* node = g_vs->createVideoFilter2("UinDubSource", &d->vi, srcGetFrame, srcFree, fmUnordered, nullptr, 0, d, core);
    if (!node) {
        g_vs->freeCore(core);
        err = "cannot create the source filter";
        return false;
    }
    m_i->script = g_vss->createScript(core);  // takes the core
    if (!m_i->script) {
        g_vs->freeNode(node);
        err = "cannot create a VapourSynth script environment";
        return false;
    }
    VSMap* vars = g_vs->createMap();
    g_vs->mapConsumeNode(vars, "uindub_src", node, maReplace);
    g_vss->setVariables(m_i->script, vars);
    g_vs->freeMap(vars);

    std::string args = "Preset='" + c.preset + "', TFF=" + (c.tff ? "True" : "False") + ", FPSDivisor=" + (c.doubleRate ? "1" : "2") +
                       ", NoiseProcess=0";
    if (c.sourceMatch > 0) args += ", SourceMatch=" + std::to_string(std::clamp(c.sourceMatch, 1, 3));
    if (c.sharpness >= 0) args += ", Sharpness=" + pyNumber(c.sharpness);
    const std::string text =
        "import vapoursynth as vs\n"
        "core = vs.core\n"
        "import havsfunc as haf\n"
        "out = haf.QTGMC(uindub_src, " +
        args +
        ")\n"
        "out.set_output(0)\n";
    if (g_vss->evaluateBuffer(m_i->script, text.c_str(), "uindub-qtgmc") != 0) {
        const char* e = g_vss->getError(m_i->script);
        err = std::string("QTGMC: ") + (e ? e : "script failed");
        g_vss->freeScript(m_i->script);
        m_i.reset();
        return false;
    }
    m_i->out = g_vss->getOutputNode(m_i->script, 0);
    if (!m_i->out) {
        err = "QTGMC set no output";
        g_vss->freeScript(m_i->script);
        m_i.reset();
        return false;
    }
    const VSVideoInfo* vi = g_vs->getVideoInfo(m_i->out);
    m_i->outFrames = vi->numFrames;
    return true;
}

int64_t QtgmcRunner::outputFrames() const { return m_i ? m_i->outFrames : 0; }

FramePtr QtgmcRunner::get(int64_t k, std::string& err) {
    if (!m_i || k < 0 || k >= m_i->outFrames) {
        err = "frame out of range";
        return nullptr;
    }
    Impl& I = *m_i;
    const VSFrame* f = nullptr;
    {
        std::unique_lock<std::mutex> lk(I.m);
        const bool present = I.done.count(k) || I.errors.count(k);
        if (!present && !I.pending.count(k) && k < I.requested) {
            // Asked again for one already handed out: fetch it on its own.
            lk.unlock();
            char buf[1024] = {0};
            f = g_vs->getFrame(int(k), I.out, buf, sizeof buf);
            if (!f) {
                err = std::string("QTGMC: ") + buf;
                return nullptr;
            }
        } else {
            if (!present && !I.pending.count(k)) I.requested = k;  // jumped ahead
            // Keep a window of frames in flight so every core has work.
            while (I.requested < std::min(I.outFrames, k + I.window)) {
                const int64_t n = I.requested++;
                I.pending.insert(n);
                ++I.inflight;
                lk.unlock();
                g_vs->getFrameAsync(int(n), I.out, Impl::onFrame, &I);
                lk.lock();
            }
            I.cv.wait(lk, [&] { return I.done.count(k) || I.errors.count(k); });
            auto e = I.errors.find(k);
            if (e != I.errors.end()) {
                err = "QTGMC: " + e->second;
                I.errors.erase(e);
                return nullptr;
            }
            auto it = I.done.find(k);
            f = it->second;
            I.done.erase(it);
            // Frames before k that nobody will ask for again.
            for (auto d = I.done.begin(); d != I.done.end() && d->first < k;) {
                g_vs->freeFrame(d->second);
                d = I.done.erase(d);
            }
        }
    }
    FramePtr o = makeFrame();
    o->format = I.gray ? AV_PIX_FMT_GRAY16 : AV_PIX_FMT_YUV444P16;
    o->width = g_vs->getFrameWidth(f, 0);
    o->height = g_vs->getFrameHeight(f, 0);
    if (av_frame_get_buffer(o.get(), 32) < 0) {
        g_vs->freeFrame(f);
        err = "out of memory";
        return nullptr;
    }
    const int planes = I.gray ? 1 : 3;
    for (int p = 0; p < planes; ++p) {
        const uint8_t* src = g_vs->getReadPtr(f, p);
        const ptrdiff_t stride = g_vs->getStride(f, p);
        const int pw = g_vs->getFrameWidth(f, p), ph = g_vs->getFrameHeight(f, p);
        for (int y = 0; y < ph; ++y) std::memcpy(o->data[p] + size_t(y) * size_t(o->linesize[p]), src + stride * y, size_t(pw) * 2);
    }
    g_vs->freeFrame(f);
    setFrameInterlace(o.get(), false, false);
    return o;
}

void QtgmcRunner::close() {
    if (!m_i) return;
    {
        std::unique_lock<std::mutex> lk(m_i->m);
        m_i->cv.wait(lk, [&] { return m_i->inflight == 0; });
        for (auto& [n, f] : m_i->done) g_vs->freeFrame(f);
        m_i->done.clear();
    }
    if (m_i->out) g_vs->freeNode(m_i->out);
    if (m_i->script) g_vss->freeScript(m_i->script);
    m_i.reset();
}

}  // namespace ud
