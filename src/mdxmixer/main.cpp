#ifndef MDXM_TEST
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW (WIN32_LEAN_AND_MEAN drops it)
#include <objbase.h>    // CoInitializeEx
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "device/endpoints.h"
#include "device/device_identity.h"
#include "device/device_info.h"
#include "dsp/resampler.h"
#include "dsp/ring_buffer.h"
#include "device/device_order.h"
#include "ipc/stream_feed.h"
#include <shlwapi.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#pragma comment(lib, "shlwapi.lib")
#include "routing/default_endpoint.h"
#include "routing/sonar_api.h"
#include "routing/sonar_control.h"
#include "engine/capture_stream.h"
#include "engine/render_stream.h"
#include "routing/audio_policy_config.h"
#include "routing/sessions.h"
#include "app/app_controller.h"

// mdxmixer.exe --devices
// Prints every endpoint (id, flow, active, name) so cable ids can be copied into
// config and the MDXM_TEST_* env vars.
static int RunDevices() {
    auto eps = mdxm::EnumerateEndpoints();
    wprintf(L"%zu endpoints:\n", eps.size());
    for (const auto& e : eps) {
        // Rate only for active endpoints: activating a client on an unplugged
        // device is slow and pointless. The rollout wants every cable at 48000.
        uint32_t rate = e.isActive ? mdxm::EndpointMixRate(e.id) : 0;
        wprintf(L"  [%s] %s  %6u Hz  %s\n      %s\n",
                e.isRender ? L"render " : L"capture",
                e.isActive ? L"active  " : L"inactive",
                rate, e.name.c_str(), e.id.c_str());
    }
    return 0;
}

// mdxmixer.exe --sonar [on|off]
//
// With no argument, what GG says Sonar is doing. With on/off, the thing that
// had no method behind it but a mouse: Sonar hangs when a Bluetooth headset
// disconnects suddenly, and the recovery is to disable it, restart the audio
// services, and enable it again.
//
// Disabling takes Sonar's virtual endpoints away, so anything playing into
// them lands somewhere else -- including whatever is being deliberately
// dumped into a muted channel. Not a quiet operation; it says so and waits
// for confirmation unless --yes is given.
// mdxmixer.exe --sonarch
// Sonar's own channels and their two levels, read through Sonar's local API.
// Read-only and silent: it moves nothing, it only asks. The point is to be
// able to check the faders mdxmixer now draws against what Sonar holds,
// without reading them off a screenshot.
static int RunSonarChannels() {
    mdxm::SonarChannels sonar;
    if (!sonar.Refresh(true)) {
        wprintf(L"Sonar did not answer. Is it enabled? (mdxmixer --sonar)\n");
        return 2;
    }
    wprintf(L"Sonar is in %s mode:\n", sonar.StreamMode() ? L"streamer" : L"classic");
    wprintf(L"  %-12s  %-20s  %-20s\n", L"channel", L"personal", L"streaming");
    for (const auto& c : sonar.Channels())
        // Raw scalars, not percentages. A restore has to put back exactly what
        // was there, and "2%" is not exact enough to do that with.
        wprintf(L"  %-12s  %.4f %-13s  %.4f %-13s%s\n",
                (c.label + L" (" + c.key + L")").c_str(),
                c.personalVol, c.personalMute ? L"(muted)" : L"",
                c.streamingVol, c.streamingMute ? L"(muted)" : L"",
                c.canMute ? L"" : L"  [mute refused: destructive]");
    return 0;
}

// mdxmixer.exe --sonarset <key> <p|s> <scalar 0..1>
// mdxmixer.exe --sonarmute <key> <p|s> <on|off>
//
// Moves one Sonar fader and prints what Sonar held before and after. These
// make SOUND possible — they are the write half of the API — so they name the
// channel explicitly rather than taking a default, and they print the old
// value first so it can always be put back.
static int RunSonarWrite(const wchar_t* key, const wchar_t* side,
                         const wchar_t* value, bool isMute) {
    const bool personal = (side[0] == L'p' || side[0] == L'P');
    mdxm::SonarChannels sonar;
    if (!sonar.Refresh(true)) {
        wprintf(L"Sonar did not answer.\n");
        return 2;
    }
    auto show = [&](const wchar_t* when) {
        for (const auto& c : sonar.Channels())
            if (c.key == key)
                wprintf(L"  %-7s %s %s: %.4f%s\n", when, c.label.c_str(),
                        personal ? L"personal" : L"streaming",
                        personal ? c.personalVol : c.streamingVol,
                        (personal ? c.personalMute : c.streamingMute) ? L" (muted)" : L"");
    };
    show(L"before");

    bool ok;
    if (isMute) {
        const bool on = (_wcsicmp(value, L"on") == 0 || _wcsicmp(value, L"true") == 0);
        ok = sonar.SetMute(key, personal, on);
    } else {
        ok = sonar.SetVolume(key, personal, (float)_wtof(value));
    }
    if (!ok) {
        wprintf(L"  refused (an unknown channel, or the master's destructive mute)\n");
        return 3;
    }
    // Forced, because the point is to replace the optimistic value with what
    // Sonar actually holds.
    sonar.Refresh(true);
    show(L"after");
    return 0;
}

