// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "MidiState.h"
#include "utility/Macros.h"
#include "utility/Debug.h"

sfz::MidiState::MidiState()
{
    for (auto & noteState : perNoteState) {
        noteState.activeCCs.reserve(128);
    }

    resetEventStates();
    resetNoteStates();
}

void sfz::MidiState::noteOnEvent(int delay, int noteNumber, float velocity) noexcept
{
    ASSERT(noteNumber >= 0 && noteNumber <= 127);
    ASSERT(velocity >= 0 && velocity <= 1.0);

    if (noteNumber >= 0 && noteNumber < 128) {
        float keydelta { 0 };

        if (lastNotePlayed >= 0) {
            keydelta = static_cast<float>(noteNumber - lastNotePlayed);
            velocityOverride = lastNoteVelocities[lastNotePlayed];
        }

        lastNoteVelocities[noteNumber] = velocity;
        noteOnTimes[noteNumber] = internalClock + static_cast<unsigned>(delay);
        lastNotePlayed = noteNumber;
        noteBasePitchEvent(delay, noteNumber, noteNumber);
        perNoteState[noteNumber].basePitchOverridden = false;
        noteStates[noteNumber] = true;
        ccEvent(delay, ExtendedCCs::noteOnVelocity, velocity);
        ccEvent(delay, ExtendedCCs::keyboardNoteNumber, normalize7Bits(noteNumber));
        ccEvent(delay, ExtendedCCs::unipolarRandom, unipolarDist(Random::randomGenerator));
        ccEvent(delay, ExtendedCCs::bipolarRandom, bipolarDist(Random::randomGenerator));
        ccEvent(delay, ExtendedCCs::keyboardNoteGate, activeNotes > 0 ? 1.0f : 0.0f);
        ccEvent(delay, AriaExtendedCCs::keydelta, keydelta);
        ccEvent(delay, AriaExtendedCCs::absoluteKeydelta, std::abs(keydelta));
        activeNotes++;

        ccEvent(delay, ExtendedCCs::alternate, alternate);
        alternate = alternate == 0.0f ? 1.0f : 0.0f;
    }

}

void sfz::MidiState::noteOnWithPitchEvent(int delay, int noteNumber, float velocity, float basePitch) noexcept
{
    ASSERT(noteNumber >= 0 && noteNumber <= 127);
    ASSERT(velocity >= 0 && velocity <= 1.0);

    if (noteNumber >= 0 && noteNumber < 128) {
        noteOnEvent(delay, noteNumber, velocity);
        noteBasePitchEvent(delay, noteNumber, basePitch);
        perNoteState[noteNumber].basePitchOverridden = true;
    }
}

void sfz::MidiState::noteOffEvent(int delay, int noteNumber, float velocity) noexcept
{
    ASSERT(delay >= 0);
    ASSERT(noteNumber >= 0 && noteNumber <= 127);
    ASSERT(velocity >= 0.0 && velocity <= 1.0);
    UNUSED(velocity);
    if (noteNumber >= 0 && noteNumber < 128) {
        noteOffTimes[noteNumber] = internalClock + static_cast<unsigned>(delay);
        ccEvent(delay, ExtendedCCs::noteOffVelocity, velocity);
        ccEvent(delay, ExtendedCCs::keyboardNoteNumber, normalize7Bits(noteNumber));
        ccEvent(delay, ExtendedCCs::unipolarRandom, unipolarDist(Random::randomGenerator));
        ccEvent(delay, ExtendedCCs::bipolarRandom, bipolarDist(Random::randomGenerator));
        if (activeNotes > 0)
            activeNotes--;
        noteStates[noteNumber] = false;
    }

}

void sfz::MidiState::allNotesOff(int delay) noexcept
{
    for (int note = 0; note < 128; note++)
        noteOffEvent(delay, note, 0.0f);
}

void sfz::MidiState::setSampleRate(float sampleRate) noexcept
{
    this->sampleRate = sampleRate;
    internalClock = 0;
    absl::c_fill(noteOnTimes, 0);
    absl::c_fill(noteOffTimes, 0);
}

void sfz::MidiState::advanceTime(int numSamples) noexcept
{
    internalClock += numSamples;
    flushEvents();
}

void sfz::MidiState::flushEvents() noexcept
{
    auto flushEventVector = [] (EventVector& events) {
        ASSERT(!events.empty()); // CC event vectors should never be empty
        events.front().value = events.back().value;
        events.front().delay = 0;
        events.resize(1);
    };

    for (auto& events : ccEvents)
        flushEventVector(events);

    for (auto& events: polyAftertouchEvents)
        flushEventVector(events);

    flushEventVector(pitchEvents);
    flushEventVector(channelAftertouchEvents);

    for (auto & noteState : perNoteState) {
        for (auto actcc : noteState.activeCCs) {
            flushEventVector(noteState.ccEvents[actcc]);
        }

        flushEventVector(noteState.basePitchEvents);
        flushEventVector(noteState.pitchBendEvents);

        if (noteState.pitchBendEvents.back().value == 0.0f) {
            // mark it inactive
            noteState.bendActive = false;
        }
    }
}


