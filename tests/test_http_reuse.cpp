// test_http_reuse.cpp — keeping the Sonar connection open without losing what
// closing it every time was protecting.
//
// Shane asked the right question directly: "does sonar allow for keeping the
// connection open?" It does -- GG is ordinary HTTP/1.1 on loopback -- and
// mdxmixer was the one refusing, opening and closing a session, a connection
// and a request for every mute. The measured cost was 8-78 ms a call against
// 0.3 ms for a channel with no HTTP in the path.
#include "test_framework.h"
#include "routing/http_reuse.h"

using namespace mdxm;

MDXM_TEST_CASE(PooledConnection_MatchesTheSameAddress) {
    CHECK(PooledConnectionMatches(L"127.0.0.1", 32371, false, L"127.0.0.1", 32371, false));
}

// Discovery can move GG. A pooled connection to the old port fails in a way
// that looks exactly like GG being down, which would send the caller
// re-discovering an address it already had.
MDXM_TEST_CASE(PooledConnection_DoesNotMatchADifferentPort) {
    CHECK(!PooledConnectionMatches(L"127.0.0.1", 32371, false, L"127.0.0.1", 40123, false));
}

MDXM_TEST_CASE(PooledConnection_DoesNotMatchADifferentSchemeOrHost) {
    CHECK(!PooledConnectionMatches(L"127.0.0.1", 32371, false, L"127.0.0.1", 32371, true));
    CHECK(!PooledConnectionMatches(L"127.0.0.1", 32371, false, L"localhost", 32371, false));
}

MDXM_TEST_CASE(PooledConnection_EmptyPoolMatchesNothing) {
    CHECK(!PooledConnectionMatches(L"", 0, false, L"127.0.0.1", 32371, false));
    CHECK(!PooledConnectionMatches(L"127.0.0.1", 0, false, L"127.0.0.1", 0, false));
}

// ── the retry rule ───────────────────────────────────────────────────────

// The case the whole design turns on: GG restarted, our kept handle is stale,
// and the user's click must still land.
MDXM_TEST_CASE(Retry_AStalePooledConnectionIsRetriedOnce) {
    CHECK(RetryOnFreshConnection(/*transportFailed*/ true, /*pooled*/ true, /*attempt*/ 0));
    CHECK(!RetryOnFreshConnection(true, true, 1));   // and only once
}

// A 404 or a 500 is GG answering. Re-connecting cannot change the answer, and
// retrying would double the cost of every error.
MDXM_TEST_CASE(Retry_AnHttpStatusFailureIsNotATransportFailure) {
    CHECK(!RetryOnFreshConnection(/*transportFailed*/ false, true, 0));
}

// A connection opened seconds ago that cannot reach GG means GG is gone. That
// is a real failure, and the caller's re-discovery is what handles it -- not a
// second identical attempt.
MDXM_TEST_CASE(Retry_AFreshConnectionFailingIsARealFailure) {
    CHECK(!RetryOnFreshConnection(true, /*pooled*/ false, 0));
}
