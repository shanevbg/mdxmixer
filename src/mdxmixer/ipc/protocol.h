#pragma once
// MDXM pipe protocol — same record grammar as MDropDX12's pipe: VERB|field=value|…,
// positional payload after VERB=, ids contain no '|'. Chunked replies are framed
// MDXM_BEGIN … MDXM_END (the MIXER_STATE contract). Errors: MDXM_ERR|msg=<text>.
#include "mixer_control.h"
#include <initializer_list>
#include <string>
#include <vector>

namespace mdxm {

struct Record {
    std::wstring verb;
    // key/value fields; positional tokens (no '=') carry an empty key, in order.
    std::vector<std::pair<std::wstring, std::wstring>> fields;
    const std::wstring* Find(const std::wstring& key) const;
};

Record       ParseRecord(const std::wstring& msg);
std::wstring BuildRecord(const std::wstring& verb,
                         std::initializer_list<std::pair<std::wstring, std::wstring>> fields);

// Returns every reply message for one incoming message, in order. Never throws.
std::vector<std::wstring> HandleProtocolMessage(const std::wstring& msg, IMixerControl& ctl,
                                                bool* wantSubscribe);

constexpr wchar_t kProtocolVersion[] = L"1";

} // namespace mdxm
