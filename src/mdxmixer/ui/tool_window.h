#pragma once
/*
  ToolWindow — reusable base class for standalone tool windows on their own threads.
  Provides: thread + message pump, dark theme painting, pin button (always-on-top),
  font +/- buttons with cross-window sync, window size persistence, owner-draw rendering.

  Subclass to create specific windows (Hotkeys, and whatever comes after).

  PORTED from MDropDX12 (src/mDropDX12/tool_window.{h,cpp} at 29c4806d), at
  Shane's direction: "as we add new windows they will all have same themes and
  settings". Kept deliberately close to the original so fixes can still be read
  across between the two repositories. What changed, and only this:

    * Engine* becomes ToolHost*, the handful of things a tool window actually
      needs from its application — the parent window, the theme, the font size,
      where its geometry file lives. Measured first: 54 references to the
      engine in 3,900 lines, and all of them one of those four.
    * Theming routes through mdxmixer's own ui/theme.h rather than carrying a
      second copy of the dark-mode code. The palette is the one the user picked
      (Dracula), so a ported window matches the tabs beside it.
    * mdx12's preset navigation, shader-error viewer and Button Board action
      types are gone: they have nothing to act on here.
*/

#include <Windows.h>
#include <climits>
#include <deque>
#include <initializer_list>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include "app/hotkeys.h"    // HotkeyAction, HotkeyBinding

namespace mdxm {

// What a tool window needs from the application around it. Everything the
// ported code used to reach for through Engine*, and nothing else.
struct ToolHost {
    HWND  parent = nullptr;          // the main window: centring, and the owner
    std::wstring configDir;          // where windows.json lives
    // A CreateFontW HEIGHT, and NEGATIVE on purpose: a negative value is a
    // character height, a positive one is a cell height. MDropDX12 defaults
    // this to -20 and clamps it to -12 (smallest) .. -32 (largest), and its
    // +/- buttons -- ported verbatim, including those bounds -- only move it
    // inside that band.
    //
    // The port kept the field and lost the value: 9 is positive, so every
    // tool window opened at a 9-PIXEL CELL, which is the "tiny font" Shane
    // kept having to fix. Worse, 9 is outside the band the buttons enforce,
    // so pressing + made it smaller still.
    //
    // `onChanged` is how it survives: the buttons move this number, and the
    // host writes it back to config. Without it the fix would have to be
    // re-applied every single time the window opened.
    int   fontSize = -20;            // the +/- buttons move this, shared by all
    std::function<void(int)> onFontSizeChanged;
    bool  testing = false;           // suppress geometry writes under test
    // The live theme, or null while none is set. The same accessor the tabs
    // use, so a tool window cannot drift to a different palette.
    std::function<const struct ThemeState*()> theme;
};

// The messages a tool window posts to its own thread. mdx12 keeps these with
// the rest of its application messages; here they belong to the framework,
// since nothing outside it sends them.
enum {
    WM_MW_BRING_TO_TOP   = WM_APP + 60,
    WM_MW_REBUILD_FONTS  = WM_APP + 61,
    WM_MW_SHOW_TAB       = WM_APP + 62,
    WM_MW_SHOW_CTRL      = WM_APP + 63,
    WM_MW_CLICK_CTRL     = WM_APP + 64,
    WM_MW_MOVE_TO        = WM_APP + 65,
    WM_MW_RESET_WINDOW   = WM_APP + 66,
};

// The base controls every tool window carries: the pin, the font pair, and
// the button that shrinks the window back onto its contents. mdx12 keeps
// these with its several thousand other control ids; here the framework owns
// them, well above anything a subclass will use.
enum {
    IDC_MW_TOOLWIN_FIT = 10200,

