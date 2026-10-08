// test_topmost.cpp — who gets the front of the always-on-top band.
//
// "when mdx12 is running in watermark mode like now and mdxmixer is running
// like now, it seems that the overlay gets lost."
//
// WS_EX_TOPMOST is not a ranking: every topmost window shares one band and the
// last one to ask sits at its front. MDropDX12 re-asserts its watermarked
// displays on a timer -- it had this same bug against KMPlayer and fixed it
// that way -- so anything that asks once and never again ends up underneath
// for good. mdxmixer's battery overlay asked once, at create time.
//
// The rule Shane chose is not "ask harder": "you can have it be one layer
// below the mdx12 layer if mdx12 is topmost ... that way doesn't fight with
// mdx12 just follows along." Two timers racing for the front of one band is a
// fight neither wins and the user watches flicker.
#include "test_framework.h"
#include "ui/topmost.h"

using namespace mdxm;

// ── the ordinary cases ───────────────────────────────────────────────────

MDXM_TEST_CASE(Topmost_NothingToDoWhenUpThereAndClear) {
    // Asking again every second when nothing is above is how a program ends
    // up fighting the compositor for ever.
    CHECK(PlanTopmost(/*inBand*/ true, /*covered*/ false, false, false) ==
          TopmostAction::None);
}

// A window that is NOT in the band is promoted whatever is above it, and this
// is the case that matters most: MDropDX12 learned (its #408) that looking for
// a covering window first and skipping when there is none reads as "nothing
// above us, nothing to fix" and is exactly wrong here. A click-through overlay
// is REFUSED promotion on every attempt, so that gate hid it for good whenever
// the screen happened to be clear.
MDXM_TEST_CASE(Topmost_OutOfTheBandIsAlwaysWorthFixing) {
    CHECK(PlanTopmost(false, false, false, false) == TopmostAction::FrontOfBand);
    CHECK(PlanTopmost(false, true, false, false) == TopmostAction::FrontOfBand);
}

MDXM_TEST_CASE(Topmost_RaisesOverAnotherTopmostAppThatIsNotInUse) {
    // Some other program asked once and landed on top of us. That one we can
    // out-rank without taking anything from anybody.
    CHECK(PlanTopmost(true, true, /*foreground*/ false, /*followed*/ false) ==
          TopmostAction::FrontOfBand);
}

// ── the two windows we never take the screen from ────────────────────────

MDXM_TEST_CASE(Topmost_NeverStealsFromTheForegroundWindow) {
    // An app the user is actually working in has earned the front; re-raising
    // over it is worse than being covered.
    CHECK(PlanTopmost(true, true, /*foreground*/ true, false) == TopmostAction::None);
}

MDXM_TEST_CASE(Topmost_FollowsMdx12RatherThanFightingIt) {
    CHECK(PlanTopmost(true, true, false, /*followed*/ true) == TopmostAction::None);
}

// Out of the band AND mdx12 is over us: we still have to get up here, but we
// arrive directly beneath it instead of on top of it. Placing a window after a
// topmost one puts it in the band too, so this is one call, not two.
MDXM_TEST_CASE(Topmost_EntersTheBandUnderMdx12NotOverIt) {
    CHECK(PlanTopmost(/*inBand*/ false, /*covered*/ true, false, /*followed*/ true) ==
          TopmostAction::BelowFollowed);
}

// Both at once: the foreground window IS mdx12's watermark. Still nothing to
// take, and the answer must not depend on which test is written first.
MDXM_TEST_CASE(Topmost_ForegroundAndFollowedAgree) {
    CHECK(PlanTopmost(true, true, true, true) == TopmostAction::None);
}

// ── reading the overlay through the watermark ────────────────────────────
//
// Following along costs something: mdx12's mirror is layered at alpha 76 of
// 255 -- about 30% opaque, measured on the live window -- and it covers the
// whole monitor the overlay sits on, so the overlay underneath is dimmed.
// "increase opacity by 30% when ... mdx12 is overlaid and opacity is between
// 40 and 80%", and then exactly: "not +30 but 30% of 40%-80%".

MDXM_TEST_CASE(WatermarkOpacity_IsThirtyPercentOfItselfNotThirtyPoints) {
    CHECK(BoostedOpacityUnderWatermark(40) == 52);    // not 70
    CHECK(BoostedOpacityUnderWatermark(60) == 78);    // not 90
    CHECK(BoostedOpacityUnderWatermark(50) == 65);
}

MDXM_TEST_CASE(WatermarkOpacity_ClampsAtFullyOpaque) {
    CHECK(BoostedOpacityUnderWatermark(80) == 100);   // 104, and there is no such opacity
    CHECK(BoostedOpacityUnderWatermark(90) == 100);   // the top of the band
}

// THE CASE THE FEATURE WAS ASKED FOR. The overlay on this machine is set to
// 85, which the first band (40-80) excluded -- so the whole thing did nothing
// where it was wanted: "okay the apative work between 40 and 90%".
MDXM_TEST_CASE(WatermarkOpacity_CoversTheSettingThisMachineUses) {
    CHECK(BoostedOpacityUnderWatermark(85) == 100);
}

MDXM_TEST_CASE(WatermarkOpacity_LeavesTheBandEdgesAlone) {
    // Below 40 the overlay is deliberately faint and lifting it would undo
    // the setting; above 90 there is nothing meaningful left to add.
    CHECK(BoostedOpacityUnderWatermark(39) == 39);
    CHECK(BoostedOpacityUnderWatermark(91) == 91);
    CHECK(BoostedOpacityUnderWatermark(10) == 10);
    CHECK(BoostedOpacityUnderWatermark(100) == 100);
}