void sfz::MidiState::setSamplesPerBlock(int samplesPerBlock) noexcept
{
    auto updateEventBufferSize = [=] (EventVector& events) {
        events.shrink_to_fit();
        events.reserve(samplesPerBlock);
    };
    this->samplesPerBlock = samplesPerBlock;
    for (auto& events: ccEvents)
        updateEventBufferSize(events);

    for (auto& events: polyAftertouchEvents)
        updateEventBufferSize(events);

    updateEventBufferSize(pitchEvents);
    updateEventBufferSize(channelAftertouchEvents);

    for (auto& noteState : perNoteState) {
        for (auto& events: noteState.ccEvents)
            updateEventBufferSize(events);
        updateEventBufferSize(noteState.pitchBendEvents);
        updateEventBufferSize(noteState.basePitchEvents);
    }
}

float sfz::MidiState::getNoteDuration(int noteNumber, int delay) const
{
    ASSERT(noteNumber >= 0 && noteNumber < 128);
    if (noteNumber < 0 || noteNumber >= 128)
        return 0.0f;

#if 0
    if (!noteStates[noteNumber])
        return 0.0f;
#endif

    const unsigned timeInSamples = internalClock + static_cast<unsigned>(delay) - noteOnTimes[noteNumber];
    return static_cast<float>(timeInSamples) / sampleRate;
}

float sfz::MidiState::getNoteVelocity(int noteNumber) const noexcept
{
    ASSERT(noteNumber >= 0 && noteNumber <= 127);

    return lastNoteVelocities[noteNumber];
}

float sfz::MidiState::getVelocityOverride() const noexcept
{
    return velocityOverride;
}

void sfz::MidiState::insertEventInVector(EventVector& events, int delay, float value)
{
    const auto insertionPoint = absl::c_lower_bound(events, delay, MidiEventDelayComparator {});
    if (insertionPoint == events.end() || insertionPoint->delay != delay)
        events.insert(insertionPoint, { delay, value });
    else
        insertionPoint->value = value;
}

void sfz::MidiState::additiveMergeEvents(const EventVector& events1, const EventVector& events2, EventVector& destEvents)
{
    ASSERT(events1.size() > 0);
    ASSERT(events2.size() > 0);

    destEvents.clear();

    auto iter1 = events1.begin();
    auto iter2 = events2.begin();
    auto prevval1 = iter1->value;
    auto prevval2 = iter2->value;

    destEvents.push_back({0, prevval1 + prevval2});
    ++iter1; ++iter2;

    while (iter1 != events1.end() || iter2 != events2.end()) {
        if (iter1 == events1.end()) {
            // finish out with events2
            for (; iter2 != events2.end(); ++iter2) {
                destEvents.push_back({iter2->delay, prevval1 + iter2->value});
            }
            break;
        }
        else if (iter2 == events2.end()) {
            // finish out with events1
            for (; iter1 != events1.end(); ++iter1) {
                destEvents.push_back({iter1->delay, iter1->value + prevval2});
            }
            break;
        }
        // both still have items
        else if (iter1->delay == iter2->delay) {
            prevval1 = iter1->value;
            prevval2 = iter2->value;
            destEvents.push_back({iter1->delay, prevval1 + prevval2});
            ++iter1;
            ++iter2;
        }
        else if (iter1->delay < iter2->delay) {
            prevval1 = iter1->value;
            destEvents.push_back({iter1->delay, prevval1 + prevval2});
            ++iter1;
        }
        else if (iter1->delay > iter2->delay) {
            prevval2 = iter2->value;
            destEvents.push_back({iter2->delay, prevval1 + prevval2});
            ++iter2;
        }
    }
}


void sfz::MidiState::pitchBendEvent(int delay, float pitchBendValue) noexcept
{
    ASSERT(pitchBendValue >= -1.0f && pitchBendValue <= 1.0f);
    insertEventInVector(pitchEvents, delay, pitchBendValue);
}

float sfz::MidiState::getPitchBend() const noexcept
{
    ASSERT(pitchEvents.size() > 0);
    return pitchEvents.back().value;
}

void sfz::MidiState::channelAftertouchEvent(int delay, float aftertouch) noexcept
{
    ASSERT(aftertouch >= -1.0f && aftertouch <= 1.0f);
    insertEventInVector(channelAftertouchEvents, delay, aftertouch);
}