static int RunSonar(const wchar_t* arg, bool assumeYes) {
    std::wstring err;
    if (!arg) {
        mdxm::SonarState s = mdxm::QuerySonar(&err);
        wprintf(L"Sonar is %s%s%s\n", mdxm::SonarStateName(s),
                err.empty() ? L"" : L" -- ", err.c_str());
        return s == mdxm::SonarState::Unknown ? 1 : 0;
    }
    const bool on = (_wcsicmp(arg, L"on") == 0 || _wcsicmp(arg, L"enable") == 0);
    const bool off = (_wcsicmp(arg, L"off") == 0 || _wcsicmp(arg, L"disable") == 0);
    if (!on && !off) { wprintf(L"--sonar wants on or off\n"); return 1; }

    mdxm::SonarState before = mdxm::QuerySonar(&err);
    wprintf(L"Sonar is %s\n", mdxm::SonarStateName(before));
    if ((before == mdxm::SonarState::Enabled) == on) {
        wprintf(L"already %s; nothing to do\n", on ? L"on" : L"off");
        return 0;
    }
    if (off && !assumeYes) {
        wprintf(L"Disabling Sonar removes its virtual endpoints. Anything playing\n"
                L"into them -- including sounds routed to a muted channel on\n"
                L"purpose -- will move to another device and may become audible.\n"
                L"Re-run with --yes to go ahead.\n");
        return 2;
    }
    if (!mdxm::SetSonarEnabled(on, &err)) {
        wprintf(L"failed: %s\n", err.c_str());
        return 1;
    }
    wprintf(L"Sonar is now %s\n", mdxm::SonarStateName(mdxm::QuerySonar(nullptr)));
    return 0;
}

// mdxmixer.exe --feed [seconds]
//
// What the shared-memory stream feed is carrying: the format mdxmixer
// published, whether it is still being written to, and the peak of what has
// arrived. The reader's view of the contract in ipc/stream_feed.h, so the
// other side can be checked before anything in MDropDX12 depends on it.
static int RunFeed(int seconds) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, mdxm::kStreamFeedName);
    if (!h) { wprintf(L"no stream feed: mdxmixer is not running its engine\n"); return 1; }
    const void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!view) { CloseHandle(h); wprintf(L"could not map the stream feed\n"); return 1; }
    const auto* hdr = (const mdxm::StreamFeedHeader*)view;
    if (hdr->magic != mdxm::kStreamFeedMagic) {
        wprintf(L"stream feed present but not ready (magic 0x%08X)\n", hdr->magic);
        UnmapViewOfFile(view); CloseHandle(h);
        return 1;
    }
    const float* samples = (const float*)((const char*)view + sizeof(mdxm::StreamFeedHeader));
    wprintf(L"stream feed: %u Hz, %u ch, %u frame ring\n",
            hdr->sampleRate, hdr->channels, hdr->capacityFrames);

    uint64_t readFrames = hdr->writeFrames;
    float highest = 0.0f;
    uint64_t total = 0;
    for (int i = 0; i < seconds * 10; ++i) {
        Sleep(100);
        uint64_t w = hdr->writeFrames;
        uint64_t start = 0;
        size_t n = mdxm::StreamFeedReadable(w, readFrames, hdr->capacityFrames, &start);
        for (size_t f = 0; f < n; ++f) {
            const float* fr = samples +
                (size_t)((start + f) % hdr->capacityFrames) * hdr->channels;
            for (uint32_t c = 0; c < hdr->channels; ++c) {
                float a = fr[c] < 0 ? -fr[c] : fr[c];
                if (a > highest) highest = a;
            }
        }
        total += n;
        readFrames = start + n;
    }
    const uint64_t age = GetTickCount64() - hdr->writeTickMs;
    wprintf(L"%llu frames in %d s, peak %.4f, last write %llu ms ago\n",
            (unsigned long long)total, seconds, highest, (unsigned long long)age);
    wprintf(L"%s\n", total > 0 ? L"=> the feed is live." :
            (age > 1000 ? L"=> nothing is being written; the engine is not running."
                        : L"=> the engine is writing silence."));
    UnmapViewOfFile(view);
    CloseHandle(h);
    return 0;
}

