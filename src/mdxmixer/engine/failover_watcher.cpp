#include "engine/failover_watcher.h"

#include <algorithm>

namespace mdxm {

void FailoverWatcher::NoteRouteDevice(const std::wstring& routeId,
                                      const std::wstring& deviceId) {
  RouteState& st = m_routes[routeId];
  if (st.current == deviceId) return;
  // A move re-baselines and abandons any pending arm: whether it came from a
  // person, from Sonar, or from our own commit, the question the watcher was
  // about to answer has just been answered.
  st.current = deviceId;
  st.candidate.clear();
  st.state = FailoverState::Idle;
  st.armedAt = 0;
  st.reason.clear();
}

std::wstring FailoverWatcher::Resolve(const AllowEntry& entry) const {
  if (Present(entry.id)) return entry.id;
  if (entry.name.empty() || !m_nameOf) return std::wstring();
  // No id match. The device may have come back under a new id carrying the
  // same name, which is exactly what a re-paired Bluetooth headset does, so
  // ask what id holds that name now.
  return m_nameOf(entry.name);
}

std::wstring FailoverWatcher::BestCandidate(const RouteRule& rule) const {
  for (const AllowEntry& entry : rule.allow) {
    const std::wstring resolved = Resolve(entry);
    if (!resolved.empty() && Present(resolved)) return resolved;
  }
  return std::wstring();
}

void FailoverWatcher::HoldFor(unsigned ms) {
  const unsigned until = Now() + ms;
  // Never shorten a hold already running: two restarts close together should
  // extend the quiet period, not reset it to the shorter of the two.
  if (m_held && (int)(until - m_holdUntil) <= 0) return;
  m_held = true;
  m_holdUntil = until;
}

void FailoverWatcher::ReleaseHold() {
  m_held = false;
  m_holdUntil = 0;
  // Leave each route's reason alone: the next Tick rewrites it from the state
  // it finds, and clearing it here would briefly report "nothing to say" for a
  // route that is in fact mid-arm.
}

unsigned FailoverWatcher::HoldRemainingMs() const {
  if (!m_held) return 0;
  const int left = (int)(m_holdUntil - Now());
  return left > 0 ? (unsigned)left : 0u;
}

void FailoverWatcher::Tick() {
  const unsigned now = Now();

  // The audio graph is rebuilding: look, but do not move (#410).
  //
  // Every endpoint reads absent while AUDIODG restarts, so acting here would
  // move every armed route at once, to devices that cannot accept the move,
  // and add churn to the graph that is already rebuilding. Routes keep their
  // state and pick up normally when the hold lifts.
  if (m_held) {
    if ((int)(m_holdUntil - now) > 0) {
      for (const auto& pair : m_rules) {
        RouteState& st = m_routes[pair.second.routeId];
        if (!pair.second.armed) continue;
        st.reason = L"the Windows audio engine is restarting; holding off";
      }
      return;
    }
    m_held = false;
  }

  for (const auto& pair : m_rules) {
    const RouteRule& rule = pair.second;
    RouteState& st = m_routes[rule.routeId];

    if (!rule.armed) {
      st.state = FailoverState::Idle;
      st.candidate.clear();
      st.reason = L"not armed";
      continue;
    }

    if (Present(st.current)) {
      // Cancel on return: whatever was being armed is dropped the moment the
      // device we are actually on is back.
      st.state = FailoverState::Idle;
      st.candidate.clear();
      st.armedAt = 0;
      st.reason.clear();
      // A move that STUCK is the only thing that clears the back-off (#410).
      // Reaching a present device is the definition of success here, whether
      // the route got there by failover, by hand, or by the device coming
      // back on its own.
      st.lastTarget.clear();
      st.attempts = 0;
      continue;
    }

    // The current device is gone.
    const unsigned dwellMs = EffectiveDwellMs(st);
    if (st.committedAt != 0 && dwellMs != 0 &&
        (now - st.committedAt) < dwellMs) {
      st.state = FailoverState::Searching;
      // Name the back-off when there is one, so a route that is retrying a
      // move that will not take says so instead of reading like an ordinary
      // dwell. This was the state the machine sat in between attempts for
      // hours, reporting nothing unusual.
      if (st.attempts > 1) {
        wchar_t buf[128];
        swprintf(buf, 128,
                 L"attempt %u did not stick; waiting %u s before retrying",
                 st.attempts, dwellMs / 1000);
        st.reason = buf;
      } else {
        st.reason = L"waiting out the minimum dwell";
      }
      continue;
    }

    const std::wstring candidate = BestCandidate(rule);
    if (candidate.empty()) {
      st.state = FailoverState::Searching;
      st.candidate.clear();
      st.armedAt = 0;
      st.reason = L"no allowed replacement is present";
      continue;
    }

    if (st.state != FailoverState::Arming || st.candidate != candidate) {
      // First sight of this candidate, or a different one than we were
      // arming: start its window from now.
      st.state = FailoverState::Arming;
      st.candidate = candidate;
      st.armedAt = now;
      st.reason = L"waiting for the replacement to be stable";
      continue;
    }

    // Space endpoint reassignments across ALL routes (#410). The dwell above
    // governs this route only, so two routes losing their device together
    // would still reassign in the same tick -- and Sonar mishandles a
    // personal-stream endpoint that changes too quickly, with its APO faulting
    // AUDIODG and taking every audio stream on the machine down.
    //
    // Deliberately does NOT disarm or re-arm: the route keeps its stability
    // window and commits on a later tick, a fraction of a second late.
    if (m_anyCommitted && m_minGapMs != 0 &&
        (now - m_lastAnyCommitAt) < m_minGapMs) {
      st.reason = L"spacing device changes; another route just moved";
      continue;
    }

    if ((now - st.armedAt) >= m_stabilityMs) {
      const std::wstring target = st.candidate;
      // Count consecutive commits to the SAME target (#410). Arriving here
      // again with the target we last committed means that move did not
      // stick -- a move that had stuck would have been caught by the
      // Present(st.current) branch at the top and cleared this.
      if (target == st.lastTarget) {
        if (st.attempts < kMaxBackoffShift + 2) st.attempts++;
      } else {
        st.lastTarget = target;
        st.attempts = 1;
      }
      st.current = target;
      st.candidate.clear();
      st.state = FailoverState::Idle;
      st.armedAt = 0;
      st.committedAt = now;
      st.reason.clear();
      m_anyCommitted = true;
      m_lastAnyCommitAt = now;
      if (m_onCommit) m_onCommit(rule.routeId, target);
    }
  }
}

FailoverState FailoverWatcher::StateOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? FailoverState::Idle : it->second.state;
}

