#include "test_framework.h"
#include "device/endpoints.h"
#include "routing/audio_policy_config.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

using namespace mdxm;

MDXM_TEST_CASE(Match_IdWinsOverName) {
    std::vector<EndpointInfo> eps = {
        {L"{id-1}", L"Headphones", true, true},
        {L"{id-2}", L"Headphones", true, true},
    };
    const EndpointInfo* m = MatchBinding(eps, {L"{id-2}", L"Headphones"});
    CHECK(m && m->id == L"{id-2}");
}

MDXM_TEST_CASE(Match_NameFallbackWhenIdGone) {
    // Re-paired Bluetooth: new id, same name.
    std::vector<EndpointInfo> eps = { {L"{id-new}", L"WF-1000XM6", true, true} };
    const EndpointInfo* m = MatchBinding(eps, {L"{id-old}", L"WF-1000XM6"});
    CHECK(m && m->id == L"{id-new}");
    const EndpointInfo* none = MatchBinding(eps, {L"{id-old}", L"Different Name"});
    CHECK(none == nullptr);
}

MDXM_TEST_CASE(Match_NameCompareIsCaseInsensitiveExact) {
    std::vector<EndpointInfo> eps = { {L"{a}", L"CABLE-A Input (VB-Audio)", true, true} };
    CHECK(MatchBinding(eps, {L"{gone}", L"cable-a input (vb-audio)"}) != nullptr);
    CHECK(MatchBinding(eps, {L"{gone}", L"CABLE-A"}) == nullptr);  // substring is NOT a match
}

MDXM_TEST_CASE(Format_ParsesFloatPcmAndRefusesElse) {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 2;
    f.nSamplesPerSec = 48000; f.wBitsPerSample = 32;
    StreamFormat sf = ParseMixFormat(&f);
    CHECK(sf.sample == StreamFormat::Sample::F32 && sf.rate == 48000 && sf.channels == 2);

    f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 16;
    CHECK(ParseMixFormat(&f).sample == StreamFormat::Sample::I16);

    WAVEFORMATEXTENSIBLE e = {};
    e.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; e.Format.nChannels = 2;
    e.Format.nSamplesPerSec = 44100; e.Format.wBitsPerSample = 32;
    e.Format.cbSize = sizeof(e) - sizeof(e.Format);
    e.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    StreamFormat sfe = ParseMixFormat(&e.Format);
    CHECK(sfe.sample == StreamFormat::Sample::F32 && sfe.rate == 44100);

    e.SubFormat = KSDATAFORMAT_SUBTYPE_ALAW;                      // exotic: refuse
    CHECK(ParseMixFormat(&e.Format).sample == StreamFormat::Sample::Unsupported);

    f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 24;        // 24-bit PCM: refuse (spec: float32/int16 only)
    CHECK(ParseMixFormat(&f).sample == StreamFormat::Sample::Unsupported);
}

MDXM_TEST_CASE(Policy_InitWithoutPriorComInit) {
    // The GUI process reaches AudioPolicyConfig::Init before anything has joined
    // an apartment, so Init must stand COM up itself. Getting this wrong returns
    // CO_E_NOTINITIALIZED (0x800401F0) once, never retries, and per-app routing
    // — the product's whole point — is dead for the life of the process.
    // This test thread deliberately has no apartment.
    mdxm::AudioPolicyConfig policy;
    std::wstring err;
    bool ok = policy.Init(&err);
    CHECK(err.find(L"800401F0") == std::wstring::npos);   // never "COM not initialized"
    if (!ok) std::printf("note: policy factory unavailable on this build: %ls\n", err.c_str());
    else CHECK(policy.IsAvailable());
}

MDXM_TEST_CASE(PolicyId_RoundTrips) {
    std::wstring mm = L"{0.0.0.00000000}.{aaaa-bbbb}";
    std::wstring pol = mdxm::MakePolicyDeviceId(mm);
    CHECK(pol.find(L"MMDEVAPI") != std::wstring::npos);
    CHECK(mdxm::ParsePolicyDeviceId(pol) == mm);
    CHECK(mdxm::ParsePolicyDeviceId(L"garbage") == L"garbage");  // unrecognized passes through
}
