// failover_watcher.h — re-home a route when the device it is on disappears.
//
// PORTED WHOLE from MDropDX12 (mixer_failover.{h,cpp} at 29c4806d). Every rule
// below was decided there against this machine's hardware, and two of them
// came out of a real outage: the back-off on repeated commits and the global
// minimum gap are #410, where a route re-committed the same move thirteen
// times, 24 s apart, each one tearing down and rebuilding the audio graph,
// ending with the audio engine restarting.
//
// It replaces mdxmixer's own FailoverDecider, which had the stability window
// and the per-route dwell and neither of those protections, and handled one
// route where this handles any number by id.
//
// The rules, all chosen explicitly:
//
//   * Fail over, NEVER back. After a commit the new device is the current one.
//     Reconnecting the old one does nothing.
//   * Cancel on return. If the original comes back during the stability
//     window, nothing moves -- so a brief Bluetooth dropout costs nothing.
//     That is the point of the debounce rather than a side effect of it.
//   * Opt-in is absolute. With no allowlisted device present the watcher does
//     nothing, forever, and says why.
//   * A minimum dwell after each commit stops one bad unplug cascading down
//     the priority list.
//
// Clock and presence are injected, so every sequence is testable without a
// device: disconnect, flap, re-pair under a new id, nothing available.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mdxm {

enum class FailoverState { Idle, Searching, Arming };

// Both halves are stored. A re-paired Bluetooth headset can return with a new
// endpoint id and the same friendly name, so an allowlist keyed on id alone
// silently stops matching -- which is already true of the user's own Sonar
// configuration, where a monitoring route points at a WF-1000XM5-3 while
// Windows reports WF-1000XM5-4.
struct AllowEntry {
  std::wstring id;
  std::wstring name;
};

struct RouteRule {
  std::wstring routeId;
  bool armed = false;
  std::vector<AllowEntry> allow;   // ordered: first present entry wins
};

// Does this entry answer to `key`? By id OR by name.
//
// Both, because a device that is not connected is listed by NAME with no id at
// all -- and a pair of Bluetooth headphones is switched off far more often
// than it is on. Matching on the id alone made exactly those entries
// unreachable: they could not be removed, and they piled up. Shared so the
// remove and the reorder cannot drift apart on it.
bool AllowEntryMatches(const AllowEntry& e, const std::wstring& key);

// Move the entry answering to `key` by `delta` places, swapping with its
// neighbour and clamping at the ends. Returns the new index, or -1 when
// nothing matches.
//
// The order is the preference: `allow` is "first present entry wins", so this
// is what decides which replacement failover reaches for. Kept here, as a pure
// function on the rule, so that decision is testable without an audio device
// or a running app.
int MoveAllowEntry(RouteRule& rule, const std::wstring& key, int delta);

class FailoverWatcher {
 public:
  void SetRule(const RouteRule& rule) { m_rules[rule.routeId] = rule; }
  void ClearRules() { m_rules.clear(); m_routes.clear(); }

  void SetStabilitySeconds(int s) { m_stabilityMs = (unsigned)(s * 1000); }
  void SetMinDwellSeconds(int s)  { m_dwellMs = (unsigned)(s * 1000); }
  void SetMinGapSeconds(int s)    { m_minGapMs = (unsigned)(s < 0 ? 0 : s * 1000); }

  void SetPresence(std::function<bool(const std::wstring&)> fn) {
    m_present = std::move(fn);
  }
  // NAME -> the id currently carrying it. That direction is the Bluetooth
  // fallback; getting it backwards makes the re-pair case silently dead.
  void SetNames(std::function<std::wstring(const std::wstring&)> fn) {
    m_nameOf = std::move(fn);
  }
  void SetClock(std::function<unsigned()> fn) { m_now = std::move(fn); }
  void SetOnCommit(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    m_onCommit = std::move(fn);
  }

  // Tell the watcher where a route currently points. Called at startup, after
  // a device event, and after anything -- including a person -- moves it. A
  // change abandons any pending arm: the question has just been answered.
  void NoteRouteDevice(const std::wstring& routeId,
                       const std::wstring& deviceId);

  // Stand down for a while, because the audio graph is being rebuilt (#410).
  //
  // When AUDIODG.EXE restarts, every endpoint is momentarily invalid and
  // Present() answers false for all of them at once. A watcher that acts on
  // that sees its device "disappear" and moves the route -- during the one
  // window where no move can succeed, and where the move itself is more churn
  // for the graph that is already rebuilding. The observed loop ran for six
  // minutes and ended with the audio engine restarting a minute later.
  //
  // Holding is not the same as disarming: a genuine headset failure during the
  // hold is picked up as soon as it lifts. It only refuses to ACT on a snapshot
  // taken while the ground was moving.
  void HoldFor(unsigned ms);
  // End the hold now, because the thing it was waiting for has happened.
  //
  // The hold length is a CEILING, not a duration. Killed cleanly, AUDIODG is
  // back in under a second; after Sonar's APO faults it, it takes about a
  // minute. Measured both. Waiting the full minute for the fast case would
  // leave a genuinely dead headset unattended for no reason, so the caller
  // releases as soon as the engine is back and has held still for a moment.
  void ReleaseHold();
  // Milliseconds of hold remaining, 0 when not held.
  unsigned HoldRemainingMs() const;