    // The action edit dialog's own controls.
    IDC_AE_LABEL       = 10210,
    IDC_AE_ACTION_TYPE = 10211,
    IDC_AE_HOTKEY      = 10212,
    IDC_AE_CLEAR_KEY   = 10213,
    IDC_AE_CONFLICT    = 10214,
    IDC_AE_TARGETS     = 10215,
    IDC_AE_STEP        = 10216,
};

class ToolWindow {
protected:
  ToolHost*   m_pHost;
  HWND        m_hWnd = NULL;
  std::thread m_thread;
  std::atomic<bool> m_bThreadRunning{false};
  bool        m_bOnTop = false;
  bool        m_bWasMaximized = false;  // last WM_SIZE state, for relayout/persist
  HFONT       m_hFont = NULL;
  HFONT       m_hFontBold = NULL;
  HFONT       m_hPinFont = NULL;       // Segoe MDL2 Assets for pin icon
  // The same face, 10% smaller, for the other glyph buttons in the title row.
  // The refresh wheel fills its em box more than the pin does, so at one size
  // it reads as the taller of the two even though both are square.
  HFONT       m_hGlyphFont = NULL;
  int         m_nWndW, m_nWndH;        // current (persisted) size
  int         m_nDefaultW, m_nDefaultH; // default size if no INI
  // Persisted position. INT_MIN means "never saved" -- plain -1 cannot mean
  // that, because a monitor placed left of or above the primary one gives real
  // windows negative coordinates.
  int         m_nPosX = INT_MIN, m_nPosY = INT_MIN;
  std::vector<HWND> m_childCtrls;      // all child HWNDs (for rebuild + dark theme)
  std::vector<HWND> m_tooltips;        // owned popups; children teardown misses these
  // Backing store for AttachTip. TTM_ADDTOOLW keeps the POINTER it is handed
  // rather than copying the text, so a caller's temporary would leave the
  // tooltip reading freed memory. A deque and not a vector: pushing another
  // string must not move the ones already handed out.
  std::deque<std::wstring> m_tipText;
  bool        m_bFirstBuild = true;    // cleared once DoBuildControls has run

  // Anchored layout. m_anchorClient is the client size the rectangles were
  // measured at, so the deltas are always relative to the build, not to the
  // last resize -- rounding cannot accumulate across a hundred mouse-moves.
  struct AnchoredControl {
    HWND hwnd = NULL;
    unsigned edges = 0;
    RECT rect = {};
  };
  std::vector<AnchoredControl> m_anchors;
  SIZE m_anchorClient = { 0, 0 };

  // ── Tab control support (optional — used by tabbed subclasses) ──
  HWND        m_hTab = NULL;
  int         m_nActivePage = 0;
  std::vector<std::vector<HWND>> m_pageCtrls;  // per-page control tracking

  // ── Subclass must override these ──

  // Window identity
  virtual const wchar_t* GetWindowTitle() const = 0;
  virtual const wchar_t* GetWindowClass() const = 0;
  virtual const wchar_t* GetINISection() const = 0;

  // Control IDs for pin, font +/-
  virtual int GetPinControlID() const = 0;
  virtual int GetFontPlusControlID() const = 0;
  virtual int GetFontMinusControlID() const = 0;

  // Minimum resize dimensions
  virtual int GetMinWidth() const { return 400; }
  virtual int GetMinHeight() const { return 350; }

  // Extra WS_* bits for the frame (e.g. WS_MINIMIZEBOX | WS_MAXIMIZEBOX).
  virtual DWORD GetExtraWindowStyle() const { return 0; }

  // Tool-window frame: small caption, no taskbar button -- and Windows hides
  // the minimise/maximise buttons on one no matter what styles are set. A
  // window that wants those must return false here as well as asking for the
  // styles above.
  virtual bool UsesToolWindowFrame() const { return true; }

  // Called on WM_SIZE.
  //
  // A window that has anchored its controls is laid out; one that has not is
  // rebuilt from scratch, which is what every window used to do. See
  // AnchorControl for how a window moves from the second to the first.
  virtual void OnResize() {
    if (HasAnchors()) ApplyAnchors();
    else RebuildFonts();
  }

  // ICC flags for InitCommonControlsEx. Override to add ICC_LISTVIEW_CLASSES etc.
  virtual DWORD GetCommonControlFlags() const;

  // Whether window accepts drag-and-drop files (DragAcceptFiles)
  virtual bool AcceptsDragDrop() const { return false; }

  // Whether the message pump forwards ALL keyboard input to the render window
  // (not just F-keys and Ctrl/Alt combos).  Override to true for windows with
  // no text edits (e.g. Button Board) so the VJ can use hotkeys while the
  // window has focus.  Escape and Ctrl+Shift+F2 are always kept local.
  virtual bool ForwardAllKeys() const { return false; }

  // Called when Open() finds window already visible. Default: SetForegroundWindow.
  // Settings overrides to move off fullscreen monitor.
  virtual void OnAlreadyOpen();

  // Build all child controls (called after window creation and on rebuild)
  virtual void DoBuildControls() = 0;

  // Handle WM_COMMAND. Return 0 if handled, -1 if not.
  virtual LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) { return -1; }

  // Handle WM_HSCROLL slider changes. Return 0 if handled, -1 if not.
  virtual LRESULT DoHScroll(HWND hWnd, int id, int pos) { return -1; }

  // Handle WM_NOTIFY. Return 0 if handled, -1 if not.
  virtual LRESULT DoNotify(HWND hWnd, NMHDR* pnm) { return -1; }