void sfz::MidiState::polyAftertouchEvent(int delay, int noteNumber, float aftertouch) noexcept
{
    ASSERT(aftertouch >= 0.0f && aftertouch <= 1.0f);
    if (noteNumber < 0 || noteNumber >= static_cast<int>(polyAftertouchEvents.size()))
        return;

    insertEventInVector(polyAftertouchEvents[noteNumber], delay, aftertouch);
}

float sfz::MidiState::getChannelAftertouch() const noexcept
{
    ASSERT(channelAftertouchEvents.size() > 0);
    return channelAftertouchEvents.back().value;
}

float sfz::MidiState::getPolyAftertouch(int noteNumber) const noexcept
{
    if (noteNumber < 0 || noteNumber > 127)
        return 0.0f;

    ASSERT(polyAftertouchEvents[noteNumber].size() > 0);
    return polyAftertouchEvents[noteNumber].back().value;
}

void sfz::MidiState::ccEvent(int delay, int ccNumber, float ccValue) noexcept
{
    insertEventInVector(ccEvents[ccNumber], delay, ccValue);
}

float sfz::MidiState::getCCValue(int ccNumber) const noexcept
{
    ASSERT(ccNumber >= 0 && ccNumber < config::numCCs);
    return ccEvents[ccNumber].back().value;
}

float sfz::MidiState::getCCValueAt(int ccNumber, int delay) const noexcept
{
    ASSERT(ccNumber >= 0 && ccNumber < config::numCCs);
    const auto ccEvent = absl::c_lower_bound(
        ccEvents[ccNumber], delay, MidiEventDelayComparator {});
    if (ccEvent != ccEvents[ccNumber].end())
        return ccEvent->value;
    else
        return ccEvents[ccNumber].back().value;
}

void sfz::MidiState::managePerNoteState(int noteNumber, int flags) noexcept
{
    ASSERT(noteNumber >= 0 && noteNumber < static_cast<int>(perNoteState.size()));
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return;

    if (flags & 0x2) { // reset
        perNoteState[noteNumber].activeCCs.clear();
    }
    if (flags & 0x1) { // detach
        // TODO
    }
}

void sfz::MidiState::resetNoteStates() noexcept
{
    for (auto& velocity: lastNoteVelocities)
        velocity = 0.0f;

    velocityOverride = 0.0f;
    activeNotes = 0;
    internalClock = 0;
    lastNotePlayed = -1;
    alternate = 0.0f;

    auto setEvents = [] (EventVector& events, float value) {
        events.clear();
        events.push_back({ 0, value });
    };

    setEvents(ccEvents[ExtendedCCs::noteOnVelocity], 0.0f);
    setEvents(ccEvents[ExtendedCCs::keyboardNoteNumber], 0.0f);
    setEvents(ccEvents[ExtendedCCs::unipolarRandom], 0.0f);
    setEvents(ccEvents[ExtendedCCs::bipolarRandom], 0.0f);
    setEvents(ccEvents[ExtendedCCs::keyboardNoteGate], 0.0f);
    setEvents(ccEvents[ExtendedCCs::alternate], 0.0f);

    noteStates.reset();
    absl::c_fill(noteOnTimes, 0);
    absl::c_fill(noteOffTimes, 0);

    for (size_t i = 0; i < perNoteState.size(); ++i) {
        perNoteState[i].basePitchOverridden = false;
        setEvents(perNoteState[i].basePitchEvents, i);
    }

}

void sfz::MidiState::resetEventStates() noexcept
{
    auto clearEvents = [] (EventVector& events) {
        events.clear();
        events.push_back({ 0, 0.0f });
    };

    for (auto& events : ccEvents)
        clearEvents(events);

    for (auto& events : polyAftertouchEvents)
        clearEvents(events);

    clearEvents(pitchEvents);
    clearEvents(channelAftertouchEvents);

    for (auto& pnState : perNoteState) {
        for (auto& events : pnState.ccEvents)
            clearEvents(events);

        clearEvents(pnState.pitchBendEvents);
        pnState.bendActive = false;
        pnState.activeCCs.clear();
    }
}

const sfz::EventVector& sfz::MidiState::getCCEvents(int ccIdx) const noexcept
{
    if (ccIdx < 0 || ccIdx >= config::numCCs)
        return nullEvent;

    return ccEvents[ccIdx];
}

const sfz::EventVector& sfz::MidiState::getPitchBendEvents() const noexcept
{
    return pitchEvents;
}

const sfz::EventVector& sfz::MidiState::getChannelAftertouchEvents() const noexcept
{
    return channelAftertouchEvents;
}

const sfz::EventVector& sfz::MidiState::getPolyAftertouchEvents(int noteNumber) const noexcept
{
    if (noteNumber < 0 || noteNumber > 127)
        return nullEvent;

    return polyAftertouchEvents[noteNumber];
}

int sfz::MidiState::getProgram() const noexcept
{
    return currentProgram;
}