// mdxmixer.exe --meter <endpointSubstring> [seconds]
//
// The peak level Windows is actually putting ON an endpoint, sampled ten times
// a second. The one measurement that separates "Windows is not sending audio\n// there" from "Windows is sending it and the device is not playing it" -- and
// nothing else on the machine answers that question without making a sound.
//
// Written on 2026-10-03, when every signal said the audio was flowing to a
// pair of connected headphones at 67% and nothing could be heard.
static int RunMeter(const wchar_t* epNeedle, int seconds) {
    auto eps = mdxm::EnumerateEndpoints();
    const mdxm::EndpointInfo* ep = nullptr;
    for (const auto& e : eps) {
        if (!e.isRender) continue;
        if (e.id == epNeedle || StrStrIW(e.name.c_str(), epNeedle)) { ep = &e; break; }
    }
    if (!ep) { wprintf(L"no render endpoint matches '%s'\n", epNeedle); return 1; }

    IMMDeviceEnumerator* enumr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)) || !enumr) {
        wprintf(L"no device enumerator\n");
        return 1;
    }
    IMMDevice* dev = nullptr;
    enumr->GetDevice(ep->id.c_str(), &dev);
    enumr->Release();
    if (!dev) { wprintf(L"could not open the endpoint\n"); return 1; }

    IAudioMeterInformation* meter = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                               (void**)&meter);
    dev->Release();
    if (FAILED(hr) || !meter) { wprintf(L"no meter on that endpoint\n"); return 1; }

    wprintf(L"metering %s for %d s\n", ep->name.c_str(), seconds);
    float peak = 0.0f, highest = 0.0f;
    int nonZero = 0, samples = seconds * 10;
    for (int i = 0; i < samples; ++i) {
        if (SUCCEEDED(meter->GetPeakValue(&peak))) {
            if (peak > highest) highest = peak;
            if (peak > 0.0001f) ++nonZero;
        }
        Sleep(100);
    }
    meter->Release();
    wprintf(L"peak %.4f, %d of %d samples non-silent\n", highest, nonZero, samples);
    wprintf(L"%s\n", highest > 0.0001f
        ? L"=> Windows IS putting audio on this endpoint. If it cannot be heard, "
          L"the problem is past Windows: the device, its link, or its own volume."
        : L"=> Windows is putting SILENCE on this endpoint, whatever the session "
          L"list says.");
    return 0;
}

// mdxmixer.exe --approute <exeSubstring> <endpointSubstring>
// mdxmixer.exe --setdefault <endpointSubstring>
//
// Routing from the command line, with NO engine and NO window.
//
// Added on 2026-10-03 while the machine had no sound at all: Sonar's engine
// had died holding the default output, and mdxmixer could not be used to fix
// it because starting the engine against that graph crashed the app inside
// AudioSes.dll. A rescue path has to work when the thing it is rescuing is
// broken, so this one touches nothing but the routing APIs.
//
// Both take a SUBSTRING of the name rather than a GUID: at the point someone
// needs this, copying a device id out of another command is not reasonable.
static const mdxm::EndpointInfo* FindEndpoint(const std::vector<mdxm::EndpointInfo>& eps,
                                              const wchar_t* needle, bool renderOnly) {
    const mdxm::EndpointInfo* hit = nullptr;
    for (const auto& e : eps) {
        if (renderOnly && !e.isRender) continue;
        if (e.id == needle) return &e;                      // an exact id always wins
        if (StrStrIW(e.name.c_str(), needle) == nullptr) continue;
        if (hit) { wprintf(L"ambiguous: both '%s' and '%s' match\n", hit->name.c_str(), e.name.c_str()); return nullptr; }
        hit = &e;
    }
    return hit;
}