  // Handle WM_CONTEXTMENU. x/y are screen coordinates. Return 0 if handled, -1 if not.
  virtual LRESULT DoContextMenu(HWND hWnd, int x, int y) { return -1; }

  // Catch-all for messages BaseWndProc doesn't handle (WM_TIMER, WM_DROPFILES, etc.)
  virtual LRESULT DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) { return -1; }

  // Called from WM_DESTROY before cleanup (subclass releases its resources)
  virtual void DoDestroy() {}

  // ── Preset loading from a tool window ──
  // Every tool window pumps messages on its own thread, so calling
  // Engine::LoadPreset() from a handler runs it off the render thread. That
  // races the render thread's LoadPresetTick() over m_presetLoadThread — two
  // threads joining or reassigning one std::thread calls std::terminate(),
  // which aborts the process (WER 0xC0000409 / FAST_FAIL_FATAL_APP_EXIT) with
  // no chance for any handler to log it. These marshal onto the render thread
  // instead; use them and never call LoadPreset/NextPreset/PrevPreset here.
  // nPresetIndex >= 0 loads by index; pass -1 with szPath to load a full path.
  void RequestLoadPreset(int nPresetIndex, const wchar_t* szPath = nullptr,
                         float fBlendTime = -1.0f);
  // +1 = next, -1 = previous. fBlendTime < 0 uses the user's configured
  // blend time (the default every existing caller relies on); pass an
  // explicit value (e.g. 0.0f) for a caller that needs an instant cut.
  void RequestNavPreset(int nDirection, float fBlendTime = -1.0f);

