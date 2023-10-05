// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once
#include <array>
#include <bitset>
#include "CCMap.h"
#include "Range.h"

namespace sfz
{
/**
 * @brief Holds the current "MIDI state", meaning the known state of all CCs
 * currently, as well as the note velocities that triggered the currently
 * pressed notes.
 *
 */
class MidiState
{
public:
    MidiState();

    /**
     * @brief Update the state after a note on event
     *
     * @param delay
     * @param noteNumber
     * @param velocity
     */
    void noteOnEvent(int delay, int noteNumber, float velocity) noexcept;

    /**
     * @brief Update the state after a note off event
     *
     * @param delay
     * @param noteNumber
     * @param velocity
     */
    void noteOffEvent(int delay, int noteNumber, float velocity) noexcept;

    /**
     * @brief Set all notes off
     *
     * @param delay
     */
    void allNotesOff(int delay) noexcept;

    /**
     * @brief Get the number of active notes
     */
    int getActiveNotes() const noexcept { return activeNotes; }

    /**
     * @brief Get the note duration since note on
     *
     * @param noteNumber
     * @param delay
     * @return float
     */
    float getNoteDuration(int noteNumber, int delay = 0) const;

    /**
     * @brief Set the maximum size of the blocks for the callback. The actual
     * size can be lower in each callback but should not be larger
     * than this value.
     *
     * @param samplesPerBlock
     */
    void setSamplesPerBlock(int samplesPerBlock) noexcept;
    /**
     * @brief Set the sample rate. If you do not call it it is initialized
     * to sfz::config::defaultSampleRate.
     *
     * @param sampleRate
     */
    void setSampleRate(float sampleRate) noexcept;
    /**
     * @brief Get the note on velocity for a given note
     *
     * @param noteNumber
     * @return float
     */
    float getNoteVelocity(int noteNumber) const noexcept;

    /**
     * @brief Get the velocity override value (sw_vel in SFZ)
     *
     * @return float
     */
    float getVelocityOverride() const noexcept;

    /**
     * @brief Register a pitch bend event
     *
     * @param pitchBendValue
     */
    void pitchBendEvent(int delay, float pitchBendValue) noexcept;

    /**
     * @brief Get the pitch bend status

     * @return float
     */
    float getPitchBend() const noexcept;

    /**
     * @brief Register a channel aftertouch event
     *
     * @param aftertouch
     */
    void channelAftertouchEvent(int delay, float aftertouch) noexcept;

    /**
     * @brief Register a channel aftertouch event
     *
     * @param aftertouch
     */
    void polyAftertouchEvent(int delay, int noteNumber, float aftertouch) noexcept;

    /**
     * @brief Get the channel aftertouch status

     * @return int
     */
    float getChannelAftertouch() const noexcept;

    /**
     * @brief Get the polyphonic aftertouch status

     * @return int
     */
    float getPolyAftertouch(int noteNumber) const noexcept;

    /**
     * @brief Get the current midi program
     *
     * @return int
     */
    int getProgram() const noexcept;
    /**
     * @brief Register a program change event
     *
     * @param delay
     * @param program
     */
    void programChangeEvent(int delay, int program) noexcept;

    /**
     * @brief Register a CC event
     *
     * @param ccNumber
     * @param ccValue
     */
    void ccEvent(int delay, int ccNumber, float ccValue) noexcept;

    /**
     * @brief Advances the internal clock of a given amount of samples.
     * You should call this at each callback. This will flush the events
     * in the midistate memory by calling flushEvents().
     *
     * @param numSamples the number of samples of clock advance
     */
    void advanceTime(int numSamples) noexcept;

    /**
     * @brief Returns current internal sample clock
     *
     */
    unsigned getInternalClock() const noexcept { return internalClock; }

    /**
     * @brief Flush events in all states, keeping only the last one as the "base" state
     *
     */
    void flushEvents() noexcept;

    /**
     * @brief Check if a note is currently depressed
     *
     * @param noteNumber
     * @return true
     * @return false
     */
    bool isNotePressed(int noteNumber) const noexcept { return noteStates[noteNumber]; }

    /**
     * @brief Get the last CC value for CC number
     *
     * @param ccNumber
     * @return float
     */
    float getCCValue(int ccNumber) const noexcept;