static int RunSetDefault(const wchar_t* epNeedle) {
    auto eps = mdxm::EnumerateEndpoints();
    const mdxm::EndpointInfo* ep = FindEndpoint(eps, epNeedle, true);
    if (!ep) { wprintf(L"no single render endpoint matches '%s'\n", epNeedle); return 1; }
    std::wstring err;
    if (!mdxm::SetDefaultRenderEndpoint(ep->id, &err)) {
        wprintf(L"failed: %s\n", err.c_str());
        return 1;
    }
    std::wstring now = mdxm::DefaultRenderEndpointId();
    wprintf(L"default -> %s\n%s\n", ep->name.c_str(),
            _wcsicmp(now.c_str(), ep->id.c_str()) == 0
                ? L"confirmed."
                : L"BUT the default did not move -- something else is holding it.");
    return 0;
}

static int RunAppRoute(const wchar_t* exeNeedle, const wchar_t* epNeedle) {
    auto eps = mdxm::EnumerateEndpoints();
    const mdxm::EndpointInfo* ep = FindEndpoint(eps, epNeedle, true);
    if (!ep) { wprintf(L"no single render endpoint matches '%s'\n", epNeedle); return 1; }

    mdxm::AudioPolicyConfig policy;
    std::wstring perr;
    if (!policy.Init(&perr)) { wprintf(L"per-app routing unavailable: %s\n", perr.c_str()); return 1; }

    int moved = 0;
    for (const auto& s : mdxm::EnumerateSessions()) {
        if (s.exePath.empty()) continue;
        if (StrStrIW(s.exePath.c_str(), exeNeedle) == nullptr) continue;
        std::wstring err;
        if (policy.SetPersistedDefaultRender(s.pid, ep->id, &err)) {
            wprintf(L"  pid %lu %s -> %s\n", s.pid, s.exePath.c_str(), ep->name.c_str());
            ++moved;
        } else {
            wprintf(L"  pid %lu FAILED: %s\n", s.pid, err.c_str());
        }
    }
    if (!moved) { wprintf(L"nothing matching '%s' has an audio session\n", exeNeedle); return 1; }
    wprintf(L"%d process(es) moved\n", moved);
    return 0;
}

// mdxmixer.exe --levels
// The device list exactly as the mixer builds it: after the aliases, after the
// pin and hide flags, after SortDevices. The order the UI shows is a thing worth
// being able to check without reading it off a screenshot.
static int RunLevels() {
    auto levels = mdxm::ListEndpointVolumes();
    // The aliases the comment above promises. Without this the CLI printed
    // Windows' names while the window printed Shane's, so checking one against
    // the other meant mapping "Headphones (9- WF-1000XM5)" onto "Wm5 White
    // (RTK)" by hand -- on a machine with five identically-named pairings,
    // which is the whole reason the aliases exist.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    mdxm::MixerConfig cfg = mdxm::LoadConfig(dir + L"\\mdxmixer.json", nullptr);
    // No engine here, so the configured route stands in for the live one.
    mdxm::ApplyDeviceView(cfg, levels, cfg.personalOutput.id);
    wprintf(L"%zu endpoints, in list order:\n", levels.size());
    for (const auto& d : levels) {
        // An unplugged endpoint has no level to show; a dash says that rather
        // than implying it is sitting at zero.
        wchar_t volText[16];
        if (d.volumeKnown) swprintf(volText, 16, L"%d%%", (int)(d.vol * 100.0f + 0.5f));
        else               wcscpy_s(volText, 16, L"-");
        wprintf(L"  %s %s %s%s  %-46s  %5s%s  batt=%3d  %s\n",
                d.isRender ? L"out" : L"in ",
                d.isDefault ? L"*" : L" ",
                d.active ? L"live  " : (d.present ? L"paired" : L"      "),
                d.isHandsFree ? L" hf" : L"   ",
                d.displayName.c_str(),
                volText, d.mute ? L" M" : L"  ", d.battery,
                mdxm::FormatLastSeen(d.lastConnectedUtc, d.active).c_str());
    }
    return 0;
}