public:
  // Every tool window currently open. mdx12 kept this on the Engine; the font
  // +/- buttons walk it so one press moves every window, which is the only
  // reason it exists.
  static std::vector<ToolWindow*> s_open;
  static void BroadcastFontSync(HWND hSender);
  // Under test, geometry is never written: a run that opens windows must not
  // move where the user's windows live.
  static void SetTestingMode(bool on);

  ToolWindow(ToolHost* pHost, int defaultW, int defaultH);
  virtual ~ToolWindow();

  // Open the window (creates thread, or brings to front if already open)
  void Open();

  // Close the window and join the thread
  void Close();

  // Two-phase close: signal first, join later (for parallel shutdown)
  void SignalClose();
  void WaitClose();

  // Is the window currently open?
  bool IsOpen() const;

  // Get the HWND (may be NULL if not open)
  HWND GetHWND() const { return m_hWnd; }

  // Destroy all children and rebuild controls at the current font size
  virtual void RebuildFonts();

  // Reset to default size, centered on primary display
  void ResetPosition();

  // Apply dark theme to the window and all children
  void ApplyDarkTheme();


  // ── Anchored layout: resizing without rebuilding ──
  //
  // The rule is the one every GUI toolkit uses, and stating it plainly explains
  // every layout that follows:
  //
  //   anchored to ONE edge   -> the control keeps its distance from that edge,
  //                             so it MOVES as the window grows;
  //   anchored to BOTH edges -> it keeps its distance from both, so it
  //                             STRETCHES.
  //
  // A control that is never anchored simply stays where it was built, which is
  // what every control did before this existed -- so a window adopts anchoring
  // one control at a time, and a window that adopts none behaves exactly as it
  // always has.
  //
  // Once a window anchors anything, OnResize lays it out instead of destroying
  // and rebuilding it: no flicker, no lost text, no re-reading files, and one
  // DeferWindowPos batch instead of a teardown per mouse-move.
  enum ToolAnchor : unsigned {
    kAnchorLeft   = 1u << 0,
    kAnchorTop    = 1u << 1,
    kAnchorRight  = 1u << 2,
    kAnchorBottom = 1u << 3,

    kAnchorTopLeft       = kAnchorLeft | kAnchorTop,      // stay put (the default)
    kAnchorTopRight      = kAnchorTop | kAnchorRight,     // slide with the right edge
    kAnchorBottomLeft    = kAnchorLeft | kAnchorBottom,   // slide with the bottom
    kAnchorBottomRight   = kAnchorRight | kAnchorBottom,
    kAnchorStretchWide   = kAnchorLeft | kAnchorTop | kAnchorRight,
    kAnchorStretchBottom = kAnchorLeft | kAnchorRight | kAnchorBottom,
    kAnchorFill          = kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom,
  };

  // Register a control with the layout. Call it in DoBuildControls, straight
  // after creating the control: the rectangle it has at that moment, and the
  // client size at that moment, are what the anchors are measured against.
  void AnchorControl(HWND h, unsigned edges);

  // Move this window back onto the render window's monitor if it has ended up
  // elsewhere. Only acts in testing mode, and only for a window that places
  // itself there (PlacesItselfInTestingMode).
  //
  // Placement is decided once, when the window opens, and at startup that can
  // be too early: the render window has been created but has not necessarily
  // reached its final display yet, so "the render monitor" is answered
  // correctly and becomes wrong a moment later. The window then sits on the
  // display the render window was PASSING THROUGH. Re-checking costs two
  // MonitorFrom calls a second and is the only thing that does not depend on
  // knowing the exact startup ordering.
  //
  // Outside testing mode this does nothing, so it can never fight a window the
  // user deliberately dragged to another screen.
  void EnsureOnRenderMonitor();

  // Track and anchor in one call -- the usual form:
  //     TrackAnchored(CreateSlider(...), kAnchorStretchWide);
  HWND TrackAnchored(HWND h, unsigned edges) {
    TrackControl(h);
    AnchorControl(h, edges);
    return h;
  }

  // Enable or disable every control this window has built.
  //
  // For a window with a master switch: with the subsystem off there is nothing
  // behind its controls, and offering them anyway means clicks that quietly do
  // nothing. `keep` names the ids that stay live -- normally just the switch
  // itself.
  //
  // The window's own furniture is always exempt: the pin, the two font
  // buttons and the tab strip. Being able to look at the other tabs before
  // turning something on is the point of having tabs.
  //
  // Owner-drawn controls are painted by HandleDarkDrawItem, which honours
  // ODS_DISABLED, and disabled labels are dimmed in HandleDarkCtlColor -- so a
  // control that ignores clicks also looks like it does.
  void SetControlsEnabled(bool enable, std::initializer_list<int> keep = {});

  // Put WS_TABSTOP children in reading order (top-to-bottom, left-to-right).
  //
  // IsDialogMessage walks Z-order. Controls are created in whatever order the
  // layout code happens to, and ShowPage used to HWND_TOP each page control
  // in that order -- which left Tab running bottom-to-top. Call after a
  // build, a page switch, or anything that restacks children.
  void SortTabOrder();

  // Show a particular page, whether or not the window is open yet.
  //
  // The page is written to the INI first, so a window that has not been built
  // lands on it when SelectInitialTab runs; a window that IS open is told
  // through a posted message, because tool windows each run on their own
  // thread and switching a tab touches its controls.
  //
  // On the base class deliberately: "here is the setting you want, in the
  // window that owns it" is a link any tool window may want to offer, not a
  // one-off.
  void ShowTab(int page);

  // Reveal one control and put the caret on it, wherever it lives.
  //
  // The caller does not have to know which tab holds it: the page is found by
  // looking the control up in the per-page lists, switched to, and only then
  // focused. That is what makes "take me to that setting" answerable from a
  // remote, which cannot see the window at all.
  void ShowControl(int ctrlId);

  // Press a control, as a click would. Answers the same way ShowControl does.
  void ClickControl(int ctrlId);

  // Move the window. `centreOnRender` puts it in the middle of whichever
  // display the visualiser is on, which is the one thing a caller cannot work
  // out for itself -- it does not know the window's size or where the render
  // window went.
  void MoveTo(int x, int y);
  void CentreOnRender();

  // Top-left for a window centred on the render window's display.
  void RenderCentredPos(int& outX, int& outY) const;

 private:
  // A control asked for before the window had been built.
  //
  // A tool window is created on its OWN thread, so for a moment after Open()
  // returns it has no controls at all. Waiting for it from the calling thread
  // was the first attempt and it was fragile -- the Settings window takes
  // longer to build than any wait worth holding an IPC thread for, and a
  // request for a control it certainly has came back "unknown". The intent is
  // remembered instead and applied the moment the window is ready, and the
  // WINDOW reports what happened rather than the caller guessing.
  int m_pendingShowCtrl = 0;

  // This window was positioned by testing mode, so its position is disposable
  // and must never be saved -- not even after testing mode has ended.
  // Testing mode normally moves every tool window to the middle of the render
  // window's monitor and throws that placement away afterwards, because a test
  // must not repossess the user's layout. A window can opt out:
  //
  //   * if it was OPEN when the app was last used, its own position is kept --
  //     read, never written, so the arrangement survives the run untouched;
  //   * otherwise it opens in the LOWER CORNER of the render display rather
  //     than over the middle of it, so it never covers the frame being
  //     measured.
  //
  // Only the Remote window wants this today. Everything else still centres,
  // which is what a window a test opens on purpose should do.
  virtual bool PlacesItselfInTestingMode() const { return false; }

  bool m_bPlacedByTestingMode = false;
  // Was this window open when the app was last used? Persisted as WasOpen,
  // written on create and cleared on an explicit close -- nothing closes tool
  // windows when the program exits, so a window still open at shutdown leaves
  // the flag set, which is exactly the question being asked.
  bool m_bWasOpenLastSession = false;

  // Broadcast the outcome of a UI_SHOW request, from the window's own thread
  // once it has actually acted.
  void ReportUiShown(int ctrlId, bool found);
  void ReportUiClicked(int ctrlId, bool pressed);

 public:


  // Has this window anchored anything? OnResize uses it to decide between
  // laying out and rebuilding.
  bool HasAnchors() const { return !m_anchors.empty(); }

  // Move every anchored control to suit the current client size.
  void ApplyAnchors();

  // Compute line height from current font
  int GetLineHeight();

  // The line height a given font size produces, measured WITHOUT a window.
  //
  // GetLineHeight needs m_hWnd and m_hFont, and the size a window opens at is
  // decided before either exists -- which is precisely where the font-blindness
  // of issue 157 lives. Measured on a screen DC rather than guessed: the font
  // is created with an explicit cell height, so the answer does not depend on
  // which monitor's DC it is taken from.
  static int LineHeightForFontSize(int nFontSize);

  // GetMinWidth/GetMinHeight scaled to the font in use.
  //
  // The declared minimum of every tool window is a constant chosen against a
  // line height of 26, the same baseline every MulDiv(n, lineH, 26) in the
  // layouts is relative to. At a larger font the layout grows and the minimum
  // does not, so the "minimum" stops meaning what it says -- a window at it has
  // controls outside itself. Scaling keeps the promise the number was making.
  int ScaledMinWidth();
  int ScaledMinHeight();

  // Helper for subclasses to track child controls for dark theme + rebuild
  void TrackControl(HWND h) { if (h) m_childCtrls.push_back(h); }

  // Read owner-draw checkbox/radio state. Use instead of IsDlgButtonChecked()
  // which does NOT work with BS_OWNERDRAW controls (always returns 0).
  bool IsChecked(int controlID) const;

  // Set owner-draw checkbox/radio state. Use instead of CheckDlgButton()
  // which does NOT work with BS_OWNERDRAW controls (silently fails).
  void SetChecked(int controlID, bool checked);

  // Track control on a specific tab page (adds to m_pageCtrls[page] + m_childCtrls)
  void TrackPageControl(int page, HWND h);

  // Points the window-geometry store at the resources directory and
  // moves any [*Wnd] sections still in settings.ini into it. Called
  // once at startup, before any tool window reads its position.
  static void InitWindowStore(const wchar_t* resourceDir);

  // Resize so every control fits at the current font size. Bound to the
  // refresh button beside the pin, which every tool window now has. Mainly for
  // a window that has been shrunk by accident: its controls are still laid out
  // past the client edge, so the fit finds them and grows back to them.
  //
  // Pressed by hand, so it also restores the window's DESIGNED size, scaled to
  // the font: the button means "put this window right", not merely "stop
  // cutting things off".
  void FitToContents();

  // The same measurement, run once when the window opens, growing ONLY what is
  // actually hanging outside the client edge (issue 157).
  //
  // A tool window's size comes either from what the user last left it at or
  // from a default constant, and neither knows anything about the font. The
  // layouts do: every one of them is built from MulDiv(n, lineH, 26), so they
  // grow with the font while the frame does not. At a large font four windows
  // opened with controls past their own edge, and one of them -- Controller --
  // put its only three buttons below the client bottom, where they cannot be
  // clicked at all. A window that comes up unusable cannot be repaired with a
  // button inside it.
  //
  // Deliberately NOT the button's behaviour. This is a repair and nothing
  // more: a dimension moves only when something overflows it, so a window the
  // user has deliberately made small is left exactly as it is.
  void FitClippedContentsOnOpen();

  // Both of the above. bRestoreDesignedSize is the only difference.
  void FitContents(bool bRestoreDesignedSize);

  // Create TCS_OWNERDRAWFIXED tab control with dark theme subclass.
  // Returns the content area rect (below tab headers).
  RECT BuildTabControl(int tabCtrlID, const wchar_t* const* tabNames, int numPages,
                       int x, int y, int w, int h);

  // Show/hide page controls + persist active tab to INI
  void ShowPage(int page);

  // Restore persisted active tab from INI (call at end of DoBuildControls)
  void SelectInitialTab();

  // Access fonts for control creation
  HFONT GetFont() const { return m_hFont; }
  HFONT GetFontBold() const { return m_hFontBold; }

  // ── Rebuilding without taking the user's work with it ──
  //
  // RebuildFonts destroys every control and calls DoBuildControls again. That
  // is how a font-size change is applied, and how most windows handle a resize.
  // It used to throw away whatever the user was in the middle of: text typed
  // into an edit, the row selected in a list, how far a list was scrolled,
  // which control had focus. Resizing the Shader Editor replaced pasted GLSL
  // with the text the shader pass held when the window opened; resizing the
  // Controller window reverted the JSON to the last saved copy.
  //
  // CaptureControlState reads that back out of the controls before they are
  // destroyed, keyed by control ID, and RestoreControlState puts it back
  // afterwards. Generic on purpose -- every window gets it without having to
  // remember, and a window added tomorrow gets it too.
  //
  // Text is only restored when the user actually typed it (EM_GETMODIFY), or
  // when the rebuilt control came back empty and we had something. That
  // matters: plenty of edits are refreshed from live state by DoBuildControls,
  // and putting the old text back over a deliberate refresh would be its own
  // bug.
  struct ControlState {
    int id = 0;
    std::wstring cls;      // only restored onto a control of the same class
    std::wstring text;
    bool hasText = false;
    bool userModified = false;        // EM_GETMODIFY: the user typed this
    int selStart = -1, selEnd = -1;   // caret / selection inside an edit
    int selection = -1;               // chosen row in a list or combo
    int topIndex = -1;                // first visible row, i.e. scroll position
    bool hadFocus = false;
  };
  std::vector<ControlState> CaptureControlState() const;

  // Restoring happens in two steps with OnRebuilt() between them, and the order
  // is the point:
  //   1. put the list selections and scroll positions back;
  //   2. let the window bring its own members into line with them (OnRebuilt);
  //   3. put the user's unsaved typing back LAST, so a window refreshing its
  //      edits from the newly-selected row cannot overwrite it.
  void RestoreControlSelections(const std::vector<ControlState>& saved);
  void RestoreControlText(const std::vector<ControlState>& saved);
  HWND MatchingControl(const ControlState& s) const;

  // Called after a rebuild has recreated, themed and restored the controls.
  // Override when the window keeps state in MEMBERS that DoBuildControls
  // resets -- restoring the control is not enough if the window also has to
  // agree with it about which row is selected.
  virtual void OnRebuilt() {}

  // Called after ShowPage has made a page visible, tab switch included.
  //
  // Override when a page holds controls that must NOT all be shown together --
  // two views sharing one rectangle, say. ShowPage shows every control it was
  // given, so a window that hides one of them has to say so again afterwards.
  virtual void OnPageShown(int /*page*/) {}

  // Register an owned tooltip so a rebuild destroys it.
  //
  // Tooltips are created WS_POPUP with the tool window as their OWNER, not as
  // children, so the GW_CHILD teardown in RebuildFonts never saw them: every
  // rebuild leaked one window per tooltip, and BuildBaseControls makes one for
  // the pin button on all 25 tool windows.
  void TrackTooltip(HWND h) { if (h) m_tooltips.push_back(h); }

  // Give one control a tooltip, and own the text.
  //
  // Shared because there were six hand-rolled copies of the same eleven lines,
  // and because issue 159 is about to need many more: a label shortened to fit
  // its box keeps its full wording here rather than losing it -- Shane, asked
  // how the detail should survive the shortening, answered "Maybe tooltips".
  //
  // One tooltip control per control, deliberately. They used to be shared, and
  // in the Presets window that meant moving the Copy button took the Edit
  // button's tooltip with it. TrackTooltip owns the teardown either way.
  void AttachTip(HWND hCtrl, const wchar_t* tip);
  void AttachTip(HWND hCtrl, const std::wstring& tip) { AttachTip(hCtrl, tip.c_str()); }

  // A label, with its full wording as a tooltip (issue 159).
  //
  // The pairing is the point: the short form is what fits, the long form is
  // what it means, and neither is any use without the other.
  HWND CreateLabelTip(HWND hw, const wchar_t* text, const wchar_t* tip,
                      int x, int y, int w, int h, HFONT hFont);

  // True while the window is being built for the FIRST time. Windows whose
  // DoBuildControls loads a document from disk (Sprites re-reads sprites.ini)
  // must only do that on the first build; on a rebuild it would discard
  // everything the user has changed but not yet saved.
  bool IsFirstBuild() const { return m_bFirstBuild; }

  // Create a report-mode ListView with standard styles. Does NOT call TrackControl().
  // When sortable=true, column headers are clickable (omits LVS_NOSORTHEADER).
  HWND CreateThemedListView(int id, int x, int y, int w, int h,
                            bool visible = true, bool sortable = false);

  // Common control setup: creates fonts, font +/- buttons, pin button with tooltip.
  // Returns the Y position below the header row for subclasses to continue from.
  // Populates lineH, gap, x, rw, clientW for the caller.
  struct BaseLayout { int y, lineH, gap, x, rw, clientW; };
  BaseLayout BuildBaseControls();
  // Re-anchors the right-edge base controls (the pin) after a resize. Called
  // from WM_SIZE before OnResize, so subclasses never have to know about it.
  void LayoutBaseControls();

