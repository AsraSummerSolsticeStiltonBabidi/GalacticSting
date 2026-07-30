#pragma once

//==============================================================================
// The per-partial amplitude editor's SELECTION and DRAG arithmetic.
//
// WHY IT IS NOT IN THE COMPONENT. The window that shows these knobs cannot be unit
// tested (the test targets build the processor, not the editor), and the two rules
// that are easiest to get subtly wrong - the two-step SHIFT range selection, and
// "one drag delta applied to a whole selection, clamped per knob" - are pure
// arithmetic. Kept here, JUCE-free, they are exercised by SpectrumTests, so a
// regression in them fails a build instead of quietly mis-editing someone's patch.
//
// The Component above this owns the geometry, the painting and the hit-testing; this
// owns what a click means and what a drag does.
//==============================================================================

#include "dsp/PartialGains.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace partialamp
{
    // The knob's travel is the FULL engine range, bottom = silence. -120 dB is not a
    // "very quiet" position that the engine rounds off: gainScaleOf returns exactly 0
    // there, so the bottom of the knob is the mute, reachable by dragging and by
    // nothing else. Capping the travel higher would have made silence unreachable
    // from the editor - a control that cannot express what the engine can.
    inline constexpr float kMinDb = spectrum::kGainMutedDb;   // -120 = silent
    inline constexpr float kMaxDb = spectrum::kGainMaxDb;     // +24

    // Drag feel: 144 dB of travel over ~290 px, or ~1800 px with Ctrl held (the same
    // fine-drag modifier every fader in this plugin uses).
    inline constexpr float kDbPerPixel     = 0.5f;
    inline constexpr float kDbPerPixelFine = 0.08f;

    inline bool isMuted (float dB) noexcept { return dB <= spectrum::kGainMutedDb; }

    struct Cell
    {
        double    ratio   = 1.0;   // partial ratio (its label comes from this)
        float     natural = 0.0f;  // the amplitude the compiler gave it (linear)
        long long key     = 0;     // spectrum::gainKey (ratio) - what an edit is stored under
        float     dB      = 0.0f;  // the user's edit; 0 = exactly as compiled
        // Two partials less than 0.01 cent apart share one gain key, and the binding is
        // one-to-one, so only the LOWER of them can ever receive an edit (see
        // PartialGains.h). Rather than let the upper one's knob move and then be
        // silently ignored by the audio, it is locked and says so.
        bool      locked  = false;
    };

    struct Model
    {
        std::vector<Cell> cells;
        std::vector<char> sel;      // parallel to cells; 1 = selected
        int               anchor = -1;

        // Drag state: which knobs this gesture moves and where each of them started.
        std::vector<int>   dragIdx;
        std::vector<float> dragStart;

        bool valid (int i) const noexcept { return i >= 0 && (size_t) i < cells.size(); }
        int  count () const noexcept      { return (int) cells.size(); }

        void setCells (std::vector<Cell> c)
        {
            cells = std::move (c);
            // Lock every cell that shares its key with the previous one (see Cell::locked).
            for (size_t i = 1; i < cells.size(); ++i)
                if (cells[i].key == cells[i - 1].key)
                    cells[i].locked = true;
            sel.assign (cells.size(), 0);
            anchor = -1;
            dragIdx.clear(); dragStart.clear();
        }

        int lockedCount() const
        {
            int n = 0;
            for (const auto& c : cells) if (c.locked) ++n;
            return n;
        }

        void clearSelection()
        {
            std::fill (sel.begin(), sel.end(), (char) 0);
            anchor = -1;
        }
        bool isSelected (int i) const noexcept { return valid (i) && sel[(size_t) i] != 0; }
        int  selectionCount() const
        {
            int n = 0;
            for (char c : sel) if (c) ++n;
            return n;
        }

        // SHIFT+click. The first one arms an anchor and selects just that knob; the
        // next one selects the anchor, the clicked knob and everything between them.
        // The anchor deliberately STAYS put afterwards, so a third shift-click
        // re-stretches the same range instead of starting a new one from the last
        // click - which is what makes "pick the low end, then walk the high end out"
        // work.
        void shiftClick (int i)
        {
            if (! valid (i)) return;
            if (anchor < 0 || ! valid (anchor))
            {
                clearSelection();
                anchor = i;
                sel[(size_t) i] = 1;
                return;
            }
            const int lo = std::min (anchor, i), hi = std::max (anchor, i);
            std::fill (sel.begin(), sel.end(), (char) 0);
            for (int k = lo; k <= hi; ++k) sel[(size_t) k] = 1;
        }

        // What a plain (no-shift) drag starting on knob i moves: the whole selection
        // when i is part of it, otherwise i alone. Locked knobs never move.
        std::vector<int> dragSetFor (int i) const
        {
            std::vector<int> out;
            if (! valid (i)) return out;
            if (isSelected (i))
            {
                for (int k = 0; k < count(); ++k)
                    if (sel[(size_t) k] && ! cells[(size_t) k].locked) out.push_back (k);
                if (! out.empty()) return out;
            }
            if (! cells[(size_t) i].locked) out.push_back (i);
            return out;
        }

        void beginDrag (int i)
        {
            dragIdx = dragSetFor (i);
            dragStart.clear();
            dragStart.reserve (dragIdx.size());
            for (int k : dragIdx) dragStart.push_back (cells[(size_t) k].dB);
        }
        void endDrag() { dragIdx.clear(); dragStart.clear(); }
        bool dragging() const noexcept { return ! dragIdx.empty(); }

        // One delta for the whole gesture, applied from each knob's OWN start value and
        // clamped individually. Clamping the delta instead (to whatever the extreme
        // knob allows) would freeze the entire selection as soon as one member hit the
        // ceiling; clamping per knob lets the rest keep moving and, because each one
        // is measured from its start, the moment you drag back they all resume their
        // original spacing instead of having silently collapsed onto the limit.
        // Returns true if anything actually changed.
        bool applyDragDelta (float deltaDb)
        {
            bool changed = false;
            for (size_t j = 0; j < dragIdx.size(); ++j)
            {
                const int   k = dragIdx[j];
                const float v = std::max (kMinDb, std::min (kMaxDb, dragStart[j] + deltaDb));
                if (cells[(size_t) k].dB != v) { cells[(size_t) k].dB = v; changed = true; }
            }
            return changed;
        }

        // Double-click: back to the compiled amplitude (0 dB = no edit at all).
        bool resetSet (const std::vector<int>& idx)
        {
            bool changed = false;
            for (int k : idx)
                if (valid (k) && ! cells[(size_t) k].locked && cells[(size_t) k].dB != 0.0f)
                { cells[(size_t) k].dB = 0.0f; changed = true; }
            return changed;
        }

        int editedCount() const
        {
            int n = 0;
            for (const auto& c : cells) if (c.dB != 0.0f) ++n;
            return n;
        }
    };

    //==========================================================================
    // Turn the knobs back into the stored edit list.
    //
    // `homeless` are the edits that did NOT bind to any partial of the current
    // spectrum. They MUST be carried through: an edit whose partial is temporarily
    // absent (a different Base N, a narrower prime limit) is not an edit the user
    // withdrew, and rebuilding the list from the visible knobs alone would delete it
    // the moment the window was touched - a spectrum drawn by hand would lose
    // everything outside the current view.
    //
    // Both inputs are ascending by key (cells follow the template's ratio order), so
    // this is a linear merge.
    //==========================================================================
    inline std::vector<spectrum::PartialGain>
    mergedGains (const Model& m, const std::vector<spectrum::PartialGain>& homeless)
    {
        std::vector<spectrum::PartialGain> out;
        out.reserve (m.cells.size() + homeless.size());

        size_t h = 0;
        for (const auto& c : m.cells)
        {
            if (c.dB == 0.0f || c.locked) continue;
            while (h < homeless.size() && homeless[h].key < c.key) out.push_back (homeless[h++]);
            // A homeless edit on the very same key cannot happen (it would have bound
            // to this partial), but if it ever did the visible knob wins - never both,
            // or the two would compound.
            while (h < homeless.size() && homeless[h].key == c.key) ++h;
            out.push_back ({ c.key, c.dB });
        }
        while (h < homeless.size()) out.push_back (homeless[h++]);
        return out;
    }
}
