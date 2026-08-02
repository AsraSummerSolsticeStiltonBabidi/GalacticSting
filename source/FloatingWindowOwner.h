#pragma once

//==============================================================================
// OS-level window OWNERSHIP for the editor's floating tool windows (the
// per-partial amplitude editor and the preset playground).
//
// THE BUG THIS PREVENTS: both floating windows are plain sibling top-level
// windows, so on Windows the first click on the main Fiblo window raises it
// ABOVE the window the user just opened - the pad or the knob grid vanishes
// behind the editor mid-gesture. The honest fix is the OS's own concept for
// this: an OWNED window (WS_POPUP-style ownership, set via GWLP_HWNDPARENT)
// always stays above its owner in Z-order, follows it on minimise, and - unlike
// setAlwaysOnTop(true) - never floats above OTHER applications.
//
// JUCE has no API for ownership-after-creation (addToDesktop's parent argument
// makes a CLIPPED child, not an owned sibling), so this talks to Win32
// directly. It lives in its OWN translation unit because <windows.h> defines
// macros (DrawText, small, min/max...) that must never leak into the editor's
// 7000-line .cpp.
//==============================================================================
namespace floatwin
{
    // Make the floating window an OS-owned window of the TOP-LEVEL window that
    // contains the editor (in a VST3 host the editor's peer is a child embedded
    // deep in the host's hierarchy, so the owner is its GA_ROOT ancestor; in the
    // Standalone it is the app window itself). Idempotent: re-applying the same
    // owner is a no-op, and a CHANGED root (a host that re-docks the editor into
    // another window without recreating it) is picked up on the next call.
    // Returns true when the OS now has the intended owner on record.
    bool attachToEditorRoot (void* floatingHandle, void* editorHandle);

    // Read-backs for the verifier - what the OS ACTUALLY has on record, not what
    // we asked for. Null on non-Windows platforms or null input.
    void* ownerOf        (void* floatingHandle); // GWLP_HWNDPARENT of the window
    void* topLevelRootOf (void* anyHandle);      // GetAncestor(..., GA_ROOT)
}
