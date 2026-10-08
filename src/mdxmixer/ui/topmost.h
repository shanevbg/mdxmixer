#pragma once
// Keeping a window that asked to be on top actually on top.
//
// PORTED from MDropDX12's Engine::ReassertMirrorTopmost (its #403, with the
// promotion detail from #408). Shane hit the same thing from the other side:
// "when mdx12 is running in watermark mode like now and mdxmixer is running
// like now, it seems that the overlay gets lost".
//
// WS_EX_TOPMOST IS NOT A RANKING. Every topmost window shares one band, and
// the last one to call SetWindowPos(HWND_TOPMOST) sits at its front. So an
// application that re-asserts itself whenever it is activated -- which mdx12's
// watermarked displays do, by design, because they had this exact bug against
// KMPlayer -- ends up permanently above anything that asked once and never
// asked again. mdxmixer's battery overlay asks once, when its styles are
// applied, and then never again. Hence: lost.
//
// Asking again on a timer is the whole fix, with the rule mdx12 arrived at:
// never take the screen from the window the user is actually working in.
#include <windows.h>

namespace mdxm {

enum class TopmostAction {
    None,            // already where it should be
    FrontOfBand,     // SetWindowPos(HWND_TOPMOST)
    BelowFollowed,   // SetWindowPos(hwnd, <the followed window>) -- in the band, under it
};

// What a window that wants to be on top should do right now.
//
// Pure, so the cases can be stated as tests rather than discovered on a screen
// with two programs fighting over it:
//
//   * NOT in the band at all -- get into it. mdx12 learned this the hard way
//     (#408): it used to look for a covering window first and skip when there
//     was none, which reads as "nothing above us, nothing to fix" and is
//     exactly wrong for a window whose promotion was REFUSED. A click-through
//     overlay is refused on every attempt, so that gate hid it for good
//     whenever the screen happened to be clear.
//   * in the band and nothing covering it -- nothing to do. Doing something
//     anyway means fighting the compositor once a second, for ever.
//   * covered by the FOREGROUND application -- leave it. An app the user is
//     working in has earned the front.
//   * covered by a FOLLOWED application -- leave it, and when we do have to
//     move, take the slot immediately beneath it. "you can have it be one
//     layer below the mdx12 layer if mdx12 is topmost ... that way doesn't
//     fight with mdx12 just follows along". mdx12 re-asserts its watermarked
//     displays on its own timer by design, so two timers racing for the front
//     of one band is a fight neither wins and the user watches flicker.
inline TopmostAction PlanTopmost(bool inBand, bool covered,
                                 bool coveredByForeground, bool coveredByFollowed) {
    if (!inBand) {
        // Getting INTO the band is not taking the front from anyone, so it
        // happens whoever is above -- but if the one above is followed, enter
        // directly beneath it rather than over it.
        return (covered && coveredByFollowed) ? TopmostAction::BelowFollowed
                                              : TopmostAction::FrontOfBand;
    }
    if (!covered) return TopmostAction::None;
    if (coveredByForeground || coveredByFollowed) return TopmostAction::None;
    return TopmostAction::FrontOfBand;
}

// Another of Shane's programs, whose topmost windows this one gets out of the
// way of rather than competing with.
//
// Only MDropDX12, and only because it is the one program on this machine that
// deliberately re-asserts its own topmost windows on a timer -- its watermarked
// displays, which exist to sit over everything. Anything else that ends up
// above the overlay is either the foreground app (already exempt) or something
// that asked once, which this can out-rank harmlessly.
bool IsFollowedTopmostApp(HWND hwnd);

// Is a followed window (mdx12's watermark) sitting over this one right now?
//
// Separate from ReassertTopmost because the answer is worth something on its
// own: a window under the watermark is being seen through a 30%-opaque layer
// and may want to compensate.
bool CoveredByFollowedTopmost(HWND hwnd);

// The opacity to use while the watermark is over us.
//
// "increase opacity by 30% when ... mdx12 is overlaid", then precisely: "not
// +30 but 30% of 40%-80%", then wider: "the apative work between 40 and 90%".
// So it is thirty percent MORE of whatever it was, not thirty points more, for
// any setting from 40 to 90 -- 40 becomes 52, 60 becomes 78, and anything from
// 77 up lands on 100.
//
// THE CEILING MATTERS MORE THAN IT LOOKS. At 80 this did nothing for the
// overlay actually on this machine, which is set to 85; at 90 it takes that
// 85 to fully opaque while the watermark is over it. That is the case the
// feature was asked for.
//
// WHY A BAND rather than always. Below 40 the overlay is deliberately faint
// and brightening it would undo the setting; above 90 it is solid enough that
// there is nothing meaningful to add. In between is where being read through a
// translucent watermark actually costs legibility.
//
// Measured on this machine: MDropDX12's watermarked mirror is layered at
// alpha 76 of 255, about 30% opaque and click-through, covering the whole
// 2560x1440 monitor the overlay sits on. So the overlay is not hidden by it --
// it is dimmed by it, which is exactly what this compensates for.
inline int BoostedOpacityUnderWatermark(int percent) {
    if (percent < 40 || percent > 90) return percent;
    const int boosted = (percent * 130 + 50) / 100;   // +30% of itself, rounded
    return boosted > 100 ? 100 : boosted;
}

// Put `hwnd` back at the front of the topmost band if it has been pushed down.
//
// Cheap enough for a one-second tick: the search walks forward from this
// window and stops at the first topmost window that overlaps it, which is a
// handful of iterations rather than an enumeration of the desktop, and the
// common answer is "nothing to do".
//
// Windows belonging to THIS process are ignored when looking for a cover: the
// mixer's own pinned window sitting over its own overlay is the user's
// arrangement, not a theft.
//
// `adaptive` is the user's "MDx12 adaptive overlay" switch. With it off,
// MDropDX12 is treated as any other topmost application -- out-ranked when
// it covers us and is not in the foreground -- which is what someone who
// wants this window above everything, watermark included, is asking for.
void ReassertTopmost(HWND hwnd, bool adaptive = true);

} // namespace mdxm