    /**
     * @brief Get the CC value for CC number
     *
     * @param ccNumber
     * @param delay
     * @return float
     */
    float getCCValueAt(int ccNumber, int delay) const noexcept;

    //=======================================
    // Per-note calls

    /**
     * @brief Update the state after a note on event, also setting basePitch independently from the note number,
     *   which can be interpreted as an identifier instead. This overrides any other fixed tuning that might be set up,
     *   but relative pitch-bend state is still respected.
     *
     * @param delay
     * @param noteNumber
     * @param velocity
     * @param basePitch is floating point representation of actual pitch to use as basis. the noteNumber effectivl
     */
    void noteOnWithPitchEvent(int delay, int noteNumber, float velocity, float basePitch) noexcept;

    /**
     * @brief Get a note's base pitch, could be different than notenum when using per-note expression or pitch. This should therefore
     *  be called as the source of truth for the actual pitch of the note, instead of the noteNumber, and note that the pitch is floating point.
     *  If the pitch is rounded to an integer it can be used for SFZ note lookup purposes, but the actual synthesized base pitch should be as returned here.
     * @param noteNumber
     * @param overridden is set to true if the basepitch was intentionally specified by a noteOnWithPitchEvent or a noteBasePitchEvent
     * @return float
     */
    float getNoteBasePitch(int noteNumber) const noexcept;

    /**
     * @brief Get if a specific notenum is using an overridden base pitch. This is used to determine
     *  if other tuning adjustments should be used or not.
     */
    bool isNoteBasePitchOverridden(int noteNumber) const noexcept;

    /**
     * @brief Register a note's base pitch change event, can be different than notenum when using this call. When
     * the basepitch is set, it overrides any other tuning system that might be set up,
     * but relative pitch-bend state is still respected.
     */
    void noteBasePitchEvent(int delay, int noteNumber, float pitch) noexcept;

    /**
     * @brief Register a per note CC event
     *
     * @param noteNumber
     * @param ccNumber
     * @param ccValue
     */
    void perNoteCCEvent(int delay, int noteNumber, int ccNumber, float ccValue) noexcept;

    /**
     * @brief Get a note's per-note CC-value. This is actually a merged version of both the non-note specific CC and
     *       any potential per-note CC, so it's the only one that needs to be called. The merging strategy is currently additive.
     * @param noteNumber
     * @param ccNumber
     * @return float
     */
    float getPerNoteCCValue(int noteNumber, int ccNumber) const noexcept;

    /**
     * @brief Get a note's per-note CC-value. This is actually a merged version of both the non-note specific CC and
     *       any potential per-note CC, so it's the only one that needs to be called. The merging strategy is currently additive.
     * @param noteNumber
     * @param ccNumber
     * @param delay
     * @return float
     */
    float getPerNoteCCValueAt(int noteNumber, int ccNumber, int delay) const noexcept;