private:
  void CreateOnThread();
  void LoadWindowPosition();
  void SaveWindowPosition();
  void SaveOpenState(bool open);

  // Pull a saved position back onto a monitor that is actually attached.
  // Displays get unplugged and desktops get rearranged; without this the
  // window opens at coordinates that no longer belong to any screen.
  static void ClampToVisibleMonitor(int& posX, int& posY, int w, int h);

  // The single shared WndProc dispatches to virtual methods
  // The registered procedure is a thin SEH guard; the real one is Impl.
  // Separate functions because __try cannot share a frame with C++ unwinding.
  // See seh_guard.h -- a fault here is otherwise unreportable and fatal.
  static LRESULT CALLBACK BaseWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK BaseWndProcImpl(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  static const wchar_t* SehContextFor(HWND hWnd, UINT uMsg);


  // Tab control dark background subclass (shared by all tabbed windows)
  static LRESULT CALLBACK TabSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};

// ── Macro to eliminate boilerplate overrides in ToolWindow subclasses ──
// Each subclass needs: title, window class, INI section, 3 control IDs, min size.
// Usage: place inside the `protected:` section of the subclass declaration.
#define TOOLWINDOW_META(title, cls, ini, pinID, fpID, fmID, minW, minH) \
  const wchar_t* GetWindowTitle() const override { return title; }     \
  const wchar_t* GetWindowClass() const override { return cls; }       \
  const wchar_t* GetINISection() const override  { return ini; }       \
  int GetPinControlID() const override       { return pinID; }         \
  int GetFontPlusControlID() const override  { return fpID; }          \
  int GetFontMinusControlID() const override { return fmID; }          \
  int GetMinWidth() const override  { return minW; }                   \
  int GetMinHeight() const override { return minH; }


