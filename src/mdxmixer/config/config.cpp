#include "config.h"
#include <windows.h>
#include <algorithm>

namespace mdxm {

namespace {

constexpr size_t kMaxEqBands = 10;   // ChannelEq::kBands; kept as a literal so config/ stays dsp-free

// JsonWriter has Float (6 sig digits) and FloatPrecise (shortest float round-trip);
// doubles go through Value(), whose Number path writes the shortest double round-trip.
void Dbl(JsonWriter& w, const wchar_t* key, double v) { w.Value(key, JsonValue(v)); }

float ClampVol(double v) { return (float)std::clamp(v, 0.0, 1.0); }

void WriteDeviceRef(JsonWriter& w, const wchar_t* key, const DeviceRef& d) {
    w.BeginObject(key);
    w.String(L"id", d.id);
    w.String(L"name", d.name);
    // Written even when empty, so the field is visibly part of the shape
    // rather than something that appears once a headset has been seen.
    w.String(L"btAddress", d.btAddress);
    w.EndObject();
}

DeviceRef ReadDeviceRef(const JsonValue& v) {
    // Absent in a file written before the address was stored, which is every
    // existing config: it reads as empty and is filled in the first time the
    // entry is matched to a live device (HealDeviceRefs).
    return { v[L"id"].asString(), v[L"name"].asString(), v[L"btAddress"].asString() };
}

void WriteCable(JsonWriter& w, const wchar_t* key, const CableRef& c) {
    // Flat shape per the spec's config example: renderId/renderName/captureId/captureName.
    w.BeginObject(key);
    w.String(L"renderId",    c.render.id);
    w.String(L"renderName",  c.render.name);
    w.String(L"captureId",   c.capture.id);
    w.String(L"captureName", c.capture.name);
    w.EndObject();
}

CableRef ReadCable(const JsonValue& v) {
    CableRef c;
    c.render  = { v[L"renderId"].asString(),  v[L"renderName"].asString() };
    c.capture = { v[L"captureId"].asString(), v[L"captureName"].asString() };
    return c;
}

void WriteMix(JsonWriter& w, const wchar_t* key, const MixSetting& m) {
    w.BeginObject(key);
    w.FloatPrecise(L"vol", m.vol);
    w.Bool(L"mute", m.mute);
    w.EndObject();
}

MixSetting ReadMix(const JsonValue& v) {
    MixSetting m;
    m.vol  = ClampVol(v[L"vol"].asNumber(1.0));
    m.mute = v[L"mute"].asBool(false);
    return m;
}

void WriteEq(JsonWriter& w, const wchar_t* key, const EqConfig& e) {
    w.BeginObject(key);
    w.Bool(L"enabled", e.enabled);
    w.BeginArray(L"bands");
    for (const auto& b : e.bands) {
        w.BeginObject();
        Dbl(w, L"freq", b.freq);
        Dbl(w, L"gain", b.gainDb);
        Dbl(w, L"q",    b.q);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

EqConfig ReadEq(const JsonValue& v) {
    EqConfig e;
    e.enabled = v[L"enabled"].asBool(false);
    const JsonValue& bands = v[L"bands"];
    for (size_t i = 0; i < bands.size() && i < kMaxEqBands; ++i) {
        const JsonValue& b = bands.at(i);
        e.bands.push_back({ b[L"freq"].asNumber(1000.0),
                            b[L"gain"].asNumber(0.0),
                            b[L"q"].asNumber(1.0) });
    }
    return e;
}

} // namespace

bool UpsertAuthorizedDevice(std::vector<VbanAuthorizedDevice>& list,
                            const std::wstring& id, const std::wstring& name,
                            const std::wstring& lastSeen) {
    if (id.empty()) return false;
    for (auto& d : list) {
        if (d.id != id) continue;
        // A rename or a newer timestamp is a change worth persisting; the same
        // name and no timestamp is not. (lastSeen empty means "do not touch it",
        // so a caller that does not know the date leaves the stored one alone.)
        const bool renamed = d.name != name;
        const bool restamped = !lastSeen.empty() && d.lastSeen != lastSeen;
        if (!renamed && !restamped) return false;
        d.name = name;
        if (!lastSeen.empty()) d.lastSeen = lastSeen;
        return true;
    }
    list.push_back({ id, name, lastSeen });
    return true;
}

bool RemoveAuthorizedDevice(std::vector<VbanAuthorizedDevice>& list,
                            const std::wstring& id) {
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
        [&id](const VbanAuthorizedDevice& d) { return d.id == id; }), list.end());
    return list.size() != before;
}

std::wstring ConfigToJson(const MixerConfig& c) {
    JsonWriter w;
    w.BeginObject();
    w.BeginArray(L"channels");
    for (const auto& ch : c.channels) {
        w.BeginObject();
        w.String(L"id", ch.id);
        w.String(L"name", ch.name);
        WriteCable(w, L"cable", ch.cable);
        WriteMix(w, L"personal", ch.personal);
        WriteMix(w, L"streaming", ch.streaming);
        WriteEq(w, L"eq", ch.eq);
        w.BeginArray(L"apps");
        for (const auto& a : ch.apps) w.ValueAnon(JsonValue(a));
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    WriteDeviceRef(w, L"personalOutput", c.personalOutput);
    w.BeginObject(L"personalFailover");
    w.Bool(L"armed", c.personalFailover.armed);
    w.BeginArray(L"allow");
    for (const auto& d : c.personalFailover.allow) {
        // The same three fields WriteDeviceRef writes, and the reader on the
        // other side is ReadDeviceRef itself. These had drifted: a btAddress
        // added to DeviceRef was written for personalOutput and silently
        // dropped for every allowlist entry, so the anchor that survives an
        // adapter swap was learned on every startup and lost on every save.
        w.BeginObject();
        w.String(L"id", d.id);
        w.String(L"name", d.name);
        w.String(L"btAddress", d.btAddress);
        w.EndObject();
    }
    w.EndArray();
    w.Int(L"stabilitySec", c.personalFailover.stabilitySec);
    w.Int(L"minGapSec", c.personalFailover.minGapSec);
    w.Int(L"dwellSec", c.personalFailover.dwellSec);
    w.EndObject();
    WriteCable(w, L"streamingCable", c.streamingCable);
    w.BeginObject(L"mic");
    WriteDeviceRef(w, L"input", c.mic.input);
    WriteCable(w, L"cable", c.mic.cable);
    w.FloatPrecise(L"gain", c.mic.gain);
    WriteEq(w, L"eq", c.mic.eq);
    w.EndObject();
    w.BeginArray(L"deviceNames");
    for (const auto& d : c.deviceNames) {
        w.BeginObject();
        w.String(L"id", d.id);
        w.String(L"btAddress", d.btAddress);
        w.String(L"containerId", d.containerId);
        w.String(L"windowsName", d.windowsName);
        w.String(L"alias", d.alias);
        w.Bool(L"hidden", d.hidden);
        w.Bool(L"pinned", d.pinned);
        w.EndObject();
    }
    w.EndArray();
    w.BeginArray(L"hotkeys");
    for (const auto& h : c.hotkeys) {
        w.BeginObject();
        w.String(L"id", h.id);
        w.String(L"label", h.label);
        w.Int(L"action", (int)h.action);
        w.Int(L"mod", (int)h.mod);
        w.Int(L"vk", (int)h.vk);
        w.Int(L"stepPercent", h.stepPercent);
        w.BeginArray(L"targets");
        for (const auto& t : h.targets) { w.BeginObject(); w.String(L"key", t); w.EndObject(); }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.BeginArray(L"order");
    for (const auto& k : c.order) w.ValueAnon(JsonValue(k));
    w.EndArray();
    w.Int(L"volumeStepPercent", c.volumeStepPercent);
    w.Int(L"mdx12FeedPercent", c.mdx12FeedPercent);
    w.Int(L"cushionMs", c.cushionMs);
    w.Int(L"cushionHeadroomPercent", c.cushionHeadroomPercent);
    w.Int(L"cushionFlatMs", c.cushionFlatMs);
    w.BeginObject(L"ui");
    w.Bool(L"taskbarButton", c.ui.taskbarButton);
    w.Bool(L"alwaysOnTop", c.ui.alwaysOnTop);
    w.Int(L"activeTab", c.ui.activeTab);
    w.Bool(L"spinBoxes", c.ui.spinBoxes);
    w.Bool(L"showVirtualEndpoints", c.ui.showVirtualEndpoints);
    w.Int(L"allowSort", c.ui.allowSort);
    w.Bool(L"allowSortDesc", c.ui.allowSortDesc);
    w.Int(L"toolFontSize", c.ui.toolFontSize);
    w.String(L"theme", c.ui.theme);
    w.EndObject();
    w.BeginObject(L"batteryOverlay");
    w.Bool(L"enabled", c.batteryOverlay.enabled);
    w.Int(L"x", c.batteryOverlay.x);
    w.Int(L"y", c.batteryOverlay.y);
    w.Int(L"opacity", c.batteryOverlay.opacity);
    w.Int(L"fontSize", c.batteryOverlay.fontSize);
    w.Bool(L"clickThrough", c.batteryOverlay.clickThrough);
    w.Bool(L"mdx12Adaptive", c.batteryOverlay.mdx12Adaptive);
    w.Bool(L"background", c.batteryOverlay.background);
    w.Bool(L"frame", c.batteryOverlay.frame);
    w.String(L"text", c.batteryOverlay.text);
    w.EndObject();
    w.BeginObject(L"window");
    w.Int(L"x", c.window.x);
    w.Int(L"y", c.window.y);
    w.Int(L"w", c.window.w);
    w.Int(L"h", c.window.h);
    w.Bool(L"maximized", c.window.maximized);
    w.EndObject();
    w.Bool(L"autostart", c.autostart);
    w.Int(L"logLevel", c.logLevel);
    w.BeginObject(L"vban");
    w.Bool(L"enabled", c.vban.enabled);
    w.Int(L"port", c.vban.port);
    w.String(L"bindAddress", c.vban.bindAddress);
    w.String(L"streamName", c.vban.streamName);
    // Written as words rather than booleans: these are the values the
    // MDXM_VBAN verb takes and the tab shows, and a hand-edited file should
    // read the same way the protocol does.
    w.String(L"source", c.vban.sourceStreaming ? L"streaming" : L"personal");
    w.String(L"format", c.vban.formatFloat32 ? L"f32" : L"i16");
    w.Int(L"gainPercent", c.vban.gainPercent);
    w.String(L"pin", c.vban.pin);
    w.BeginArray(L"authorizedDevices");
    for (const auto& d : c.vban.authorizedDevices) {
        w.BeginObject();
        w.String(L"id", d.id);
        w.String(L"name", d.name);
        w.String(L"lastSeen", d.lastSeen);
        w.EndObject();
    }
    w.EndArray();
    w.Bool(L"openSubscribe", c.vban.openSubscribe);
    w.Bool(L"alwaysStream", c.vban.alwaysStream);
    w.Bool(L"alwaysFrames", c.vban.alwaysFrames);
    w.String(L"alwaysStreamTarget", c.vban.alwaysStreamTarget);
    w.BeginObject(L"frames");
    Dbl(w, L"fps", c.vban.frames.fps);
    w.Int(L"quality", c.vban.frames.quality);
    w.Int(L"maxEdge", c.vban.frames.maxEdge);
    w.EndObject();
    w.EndObject();
    // LAST key on purpose: the lenient parser turns a truncated file into a
    // partial object, so completeness is proven by this marker surviving.
    w.Bool(L"complete", true);
    w.EndObject();
    return w.ToString();
}

MixerConfig ConfigFromJson(const JsonValue& root) {
    MixerConfig c;
    const JsonValue& chans = root[L"channels"];
    for (size_t i = 0; i < chans.size(); ++i) {
        const JsonValue& v = chans.at(i);
        ChannelConfig ch;
        ch.id        = v[L"id"].asString();
        ch.name      = v[L"name"].asString();
        ch.cable     = ReadCable(v[L"cable"]);
        ch.personal  = ReadMix(v[L"personal"]);
        ch.streaming = ReadMix(v[L"streaming"]);
        ch.eq        = ReadEq(v[L"eq"]);
        const JsonValue& apps = v[L"apps"];
        for (size_t a = 0; a < apps.size(); ++a)
            if (apps.at(a).isString()) ch.apps.push_back(apps.at(a).sVal);
        c.channels.push_back(std::move(ch));
    }
    c.personalOutput = ReadDeviceRef(root[L"personalOutput"]);
    const JsonValue& fo = root[L"personalFailover"];
    c.personalFailover.armed = fo[L"armed"].asBool(false);
    const JsonValue& allow = fo[L"allow"];
    for (size_t i = 0; i < allow.size(); ++i)
        c.personalFailover.allow.push_back(ReadDeviceRef(allow.at(i)));
    c.personalFailover.stabilitySec = fo[L"stabilitySec"].asInt(5);
    c.personalFailover.dwellSec     = fo[L"dwellSec"].asInt(30);
    c.personalFailover.minGapSec    = fo[L"minGapSec"].asInt(5);
    c.streamingCable = ReadCable(root[L"streamingCable"]);
    const JsonValue& mic = root[L"mic"];
    c.mic.input = ReadDeviceRef(mic[L"input"]);
    c.mic.cable = ReadCable(mic[L"cable"]);
    c.mic.gain  = ClampVol(mic[L"gain"].asNumber(1.0));
    c.mic.eq    = ReadEq(mic[L"eq"]);
    const JsonValue& names = root[L"deviceNames"];
    for (size_t i = 0; i < names.size(); ++i) {
        const JsonValue& n = names.at(i);
        DeviceAlias a{ n[L"id"].asString(), n[L"btAddress"].asString(),
                       n[L"containerId"].asString(),
                       n[L"windowsName"].asString(), n[L"alias"].asString() };
        a.hidden = n[L"hidden"].asBool(false);
        a.pinned = n[L"pinned"].asBool(false);
        // An entry earns its place by carrying a name, a pin or a hide; one
        // that carries none of them is stale and dropped on load.
        if (!a.alias.empty() || a.hidden || a.pinned) c.deviceNames.push_back(a);
    }
    const JsonValue& keys = root[L"hotkeys"];
    for (size_t i = 0; i < keys.size(); ++i) {
        const JsonValue& k = keys.at(i);
        HotkeyBinding h;
        h.id = k[L"id"].asString();
        h.label = k[L"label"].asString();
        int act = k[L"action"].asInt(0);
        if (act < 0 || act > (int)HotkeyAction::ShowWindow) act = 0;
        h.action = (HotkeyAction)act;
        h.mod = (unsigned)k[L"mod"].asInt(0);
        // 0 keeps its meaning of "use the default"; anything else is clamped
        // to the band the Options box allows, so a hand-edited file cannot
        // make a key that moves nothing or jumps the whole fader.
        const int step = k[L"stepPercent"].asInt(0);
        h.stepPercent = (step <= 0) ? 0 : (step > 50 ? 50 : step);
        h.vk  = (unsigned)k[L"vk"].asInt(0);
        const JsonValue& tg = k[L"targets"];
        for (size_t j = 0; j < tg.size(); ++j) {
            std::wstring key = tg.at(j)[L"key"].asString();
            if (!key.empty()) h.targets.push_back(key);
        }
        if (!h.id.empty()) c.hotkeys.push_back(h);
    }
    const JsonValue& ord = root[L"order"];
    for (size_t i = 0; i < ord.size(); ++i)
        if (ord.at(i).isString()) c.order.push_back(ord.at(i).sVal);
    c.volumeStepPercent = root[L"volumeStepPercent"].asInt(5);
    c.mdx12FeedPercent = root[L"mdx12FeedPercent"].asInt(100);
    if (c.mdx12FeedPercent < 0) c.mdx12FeedPercent = 0;
    if (c.mdx12FeedPercent > 200) c.mdx12FeedPercent = 200;
    // The cushion floor, in ms. Clamped to what the engine can honour: below a
    // few milliseconds there is no room for ordinary jitter, and above 200 it
    // has stopped being a latency control.
    c.cushionMs = root[L"cushionMs"].asInt(30);
    if (c.cushionMs < 5) c.cushionMs = 5;
    if (c.cushionMs > 200) c.cushionMs = 200;
    // Zero is legitimate for both: "no proportional slack" and "no flat
    // margin" are things worth being able to try, and the floor still applies
    // underneath them.
    c.cushionHeadroomPercent = root[L"cushionHeadroomPercent"].asInt(50);
    if (c.cushionHeadroomPercent < 0) c.cushionHeadroomPercent = 0;
    if (c.cushionHeadroomPercent > 400) c.cushionHeadroomPercent = 400;
    c.cushionFlatMs = root[L"cushionFlatMs"].asInt(10);
    if (c.cushionFlatMs < 0) c.cushionFlatMs = 0;
    if (c.cushionFlatMs > 100) c.cushionFlatMs = 100;
    if (c.volumeStepPercent < 1) c.volumeStepPercent = 1;
    if (c.volumeStepPercent > 50) c.volumeStepPercent = 50;
    c.ui.taskbarButton = root[L"ui"][L"taskbarButton"].asBool(true);
    c.ui.alwaysOnTop = root[L"ui"][L"alwaysOnTop"].asBool(false);
    c.ui.activeTab = root[L"ui"][L"activeTab"].asInt(0);
    c.ui.spinBoxes = root[L"ui"][L"spinBoxes"].asBool(false);
    c.ui.showVirtualEndpoints = root[L"ui"][L"showVirtualEndpoints"].asBool(false);
    c.ui.allowSort = std::clamp(root[L"ui"][L"allowSort"].asInt(0), 0, 3);
    c.ui.allowSortDesc = root[L"ui"][L"allowSortDesc"].asBool(false);
    // -12 smallest, -32 largest; a positive value is a cell height and
    // renders as the tiny font this clamp exists to prevent.
    c.ui.toolFontSize = std::clamp(root[L"ui"][L"toolFontSize"].asInt(-20), -32, -12);
    c.ui.theme = root[L"ui"][L"theme"].asString(L"system");
    {
        const JsonValue& b = root[L"batteryOverlay"];
        BatteryOverlayConfig d;      // the defaults, so a missing key keeps one
        c.batteryOverlay.enabled      = b[L"enabled"].asBool(d.enabled);
        c.batteryOverlay.x            = b[L"x"].asInt(d.x);
        c.batteryOverlay.y            = b[L"y"].asInt(d.y);
        c.batteryOverlay.opacity      = b[L"opacity"].asInt(d.opacity);
        c.batteryOverlay.fontSize     = b[L"fontSize"].asInt(d.fontSize);
        c.batteryOverlay.clickThrough = b[L"clickThrough"].asBool(d.clickThrough);
        c.batteryOverlay.mdx12Adaptive = b[L"mdx12Adaptive"].asBool(d.mdx12Adaptive);
        c.batteryOverlay.background   = b[L"background"].asBool(d.background);
        c.batteryOverlay.frame        = b[L"frame"].asBool(d.frame);
        c.batteryOverlay.text         = b[L"text"].asString(d.text.c_str());
    }
    {
        const JsonValue& wn = root[L"window"];
        c.window.x = wn[L"x"].asInt(0);
        c.window.y = wn[L"y"].asInt(0);
        c.window.w = wn[L"w"].asInt(0);
        c.window.h = wn[L"h"].asInt(0);
        c.window.maximized = wn[L"maximized"].asBool(false);
    }
    c.autostart = root[L"autostart"].asBool(false);
    c.logLevel  = root[L"logLevel"].asInt(2);
    {
        // Every config written before this version has no "vban" key, so each
        // field falls back to its default rather than to a zero: a port of 0 or
        // an empty stream name would be a listener nothing could reach.
        const JsonValue& v = root[L"vban"];
        const VbanConfig d;      // the defaults, so a missing key keeps one
        c.vban.enabled = v[L"enabled"].asBool(d.enabled);
        c.vban.port = v[L"port"].asInt(d.port);
        if (c.vban.port < 1 || c.vban.port > 65535) c.vban.port = d.port;
        c.vban.bindAddress = v[L"bindAddress"].asString();
        c.vban.streamName = v[L"streamName"].asString(d.streamName.c_str());
        // A stream name is how a receiver picks this stream out; empty would
        // match whatever else is on the network under no name at all.
        if (c.vban.streamName.empty()) c.vban.streamName = d.streamName;
        c.vban.sourceStreaming = v[L"source"].asString(L"personal") == L"streaming";
        c.vban.formatFloat32 = v[L"format"].asString(L"i16") == L"f32";
        c.vban.gainPercent = std::clamp(v[L"gainPercent"].asInt(d.gainPercent), 0, 6400);
        c.vban.pin = v[L"pin"].asString();
        const JsonValue& devs = v[L"authorizedDevices"];
        for (size_t i = 0; i < devs.size(); ++i) {
            const JsonValue& dev = devs.at(i);
            VbanAuthorizedDevice a{ dev[L"id"].asString(), dev[L"name"].asString(),
                                    dev[L"lastSeen"].asString() };
            // The id is what an approval is recorded against; an entry without
            // one can never match a device and would sit there unexplained.
            if (!a.id.empty()) c.vban.authorizedDevices.push_back(a);
        }
        c.vban.openSubscribe = v[L"openSubscribe"].asBool(d.openSubscribe);
        c.vban.alwaysStream = v[L"alwaysStream"].asBool(d.alwaysStream);
        c.vban.alwaysFrames = v[L"alwaysFrames"].asBool(d.alwaysFrames);
        c.vban.alwaysStreamTarget = v[L"alwaysStreamTarget"].asString();
        const JsonValue& fr = v[L"frames"];
        // Clamped because these reach a GPU capture and a JPEG encoder: 10 fps
        // of full-screen duplication is a real cost, and a quality of 999 is
        // not a number WIC has any use for.
        c.vban.frames.fps = std::clamp(fr[L"fps"].asNumber(d.frames.fps), 0.2, 10.0);
        c.vban.frames.quality = std::clamp(fr[L"quality"].asInt(d.frames.quality), 10, 95);
        c.vban.frames.maxEdge = std::clamp(fr[L"maxEdge"].asInt(d.frames.maxEdge), 64, 1024);
    }
    return c;
}

MixerConfig LoadConfig(const std::wstring& path, bool* usedDefaults) {
    try {
        JsonValue root = JsonLoadFile(path.c_str());
        // The completeness marker is written last; a truncated or foreign file
        // lacks it and gets defaults rather than a half-parsed configuration.
        if (root.isObject() && root[L"complete"].asBool(false)) {
            if (usedDefaults) *usedDefaults = false;
            return ConfigFromJson(root);
        }
    } catch (...) {
        // fall through to defaults — LoadConfig never throws
    }
    if (usedDefaults) *usedDefaults = true;
    return MixerConfig{};
}

bool SaveConfigAtomic(const std::wstring& path, const MixerConfig& c) {
    // JsonSaveFile writes to <path>.tmp and renames over the target (its #121 lesson).
    return JsonSaveFile(path.c_str(), ConfigToJson(c));
}

void ConfigStore::Mutate(const std::function<void(MixerConfig&)>& fn) {
    fn(m_config);
    m_dirty = true;
    m_lastMutateMs = GetTickCount64();
}

void ConfigStore::FlushIfDue() {
    if (!m_dirty || m_path.empty()) return;
    if (GetTickCount64() - m_lastMutateMs < 1000) return;
    if (SaveConfigAtomic(m_path, m_config)) m_dirty = false;
}

void ConfigStore::FlushNow() {
    if (!m_dirty || m_path.empty()) return;
    if (SaveConfigAtomic(m_path, m_config)) m_dirty = false;
}

} // namespace mdxm
