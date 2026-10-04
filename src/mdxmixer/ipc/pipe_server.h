#pragma once
// \\.\pipe\mdxmixer — fixed name (single instance), duplex, PIPE_TYPE_MESSAGE |
// PIPE_READMODE_MESSAGE, UTF-16LE null-terminated messages, multi-client.
// A simplified MDropDX12 pipe_server: accept thread + one thread per client with
// an outgoing queue and event; no SIGNAL dispatch, no strict mode, no renames.
// Every thread body is try/catch (no-crash rule).
#include <functional>
#include <string>
#include <vector>

namespace mdxm {

class PipeServer {
public:
    ~PipeServer();
    // onMessage runs on the client's thread; its returned strings are sent back in
    // order. wantSubscribe (from the protocol layer) flips that client's flag.
    using Handler = std::function<std::vector<std::wstring>(const std::wstring& msg, bool* wantSubscribe)>;
    // pipeName defaults to the production name. Tests pass their own: sharing
    // the fixed name with a running mdxmixer made them connect to the app
    // instead of their own server and fail for reasons that had nothing to do
    // with the code under test.
    bool Start(Handler handler, std::wstring* err, const std::wstring& pipeName = kPipeName);
    void Stop();
    void Broadcast(const std::wstring& msg);   // to subscribed clients only (MDXM_SUBSCRIBE=1)
    int  ClientCount() const;
    // Is anyone listening? Asked before BUILDING a broadcast, not just before
    // sending one: the peak push runs four times a second forever, and
    // formatting thirty-odd records to hand them to nobody is the one cost
    // this feature could have had and does not.
    bool HasSubscribers() const;
    static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\mdxmixer";

    struct Impl;   // public: the per-client context struct points back at it

private:
    Impl* m_impl = nullptr;
};

} // namespace mdxm
