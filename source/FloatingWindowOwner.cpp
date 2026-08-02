#include "FloatingWindowOwner.h"

// See the header for WHY this file exists (and why it is the only one that may
// include <windows.h>).

#if defined(_WIN32)

 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

namespace floatwin
{
    void* topLevelRootOf (void* anyHandle)
    {
        return anyHandle != nullptr ? (void*) GetAncestor ((HWND) anyHandle, GA_ROOT)
                                    : nullptr;
    }

    void* ownerOf (void* floatingHandle)
    {
        // GWLP_HWNDPARENT reads the OWNER for a top-level window (the name is a
        // Win32 historical accident) - exactly the relationship attach sets.
        return floatingHandle != nullptr
                 ? (void*) GetWindowLongPtrW ((HWND) floatingHandle, GWLP_HWNDPARENT)
                 : nullptr;
    }

    bool attachToEditorRoot (void* floatingHandle, void* editorHandle)
    {
        auto floating = (HWND) floatingHandle;
        auto owner    = (HWND) topLevelRootOf (editorHandle);
        if (floating == nullptr || owner == nullptr || floating == owner)
            return false;

        if ((HWND) ownerOf (floating) != owner)
        {
            // Setting the owner after creation is the documented escape hatch
            // (SetParent is WRONG here: it would make a clipped CHILD window).
            SetWindowLongPtrW (floating, GWLP_HWNDPARENT, (LONG_PTR) owner);
            // The Z-order guarantee of ownership is enforced on ACTIVATION, so a
            // window that was already visible when it gained its owner could sit
            // below it until the next click. This nudge re-inserts it above its
            // owner right now, without stealing focus or moving it.
            SetWindowPos (floating, HWND_TOP, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return (HWND) ownerOf (floating) == owner; // read BACK: assert the OS took it
    }
}

#else // not Windows: the sibling-raises-over-tool-window problem is Windows'
      // Z-order model; other platforms keep JUCE's default behaviour.

namespace floatwin
{
    void* topLevelRootOf (void*)      { return nullptr; }
    void* ownerOf (void*)             { return nullptr; }
    bool  attachToEditorRoot (void*, void*) { return false; }
}

#endif