// mdxmixer.exe --btinfo
// Every Bluetooth container with its battery and last-connected time, printed
// under BOTH interpretations of the raw FILETIME. The property is documented
// as local time, but MDropDX12's mixer shows at least one device a year out,
// so this prints the evidence rather than assuming either reading.
static int RunBtInfo() {
    auto infos = mdxm::ReadBluetoothInfo();
    wprintf(L"%zu bluetooth containers\n", infos.size());
    for (const auto& b : infos) {
        auto fmt = [](uint64_t ft) {
            if (!ft) return std::wstring(L"(none)");
            FILETIME f = { (DWORD)(ft & 0xFFFFFFFF), (DWORD)(ft >> 32) };
            SYSTEMTIME st = {};
            if (!FileTimeToSystemTime(&f, &st)) return std::wstring(L"(bad)");
            wchar_t buf[48];
            swprintf(buf, 48, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute);
            return std::wstring(buf);
        };
        wprintf(L"  %s\n    present=%d battery=%d bt=%s\n", b.containerId.c_str(),
                b.present ? 1 : 0, b.battery,
                b.btAddress.empty() ? L"(none)" : b.btAddress.c_str());
        wprintf(L"    raw as-is       : %s\n", fmt(b.lastConnectedRaw).c_str());
        wprintf(L"    raw as local->utc: %s\n",
                fmt(mdxm::LocalFileTimeToUtc(b.lastConnectedRaw)).c_str());
        wprintf(L"    shown to user    : %s\n",
                mdxm::FormatLastSeen(mdxm::LocalFileTimeToUtc(b.lastConnectedRaw),
                                     b.present).c_str());
    }
    // Which endpoints join to which container.
    // Every endpoint, not just the active ones. The dormant pairings are the
    // whole question here: whether one physical headset re-paired through a
    // different adapter still reports the same Bluetooth address, which is
    // what decides whether an alias can be keyed on it.
    wprintf(L"\nendpoint -> container -> bt:\n");
    for (const auto& d : mdxm::ListEndpointVolumes()) {
        if (d.containerId.empty()) continue;
        std::wstring bt = L"(none)";
        for (const auto& b : infos)
            if (b.containerId == d.containerId) {
                if (!b.btAddress.empty()) bt = b.btAddress;
                break;
            }
        wprintf(L"  %-48s %-40s %s\n", d.name.c_str(), d.containerId.c_str(), bt.c_str());
    }
    return 0;
}

// mdxmixer.exe --sessions
// Lists what is playing where, and whether the undocumented policy API answers
// on this Windows build — the smoke test the spec's risk posture calls for.
static int RunSessions() {
    auto sessions = mdxm::EnumerateSessions();
    wprintf(L"%zu render sessions:\n", sessions.size());
    for (const auto& s : sessions)
        wprintf(L"  pid %6lu  %s  %s\n      -> %s\n",
                s.pid, s.active ? L"active " : L"idle   ", s.exePath.c_str(), s.endpointName.c_str());
    mdxm::AudioPolicyConfig policy;
    std::wstring err;
    if (policy.Init(&err))
        wprintf(L"policy API: available (%s iid)\n", policy.IidUsed());
    else
        wprintf(L"policy API: NOT available — %s\n", err.c_str());
    return 0;
}

// mdxmixer.exe --monitor <captureIdOrName> <renderIdOrName>
// Capture -> 100 ms prefill cushion -> ring -> resample if rates differ -> unity render.
// REFUSES to start when the two selectors resolve to the same endpoint (feedback guard;
// the structural one — there is deliberately NO level-based runaway detector).
static int RunMonitor(const std::wstring& capSel, const std::wstring& renSel) {
    auto eps = mdxm::EnumerateEndpoints();
    const mdxm::EndpointInfo* cap = mdxm::MatchBinding(eps, {capSel, capSel});
    const mdxm::EndpointInfo* ren = mdxm::MatchBinding(eps, {renSel, renSel});
    if (!cap || !ren) { wprintf(L"endpoint not found — try --devices\n"); return 1; }
    if (_wcsicmp(cap->id.c_str(), ren->id.c_str()) == 0) { wprintf(L"refusing: capture == render (feedback)\n"); return 1; }
    wprintf(L"capture: %s\nrender : %s\n", cap->name.c_str(), ren->name.c_str());

    mdxm::RingBuffer ring(48000 / 5);                  // ~200 ms
    mdxm::CaptureStream cs; mdxm::RenderStream rs;
    std::wstring err;
    if (!cs.Start(cap->id, false, [&](const float* f, size_t n) { ring.Write(f, n); }, &err)) { wprintf(L"cap: %s\n", err.c_str()); return 1; }
    mdxm::LinearResampler rsmp;
    bool cushionDone = false;
    const size_t cushion = 48000 / 10;                 // ~100 ms prefill before first real audio
    std::vector<float> scratch(2 * 8192);
    if (!rs.Start(ren->id, [&](float* out, size_t frames) {
            // Unity gain, absolutely: listening level is the OUTPUT DEVICE's own
            // volume (the decisive correction in the passthrough-monitor lessons).
            if (!cushionDone) {
                if (ring.Depth() < cushion) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                cushionDone = true;
                rsmp.SetRates((double)cs.SourceRate(), (double)rs.DeviceRate());
            }
            if (rsmp.IsPassthrough()) { ring.Read(out, frames); return; }
            // Exact-need pull (see LinearResampler::NeedInput) — slack over-consumes.
            size_t srcNeed = rsmp.NeedInput(frames);
            if (scratch.size() < srcNeed * 2) scratch.resize(srcNeed * 2);
            ring.Read(scratch.data(), srcNeed);            // zero-fills any shortfall
            size_t produced = rsmp.Process(scratch.data(), srcNeed, out, frames);
            for (size_t i = produced; i < frames; ++i) {   // cannot happen by construction; belt only
                out[i*2] = 0.0f;
                out[i*2+1] = 0.0f;
            }
        }, &err)) { wprintf(L"ren: %s\n", err.c_str()); return 1; }
    wprintf(L"monitoring — Ctrl+C to stop. depth/drops/underruns print every 5 s\n");
    for (;;) {
        Sleep(5000);
        wprintf(L"depth=%zu drops=%llu under=%llu\n",
                ring.Depth(), (unsigned long long)ring.Drops(), (unsigned long long)ring.Underruns());
        fflush(stdout);
    }
}

