// test_docs.cpp — docs/ipc.md is not a description of the protocol. For
// MDropDX12 it IS the protocol: it is the only thing that tells the other side
// what to speak, and that side now depends on it for the mixer channels, the
// failover rule, the audio feed and the HUD battery.
//
// So a verb accepted by HandleInner and absent from that document is a defect,
// not doc drift — it is a verb nobody outside this repository can use. The
// comparison is mechanical, which is why it is a test rather than a habit
// (fj#9: MDXM_TAB and MDXM_CAPTURE were both accepted and both undocumented,
// discoverable only by reading their own error strings).
//
// Only one direction is checked. A verb in the code must appear in the
// document; the reverse is not a defect, because the document also names the
// records that come BACK — MDXM_CHAN, MDXM_FOENTRY, MDXM_PEAK — and those are
// not verbs anyone sends.
#include "test_framework.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// The repository root, found by walking up from the executable until the
// document itself is there. bin\Test\mdxmixer_test.exe is two levels down, but
// the walk means a build laid out differently still finds it.
std::string RepoRoot() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    for (int up = 0; up < 8; ++up) {
        const size_t slash = dir.find_last_of('\\');
        if (slash == std::string::npos) break;
        dir = dir.substr(0, slash);
        std::ifstream probe(dir + "\\docs\\ipc.md");
        if (probe) return dir;
    }
    return {};
}

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Every `r.verb == L"MDXM_…"` in the protocol handler, which is the one place
// an incoming verb is recognised.
std::vector<std::string> AcceptedVerbs(const std::string& src) {
    std::vector<std::string> out;
    const std::string needle = "r.verb == L\"";
    size_t pos = 0;
    while ((pos = src.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        const size_t end = src.find('"', pos);
        if (end == std::string::npos) break;
        out.push_back(src.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

} // namespace

MDXM_TEST_CASE(Docs_EveryAcceptedVerbIsDocumented) {
    const std::string root = RepoRoot();
    // Failed loudly rather than skipped: a drift guard that quietly does
    // nothing when it cannot find the files is the guard not working.
    CHECK(!root.empty());
    if (root.empty()) return;

    const std::string doc = ReadFile(root + "\\docs\\ipc.md");
    const std::string src = ReadFile(root + "\\src\\mdxmixer\\ipc\\protocol.cpp");
    CHECK(!doc.empty());
    CHECK(!src.empty());
    if (doc.empty() || src.empty()) return;

    const auto verbs = AcceptedVerbs(src);
    // If this ever reads zero the extraction has broken, and an empty list
    // would otherwise pass every check below.
    CHECK(verbs.size() >= 20);

    std::vector<std::string> missing;
    for (const auto& v : verbs)
        if (doc.find(v) == std::string::npos) missing.push_back(v);

    for (const auto& m : missing)
        std::printf("     undocumented verb: %s\n", m.c_str());
    CHECK(missing.empty());
}

// The check above can only see verbs mdxmixer ACCEPTS, because that is what the
// extractor scans for. The records it SENDS BACK are invisible to it -- and a
// reply record nobody outside this repository knows the shape of is exactly as
// useless as an undocumented verb. The existing ones (MDXM_CHAN, MDXM_PEAK,
// MDXM_FOENTRY) predate the guard; these are pinned by name as they are added.
MDXM_TEST_CASE(Docs_VbanReplyRecordsAreDocumented) {
    const std::string root = RepoRoot();
    CHECK(!root.empty());
    if (root.empty()) return;
    const std::string doc = ReadFile(root + "\\docs\\ipc.md");
    CHECK(!doc.empty());
    if (doc.empty()) return;
    for (const char* rec : { "MDXM_VBANSTATE", "MDXM_VBANPEER", "MDXM_AUTHSTATE" }) {
        const bool found = doc.find(rec) != std::string::npos;
        if (!found) std::printf("     undocumented reply record: %s\n", rec);
        CHECK(found);
    }
}