// ── ModalDialog — lightweight base class for modal popup dialogs ──────
// No thread, no INI persistence, no pin/font buttons.  Shares the same
// dark theme helpers as ToolWindow so popups get correct theming for free.

class ModalDialog {
protected:
    ToolHost*   m_pHost;
    HWND        m_hWnd = NULL;
    HWND        m_hParent = NULL;
    HFONT       m_hFont = NULL;
    std::vector<HWND> m_childCtrls;
    bool        m_bDone = false;
    bool        m_bResult = false;

    virtual const wchar_t* GetDialogTitle() const = 0;
    virtual const wchar_t* GetDialogClass() const = 0;
    virtual void DoBuildControls(int clientW, int clientH) = 0;
    virtual LRESULT DoCommand(int id, int code, LPARAM lParam) { return -1; }
    virtual LRESULT DoNotify(NMHDR* pnm) { return -1; }
    virtual LRESULT DoMessage(UINT msg, WPARAM wParam, LPARAM lParam) { return -1; }

    // Layout metrics — computed from actual font, consistent with ToolWindow
    struct BaseLayout { int lineH, gap, margin, labelW; };
    BaseLayout GetBaseLayout();

    // Resize window to fit content height (call at end of DoBuildControls)
    void FitToContent(int clientW, int contentH);

public:
    ModalDialog(ToolHost* pHost) : m_pHost(pHost) {}
    virtual ~ModalDialog() {}