std::wstring FailoverWatcher::CurrentOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? std::wstring() : it->second.current;
}

std::wstring FailoverWatcher::TargetOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? std::wstring() : it->second.lastTarget;
}

unsigned FailoverWatcher::SinceCommitMs(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  if (it == m_routes.end() || it->second.committedAt == 0) return 0;
  // GetTickCount wraps every 49.7 days. Unsigned subtraction is correct across
  // the wrap, so this needs no special case -- but it must stay unsigned.
  return Now() - it->second.committedAt;
}

unsigned FailoverWatcher::AttemptsOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? 0u : it->second.attempts;
}

unsigned FailoverWatcher::DwellMsOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? m_dwellMs : EffectiveDwellMs(it->second);
}

std::wstring FailoverWatcher::ReasonOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? std::wstring() : it->second.reason;
}

bool AllowEntryMatches(const AllowEntry& e, const std::wstring& key) {
  if (key.empty()) return false;
  return (!e.id.empty()   && e.id   == key) ||
         (!e.name.empty() && e.name == key);
}

int MoveAllowEntry(RouteRule& rule, const std::wstring& key, int delta) {
  int from = -1;
  for (size_t i = 0; i < rule.allow.size(); i++) {
    if (AllowEntryMatches(rule.allow[i], key)) { from = (int)i; break; }
  }
  if (from < 0) return -1;

  int to = from + delta;
  if (to < 0) to = 0;
  if (to >= (int)rule.allow.size()) to = (int)rule.allow.size() - 1;

  // Clamping rather than refusing. Up from the top is what a user pressing the
  // button repeatedly does, and answering that with an error would make the
  // control feel broken at exactly the moment it is doing the right thing.
  if (to != from) std::swap(rule.allow[from], rule.allow[to]);
  return to;
}

}  // namespace mdxm