    /**
     * @brief See if per-note CC has been actively set
     * @param noteNumber
     * @param ccNumber
     * @return bool
     */
    bool isPerNoteCCActive(int noteNumber, int ccNumber) const noexcept {
        if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size())) return false;
        const auto & pns = perNoteState[noteNumber];
        return std::find(pns.activeCCs.begin(), pns.activeCCs.end(), ccNumber) != pns.activeCCs.end();
    }

    /**
     * @brief Register a per-note pitch bend event
     *
     * @param delay
     * @param noteNumber
     * @param pitchBendValue
     */
    void perNotePitchBendEvent(int delay, int noteNumber, float pitchBendValue) noexcept;

    /**
     * @brief Get the per-note pitch bend status. This is actually a merged version of both the non-note specific pitchbend and
     *       any potential per-note pitchbend, so it's the only one that needs to be called. The merging strategy is additive.
     * @param noteNumber
     * @return float
     */
    float getPerNotePitchBend(int noteNumber) const noexcept;

    /**
     * @brief Manage reset or detaching of per-note controller state
     *
     * @param noteNumber the note number/index to apply this CC event to only
     * @param flags bitmask flags where 0x1 = Detach, and 0x2 = Reset
     */
    void managePerNoteState(int notenumber, int flags) noexcept;

    /**
     * @brief Reset the midi note states
     *
     */
    void resetNoteStates() noexcept;

    const EventVector& getCCEvents(int ccIdx) const noexcept;
    const EventVector& getPolyAftertouchEvents(int noteNumber) const noexcept;
    const EventVector& getPitchBendEvents() const noexcept;
    const EventVector& getChannelAftertouchEvents() const noexcept;

    /**
     * @brief Get the per-note pitch bend events. This is actually a merged version of both the non-note specific pitchbend and
     *       any potential per-note pitchbend, so it's the only one that needs to be called. The merging strategy is additive.
     * @param noteNumber
     * @return EventVector &
     */
    const EventVector& getPerNotePitchBendEvents(int noteNumber) const noexcept;

    /**
     * @brief Get the per-note CC events. This is actually a merged version of both the non-note specific CC and
     *       any potential per-note CC, so it's the only one that needs to be called. The merging strategy is additive.
     * @param noteNumber
     * @return EventVector &
     */
    const EventVector& getPerNoteCCEvents(int noteNumber, int ccIdx) const noexcept;

    /**
     * @brief Reset the midi event states (CC, AT, and pitch bend)
     *
     */
    void resetEventStates() noexcept;


    static void additiveMergeEvents(const EventVector& events1, const EventVector& events2, EventVector& destEvents);


private:

    /**
     * @brief Insert events in a sorted event vector.
     *
     * @param events
     * @param delay
     * @param value
     */
    void insertEventInVector(EventVector& events, int delay, float value);



    int activeNotes { 0 };

    /**
     * @brief Stores the note on times.
     *
     */
    MidiNoteArray<unsigned> noteOnTimes { {} };

    /**
     * @brief Stores the note off times.
     *
     */

    MidiNoteArray<unsigned> noteOffTimes { {} };

    /**
     * @brief Store the note states
     *
     */
    std::bitset<128> noteStates;

    /**
     * @brief Stores the velocity of the note ons for currently
     * depressed notes.
     *
     */
    MidiNoteArray<float> lastNoteVelocities;

    /**
     * @brief Velocity override value (sw_vel in SFZ)
     */
    float velocityOverride;

    /**
     * @brief Last note played
     */
    int lastNotePlayed { -1 };

    /**
     * @brief Current known values for the CCs.
     *
     */
    std::array<EventVector, config::numCCs> ccEvents;

    /**
     * @brief Null event
     *
     */
    const EventVector nullEvent { { 0, 0.0f } };

    /**
     * @brief Pitch bend status
     */
    EventVector pitchEvents;

    /**
     * @brief Aftertouch status
     */
    EventVector channelAftertouchEvents;

    /**
     * @brief Polyphonic aftertouch status.
     */
    std::array<EventVector, 128> polyAftertouchEvents;

    /**
     * @brief Current midi program
     */
    int currentProgram { 0 };

    /**
     * @brief Current known values for the per-note state
     */

    struct PerNoteState
    {
        bool basePitchOverridden = false;
        EventVector basePitchEvents; // for the pitch basis of the note, since perNote state is not necessarily tied to the note number pitch

        bool bendActive = false; // if any per-note bend has been set
        EventVector pitchBendEvents; // for relative pitch bend away from base

        std::vector<int> activeCCs; // list of CCs that have had a per-note event since the last reset, used for fast iteration

        std::array<EventVector, config::numCCs> ccEvents;
    };

    std::array<PerNoteState, 128> perNoteState;

    static void insertValueInVector(std::vector<int> & items, int value) {
        const auto found = absl::c_find(items, value);
        if (found == items.end())
            items.push_back(value);
    }

    static void removeValueFromVector(std::vector<int> & items, int value) {
        const auto found = absl::c_find(items, value);
        if (found != items.end())
            items.erase(found);
    }

    float sampleRate { config::defaultSampleRate };
    int samplesPerBlock { config::defaultSamplesPerBlock };
    float alternate { 0.0f };
    unsigned internalClock { 0 };
    fast_real_distribution<float> unipolarDist { 0.0f, 1.0f };
    fast_real_distribution<float> bipolarDist { -1.0f, 1.0f };
};
}
