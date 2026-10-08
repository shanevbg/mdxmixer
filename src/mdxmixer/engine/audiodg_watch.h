// audiodg_watch.h — notice AUDIODG.EXE going away and coming back.
//
// PORTED from MDropDX12's Engine::MixerAudioEngineTick (#410). Every rule here
// was decided there against this machine, and the two measurements in it cost
// real time to establish: killed cleanly AUDIODG is back in under a second,
// and about a minute after SteelSeries' Sonar.APO.dll faults it.
//
// AUDIODG hosts every audio processing object on the machine, Sonar.APO.dll
// among them. When it goes, every endpoint is momentarily invalid and NO route
// works until it is back -- so the failover watcher has to be held off, because
// moving a route onto a device that cannot accept it is both futile and more
// churn for the graph that is already rebuilding. Measured on the other side:
// thirteen route moves in six minutes, ending with the audio engine restarting.
//
// Identified by PROCESS ID, which is enough: a restart always gets a new one.
// The name is read from a toolhelp snapshot rather than by opening the process,
// because AUDIODG runs as SYSTEM and cannot be opened from here.
//
// The decision is kept pure -- the pid is handed in -- so a restart, a
// disappearance and a recovery are testable without killing the audio engine
// on the machine this is built on. FindAudiodgPid() is the one impure part and
// does nothing but look.
#pragma once

#include <windows.h>

namespace mdxm {

// What one observation of the pid means.
enum class AudiodgEvent {
    Nothing,       // no change worth acting on
    Appeared,      // first sighting, or back after an outage. NOT a restart.
    Restarted,     // a different pid, or none where there was one. Hold.
    SteadyAgain,   // present and unchanged long enough to trust. Release.
};

// The pid of AUDIODG.EXE, or 0 when it is not running.
DWORD FindAudiodgPid();

class AudiodgWatch {
public:
    // Is a scan due? A toolhelp snapshot is not free and the thing being
    // watched takes about a minute to come back, so two seconds is plenty.
    bool ShouldScan(unsigned nowMs) const {
        return m_lastScanMs == 0 || (nowMs - m_lastScanMs) >= kScanIntervalMs;
    }

    // Record one observation and say what it means. `pid` is 0 for absent.
    AudiodgEvent Observe(DWORD pid, unsigned nowMs) {
        m_lastScanMs = nowMs;
        if (pid == m_pid)
            return Settled(nowMs) ? AudiodgEvent::SteadyAgain : AudiodgEvent::Nothing;

        const DWORD was = m_pid;
        m_pid = pid;
        m_seenSinceMs = (pid != 0) ? nowMs : 0;

        // The usual shape of a real outage is pid -> 0 -> a different pid, and
        // it is ONE restart: the hold is armed when the process GOES, which is
        // the moment every endpoint starts reading absent. Coming back is
        // worth saying and must not re-arm the hold -- that would turn a
        // ceiling into a floor, and a route whose headset genuinely died would
        // sit unattended for the whole of it.
        if (was == 0) return AudiodgEvent::Appeared;

        ++m_restarts;
        return AudiodgEvent::Restarted;
    }

    // Present, and unchanged for long enough that the graph can be trusted.
    // Absence is never settled, however long it lasts: lifting the hold then
    // is exactly the mistake the hold exists to prevent.
    bool Settled(unsigned nowMs) const {
        return m_pid != 0 && m_seenSinceMs != 0 &&
               (nowMs - m_seenSinceMs) >= kSettleMs;
    }

    DWORD Pid() const { return m_pid; }
    unsigned Restarts() const { return m_restarts; }

private:
    // Every interval here is an unsigned difference, which is correct across
    // the 49-day GetTickCount wrap -- and this machine stays up for weeks.
    static const unsigned kScanIntervalMs = 2000;
    // How long recovery takes depends entirely on how it died, so the hold is
    // released once the engine has been present and unchanged for this long
    // rather than by serving out its ceiling.
    static const unsigned kSettleMs = 5000;

    DWORD m_pid = 0;
    unsigned m_restarts = 0;
    unsigned m_lastScanMs = 0;
    unsigned m_seenSinceMs = 0;
};

} // namespace mdxm