static void AttachOrAllocConsole() {
    // Launched with stdout redirected (a pipe or file)? Keep it — the CRT is
    // already wired to the inherited handle. Only conjure a console otherwise.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
    _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
}

static int AppMain(HINSTANCE hInstance) {
    (void)hInstance;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--btinfo") == 0) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                int rc = RunBtInfo();
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--sessions") == 0) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                int rc = RunSessions();
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--sonar") == 0) {
                AttachOrAllocConsole();
                const wchar_t* arg = (i + 1 < argc && argv[i + 1][0] != L'-') ? argv[i + 1] : nullptr;
                bool yes = false;
                for (int j = 1; j < argc; ++j)
                    if (wcscmp(argv[j], L"--yes") == 0) yes = true;
                int rc = RunSonar(arg, yes);
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--sonarch") == 0) {
                AttachOrAllocConsole();
                int rc = RunSonarChannels();
                LocalFree(argv);
                return rc;
            }
            if ((wcscmp(argv[i], L"--sonarset") == 0 ||
                 wcscmp(argv[i], L"--sonarmute") == 0) && i + 3 < argc) {
                AttachOrAllocConsole();
                int rc = RunSonarWrite(argv[i + 1], argv[i + 2], argv[i + 3],
                                       wcscmp(argv[i], L"--sonarmute") == 0);
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--feed") == 0) {
                AttachOrAllocConsole();
                int secs = (i + 1 < argc) ? _wtoi(argv[i + 1]) : 3;
                if (secs < 1 || secs > 60) secs = 3;
                int rc = RunFeed(secs);
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--meter") == 0 && i + 1 < argc) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                int secs = (i + 2 < argc) ? _wtoi(argv[i + 2]) : 5;
                if (secs < 1 || secs > 60) secs = 5;
                int rc = RunMeter(argv[i + 1], secs);
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--setdefault") == 0 && i + 1 < argc) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                int rc = RunSetDefault(argv[i + 1]);
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--approute") == 0 && i + 2 < argc) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                int rc = RunAppRoute(argv[i + 1], argv[i + 2]);
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--levels") == 0) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                int rc = RunLevels();
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--devices") == 0) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                int rc = RunDevices();
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
            if (wcscmp(argv[i], L"--monitor") == 0 && i + 2 < argc) {
                AttachOrAllocConsole();
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                int rc = RunMonitor(argv[i + 1], argv[i + 2]);
                CoUninitialize();
                LocalFree(argv);
                return rc;
            }
        }
        LocalFree(argv);
    }

    // Single instance: the second launch asks the first to show its window, then leaves.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\mdxmixer_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\mdxmixer", GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            const wchar_t show[] = L"MDXM_SHOW";
            DWORD wr = 0;
            WriteFile(pipe, show, sizeof(show), &wr, nullptr);
            CloseHandle(pipe);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // The UI thread runs COM for the whole session: the policy API, endpoint
    // enumeration and the device watcher are all called from here, and without
    // an apartment the very first of them fails with CO_E_NOTINITIALIZED.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    mdxm::AppController app;
    int rc = 0;
    if (app.Start(hInstance)) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        rc = (int)msg.wParam;
    } else {
        rc = 2;
    }
    app.Stop();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    __try {
        return AppMain(hInstance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3; // no-crash rule: swallow, exit nonzero. Logging attached in Task 15.
    }
}
#endif
