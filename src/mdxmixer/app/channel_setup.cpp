#include "app/channel_setup.h"
#include <algorithm>

namespace mdxm {

std::wstring MakeChannelId(const std::vector<ChannelConfig>& existing,
                           const std::wstring& name) {
    std::wstring base;
    for (wchar_t c : name) {
        if (c >= L'A' && c <= L'Z') base += (wchar_t)(c - L'A' + L'a');
        else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) base += c;
        else if (!base.empty() && base.back() != L'-') base += L'-';
    }
    while (!base.empty() && base.back() == L'-') base.pop_back();
    if (base.empty()) base = L"channel";

    auto taken = [&existing](const std::wstring& id) {
        for (const auto& c : existing) if (c.id == id) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int n = 2; n < 100; ++n) {
        std::wstring candidate = base + L"-" + std::to_wstring(n);
        if (!taken(candidate)) return candidate;
    }
    return base + L"-x";
}

std::wstring SonarChannelName(const std::wstring& endpointName) {
    // Match on Sonar's own naming: "SteelSeries Sonar - <channel> (<device>)".
    const std::wstring prefix = L"SteelSeries Sonar - ";
    if (endpointName.compare(0, prefix.size(), prefix) != 0) return {};
    std::wstring rest = endpointName.substr(prefix.size());
    size_t paren = rest.find(L" (");
    if (paren != std::wstring::npos) rest = rest.substr(0, paren);
    while (!rest.empty() && rest.back() == L' ') rest.pop_back();
    // Sonar publishes its microphone and stream endpoints under the same
    // naming. Those are not channels to mix — the mic has its own chain, and
    // Stream is where the streaming mix GOES, so adopting it would be a loop.
    std::wstring lower = rest;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower == L"microphone" || lower == L"stream") return {};
    return rest;
}

bool WouldFeedBack(const std::wstring& sourceEndpointId,
                   const std::wstring& personalRenderId) {
    if (sourceEndpointId.empty() || personalRenderId.empty()) return false;
    return _wcsicmp(sourceEndpointId.c_str(), personalRenderId.c_str()) == 0;
}

} // namespace mdxm