  // Advance the machine. MUST be called periodically, not only on device
  // events: arming and committing are deliberately separate ticks, so a route
  // that arms and then sees no further event would never commit. The mixer
  // worker wakes at least every 100ms, which is where this belongs. It is
  // cheap and does no I/O.
  void Tick();

  FailoverState StateOf(const std::wstring& routeId) const;
  std::wstring ReasonOf(const std::wstring& routeId) const;

  // What a route is on, what it last tried to move to, and how that went
  // (#410). The state machine's whole decision turns on `current`, and nothing
  // could read it -- DIAG_FAILOVER reported state and reason, which for a route
  // stuck in a commit loop read "idle" with no reason at all between attempts.
  std::wstring CurrentOf(const std::wstring& routeId) const;
  std::wstring TargetOf(const std::wstring& routeId) const;
  // Milliseconds since the last commit, or 0 if there has not been one.
  unsigned SinceCommitMs(const std::wstring& routeId) const;
  // Consecutive commits to the SAME target, i.e. how many times a move has
  // been made and not stuck. 0 or 1 is healthy.
  unsigned AttemptsOf(const std::wstring& routeId) const;
  // The dwell actually in force for this route, after any back-off.
  unsigned DwellMsOf(const std::wstring& routeId) const;

 private:
  struct RouteState {
    std::wstring current;      // device the route is on
    std::wstring candidate;    // device being armed
    FailoverState state = FailoverState::Idle;
    unsigned armedAt = 0;
    unsigned committedAt = 0;
    std::wstring reason;
    // The last device committed to, and how many times in a row (#410).
    //
    // A commit sets `current` to the target, so the next tick should see it
    // present and go idle. It does not when the mixer snapshot re-baselines
    // `current` to something else before that tick -- Sonar not taking the
    // move, or reporting the device under another identifier -- and the route
    // then re-commits the SAME target every dwell, for ever. Measured at
    // thirteen moves in a row, 24 s apart, each one a rebuild of the audio
    // graph.
    //
    // Keyed on the TARGET rather than on who called NoteRouteDevice, because
    // the re-baseline is exactly what the loop does: a counter that reset on it
    // would never rise. A move that actually sticks clears this, and that is
    // the only thing that does.
    std::wstring lastTarget;
    unsigned attempts = 0;
  };

  unsigned Now() const { return m_now ? m_now() : 0; }
  bool Present(const std::wstring& id) const {
    return m_present && !id.empty() && m_present(id);
  }
  // Matches by id, then by an exact friendly-name match when no id matches.
  // The fallback can only ever resolve to an entry the user added, so opt-in
  // is preserved: it never enrols a device.
  std::wstring Resolve(const AllowEntry& entry) const;
  std::wstring BestCandidate(const RouteRule& rule) const;
  // The dwell for this route, doubled once per consecutive failed attempt.
  //
  // One retry is worth making -- a device can genuinely be slow to settle.
  // Repeating the same move on a fixed 24 s beat is not a retry, it is a loop,
  // and each turn of it tears the audio graph down and builds it again. The
  // doubling means a move that never sticks costs 10 s, then 20, 40, 80 and so
  // on instead of for ever, while a real failover is untouched: the first
  // attempt uses the configured dwell exactly.
  //
  // Capped, so the interval cannot run away or overflow, and because a route
  // that has failed this many times is not going to be fixed by waiting
  // longer -- at that point the reason string is the useful output, not
  // another attempt.
  static const unsigned kMaxBackoffShift = 6;   // 10s -> ~10.7 min
  unsigned EffectiveDwellMs(const RouteState& st) const {
    if (m_dwellMs == 0 || st.attempts <= 1) return m_dwellMs;
    const unsigned shift =
        (st.attempts - 1 > kMaxBackoffShift) ? kMaxBackoffShift
                                             : (st.attempts - 1);
    return m_dwellMs << shift;
  }

  std::map<std::wstring, RouteRule> m_rules;
  std::map<std::wstring, RouteState> m_routes;

  unsigned m_stabilityMs = 3000;
  unsigned m_dwellMs = 10000;
  // Absolute clock value the hold expires at, and whether one is running.
  // Kept as a flag plus a deadline rather than a bare deadline, because the
  // clock is GetTickCount and 0 is a value it really takes.
  bool m_held = false;
  unsigned m_holdUntil = 0;
  // The floor between ANY two endpoint reassignments, across every route
  // (#410). minDwellSeconds governs one route and cannot see the others, so
  // two routes losing their device in the same tick reassign together, and a
  // route handed a different device on each of a rapid series of ticks is not
  // covered by a dwell at all. Sonar mishandles a personal-stream endpoint
  // that changes too quickly, and its APO faults AUDIODG when it does, which
  // takes every audio stream on the machine with it.
  unsigned m_minGapMs = 5000;
  bool m_anyCommitted = false;
  unsigned m_lastAnyCommitAt = 0;

  std::function<bool(const std::wstring&)> m_present;
  std::function<std::wstring(const std::wstring&)> m_nameOf;
  std::function<unsigned()> m_now;
  std::function<void(const std::wstring&, const std::wstring&)> m_onCommit;
};

}  // namespace mdxm