    bool Show(HWND hParent, int clientW, int clientH);
    void EndDialog(bool result) { m_bResult = result; m_bDone = true; }
    void TrackControl(HWND h) { if (h) m_childCtrls.push_back(h); }
    bool IsChecked(int id) const;
    void SetChecked(int id, bool checked);
    int  GetLineHeight();
    HFONT GetFont() const { return m_hFont; }
    HWND GetHWND() const { return m_hWnd; }

private:
    // A thin SEH guard; the body is Impl. See seh_guard.h -- a fault inside
    // a window procedure is otherwise fatal and leaves no trace at all.
    static LRESULT CALLBACK ModalWndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ModalWndProcImpl(HWND, UINT, WPARAM, LPARAM);
};

// ── Ask the user to name something ────────────────────────────────────
// Returns false if cancelled. `text` carries the initial value in and the
// entered value out. Replaces GetSaveFileNameW wherever a thing is named
// rather than saved to a file of its own.
//
// `choices` are the names that already exist. They are offered in a dropdown
// the user can also type into: a save box that only accepts a fresh name makes
// overwriting one mean retyping it exactly, which is both tedious and how
// duplicates-by-typo get made.
bool PromptForName(ToolHost* pHost, HWND hParent, const wchar_t* title,
                   const wchar_t* prompt, std::wstring& text, size_t maxLen,
                   const std::vector<std::wstring>& choices);