void sfz::MidiState::programChangeEvent(int delay, int program) noexcept
{
    UNUSED(delay);
    ASSERT(program >= 0 && program <= 127);
    currentProgram = program;
}


/**
 * @brief Get a note's base pitch, could be different than notenum when using per-note pitch and CC
 */
float sfz::MidiState::getNoteBasePitch(int noteNumber) const noexcept
{
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return 0.0f;
    ASSERT(perNoteState[noteNumber].basePitchEvents.size() > 0);
    if (perNoteState[noteNumber].basePitchOverridden) {
        return perNoteState[noteNumber].basePitchEvents.back().value;
    } else {
        return static_cast<float>(noteNumber);
    }
}

bool sfz::MidiState::isNoteBasePitchOverridden(int noteNumber) const noexcept
{
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return false;
    return perNoteState[noteNumber].basePitchOverridden;
}


/**
 * @brief Register a note's base pitch change event, can be different than notenum when using this call. When
 * the basepitch is set, it overrides any other tuning system that might be set up,
 * but relative pitch-bend state is still respected.
 */
void sfz::MidiState::noteBasePitchEvent(int delay, int noteNumber, float pitch) noexcept
{
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return;

    insertEventInVector(perNoteState[noteNumber].basePitchEvents, delay, pitch);
    perNoteState[noteNumber].basePitchOverridden = true;
}


void sfz::MidiState::perNoteCCEvent(int delay, int noteNumber, int ccNumber, float ccValue) noexcept
{
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return;
    insertEventInVector(perNoteState[noteNumber].ccEvents[ccNumber], delay, ccValue);
    insertValueInVector(perNoteState[noteNumber].activeCCs, ccNumber);
}


/**
 * @brief Get a note's per-note CC-value
 * @param noteNumber
 * @param ccNumber
 * @return float
 */
float sfz::MidiState::getPerNoteCCValue(int noteNumber, int ccNumber) const noexcept
{
    ASSERT(ccNumber >= 0 && ccNumber < config::numCCs);
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return 0.0f;

    const auto & pns = perNoteState[noteNumber];
    if (std::find(pns.activeCCs.begin(), pns.activeCCs.end(), ccNumber) != pns.activeCCs.end()) {
        return pns.ccEvents[ccNumber].back().value;
    } else {
        return 0.0f;
    }
}

float sfz::MidiState::getPerNoteCCValueAt(int noteNumber, int ccNumber, int delay) const noexcept
{
    ASSERT(ccNumber >= 0 && ccNumber < config::numCCs);

    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return 0.0f;

    const auto & pns = perNoteState[noteNumber];
    if (std::find(pns.activeCCs.begin(), pns.activeCCs.end(), ccNumber) != pns.activeCCs.end()) {

        const auto ccEvent = absl::c_lower_bound(
                                                 pns.ccEvents[ccNumber], delay, MidiEventDelayComparator {});
        if (ccEvent != pns.ccEvents[ccNumber].end())
            return ccEvent->value;
        else
            return pns.ccEvents[ccNumber].back().value;
    } else {
        return 0.0f;
    }
}

void sfz::MidiState::perNotePitchBendEvent(int delay, int noteNumber, float pitchBendValue) noexcept
{
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return;
    ASSERT(pitchBendValue >= -1.0f && pitchBendValue <= 1.0f);
    perNoteState[noteNumber].bendActive = true;
    insertEventInVector(perNoteState[noteNumber].pitchBendEvents, delay, pitchBendValue);
}

float sfz::MidiState::getPerNotePitchBend(int noteNumber) const noexcept
{
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return 0.0f;
    if (perNoteState[noteNumber].bendActive) {
        ASSERT(perNoteState[noteNumber].pitchBendEvents.size() > 0);
        return perNoteState[noteNumber].pitchBendEvents.back().value;
    } else {
        return 0.0f;
    }
}

const sfz::EventVector & sfz::MidiState::getPerNotePitchBendEvents(int noteNumber) const noexcept
{
    if (noteNumber < 0 || noteNumber > 127 || !perNoteState[noteNumber].bendActive)
        return nullEvent;

    return perNoteState[noteNumber].pitchBendEvents;
}

const sfz::EventVector& sfz::MidiState::getPerNoteCCEvents(int noteNumber, int ccIdx) const noexcept
{
    if (ccIdx < 0 || ccIdx >= config::numCCs)
        return nullEvent;
    if (noteNumber < 0 || noteNumber >= static_cast<int>(perNoteState.size()))
        return nullEvent;

    const auto & pns = perNoteState[noteNumber];
    if (std::find(pns.activeCCs.begin(), pns.activeCCs.end(), ccIdx) != pns.activeCCs.end()) {
        return pns.ccEvents[ccIdx];
    } else {
        return nullEvent;
    }
}
