#pragma once

//==============================================================================
// WHAT THE CONTROLLER IS REALLY SENDING - "arregla bugs de mpe que tengo al tocarlo con
// el hexboard" (2026-10-03).
//
// The voice's gain term is 0.35 + 0.65 * pressure, and JUCE starts every note's pressure
// at rest, so an instrument played from anything that sends NO aftertouch - the Hexboard
// (its firmware sends no channel pressure, no poly aftertouch and no CC 74), most
// keyboards, a clip in the host - sounded 9.1 dB under its own full scale for ever, with
// nothing on screen saying why.
//
// A voice cannot tell "the player's finger is at rest" from "nobody is sending pressure":
// both read as 0. Only the MIDI stream knows, so the processor marks the channels it has
// actually seen pressure on and every voice - the main engine's and the playground
// corners' alike - reads that one word before it decides whether pressure owns its gain.
//
// A bit per channel (1..16), set on the audio thread as the block is scanned and read on
// the audio thread by the voices: one relaxed atomic, no allocation, no lock. It is never
// cleared while the instrument is open: a controller that sent pressure once is a
// controller that sends pressure, and a note that then rests at 0 means it.
//==============================================================================

#include <atomic>
#include <juce_core/juce_core.h>

struct ExpressionState
{
    void noteSeenPressure (int channel) noexcept
    {
        if (channel >= 1 && channel <= 16)
            pressureSeen.fetch_or (1u << (channel - 1), std::memory_order_relaxed);
    }
    bool sendsPressure (int channel) const noexcept
    {
        if (! (channel >= 1 && channel <= 16)) return false;
        return (pressureSeen.load (std::memory_order_relaxed) & (1u << (channel - 1))) != 0;
    }
    // WHICH CHANNELS THE OUTSIDE WORLD PLAYS ON. The window's own keyboards have to put
    // their notes somewhere, and "somewhere" must not be a channel the host or the
    // controller is already playing: two note-ons for one pitch on one channel are ONE
    // note to a MIDI receiver, so the second press retriggers the first and either
    // release kills both. Sticky, like the pressure mask: a channel that has carried a
    // note is a channel that belongs to the player.
    void noteSeenNote (int channel) noexcept
    {
        if (channel >= 1 && channel <= 16)
            noteChannels.fetch_or (1u << (channel - 1), std::memory_order_relaxed);
    }
    bool hostPlaysOn (int channel) const noexcept
    {
        if (! (channel >= 1 && channel <= 16)) return false;
        return (noteChannels.load (std::memory_order_relaxed) & (1u << (channel - 1))) != 0;
    }
    juce::uint32 noteChannelMask() const noexcept { return noteChannels.load (std::memory_order_relaxed); }

    // TESTS / a fresh instrument: forget what was seen (prepareToPlay does NOT - a pedal
    // change or a sample-rate change is not the controller going away).
    void forgetPressure() noexcept { pressureSeen.store (0, std::memory_order_relaxed); }
    void forgetChannels() noexcept { noteChannels.store (0, std::memory_order_relaxed); }
    juce::uint32 pressureMaskForTests() const noexcept { return pressureSeen.load (std::memory_order_relaxed); }

private:
    std::atomic<juce::uint32> pressureSeen { 0 };
    std::atomic<juce::uint32> noteChannels { 0 };
};