// ── Clipboard ─────────────────────────────────────────────────────────
// Retries: another process can hold the clipboard for a few milliseconds and
// OpenClipboard fails rather than waiting.
bool CopyTextToClipboard(HWND owner, const wchar_t* text);

// The palette, read through the host so a tool window matches the tabs.
bool     HostIsDark(const ToolHost* h);
COLORREF HostBg(const ToolHost* h);
COLORREF HostText(const ToolHost* h);
COLORREF HostSurface(const ToolHost* h);
COLORREF HostAccent(const ToolHost* h);
COLORREF HostBorder(const ToolHost* h);

// ── Shared dark theme helpers ─────────────────────────────────────────
// Used by both ToolWindow::BaseWndProc and ModalDialog::ModalWndProc
// to avoid duplicating theme painting across popup dialogs.

// Handle WM_CTLCOLOREDIT/LISTBOX/STATIC/BTN/DLG. Returns brush LRESULT if dark, 0 if not.
LRESULT HandleDarkCtlColor(ToolHost* p, UINT msg, WPARAM wParam, LPARAM lParam);

// Handle WM_DRAWITEM for ODT_TAB, ODT_BUTTON (checkbox/radio/button), ODT_STATIC (swatch).
// Does NOT handle pin button (ToolWindow-specific). Returns TRUE if painted, FALSE if not.
LRESULT HandleDarkDrawItem(ToolHost* p, DRAWITEMSTRUCT* pDIS);

// Handle WM_ERASEBKGND — fills with dark or light bg. Returns 1.
LRESULT HandleDarkEraseBkgnd(ToolHost* p, HWND hWnd, HDC hdc);

// Apply DWM dark mode attributes to a window (title bar, border, caption color).
void ApplyDarkThemeToWindow(ToolHost* p, HWND hWnd);

// Apply SetWindowTheme to tracked child controls (tab, listview, hotkey, etc).
void ApplyDarkThemeToChildren(ToolHost* p, const std::vector<HWND>& ctrls);

// Dark tab background subclass — apply to tab controls for dark theme support.
LRESULT CALLBACK DarkTabSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

// Paint a ListView header in dark theme via NM_CUSTOMDRAW.
// Returns LRESULT to return from WndProc; sets *pHandled=true if the notification was handled.
LRESULT PaintDarkListViewHeader(NMHDR* pnm, LPARAM lParam, HWND hListView,
                                COLORREF colBg, COLORREF colBorder, COLORREF colText,
                                bool* pHandled);

// ── Shared Action Edit Dialog ────────────────────────────────────────────
//
// mdx12 shares this dialog between its Button Board and its Hotkeys window, so
// it carries an action type, a label, a payload and both a local and a global
// binding. mdxmixer has no button board and no local bindings — a mixer key
// that only worked while the mixer had focus would be no use at all — so what
// is left is the global half: what the key does, what it does it to, and which
// combination asks for it.
struct ActionEditData {
    std::wstring label;
    HotkeyAction action = HotkeyAction::VolumeUp;
    unsigned     mod = 0;       // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    unsigned     vk  = 0;       // 0 = unbound, which is how everything ships
    // How far one press moves a fader. 0 means "use the default", and the
    // dialog shows the default in grey so the box is answerable without
    // going to look it up.
    int          stepPercent = 0;
    int          defaultStepPercent = 5;   // only to label the empty case

    // Every fader that exists, and which of them this key moves. Zero targets
    // is allowed: Sonar lets a key address none, one or several channels, and
    // a key with none says so rather than quietly doing nothing.
    std::vector<std::pair<std::wstring, std::wstring>> allTargets;  // key, label
    std::vector<std::wstring> targets;

    // The binding being edited, so the live conflict line does not report it
    // clashing with itself. Empty when adding a new one.
    std::wstring selfId;
    // The other bindings, for that same conflict line.
    std::vector<HotkeyBinding> others;

    ToolHost* pHost = nullptr;
    bool accepted = false;
};

// Show the action edit dialog. Returns true if the user pressed OK.
bool ShowActionEditDialog(HWND hParent, ActionEditData& data);

} // namespace mdxm
