// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Region.h"
#include "Opcode.h"
#include "MathHelpers.h"
#include "FilePool.h" // for region length check in output
#include "utility/SwapAndPop.h"
#include "utility/StringViewHelpers.h"
#include "utility/Macros.h"
#include "utility/Debug.h"
#include "modulations/ModId.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/match.h"
#include "absl/algorithm/container.h"
#include <random>
#include <cassert>

template<class T>
bool extendIfNecessary(std::vector<T>& vec, unsigned size, unsigned defaultCapacity)
{
    if (size == 0)
        return false;

    if (vec.capacity() == 0)
        vec.reserve(defaultCapacity);

    if (vec.size() < size)
        vec.resize(size);

    return true;
}

sfz::Region::Region(int regionNumber, absl::string_view defaultPath)
: id{regionNumber}, defaultPath(defaultPath)
{
    gainToEffect.reserve(5); // sufficient room for main and fx1-4
    gainToEffect.push_back(1.0); // contribute 100% into the main bus

    // Default amplitude release
    amplitudeEG.release = Default::egRelease;
}

// Helper for ccN processing
#define case_any_ccN(x)        \
    case hash(x "_oncc&"):     \
    case hash(x "_curvecc&"):  \
    case hash(x "_stepcc&"):   \
    case hash(x "_smoothcc&")

bool sfz::Region::parseOpcode(const Opcode& rawOpcode, bool cleanOpcode)
{
    const Opcode opcode = cleanOpcode ? rawOpcode.cleanUp(kOpcodeScopeRegion) : rawOpcode;

    switch (opcode.lettersOnlyHash) {

    // Sound source: sample playback
    case hash("sample"):
        {
            const auto trimmedSample = trim(opcode.value);
            if (trimmedSample.empty())
                break;

            std::string filename;
            if (trimmedSample[0] == '*')
                filename = std::string(trimmedSample);
            else
                filename = absl::StrCat(defaultPath, absl::StrReplaceAll(trimmedSample, { { "\\", "/" } }));

            *sampleId = FileId(std::move(filename), sampleId->isReverse());
        }
        break;
    case hash("sample_quality"):
        sampleQuality = opcode.read(Default::sampleQuality);
        break;
    case hash("direction"):
        *sampleId = sampleId->reversed(opcode.value == "reverse");
        break;
    case hash("delay"):
        delay = opcode.read(Default::delay);
        break;
    case hash("delay_oncc&"): // also delay_cc&
        if (opcode.parameters.back() > config::numCCs)
            return false;

        delayCC[opcode.parameters.back()] = opcode.read(Default::delayMod);
        break;
    case hash("delay_random"):
        delayRandom = opcode.read(Default::delayRandom);
        break;
    case hash("offset"):
        offset = opcode.read(Default::offset);
        break;
    case hash("offset_random"):
        offsetRandom = opcode.read(Default::offsetRandom);
        break;
    case hash("offset_oncc&"): // also offset_cc&
        if (opcode.parameters.back() > config::numCCs)
            return false;

        offsetCC[opcode.parameters.back()] = opcode.read(Default::offsetMod);
        break;
    case hash("end"):
        sampleEnd = opcode.read(Default::sampleEnd);
        break;
    case hash("end_oncc&"): // also end_cc&
        if (opcode.parameters.back() > config::numCCs)
            return false;

        endCC[opcode.parameters.back()] = opcode.read(Default::sampleEndMod);
        break;
    case hash("count"):
        sampleCount = opcode.readOptional(Default::sampleCount);
        loopMode = LoopMode::one_shot;
        break;
    case hash("loop_mode"): // also loopmode
        loopMode = opcode.readOptional(Default::loopMode);
        break;
    case hash("loop_end"): // also loopend
        loopRange.setEnd(opcode.read(Default::loopEnd));
        break;
    case hash("loop_count"):
        loopCount = opcode.readOptional(Default::loopCount);
        break;
    case hash("loop_start"): // also loopstart
        loopRange.setStart(opcode.read(Default::loopStart));
        break;
    case hash("loop_start_oncc&"): // also loop_start_cc&, loop_startcc&
        if (opcode.parameters.back() > config::numCCs)
            return false;

        loopStartCC[opcode.parameters.back()] = opcode.read(Default::loopMod);
        break;
    case hash("loop_end_oncc&"): // also loop_end_cc&, loop_lengthcc&, loop_length_oncc&, loop_length_cc&
        if (opcode.parameters.back() > config::numCCs)
            return false;

        loopEndCC[opcode.parameters.back()] = opcode.read(Default::loopMod);
        break;
    case hash("loop_crossfade"):
        loopCrossfade = opcode.read(Default::loopCrossfade);
        break;

    // Wavetable oscillator
    case hash("oscillator_phase"):
        {
            auto phase = opcode.read(Default::oscillatorPhase);
            oscillatorPhase = (phase >= 0) ? wrapPhase(phase) : -1.0f;
        }
        break;
    case hash("oscillator"):
        oscillatorEnabled = opcode.read(Default::oscillator);
        break;
    case hash("oscillator_mode"):
        oscillatorMode = opcode.read(Default::oscillatorMode);
        break;
    case hash("oscillator_multi"):
        oscillatorMulti = opcode.read(Default::oscillatorMulti);
        break;
    case hash("oscillator_detune"):
        oscillatorDetune = opcode.read(Default::oscillatorDetune);
        break;
    case_any_ccN("oscillator_detune"):
        processGenericCc(opcode, Default::oscillatorDetuneMod,
            ModKey::createNXYZ(ModId::OscillatorDetune, id));
        break;
    case hash("oscillator_mod_depth"):
        oscillatorModDepth = opcode.read(Default::oscillatorModDepth);
        break;
    case_any_ccN("oscillator_mod_depth"):
        processGenericCc(opcode, Default::oscillatorModDepthMod,
            ModKey::createNXYZ(ModId::OscillatorModDepth, id));
        break;
    case hash("oscillator_quality"):
        oscillatorQuality = opcode.readOptional(Default::oscillatorQuality);
        break;

    // Instrument settings: voice lifecycle
    case hash("group"): // also polyphony_group
        group = opcode.read(Default::group);
        break;
    case hash("output"):
        output = opcode.read(Default::output);
        break;
    case hash("off_by"): // also offby
        offBy = opcode.readOptional(Default::group);
        break;
    case hash("off_mode"): // also offmode
         offMode = opcode.read(Default::offMode);
        break;
    case hash("off_time"):
        offMode = OffMode::time;
        offTime = opcode.read(Default::offTime);
        break;
    case hash("polyphony"):
        polyphony = opcode.read(Default::polyphony);
        break;
    case hash("note_polyphony"):
        notePolyphony = opcode.read(Default::notePolyphony);
        break;
    case hash("note_selfmask"):
        selfMask = opcode.read(Default::selfMask);
        break;
    case hash("rt_dead"):
        rtDead = opcode.read(Default::rtDead);
        break;
    // Region logic: key mapping
    case hash("lokey"):
        keyRange.setStart(opcode.read(Default::loKey));
        break;
    case hash("hikey"):
        {
            absl::optional<uint8_t> optValue = opcode.readOptional(Default::hiKey);
            triggerOnNote = optValue != absl::nullopt;
            uint8_t value = optValue.value_or(Default::hiKey);
            keyRange.setEnd(value);
        }
        break;
    case hash("key"):
        {
            absl::optional<uint8_t> optValue = opcode.readOptional(Default::key);
            triggerOnNote = optValue != absl::nullopt;
            uint8_t value = optValue.value_or(Default::key);
            keyRange.setStart(value);
            keyRange.setEnd(value);
            pitchKeycenter = value;
        }
        break;
    case hash("lovel"):
        velocityRange.setStart(opcode.read(Default::loVel));
        break;
    case hash("hivel"):
        velocityRange.setEnd(opcode.read(Default::hiVel));
        break;

    // Region logic: MIDI conditions
    case hash("lobend"):
        bendRange.setStart(opcode.read(Default::loBend));
        break;
    case hash("hibend"):
        bendRange.setEnd(opcode.read(Default::hiBend));
        break;
    case hash("loprog"):
        programRange.setStart(opcode.read(Default::loProgram));
        break;
    case hash("hiprog"):
        programRange.setEnd(opcode.read(Default::hiProgram));
        break;
    case hash("locc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        ccConditions[opcode.parameters.back()].setStart(
            opcode.read(Default::loCC)
        );
        break;
    case hash("hicc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        ccConditions[opcode.parameters.back()].setEnd(
            opcode.read(Default::hiCC)
        );
        break;
    case hash("lohdcc&"): // also lorealcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        ccConditions[opcode.parameters.back()].setStart(
            opcode.read(Default::loNormalized)
        );
        break;
    case hash("hihdcc&"): // also hirealcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        ccConditions[opcode.parameters.back()].setEnd(
            opcode.read(Default::hiNormalized)
        );
        break;
    case hash("sw_lokey"): // fallthrough
    case hash("sw_hikey"):
        break;
    case hash("sw_last"):
        if (!lastKeyswitchRange) {
            lastKeyswitch = opcode.readOptional(Default::key);
            usesKeySwitches = lastKeyswitch.has_value();
        }
        break;
    case hash("sw_lolast"):
        {
            auto value = opcode.read(Default::key);
            if (!lastKeyswitchRange)
                lastKeyswitchRange.emplace(value, value);
            else
                lastKeyswitchRange->setStart(value);

            usesKeySwitches = true;
            lastKeyswitch = absl::nullopt;
        }
        break;
    case hash("sw_hilast"):
        {
            auto value = opcode.read(Default::key);
            if (!lastKeyswitchRange)
                lastKeyswitchRange.emplace(value, value);
            else
                lastKeyswitchRange->setEnd(value);

            usesKeySwitches = true;
            lastKeyswitch = absl::nullopt;
        }
        break;
    case hash("sw_label"):
        keyswitchLabel = opcode.value;
        break;
    case hash("sw_down"):
        downKeyswitch = opcode.readOptional(Default::key);
        usesKeySwitches = downKeyswitch.has_value();
        break;
    case hash("sw_up"):
        upKeyswitch = opcode.readOptional(Default::key);
        break;
    case hash("sw_previous"):
        previousKeyswitch = opcode.readOptional(Default::key);
        usesPreviousKeySwitches = previousKeyswitch.has_value();
        break;
    case hash("sw_vel"):
        velocityOverride =
            opcode.read(Default::velocityOverride);
        break;

    case hash("sustain_cc"):
        sustainCC = opcode.read(Default::sustainCC);
        break;
    case hash("sostenuto_cc"):
        sostenutoCC = opcode.read(Default::sostenutoCC);
        break;
    case hash("sustain_lo"):
        sustainThreshold = opcode.read(Default::sustainThreshold);
        break;
    case hash("sostenuto_lo"):
        sostenutoThreshold = opcode.read(Default::sostenutoThreshold);
        break;
    case hash("sustain_sw"):
        checkSustain = opcode.read(Default::checkSustain);
        break;
    case hash("sostenuto_sw"):
        checkSostenuto = opcode.read(Default::checkSostenuto);
        break;
    // Region logic: internal conditions
    case hash("lochanaft"):
        aftertouchRange.setStart(opcode.read(Default::loChannelAftertouch));
        break;
    case hash("hichanaft"):
        aftertouchRange.setEnd(opcode.read(Default::hiChannelAftertouch));
        break;
    case hash("lopolyaft"):
        polyAftertouchRange.setStart(opcode.read(Default::loPolyAftertouch));
        break;
    case hash("hipolyaft"):
        polyAftertouchRange.setEnd(opcode.read(Default::hiPolyAftertouch));
        break;
    case hash("lobpm"):
        bpmRange.setStart(opcode.read(Default::loBPM));
        break;
    case hash("hibpm"):
        bpmRange.setEnd(opcode.read(Default::hiBPM));
        break;
    case hash("lorand"):
        randRange.setStart(opcode.read(Default::loNormalized));
        break;
    case hash("hirand"):
        randRange.setEnd(opcode.read(Default::hiNormalized));
        break;
    case hash("seq_length"):
        sequenceLength = opcode.read(Default::sequence);
        break;
    case hash("seq_position"):
        sequencePosition = opcode.read(Default::sequence);
        usesSequenceSwitches = true;
        break;
    // Region logic: triggers
    case hash("trigger"):
        trigger = opcode.read(Default::trigger);
        break;
    case hash("start_locc&"): // also on_locc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        triggerOnCC = true;
        ccTriggers[opcode.parameters.back()].setStart(
            opcode.read(Default::loCC)
        );
        break;
    case hash("start_hicc&"): // also on_hicc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        triggerOnCC = true;
        ccTriggers[opcode.parameters.back()].setEnd(
            opcode.read(Default::hiCC)
        );
        break;
    case hash("start_lohdcc&"): // also on_lohdcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        triggerOnCC = true;
        ccTriggers[opcode.parameters.back()].setStart(
            opcode.read(Default::loNormalized)
        );
        break;
    case hash("start_hihdcc&"): // also on_hihdcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        ccTriggers[opcode.parameters.back()].setEnd(
            opcode.read(Default::hiNormalized)
        );
        break;

    // Performance parameters: amplifier
    case hash("volume"): // also gain
        volume = opcode.read(Default::volume);
        break;
    case_any_ccN("volume"): // also gain
        processGenericCc(opcode, Default::volumeMod, ModKey::createNXYZ(ModId::Volume, id));
        break;
    case hash("amplitude"):
        amplitude = opcode.read(Default::amplitude);
        break;
    case_any_ccN("amplitude"):
        processGenericCc(opcode, Default::amplitudeMod, ModKey::createNXYZ(ModId::Amplitude, id));
        break;
    case hash("pan"):
        pan = opcode.read(Default::pan);
        break;
    case_any_ccN("pan"):
        processGenericCc(opcode, Default::panMod, ModKey::createNXYZ(ModId::Pan, id));
        break;
    case hash("position"):
        position = opcode.read(Default::position);
        break;
    case_any_ccN("position"):
        processGenericCc(opcode, Default::positionMod, ModKey::createNXYZ(ModId::Position, id));
        break;
    case hash("width"):
        width = opcode.read(Default::width);
        break;
    case_any_ccN("width"):
        processGenericCc(opcode, Default::widthMod, ModKey::createNXYZ(ModId::Width, id));
        break;
    case hash("amp_keycenter"):
        ampKeycenter = opcode.read(Default::key);
        break;
    case hash("amp_keytrack"):
        ampKeytrack = opcode.read(Default::ampKeytrack);
        break;
    case hash("amp_veltrack"):
        ampVeltrack = opcode.read(Default::ampVeltrack);
        break;
    case hash("amp_veltrack_oncc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        ampVeltrackCC[opcode.parameters.back()].modifier = opcode.read(Default::ampVeltrackMod);
        break;
    case hash("amp_veltrack_curvecc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        ampVeltrackCC[opcode.parameters.back()].curve = opcode.read(Default::curveCC);
        break;
    case hash("amp_random"):
        ampRandom = opcode.read(Default::ampRandom);
        break;
    case hash("amp_velcurve_&"):
        {
            if (opcode.parameters.back() > 127)
                return false;

            const auto inputVelocity = static_cast<uint8_t>(opcode.parameters.back());
            velocityPoints.emplace_back(inputVelocity, opcode.read(Default::ampVelcurve));
        }
        break;
    case hash("xfin_lokey"):
        crossfadeKeyInRange.setStart(opcode.read(Default::loKey));
        break;
    case hash("xfin_hikey"):
        crossfadeKeyInRange.setEnd(opcode.read(Default::loKey)); // loKey for the proper default
        break;
    case hash("xfout_lokey"):
        crossfadeKeyOutRange.setStart(opcode.read(Default::hiKey)); // hiKey for the proper default
        break;
    case hash("xfout_hikey"):
        crossfadeKeyOutRange.setEnd(opcode.read(Default::hiKey));
        break;
    case hash("xfin_lovel"):
        crossfadeVelInRange.setStart(opcode.read(Default::xfinLo));
        break;
    case hash("xfin_hivel"):
        crossfadeVelInRange.setEnd(opcode.read(Default::xfinHi));
        break;
    case hash("xfout_lovel"):
        crossfadeVelOutRange.setStart(opcode.read(Default::xfoutLo));
        break;
    case hash("xfout_hivel"):
        crossfadeVelOutRange.setEnd(opcode.read(Default::xfoutHi));
        break;
    case hash("xf_keycurve"):
        crossfadeKeyCurve = opcode.read(Default::crossfadeCurve);
        break;
    case hash("xf_velcurve"):
        crossfadeVelCurve = opcode.read(Default::crossfadeCurve);
        break;
    case hash("xfin_locc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        crossfadeCCInRange[opcode.parameters.back()].setStart(
            opcode.read(Default::xfinLo)
        );
        break;
    case hash("xfin_hicc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        crossfadeCCInRange[opcode.parameters.back()].setEnd(
            opcode.read(Default::xfinHi)
        );
        break;
    case hash("xfout_locc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        crossfadeCCOutRange[opcode.parameters.back()].setStart(
            opcode.read(Default::xfoutLo)
        );
        break;
    case hash("xfout_hicc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;
        crossfadeCCOutRange[opcode.parameters.back()].setEnd(
            opcode.read(Default::xfoutHi)
        );
        break;
    case hash("xf_cccurve"):
        crossfadeCCCurve = opcode.read(Default::crossfadeCurve);
        break;
    case hash("rt_decay"):
        rtDecay = opcode.read(Default::rtDecay);
        break;
    case hash("global_amplitude"):
        globalAmplitude = opcode.read(Default::amplitude);
        break;
    case hash("master_amplitude"):
        masterAmplitude = opcode.read(Default::amplitude);
        break;
    case hash("group_amplitude"):
        groupAmplitude = opcode.read(Default::amplitude);
        break;
    case hash("global_volume"):
        globalVolume = opcode.read(Default::volume);
        break;
    case hash("master_volume"):
        masterVolume = opcode.read(Default::volume);
        break;
    case hash("group_volume"):
        groupVolume = opcode.read(Default::volume);
        break;

    // Performance parameters: filters
    case hash("cutoff&"): // also cutoff
        {
            const auto filterIndex = opcode.parameters.empty() ? 0 : (opcode.parameters.back() - 1);
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].cutoff = opcode.read(Default::filterCutoff);
        }
        break;
    case hash("resonance&"): // also resonance
        {
            const auto filterIndex = opcode.parameters.empty() ? 0 : (opcode.parameters.back() - 1);
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].resonance = opcode.read(Default::filterResonance);
        }
        break;
    case_any_ccN("cutoff&"): // also cutoff_oncc&, cutoff_cc&, cutoff&_cc&
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            processGenericCc(opcode, Default::filterCutoffMod, ModKey::createNXYZ(ModId::FilCutoff, id, filterIndex));
        }
        break;
    case_any_ccN("resonance&"): // also resonance_oncc&, resonance_cc&, resonance&_cc&
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            processGenericCc(opcode, Default::filterResonanceMod, ModKey::createNXYZ(ModId::FilResonance, id, filterIndex));
        }
        break;
    case hash("cutoff&_chanaft"):
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            const ModKey source = ModKey::createNXYZ(ModId::ChannelAftertouch);
            const ModKey target = ModKey::createNXYZ(ModId::FilCutoff, id, filterIndex);
            getOrCreateConnection(source, target).sourceDepth = opcode.read(Default::filterCutoffMod);
        }
        break;
    case hash("cutoff&_polyaft"):
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            const ModKey source = ModKey::createNXYZ(ModId::PolyAftertouch, id);
            const ModKey target = ModKey::createNXYZ(ModId::FilCutoff, id, filterIndex);
            getOrCreateConnection(source, target).sourceDepth = opcode.read(Default::filterCutoffMod);
        }
        break;
    case hash("fil&_keytrack"): // also fil_keytrack
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].keytrack = opcode.read(Default::filterKeytrack);
        }
        break;
    case hash("fil&_keycenter"): // also fil_keycenter
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].keycenter = opcode.read(Default::key);
        }
        break;
    case hash("fil&_veltrack"): // also fil_veltrack
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].veltrack = opcode.read(Default::filterVeltrack);
        }
        break;
    case hash("fil&_veltrack_oncc&"):
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            const auto cc = opcode.parameters.back();
            if (cc >= config::numCCs)
                return false;

            filters[filterIndex].veltrackCC[cc].modifier = opcode.read(Default::filterVeltrackMod);
        }
        break;
    case hash("fil&_veltrack_curvecc&"):
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            const auto cc = opcode.parameters.back();
            if (cc >= config::numCCs)
                return false;

            filters[filterIndex].veltrackCC[cc].curve = opcode.read(Default::curveCC);
        }
        break;
    case hash("fil&_random"): // also fil_random, cutoff_random, cutoff&_random
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].random = opcode.read(Default::filterRandom);
        }
        break;
    case hash("fil&_gain"): // also fil_gain
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;
            filters[filterIndex].gain = opcode.read(Default::filterGain);
        }
        break;
    case_any_ccN("fil&_gain"): // also fil_gain_oncc&
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            processGenericCc(opcode, Default::filterGainMod, ModKey::createNXYZ(ModId::FilGain, id, filterIndex));
        }
        break;
    case hash("fil&_type"): // also fil_type, filtype
        {
            const auto filterIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(filters, filterIndex + 1, Default::numFilters))
                return false;

            filters[filterIndex].type = opcode.read(Default::filter);
        }
        break;

    // Performance parameters: EQ
    case hash("eq&_bw"):
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;
            equalizers[eqIndex].bandwidth = opcode.read(Default::eqBandwidth);
        }
        break;
    case_any_ccN("eq&_bw"): // also eq&_bwcc&
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;

            processGenericCc(opcode, Default::eqBandwidthMod, ModKey::createNXYZ(ModId::EqBandwidth, id, eqIndex));
        }
        break;
    case hash("eq&_freq"):
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;
            equalizers[eqIndex].frequency = opcode.read(Default::eqFrequency);
        }
        break;
    case_any_ccN("eq&_freq"): // also eq&_freqcc&
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;

            processGenericCc(opcode, Default::eqFrequencyMod, ModKey::createNXYZ(ModId::EqFrequency, id, eqIndex));
        }
        break;
    case hash("eq&_veltofreq"): // also eq&_vel2freq
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;
            equalizers[eqIndex].vel2frequency = opcode.read(Default::eqVel2Frequency);
        }
        break;
    case hash("eq&_gain"):
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;
            equalizers[eqIndex].gain = opcode.read(Default::eqGain);
        }
        break;
    case_any_ccN("eq&_gain"): // also eq&_gaincc&
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;

            processGenericCc(opcode, Default::eqGainMod, ModKey::createNXYZ(ModId::EqGain, id, eqIndex));
        }
        break;
    case hash("eq&_veltogain"): // also eq&_vel2gain
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;
            equalizers[eqIndex].vel2gain = opcode.read(Default::eqVel2Gain);
        }
        break;
    case hash("eq&_type"):
        {
            const auto eqIndex = opcode.parameters.front() - 1;
            if (!extendIfNecessary(equalizers, eqIndex + 1, Default::numEQs))
                return false;

            equalizers[eqIndex].type =
                opcode.read(Default::eq);

       }
        break;

    // Performance parameters: pitch
    case hash("pitch_keycenter"):
        if (opcode.value == "sample")
            pitchKeycenterFromSample = true;
        else {
            pitchKeycenterFromSample = false;
            pitchKeycenter = opcode.read(Default::key);
        }
        break;
    case hash("pitch_keytrack"):
        pitchKeytrack = opcode.read(Default::pitchKeytrack);
        break;
    case hash("pitch_veltrack"):
        pitchVeltrack = opcode.read(Default::pitchVeltrack);
        break;
    case hash("pitch_veltrack_oncc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        pitchVeltrackCC[opcode.parameters.back()].modifier = opcode.read(Default::pitchVeltrackMod);
        break;
    case hash("pitch_veltrack_curvecc&"):
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        pitchVeltrackCC[opcode.parameters.back()].curve = opcode.read(Default::curveCC);
        break;
    case hash("pitch_random"):
        pitchRandom = opcode.read(Default::pitchRandom);
        break;
    case hash("transpose"):
        transpose = opcode.read(Default::transpose);
        break;
    case hash("pitch"): // also tune
        pitch = opcode.read(Default::pitch);
        break;
    case_any_ccN("pitch"): // also tune
        processGenericCc(opcode, Default::pitchMod, ModKey::createNXYZ(ModId::Pitch, id));
        break;
    case hash("bend_up"): // also bendup
        bendUp = opcode.read(Default::bendUp);
        break;
    case hash("bend_down"): // also benddown
        bendDown = opcode.read(Default::bendDown);
        break;
    case hash("bend_step"):
        bendStep = opcode.read(Default::bendStep);
        break;
    case hash("bend_smooth"):
        bendSmooth = opcode.read(Default::smoothCC);
        break;

    case hash("effect&"):
    {
        const auto effectNumber = opcode.parameters.back();
        if (!effectNumber || effectNumber < 1 || effectNumber > config::maxEffectBuses)
            break;
        if (static_cast<size_t>(effectNumber + 1) > gainToEffect.size())
            gainToEffect.resize(effectNumber + 1);
        gainToEffect[effectNumber] = opcode.read(Default::effect);
        break;
    }
    case hash("sw_default"):
        defaultSwitch = opcode.read(Default::key);
        break;

    // Ignored opcodes
    case hash("hichan"):
    case hash("lochan"):
    case hash("ampeg_depth"):
    case hash("ampeg_veltodepth"): // also ampeg_vel2depth
        break;

    default: {
        // Amplitude Envelope
        if (absl::StartsWith(opcode.name, "ampeg_")) {
            if (parseEGOpcode(opcode, amplitudeEG))
                return true;
        }
        // Pitch Envelope
        if (absl::StartsWith(opcode.name, "pitcheg_")) {
            if (parseEGOpcode(opcode, pitchEG)) {
                getOrCreateConnection(
                    ModKey::createNXYZ(ModId::PitchEG, id),
                    ModKey::createNXYZ(ModId::Pitch, id));
                return true;
            }
        }
        // Filter Envelope
        if (absl::StartsWith(opcode.name, "fileg_")) {
            if (parseEGOpcode(opcode, filterEG)) {
                getOrCreateConnection(
                    ModKey::createNXYZ(ModId::FilEG, id),
                    ModKey::createNXYZ(ModId::FilCutoff, id, 0));
                return true;
            }
        }

        // Amplitude LFO
        if (absl::StartsWith(opcode.name, "amplfo_")) {
            if (parseLFOOpcode(opcode, amplitudeLFO)) {
                getOrCreateConnection(
                    ModKey::createNXYZ(ModId::AmpLFO, id),
                    ModKey::createNXYZ(ModId::Volume, id));
                return true;
            }
        }
        // Pitch LFO
        if (absl::StartsWith(opcode.name, "pitchlfo_")) {
            if (parseLFOOpcode(opcode, pitchLFO)) {
                getOrCreateConnection(
                    ModKey::createNXYZ(ModId::PitchLFO, id),
                    ModKey::createNXYZ(ModId::Pitch, id));
                return true;
            }
        }
        // Filter LFO
        if (absl::StartsWith(opcode.name, "fillfo_")) {
            if (parseLFOOpcode(opcode, filterLFO)) {
                getOrCreateConnection(
                    ModKey::createNXYZ(ModId::FilLFO, id),
                    ModKey::createNXYZ(ModId::FilCutoff, id, 0));
                return true;
            }
        }

        //
        const std::string letterOnlyName = opcode.getLetterOnlyName();

        // Modulation: LFO
        if (absl::StartsWith(letterOnlyName, "lfo&_")) {
            if (parseLFOOpcodeV2(opcode))
                return true;
        }
        // Modulation: Flex EG
        if (absl::StartsWith(letterOnlyName, "eg&_")) {
            if (parseEGOpcodeV2(opcode))
                return true;
        }

        return false;
    }

    }

    return true;
}

bool sfz::Region::parseLFOOpcode(const Opcode& opcode, LFODescription& lfo)
{
    #define case_any_lfo(param)                     \
        case hash("amplfo_" param):                 \
        case hash("pitchlfo_" param):               \
        case hash("fillfo_" param)                  \

    #define case_any_lfo_any_ccN(param)             \
        case_any_ccN("amplfo_" param):              \
        case_any_ccN("pitchlfo_" param):            \
        case_any_ccN("fillfo_" param)               \

    //
    ModKey sourceKey;
    ModKey sourceDepthKey;
    ModKey targetKey;
    OpcodeSpec<float> depthSpec;
    OpcodeSpec<float> depthModSpec;

    if (absl::StartsWith(opcode.name, "amplfo_")) {
        sourceKey = ModKey::createNXYZ(ModId::AmpLFO, id);
        sourceDepthKey = ModKey::createNXYZ(ModId::AmpLFODepth, id);
        targetKey = ModKey::createNXYZ(ModId::Volume, id);
        lfo.freqKey = ModKey::createNXYZ(ModId::AmpLFOFrequency, id);
        depthSpec = Default::ampLFODepth;
        depthModSpec = Default::volumeMod;
    }
    else if (absl::StartsWith(opcode.name, "pitchlfo_")) {
        sourceKey = ModKey::createNXYZ(ModId::PitchLFO, id);
        sourceDepthKey = ModKey::createNXYZ(ModId::PitchLFODepth, id);
        targetKey = ModKey::createNXYZ(ModId::Pitch, id);
        lfo.freqKey = ModKey::createNXYZ(ModId::PitchLFOFrequency, id);
        depthSpec = Default::pitchLFODepth;
        depthModSpec = Default::pitchMod;
    }
    else if (absl::StartsWith(opcode.name, "fillfo_")) {
        sourceKey = ModKey::createNXYZ(ModId::FilLFO, id);
        sourceDepthKey = ModKey::createNXYZ(ModId::FilLFODepth, id);
        targetKey = ModKey::createNXYZ(ModId::FilCutoff, id, 0);
        lfo.freqKey = ModKey::createNXYZ(ModId::FilLFOFrequency, id);
        depthSpec = Default::filLFODepth;
        depthModSpec = Default::filterCutoffMod;
    }
    else {
        ASSERTFALSE;
        return false;
    }

    //
    switch (opcode.lettersOnlyHash) {

    case_any_lfo("delay"):
        lfo.delay = opcode.read(Default::lfoDelay);
        break;
    case_any_lfo("depth"):
        getOrCreateConnection(sourceKey, targetKey).sourceDepth = opcode.read(depthSpec);
        break;
    case_any_lfo_any_ccN("depth"): // also depthcc&
        getOrCreateConnection(sourceKey, targetKey).sourceDepthMod = sourceDepthKey;
        processGenericCc(opcode, depthModSpec, sourceDepthKey);
        break;
    case_any_lfo("depthchanaft"):
        getOrCreateConnection(sourceKey, targetKey).sourceDepthMod = sourceDepthKey;
        getOrCreateConnection(ModKey::createNXYZ(ModId::ChannelAftertouch), sourceDepthKey).sourceDepth
            = opcode.read(depthModSpec);
        break;
    case_any_lfo("depthpolyaft"):
        getOrCreateConnection(sourceKey, targetKey).sourceDepthMod = sourceDepthKey;
        getOrCreateConnection(ModKey::createNXYZ(ModId::PolyAftertouch, id), sourceDepthKey).sourceDepth
            = opcode.read(depthModSpec);
        break;
    case_any_lfo("fade"):
        lfo.fade = opcode.read(Default::lfoFade);
        break;
    case_any_lfo("freq"):
        lfo.freq = opcode.read(Default::lfoFreq);
        break;
    case_any_lfo_any_ccN("freq"): // also freqcc&
        processGenericCc(opcode, Default::lfoFreqMod, lfo.freqKey);
        break;
    case_any_lfo("freqchanaft"):
        getOrCreateConnection(ModKey::createNXYZ(ModId::ChannelAftertouch), lfo.freqKey).sourceDepth
            = opcode.read(Default::lfoFreqMod);
        break;
    case_any_lfo("freqpolyaft"):
        getOrCreateConnection(ModKey::createNXYZ(ModId::PolyAftertouch, id), lfo.freqKey).sourceDepth
            = opcode.read(Default::lfoFreqMod);
        break;

    // sfizz extension
    case_any_lfo("wave"):
        lfo.sub[0].wave = opcode.read(Default::lfoWave);
        break;

    default:
        return false;
    }

    #undef case_any_lfo

    return true;
}

bool sfz::Region::parseLFOOpcode(const Opcode& opcode, absl::optional<LFODescription>& lfo)
{
    bool create = lfo == absl::nullopt;
    if (create) {
        lfo = LFODescription();
        lfo->sub[0].wave = LFOWave::Sine; // the LFO v1 default
    }

    bool parsed = parseLFOOpcode(opcode, *lfo);
    if (!parsed && create)
        lfo = absl::nullopt;

    return parsed;
}

bool sfz::Region::parseEGOpcode(const Opcode& opcode, EGDescription& eg)
{
    #define case_any_eg(param)                      \
        case hash("ampeg_" param):                  \
        case hash("pitcheg_" param):                \
        case hash("fileg_" param)                   \

    switch (opcode.lettersOnlyHash) {
    case_any_eg("attack"):
        eg.attack = opcode.read(Default::egTime);
        break;
    case_any_eg("decay"):
        eg.decay = opcode.read(Default::egTime);
        break;
    case_any_eg("delay"):
        eg.delay = opcode.read(Default::egTime);
        break;
    case_any_eg("hold"):
        eg.hold = opcode.read(Default::egTime);
        break;
    case_any_eg("release"):
        eg.release = opcode.read(Default::egRelease);
        break;
    case_any_eg("start"):
        eg.start = opcode.read(Default::egPercent);
        break;
    case_any_eg("sustain"):
        eg.sustain = opcode.read(Default::egPercent);
        break;
    case_any_eg("veltoattack"): // also vel2attack
        eg.vel2attack = opcode.read(Default::egTimeMod);
        break;
    case_any_eg("veltodecay"): // also vel2decay
        eg.vel2decay = opcode.read(Default::egTimeMod);
        break;
    case_any_eg("veltodelay"): // also vel2delay
        eg.vel2delay = opcode.read(Default::egTimeMod);
        break;
    case_any_eg("veltohold"): // also vel2hold
        eg.vel2hold = opcode.read(Default::egTimeMod);
        break;
    case_any_eg("veltorelease"): // also vel2release
        eg.vel2release = opcode.read(Default::egTimeMod);
        break;
    case_any_eg("veltosustain"): // also vel2sustain
        eg.vel2sustain = opcode.read(Default::egPercentMod);
        break;
    case_any_eg("attack_oncc&"): // also attackcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccAttack[opcode.parameters.back()] = opcode.read(Default::egTimeMod);

        break;
    case_any_eg("decay_oncc&"): // also decaycc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccDecay[opcode.parameters.back()] = opcode.read(Default::egTimeMod);

        break;
    case_any_eg("delay_oncc&"): // also delaycc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccDelay[opcode.parameters.back()] = opcode.read(Default::egTimeMod);

        break;
    case_any_eg("hold_oncc&"): // also holdcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccHold[opcode.parameters.back()] = opcode.read(Default::egTimeMod);

        break;
    case_any_eg("release_oncc&"): // also releasecc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccRelease[opcode.parameters.back()] = opcode.read(Default::egTimeMod);

        break;
    case_any_eg("start_oncc&"): // also startcc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccStart[opcode.parameters.back()] = opcode.read(Default::egPercentMod);

        break;
    case_any_eg("sustain_oncc&"): // also sustaincc&
        if (opcode.parameters.back() >= config::numCCs)
            return false;

        eg.ccSustain[opcode.parameters.back()] = opcode.read(Default::egPercentMod);

        break;

    case_any_eg("dynamic"):
        eg.dynamic = opcode.read(Default::egDynamic);
        break;

    case hash("pitcheg_depth"):
        getOrCreateConnection(
            ModKey::createNXYZ(ModId::PitchEG, id),
            ModKey::createNXYZ(ModId::Pitch, id)).sourceDepth = opcode.read(Default::egDepth);
        break;
    case hash("fileg_depth"):
        getOrCreateConnection(
            ModKey::createNXYZ(ModId::FilEG, id),
            ModKey::createNXYZ(ModId::FilCutoff, id, 0)).sourceDepth = opcode.read(Default::egDepth);
        break;

    case hash("pitcheg_veltodepth"): // also pitcheg_vel2depth
        getOrCreateConnection(
            ModKey::createNXYZ(ModId::PitchEG, id),
            ModKey::createNXYZ(ModId::Pitch, id)).velToDepth = opcode.read(Default::egVel2Depth);
        break;
    case hash("fileg_veltodepth"): // also fileg_vel2depth
        getOrCreateConnection(
            ModKey::createNXYZ(ModId::FilEG, id),
            ModKey::createNXYZ(ModId::FilCutoff, id, 0)).velToDepth = opcode.read(Default::egVel2Depth);
        break;

    case_any_ccN("pitcheg_depth"):
        getOrCreateConnection(
            ModKey::createNXYZ(ModId::PitchEG, id),
            ModKey::createNXYZ(ModId::Pitch, id)).sourceDepthMod = ModKey::createNXYZ(ModId::PitchEGDepth, id);
        processGenericCc(opcode, Default::pitchMod, ModKey::createNXYZ(ModId::PitchEGDepth, id));
        break;
    case_any_ccN("fileg_depth"):
        getOrCreateConnection(
            ModKey::createNXYZ(ModId::FilEG, id),
            ModKey::createNXYZ(ModId::FilCutoff, id, 0)).sourceDepthMod = ModKey::createNXYZ(ModId::FilEGDepth, id);
        processGenericCc(opcode, Default::filterCutoffMod, ModKey::createNXYZ(ModId::FilEGDepth, id));
        break;

    default:
        return false;
    }

    return true;

    #undef case_any_eg
}

bool sfz::Region::parseEGOpcode(const Opcode& opcode, absl::optional<EGDescription>& eg)
{
    bool create = eg == absl::nullopt;
    if (create)
        eg = EGDescription();

    bool parsed = parseEGOpcode(opcode, *eg);
    if (!parsed && create)
        eg = absl::nullopt;

    return parsed;
}

bool sfz::Region::parseLFOOpcodeV2(const Opcode& opcode)
{
    const unsigned lfoNumber1Based = opcode.parameters.front();

    if (lfoNumber1Based <= 0)
        return false;
    if (!extendIfNecessary(lfos, lfoNumber1Based, Default::numLFOs))
        return false;

    const unsigned lfoNumber = lfoNumber1Based - 1;
    LFODescription& lfo = lfos[lfoNumber];

    //
    lfo.beatsKey = ModKey::createNXYZ(ModId::LFOBeats, id, lfoNumber);
    lfo.freqKey = ModKey::createNXYZ(ModId::LFOFrequency, id, lfoNumber);
    lfo.phaseKey = ModKey::createNXYZ(ModId::LFOPhase, id, lfoNumber);

    //
    auto getOrCreateLFOStep = [&opcode, &lfo]() -> float* {
        const unsigned stepNumber1Based = opcode.parameters[1];
        if (stepNumber1Based <= 0 || stepNumber1Based > config::maxLFOSteps)
            return nullptr;
        if (!lfo.seq)
            lfo.seq = LFODescription::StepSequence();
        if (!extendIfNecessary(lfo.seq->steps, stepNumber1Based, Default::numLFOSteps))
            return nullptr;
        return &lfo.seq->steps[stepNumber1Based - 1];
    };
    auto getOrCreateLFOSub = [&opcode, &lfo]() -> LFODescription::Sub* {
        const unsigned subNumber1Based = opcode.parameters[1];
        if (subNumber1Based <= 0 || subNumber1Based > config::maxLFOSubs)
            return nullptr;
        if (!extendIfNecessary(lfo.sub, subNumber1Based, Default::numLFOSubs))
            return nullptr;
        return &lfo.sub[subNumber1Based - 1];
    };
    auto LFO_target = [this, &opcode, lfoNumber](const ModKey& target, const OpcodeSpec<float>& spec) -> bool {
        const ModKey source = ModKey::createNXYZ(ModId::LFO, id, lfoNumber);
        getOrCreateConnection(source, target).sourceDepth = opcode.read(spec);
        return true;
    };
    auto LFO_target_cc = [this, &opcode, lfoNumber](const ModKey& target, const OpcodeSpec<float>& spec) -> bool {
        const ModKey source = ModKey::createNXYZ(ModId::LFO, id, lfoNumber);
        const ModKey depth = ModKey::getSourceDepthKey(source, target);
        ASSERT(depth);
        Connection& conn = getOrCreateConnection(source, target);
        conn.sourceDepthMod = depth;
        processGenericCc(opcode, spec, depth);
        return true;
    };
    auto ensureFilter = [this, &opcode]() {
        ASSERT(opcode.parameters.size() >= 2);
        const unsigned index = opcode.parameters[1] - 1;
        return extendIfNecessary(filters, index + 1, Default::numFilters);
    };
    auto ensureEQ = [this, &opcode]() {
        ASSERT(opcode.parameters.size() >= 2);
        const unsigned index = opcode.parameters[1] - 1;
        return extendIfNecessary(equalizers, index + 1, Default::numEQs);
    };

    //
    switch (opcode.lettersOnlyHash) {

    // Modulation: LFO
    case hash("lfo&_freq"):
        lfo.freq = opcode.read(Default::lfoFreq);
        break;
    case_any_ccN("lfo&_freq"):
        processGenericCc(opcode, Default::lfoFreqMod, ModKey::createNXYZ(ModId::LFOFrequency, id, lfoNumber));
        break;
    case hash("lfo&_beats"):
        lfo.beats = opcode.read(Default::lfoBeats);
        break;
    case_any_ccN("lfo&_beats"):
        processGenericCc(opcode, Default::lfoBeatsMod, ModKey::createNXYZ(ModId::LFOBeats, id, lfoNumber));
        break;
    case hash("lfo&_phase"):
        lfo.phase0 = opcode.read(Default::lfoPhase);
        break;
    case_any_ccN("lfo&_phase"):
        processGenericCc(opcode, Default::lfoPhaseMod, ModKey::createNXYZ(ModId::LFOPhase, id, lfoNumber));
        break;
    case hash("lfo&_delay"):
        lfo.delay = opcode.read(Default::lfoDelay);
        break;
    case hash("lfo&_delay_oncc&"):
        if (opcode.parameters.back() > config::numCCs)
            return false;

        lfo.delayCC[opcode.parameters.back()] = opcode.read(Default::lfoDelayMod);
        break;
    case hash("lfo&_fade"):
        lfo.fade = opcode.read(Default::lfoFade);
        break;
    case hash("lfo&_fade_oncc&"):
        if (opcode.parameters.back() > config::numCCs)
            return false;

        lfo.fadeCC[opcode.parameters.back()] = opcode.read(Default::lfoFadeMod);
        break;
    case hash("lfo&_count"):
        lfo.count = opcode.read(Default::lfoCount);
        break;
    case hash("lfo&_steps"):
        if (!lfo.seq)
            lfo.seq = LFODescription::StepSequence();
        lfo.seq->steps.resize(opcode.read(Default::lfoSteps));
        break;
    case hash("lfo&_step&"):
        if (float* step = getOrCreateLFOStep())
            *step = opcode.read(Default::lfoStepX);
        else
            return false;
        break;
    case hash("lfo&_wave&"): // also lfo&_wave
        if (LFODescription::Sub* sub = getOrCreateLFOSub())
            sub->wave = opcode.read(Default::lfoWave);
        else
            return false;
        break;
    case hash("lfo&_offset&"): // also lfo&_offset
        if (LFODescription::Sub* sub = getOrCreateLFOSub())
            sub->offset = opcode.read(Default::lfoOffset);
        else
            return false;
        break;
    case hash("lfo&_ratio&"): // also lfo&_ratio
        if (LFODescription::Sub* sub = getOrCreateLFOSub())
            sub->ratio = opcode.read(Default::lfoRatio);
        else
            return false;
        break;
    case hash("lfo&_scale&"): // also lfo&_scale
        if (LFODescription::Sub* sub = getOrCreateLFOSub())
            sub->scale = opcode.read(Default::lfoScale);
        else
            return false;
        break;

    // Modulation: LFO (targets)
    case hash("lfo&_amplitude"):
        LFO_target(ModKey::createNXYZ(ModId::Amplitude, id), Default::amplitudeMod);
        break;
    case_any_ccN("lfo&_amplitude"):
        LFO_target_cc(ModKey::createNXYZ(ModId::Amplitude, id), Default::amplitudeMod);
        break;
    case hash("lfo&_pan"):
        LFO_target(ModKey::createNXYZ(ModId::Pan, id), Default::panMod);
        break;
    case_any_ccN("lfo&_pan"):
        LFO_target_cc(ModKey::createNXYZ(ModId::Pan, id), Default::panMod);
        break;
    case hash("lfo&_width"):
        LFO_target(ModKey::createNXYZ(ModId::Width, id), Default::widthMod);
        break;
    case_any_ccN("lfo&_width"):
        LFO_target_cc(ModKey::createNXYZ(ModId::Width, id), Default::widthMod);
        break;
    case hash("lfo&_position"): // sfizz extension
        LFO_target(ModKey::createNXYZ(ModId::Position, id), Default::positionMod);
        break;
    case_any_ccN("lfo&_position"): // sfizz extension
        LFO_target_cc(ModKey::createNXYZ(ModId::Position, id), Default::positionMod);
        break;
    case hash("lfo&_pitch"):
        LFO_target(ModKey::createNXYZ(ModId::Pitch, id), Default::pitchMod);
        break;
    case_any_ccN("lfo&_pitch"):
        LFO_target_cc(ModKey::createNXYZ(ModId::Pitch, id), Default::pitchMod);
        break;
    case hash("lfo&_volume"):
        LFO_target(ModKey::createNXYZ(ModId::Volume, id), Default::volumeMod);
        break;
    case_any_ccN("lfo&_volume"):
        LFO_target_cc(ModKey::createNXYZ(ModId::Volume, id), Default::volumeMod);
        break;
    case hash("lfo&_cutoff&"):
        if (!ensureFilter())
            return false;
        LFO_target(ModKey::createNXYZ(ModId::FilCutoff, id, opcode.parameters[1] - 1), Default::filterCutoffMod);
        break;
    case_any_ccN("lfo&_cutoff&"):
        if (!ensureFilter())
            return false;
        LFO_target_cc(ModKey::createNXYZ(ModId::FilCutoff, id, opcode.parameters[1] - 1), Default::filterCutoffMod);
        break;
    case hash("lfo&_resonance&"):
        if (!ensureFilter())
            return false;
        LFO_target(ModKey::createNXYZ(ModId::FilResonance, id, opcode.parameters[1] - 1), Default::filterResonanceMod);
        break;
    case_any_ccN("lfo&_resonance&"):
        if (!ensureFilter())
            return false;
        LFO_target_cc(ModKey::createNXYZ(ModId::FilResonance, id, opcode.parameters[1] - 1), Default::filterResonanceMod);
        break;
    case hash("lfo&_fil&gain"):
        if (!ensureFilter())
            return false;
        LFO_target(ModKey::createNXYZ(ModId::FilGain, id, opcode.parameters[1] - 1), Default::filterGainMod);
        break;
    case_any_ccN("lfo&_fil&gain"):
        if (!ensureFilter())
            return false;
        LFO_target_cc(ModKey::createNXYZ(ModId::FilGain, id, opcode.parameters[1] - 1), Default::filterGainMod);
        break;
    case hash("lfo&_eq&gain"):
        if (!ensureEQ())
            return false;
        LFO_target(ModKey::createNXYZ(ModId::EqGain, id, opcode.parameters[1] - 1), Default::eqGainMod);
        break;
    case_any_ccN("lfo&_eq&gain"):
        if (!ensureEQ())
            return false;
        LFO_target_cc(ModKey::createNXYZ(ModId::EqGain, id, opcode.parameters[1] - 1), Default::eqGainMod);
        break;
    case hash("lfo&_eq&freq"):
        if (!ensureEQ())
            return false;
        LFO_target(ModKey::createNXYZ(ModId::EqFrequency, id, opcode.parameters[1] - 1), Default::eqFrequencyMod);
        break;
    case_any_ccN("lfo&_eq&freq"):
        if (!ensureEQ())
            return false;
        LFO_target_cc(ModKey::createNXYZ(ModId::EqFrequency, id, opcode.parameters[1] - 1), Default::eqFrequencyMod);
        break;
    case hash("lfo&_eq&bw"):
        if (!ensureEQ())
            return false;
        LFO_target(ModKey::createNXYZ(ModId::EqBandwidth, id, opcode.parameters[1] - 1), Default::eqBandwidthMod);
        break;
    case_any_ccN("lfo&_eq&bw"):
        if (!ensureEQ())
            return false;
        LFO_target_cc(ModKey::createNXYZ(ModId::EqBandwidth, id, opcode.parameters[1] - 1), Default::eqBandwidthMod);
        break;

    default:
        return false;
    }

    return true;
}

bool sfz::Region::parseEGOpcodeV2(const Opcode& opcode)
{
    const unsigned egNumber1Based = opcode.parameters.front();
    if (egNumber1Based <= 0)
        return false;
    if (!extendIfNecessary(flexEGs, egNumber1Based, Default::numFlexEGs))
        return false;

    const unsigned egNumber = egNumber1Based - 1;
    FlexEGDescription& eg = flexEGs[egNumber];

    //
    auto getOrCreateEGPoint = [&opcode, &eg]() -> FlexEGPoint* {
        const auto pointNumber = opcode.parameters[1];
        if (!extendIfNecessary(eg.points, pointNumber + 1, Default::numFlexEGPoints))
            return nullptr;
        return &eg.points[pointNumber];
    };
    auto EG_target = [this, &opcode, egNumber](const ModKey& target, const OpcodeSpec<float>& spec) -> bool {
        const ModKey source = ModKey::createNXYZ(ModId::Envelope, id, egNumber);
        getOrCreateConnection(source, target).sourceDepth = opcode.read(spec);
        return true;
    };
    auto EG_target_cc = [this, &opcode, egNumber](const ModKey& target, const OpcodeSpec<float>& spec) -> bool {
        const ModKey source = ModKey::createNXYZ(ModId::Envelope, id, egNumber);
        const ModKey depth = ModKey::getSourceDepthKey(source, target);
        ASSERT(depth);
        Connection& conn = getOrCreateConnection(source, target);
        conn.sourceDepthMod = depth;
        processGenericCc(opcode, spec, depth);
        return true;
    };
    auto ensureFilter = [this, &opcode]() {
        ASSERT(opcode.parameters.size() >= 2);
        const unsigned index = opcode.parameters[1] - 1;
        return extendIfNecessary(filters, index + 1, Default::numFilters);
    };
    auto ensureEQ = [this, &opcode]() {
        ASSERT(opcode.parameters.size() >= 2);
        const unsigned index = opcode.parameters[1] - 1;
        return extendIfNecessary(equalizers, index + 1, Default::numEQs);
    };

    //
    switch (opcode.lettersOnlyHash) {

    // Flex envelopes
    case hash("eg&_dynamic"):
        eg.dynamic = opcode.read(Default::flexEGDynamic);
        break;
    case hash("eg&_sustain"):
        eg.sustain = opcode.read(Default::flexEGSustain);
        break;
    case hash("eg&_time&"):
        if (FlexEGPoint* point = getOrCreateEGPoint())
            point->time = opcode.read(Default::flexEGPointTime);
        else
            return false;
        break;
    case hash("eg&_time&_oncc&"):
        if (FlexEGPoint* point = getOrCreateEGPoint()) {
            auto ccNumber = opcode.parameters.back();
            if (ccNumber >= config::numCCs)
                return false;
            point->ccTime[ccNumber] = opcode.read(Default::flexEGPointTimeMod);
        }
        else
            return false;
        break;
    case hash("eg&_level&"):
        if (FlexEGPoint* point = getOrCreateEGPoint())
            point->level = opcode.read(Default::flexEGPointLevel);
        else
            return false;
        break;
    case hash("eg&_level&_oncc&"):
        if (FlexEGPoint* point = getOrCreateEGPoint()) {
            auto ccNumber = opcode.parameters.back();
            if (ccNumber >= config::numCCs)
                return false;
            point->ccLevel[ccNumber] = opcode.read(Default::flexEGPointLevelMod);
        }
        else
            return false;
        break;
    case hash("eg&_shape&"):
        if (FlexEGPoint* point = getOrCreateEGPoint())
            point->setShape(opcode.read(Default::flexEGPointShape));
        else
            return false;
        break;

    // Modulation: Flex EG (targets)
    case hash("eg&_amplitude"):
        EG_target(ModKey::createNXYZ(ModId::Amplitude, id), Default::amplitudeMod);
        break;
    case_any_ccN("eg&_amplitude"):
        EG_target_cc(ModKey::createNXYZ(ModId::Amplitude, id), Default::amplitudeMod);
        break;
    case hash("eg&_pan"):
        EG_target(ModKey::createNXYZ(ModId::Pan, id), Default::panMod);
        break;
    case_any_ccN("eg&_pan"):
        EG_target_cc(ModKey::createNXYZ(ModId::Pan, id), Default::panMod);
        break;
    case hash("eg&_width"):
        EG_target(ModKey::createNXYZ(ModId::Width, id), Default::widthMod);
        break;
    case_any_ccN("eg&_width"):
        EG_target_cc(ModKey::createNXYZ(ModId::Width, id), Default::widthMod);
        break;
    case hash("eg&_position"): // sfizz extension
        EG_target(ModKey::createNXYZ(ModId::Position, id), Default::positionMod);
        break;
    case_any_ccN("eg&_position"): // sfizz extension
        EG_target_cc(ModKey::createNXYZ(ModId::Position, id), Default::positionMod);
        break;
    case hash("eg&_pitch"):
        EG_target(ModKey::createNXYZ(ModId::Pitch, id), Default::pitchMod);
        break;
    case_any_ccN("eg&_pitch"):
        EG_target_cc(ModKey::createNXYZ(ModId::Pitch, id), Default::pitchMod);
        break;
    case hash("eg&_volume"):
        EG_target(ModKey::createNXYZ(ModId::Volume, id), Default::volumeMod);
        break;
    case_any_ccN("eg&_volume"):
        EG_target_cc(ModKey::createNXYZ(ModId::Volume, id), Default::volumeMod);
        break;
    case hash("eg&_cutoff&"):
        if (!ensureFilter())
            return false;
        EG_target(ModKey::createNXYZ(ModId::FilCutoff, id, opcode.parameters[1] - 1), Default::filterCutoffMod);
        break;
    case_any_ccN("eg&_cutoff&"):
        if (!ensureFilter())
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::FilCutoff, id, opcode.parameters[1] - 1), Default::filterCutoffMod);
        break;
    case hash("eg&_resonance&"):
        if (!ensureFilter())
            return false;
        EG_target(ModKey::createNXYZ(ModId::FilResonance, id, opcode.parameters[1] - 1), Default::filterResonanceMod);
        break;
    case_any_ccN("eg&_resonance&"):
        if (!ensureFilter())
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::FilResonance, id, opcode.parameters[1] - 1), Default::filterResonanceMod);
        break;
    case hash("eg&_fil&gain"):
        if (!ensureFilter())
            return false;
        EG_target(ModKey::createNXYZ(ModId::FilGain, id, opcode.parameters[1] - 1), Default::filterGainMod);
        break;
    case_any_ccN("eg&_fil&gain"):
        if (!ensureFilter())
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::FilGain, id, opcode.parameters[1] - 1), Default::filterGainMod);
        break;
    case hash("eg&_eq&gain"):
        if (!ensureEQ())
            return false;
        EG_target(ModKey::createNXYZ(ModId::EqGain, id, opcode.parameters[1] - 1), Default::eqGainMod);
        break;
    case_any_ccN("eg&_eq&gain"):
        if (!ensureEQ())
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::EqGain, id, opcode.parameters[1] - 1), Default::eqGainMod);
        break;
    case hash("eg&_eq&freq"):
        if (!ensureEQ())
            return false;
        EG_target(ModKey::createNXYZ(ModId::EqFrequency, id, opcode.parameters[1] - 1), Default::eqFrequencyMod);
        break;
    case_any_ccN("eg&_eq&freq"):
        if (!ensureEQ())
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::EqFrequency, id, opcode.parameters[1] - 1), Default::eqFrequencyMod);
        break;
    case hash("eg&_eq&bw"):
        if (!ensureEQ())
            return false;
        EG_target(ModKey::createNXYZ(ModId::EqBandwidth, id, opcode.parameters[1] - 1), Default::eqBandwidthMod);
        break;
    case_any_ccN("eg&_eq&bw"):
        if (!ensureEQ())
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::EqBandwidth, id, opcode.parameters[1] - 1), Default::eqBandwidthMod);
        break;

    case hash("eg&_ampeg"):
    {
        auto ampeg = opcode.read(Default::flexEGAmpeg);
        if (eg.ampeg != ampeg) {
            eg.ampeg = ampeg;
            flexAmpEG = absl::nullopt;
            for (size_t i = 0, n = flexEGs.size(); i < n && !flexAmpEG; ++i) {
                if (flexEGs[i].ampeg)
                    flexAmpEG = static_cast<uint8_t>(i);
            }
        }
        break;
    }

    case hash("eg&_freq_lfo&"):
        if (lfos.size() < opcode.parameters[1] - 1)
            return false;
        EG_target(ModKey::createNXYZ(ModId::LFOFrequency, id, opcode.parameters[1] - 1), Default::lfoFreqMod);
        break;

    case_any_ccN("eg&_freq_lfo&"):
        if (lfos.size() < opcode.parameters[1] - 1)
            return false;
        EG_target_cc(ModKey::createNXYZ(ModId::LFOFrequency, id, opcode.parameters[1] - 1), Default::lfoFreqMod);
        break;


    default:
        return false;
    }

    return true;
}

bool sfz::Region::processGenericCc(const Opcode& opcode, OpcodeSpec<float> spec, const ModKey& target)
{
    if (!opcode.isAnyCcN())
        return false;

    const auto ccNumber = opcode.parameters.back();
    if (ccNumber >= config::numCCs)
        return false;

    if (target) {
        // search an existing connection of same CC number and target
        // if it exists, modify, otherwise create
        auto it = std::find_if(connections.begin(), connections.end(),
            [ccNumber, &target, this](const Connection& x) -> bool
            {
                if (ccModulationIsPerVoice(ccNumber))
                    return x.source.id() == ModId::PerVoiceController &&
                        x.source.region() == id &&
                        x.source.parameters().cc == ccNumber &&
                        x.target == target;
                return x.source.id() == ModId::Controller &&
                    x.source.parameters().cc == ccNumber &&
                    x.target == target;
            });

        Connection *conn;
        if (it != connections.end())
            conn = &*it;
        else {
            connections.emplace_back();
            conn = &connections.back();
            conn->source = ModKey::createCC(ccNumber, 0, 0, 0);
            conn->target = target;
        }

        //
        ModKey::Parameters p = conn->source.parameters();
        switch (opcode.category) {
        case kOpcodeOnCcN:
            conn->sourceDepth = opcode.read(spec);
            break;
        case kOpcodeCurveCcN:
                p.curve = opcode.read(Default::curveCC);
            break;
        case kOpcodeStepCcN:
            {
                const OpcodeSpec<float> stepCC { 0.0f, {}, kPermissiveBounds };
                p.step = spec.normalizeInput(opcode.read(stepCC));
            }
            break;
        case kOpcodeSmoothCcN:
            p.smooth = opcode.read(Default::smoothCC);
            break;
        default:
            assert(false);
            break;
        }

       if (ccModulationIsPerVoice(p.cc)) {
            conn->source = ModKey(ModId::PerVoiceController, id, p);
       } else {
            conn->source = ModKey(ModId::Controller, {}, p);
       }
    }

    return true;
}

float sfz::Region::getBaseGain() const noexcept
{
    float baseGain = amplitude;

    baseGain *= globalAmplitude;
    baseGain *= masterAmplitude;
    baseGain *= groupAmplitude;

    return baseGain;
}

float sfz::Region::getPhase() const noexcept
{
    float phase;
    if (oscillatorPhase >= 0)
        phase = oscillatorPhase;
    else {
        fast_real_distribution<float> phaseDist { 0.0001f, 0.9999f };
        phase = phaseDist(Random::randomGenerator);
    }
    return phase;
}

void sfz::Region::offsetAllKeys(int offset) noexcept
{
    // Offset key range
    if (keyRange != Default::key.bounds) {
        const auto start = keyRange.getStart();
        const auto end = keyRange.getEnd();
        keyRange.setStart(offsetAndClampKey(start, offset));
        keyRange.setEnd(offsetAndClampKey(end, offset));
    }
    pitchKeycenter = offsetAndClampKey(pitchKeycenter, offset);

    // Offset key switches
    if (upKeyswitch)
        upKeyswitch = offsetAndClampKey(*upKeyswitch, offset);
    if (lastKeyswitch)
        lastKeyswitch = offsetAndClampKey(*lastKeyswitch, offset);
    if (downKeyswitch)
        downKeyswitch = offsetAndClampKey(*downKeyswitch, offset);
    if (previousKeyswitch)
        previousKeyswitch = offsetAndClampKey(*previousKeyswitch, offset);

    // Offset crossfade ranges
    if (crossfadeKeyInRange != Default::crossfadeKeyInRange) {
        const auto start = crossfadeKeyInRange.getStart();
        const auto end = crossfadeKeyInRange.getEnd();
        crossfadeKeyInRange.setStart(offsetAndClampKey(start, offset));
        crossfadeKeyInRange.setEnd(offsetAndClampKey(end, offset));
    }

    if (crossfadeKeyOutRange != Default::crossfadeKeyOutRange) {
        const auto start = crossfadeKeyOutRange.getStart();
        const auto end = crossfadeKeyOutRange.getEnd();
        crossfadeKeyOutRange.setStart(offsetAndClampKey(start, offset));
        crossfadeKeyOutRange.setEnd(offsetAndClampKey(end, offset));
    }
}

float sfz::Region::getGainToEffectBus(unsigned number) const noexcept
{
    if (number >= gainToEffect.size())
        return 0.0;

    return gainToEffect[number];
}

float sfz::Region::getBendInCents(float bend) const noexcept
{
    return bend > 0.0f ? bend * static_cast<float>(bendUp) : -bend * static_cast<float>(bendDown);
}

sfz::Region::Connection* sfz::Region::getConnection(const ModKey& source, const ModKey& target)
{
    auto pred = [&source, &target](const Connection& c)
    {
        return c.source == source && c.target == target;
    };

    auto it = std::find_if(connections.begin(), connections.end(), pred);
    return (it == connections.end()) ? nullptr : &*it;
}

sfz::Region::Connection& sfz::Region::getOrCreateConnection(const ModKey& source, const ModKey& target)
{
    if (Connection* c = getConnection(source, target))
        return *c;

    sfz::Region::Connection c;
    c.source = source;
    c.target = target;

    connections.push_back(c);
    return connections.back();
}

sfz::Region::Connection* sfz::Region::getConnectionFromCC(int sourceCC, const ModKey& target)
{
    if (ccModulationIsPerVoice(sourceCC)) {
        for (sfz::Region::Connection& conn : connections) {
            if (conn.source.id() == sfz::ModId::PerVoiceController && conn.target == target && conn.source.region() == id) {
                const auto& p = conn.source.parameters();
                if (p.cc == sourceCC)
                    return &conn;
            }
        }
    } else {
        for (sfz::Region::Connection& conn : connections) {
            if (conn.source.id() == sfz::ModId::Controller && conn.target == target) {
                const auto& p = conn.source.parameters();
                if (p.cc == sourceCC)
                    return &conn;
            }
        }
    }
    return nullptr;
}

bool sfz::Region::disabled() const noexcept
{
    return (sampleEnd == 0);
}

absl::optional<float> sfz::Region::ccModDepth(int cc, ModId id, uint8_t N, uint8_t X, uint8_t Y, uint8_t Z) const noexcept
{
    const ModKey target = ModKey::createNXYZ(id, getId(), N, X, Y, Z);
    const Connection *conn = const_cast<Region*>(this)->getConnectionFromCC(cc, target);
    if (!conn)
        return {};
    return conn->sourceDepth;
}

absl::optional<sfz::ModKey::Parameters> sfz::Region::ccModParameters(int cc, ModId id, uint8_t N, uint8_t X, uint8_t Y, uint8_t Z) const noexcept
{
    const ModKey target = ModKey::createNXYZ(id, getId(), N, X, Y, Z);
    const Connection *conn = const_cast<Region*>(this)->getConnectionFromCC(cc, target);
    if (!conn)
        return {};
    return conn->source.parameters();
}


bool sfz::Region::generateOpcodes(std::vector<Opcode> & retOpcodes, bool forceAll) const
{
    
    
    if (sampleQuality) {
        if (forceAll || *sampleQuality != Default::sampleQuality.defaultInputValue) {
            retOpcodes.emplace_back("sample_quality", absl::StrCat(*sampleQuality));
        }
    }

    if (sampleId->isReverse()) {
        retOpcodes.emplace_back("direction", "reverse");
    }

    if (forceAll || delay != Default::delay.defaultInputValue) {
        retOpcodes.emplace_back("delay", absl::StrCat(delay));
    }
    for (const auto & val : delayCC) {
        retOpcodes.emplace_back( absl::StrCat("delay_oncc", val.cc), absl::StrCat(val.data) );
    }

    if (forceAll || delayRandom != Default::delayRandom.defaultInputValue) {
        retOpcodes.emplace_back("delay_random", absl::StrCat(delayRandom));
    }

    if (forceAll || offset != Default::offset.defaultInputValue) {
        retOpcodes.emplace_back("offset", absl::StrCat(offset));
    }
    if (forceAll || offsetRandom != Default::offsetRandom.defaultInputValue) {
        retOpcodes.emplace_back("offset_random", absl::StrCat(offsetRandom));
    }
    for (const auto & val : offsetCC) {
        retOpcodes.emplace_back( absl::StrCat("offset_oncc", val.cc), absl::StrCat(val.data) );
    }

    if (forceAll || sampleEnd != Default::sampleEnd.defaultInputValue) {
        if (sampleEnd != fileSampleEnd) {
            retOpcodes.emplace_back("end", absl::StrCat(sampleEnd));
        }
    }
    for (const auto & val : endCC) {
        retOpcodes.emplace_back( absl::StrCat("end_oncc", val.cc), absl::StrCat(val.data) );
    }

    if (sampleCount) {
        if (forceAll || *sampleCount != Default::sampleCount.defaultInputValue) {
            retOpcodes.emplace_back("count", absl::StrCat(*sampleCount));
        }
    }

    if (loopMode) {
        if (forceAll || *loopMode != Default::loopMode.defaultInputValue) {
            std::string lmodestr;
            switch(*loopMode) {
                case LoopMode::no_loop: lmodestr = "no_loop"; break;
                case LoopMode::loop_continuous: lmodestr = "loop_continuous"; break;
                case LoopMode::loop_sustain: lmodestr = "loop_sustain"; break;
                case LoopMode::one_shot: lmodestr = "one_shot"; break;
            }
            retOpcodes.emplace_back("loop_mode", lmodestr);
        }
    }

    if (forceAll || loopRange.getEnd() != Default::loopEnd.defaultInputValue) {
        if (loopRange.getEnd() != sampleEnd) {
            retOpcodes.emplace_back("loop_end", absl::StrCat(loopRange.getEnd()));
        }
    }
    if (forceAll || loopRange.getStart() != Default::loopStart.defaultInputValue) {
        retOpcodes.emplace_back("loop_start", absl::StrCat(loopRange.getStart()));
    }
    if (loopCount) {
        if (forceAll || *loopCount != Default::loopCount.defaultInputValue) {
            retOpcodes.emplace_back("loop_count", absl::StrCat(*loopCount));
        }
    }
    for (const auto & val : loopStartCC) {
        retOpcodes.emplace_back( absl::StrCat("loop_start_oncc", val.cc), absl::StrCat(val.data) );
    }
    for (const auto & val : loopEndCC) {
        retOpcodes.emplace_back( absl::StrCat("loop_end_oncc", val.cc), absl::StrCat(val.data) );
    }
    if (forceAll || loopCrossfade != Default::loopCrossfade.defaultInputValue) {
        retOpcodes.emplace_back("loop_crossfade", absl::StrCat(loopCrossfade));
    }

    if (forceAll || oscillatorPhase != Default::oscillatorPhase.defaultInputValue) {
        retOpcodes.emplace_back("oscillator_phase", absl::StrCat(oscillatorPhase));
    }
    if (forceAll || oscillatorEnabled != Default::oscillator.defaultInputValue) {
        retOpcodes.emplace_back("oscillator_phase", oscillatorEnabled == OscillatorEnabled::Auto ? "auto" : oscillatorEnabled == OscillatorEnabled::On ? "on" : "off" );
    }
    if (forceAll || oscillatorMode != Default::oscillatorMode.defaultInputValue) {
        retOpcodes.emplace_back("oscillator_mode", absl::StrCat(oscillatorMode));
    }
    if (forceAll || oscillatorMulti != Default::oscillatorMulti.defaultInputValue) {
        retOpcodes.emplace_back("oscillator_multi", absl::StrCat(oscillatorMulti));
    }
    if (forceAll || oscillatorDetune != Default::oscillatorDetune.defaultInputValue) {
        retOpcodes.emplace_back("oscillator_detune", absl::StrCat(oscillatorDetune));
    }
    if (forceAll || oscillatorModDepth*100.0f != Default::oscillatorModDepth.defaultInputValue) {
        retOpcodes.emplace_back("oscillator_mod_depth", absl::StrCat(oscillatorModDepth*100.0f));
    }
    if (oscillatorQuality) {
        if (forceAll || *oscillatorQuality != Default::oscillatorQuality.defaultInputValue) {
            retOpcodes.emplace_back("oscillator_quality", absl::StrCat(*oscillatorQuality));
        }
    }

    
    if (forceAll || group != Default::group.defaultInputValue) {
        retOpcodes.emplace_back("group", absl::StrCat(group));
    }

    if (forceAll || output != Default::output.defaultInputValue) {
        retOpcodes.emplace_back("output", absl::StrCat(output));
    }

    if (offBy) {
        if (forceAll || *offBy != Default::group.defaultInputValue) {
            retOpcodes.emplace_back("off_by", absl::StrCat(*offBy));
        }
    }

    if (forceAll || offMode != Default::offMode.defaultInputValue) {
        std::string lmodestr;
        switch (offMode) {
            case OffMode::time: lmodestr = "time"; break;
            case OffMode::normal: lmodestr = "normal"; break;
            case OffMode::fast: lmodestr = "fast"; break;
        }
        
        retOpcodes.emplace_back("off_mode", lmodestr);
        
        if (offMode == OffMode::time && (forceAll || offTime != Default::offTime.defaultInputValue)) {
            retOpcodes.emplace_back("off_time", absl::StrCat(offTime));
        }
    }

    if (forceAll || polyphony != Default::polyphony.defaultInputValue) {
        retOpcodes.emplace_back("polyphony", absl::StrCat(polyphony));
    }
    if (notePolyphony) {
        if (forceAll || *notePolyphony != Default::notePolyphony.defaultInputValue) {
            retOpcodes.emplace_back("note_polyphony", absl::StrCat(*notePolyphony));
        }
    }

    if (forceAll || selfMask != Default::selfMask.defaultInputValue) {
        std::string lmodestr;
        switch (selfMask) {
            case SelfMask::dontMask : lmodestr = "off"; break;
            case SelfMask::mask : lmodestr = "on"; break;
        }
        retOpcodes.emplace_back("note_selfmask", lmodestr);
    }

    if (forceAll || rtDead != Default::rtDead.defaultInputValue) {
        retOpcodes.emplace_back("rt_dead", rtDead ? "on" : "off");
    }

    // if pitchkeycenter lokey and hikey are the same, use "key" only
    if (!pitchKeycenterFromSample && pitchKeycenter == keyRange.getStart() && pitchKeycenter == keyRange.getEnd()) {
        retOpcodes.emplace_back("key", absl::StrCat(pitchKeycenter));
    }
    else {
        if (pitchKeycenterFromSample) {
            retOpcodes.emplace_back("pitch_keycenter", "sample");
        }
        else if (forceAll || pitchKeycenter != Default::key.defaultInputValue) {
            retOpcodes.emplace_back("pitch_keycenter", absl::StrCat(pitchKeycenter));
        }

        if (forceAll || keyRange.getStart() != Default::loKey.defaultInputValue) {
            retOpcodes.emplace_back("lokey", absl::StrCat(keyRange.getStart()));
        }
        if (forceAll || keyRange.getEnd() != Default::hiKey.defaultInputValue) {
            retOpcodes.emplace_back("hikey", absl::StrCat(keyRange.getEnd()));
        }
    }

    if (forceAll || velocityRange.getStart()*127.0f != Default::loVel.defaultInputValue) {
        retOpcodes.emplace_back("lovel", absl::StrCat((int)(velocityRange.getStart()*127.0f)));
    }
    if (forceAll || velocityRange.getEnd()*127.0f != Default::hiVel.defaultInputValue) {
        retOpcodes.emplace_back("hivel", absl::StrCat((int)(velocityRange.getEnd()*127.0f)));
    }

    if (forceAll || crossfadeKeyInRange.getStart() != Default::loKey.defaultInputValue) {
        retOpcodes.emplace_back("lobend", absl::StrCat(crossfadeKeyInRange.getStart()));
    }
    if (forceAll || crossfadeKeyInRange.getEnd() != Default::loKey.defaultInputValue) {
        retOpcodes.emplace_back("hibend", absl::StrCat(crossfadeKeyInRange.getEnd()));
    }

    if (forceAll || programRange.getStart() != Default::loProgram.defaultInputValue) {
        retOpcodes.emplace_back("loprog", absl::StrCat(programRange.getStart()));
    }
    if (forceAll || programRange.getEnd() != Default::hiProgram.defaultInputValue) {
        retOpcodes.emplace_back("hiprog", absl::StrCat(programRange.getEnd()));
    }

    for (const auto & val : ccConditions) {
        if (forceAll || val.data.getStart() != Default::loNormalized.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("lohdcc", val.cc), absl::StrCat(val.data.getStart()) );
        }
        if (forceAll || val.data.getEnd() != Default::hiNormalized.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("hihdcc", val.cc), absl::StrCat(val.data.getEnd()) );
        }
    }
    
    if (usesKeySwitches) {
        if (lastKeyswitch) {
            retOpcodes.emplace_back( "sw_last", absl::StrCat(*lastKeyswitch) );
        }
        else if (lastKeyswitchRange) {
            retOpcodes.emplace_back( "sw_lolast", absl::StrCat(lastKeyswitchRange->getStart()) );
            retOpcodes.emplace_back( "sw_hilast", absl::StrCat(lastKeyswitchRange->getEnd()) );
        }
        
        if (downKeyswitch) {
            retOpcodes.emplace_back( "sw_down", absl::StrCat(*downKeyswitch) );
        }
        if (upKeyswitch) {
            retOpcodes.emplace_back( "sw_up", absl::StrCat(*upKeyswitch) );
        }
    }

    if (keyswitchLabel) {
        retOpcodes.emplace_back( "sw_label", *keyswitchLabel);
    }

    if (usesPreviousKeySwitches && previousKeyswitch) {
        retOpcodes.emplace_back( "sw_previous", absl::StrCat(*previousKeyswitch));
    }
    
    if (forceAll || velocityOverride != Default::velocityOverride.defaultInputValue) {
        std::string valstr = velocityOverride == VelocityOverride::current ? "current" : "previous";
        retOpcodes.emplace_back("sw_vel", valstr);
    }

    if (forceAll || sustainCC != Default::sustainCC.defaultInputValue) {
        retOpcodes.emplace_back("sustain_cc", absl::StrCat(sustainCC));
    }
    if (forceAll || sostenutoCC != Default::sostenutoCC.defaultInputValue) {
        retOpcodes.emplace_back("sostenuto_cc", absl::StrCat(sostenutoCC));
    }

    if (forceAll || sustainThreshold*127.0f != Default::sustainThreshold.defaultInputValue) {
        retOpcodes.emplace_back("sustain_lo", absl::StrCat((int)(sustainThreshold*127.0f)));
    }
    if (forceAll || sostenutoThreshold*127.0f != Default::sostenutoThreshold.defaultInputValue) {
        retOpcodes.emplace_back("sostenuto_lo", absl::StrCat((int)(sostenutoThreshold*127.0f)));
    }

    if (forceAll || checkSustain != Default::checkSustain.defaultInputValue) {
        retOpcodes.emplace_back("sustain_sw", checkSustain ? "on" : "off");
    }
    if (forceAll || checkSostenuto != Default::checkSostenuto.defaultInputValue) {
        retOpcodes.emplace_back("sostenuto_sw", checkSostenuto ? "on" : "off");
    }

    if (forceAll || aftertouchRange.getStart()*127.0f != Default::loChannelAftertouch.defaultInputValue) {
        retOpcodes.emplace_back("lochanaft", absl::StrCat((int)(aftertouchRange.getStart()*127.0f)));
    }
    if (forceAll || aftertouchRange.getEnd()*127.0f != Default::hiChannelAftertouch.defaultInputValue) {
        retOpcodes.emplace_back("hichanaft", absl::StrCat((int)(aftertouchRange.getEnd()*127.0f)));
    }
    if (forceAll || polyAftertouchRange.getStart()*127.0f != Default::loPolyAftertouch.defaultInputValue) {
        retOpcodes.emplace_back("lopolyaft", absl::StrCat((int)(polyAftertouchRange.getStart()*127.0f)));
    }
    if (forceAll || polyAftertouchRange.getEnd()*127.0f != Default::hiPolyAftertouch.defaultInputValue) {
        retOpcodes.emplace_back("hipolyaft", absl::StrCat((int)(polyAftertouchRange.getEnd()*127.0f)));
    }

    if (forceAll || bpmRange.getStart() != Default::loBPM.defaultInputValue) {
        retOpcodes.emplace_back("lobpm", absl::StrCat(bpmRange.getStart()));
    }
    if (forceAll || bpmRange.getEnd() != Default::hiBPM.defaultInputValue) {
        retOpcodes.emplace_back("hibpm", absl::StrCat(bpmRange.getEnd()));
    }
    
    if (forceAll || randRange.getStart() != Default::loNormalized.defaultInputValue) {
        retOpcodes.emplace_back("lorand", absl::StrCat(randRange.getStart()));
    }
    if (forceAll || randRange.getEnd() != Default::hiNormalized.defaultInputValue) {
        retOpcodes.emplace_back("hirand", absl::StrCat(randRange.getEnd()));
    }

    if (forceAll || sequenceLength != Default::sequence.defaultInputValue) {
        retOpcodes.emplace_back("seq_length", absl::StrCat(sequenceLength));
    }
    if (forceAll || sequencePosition != Default::sequence.defaultInputValue) {
        retOpcodes.emplace_back("seq_position", absl::StrCat(sequencePosition));
    }

    if (forceAll || trigger != Default::trigger.defaultInputValue) {
        std::string lmodestr;
        switch (trigger) {
            case Trigger::attack: lmodestr = "attack"; break;
            case Trigger::first: lmodestr = "first"; break;
            case Trigger::release: lmodestr = "release"; break;
            case Trigger::release_key: lmodestr = "release_key"; break;
            case Trigger::legato: lmodestr = "legato"; break;
        }
        retOpcodes.emplace_back("trigger", lmodestr);
    }

    for (const auto & val : ccTriggers) {
        if (val.data.getStart() != Default::loNormalized.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("start_lohdcc", val.cc), absl::StrCat(val.data.getStart()));
        if (val.data.getEnd() != Default::hiNormalized.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("start_hihdcc", val.cc), absl::StrCat(val.data.getEnd()));
    }

    
    if (forceAll || volume != Default::volume.defaultInputValue) {
        retOpcodes.emplace_back("volume", absl::StrCat(volume));
    }
    if (forceAll || amplitude*100.0 != Default::amplitude.defaultInputValue) {
        retOpcodes.emplace_back("amplitude", absl::StrCat(amplitude*100.0));
    }

    if (forceAll || pan*100.0f != Default::pan.defaultInputValue) {
        retOpcodes.emplace_back("pan", absl::StrCat(pan*100.0f));
    }

    if (forceAll || position*100.0f != Default::position.defaultInputValue) {
        retOpcodes.emplace_back("position", absl::StrCat(position*100.0f));
    }

    if (forceAll || width*100.0f != Default::width.defaultInputValue) {
        retOpcodes.emplace_back("width", absl::StrCat(width*100.0f));
    }


    if (forceAll || ampKeycenter != Default::key.defaultInputValue) {
        retOpcodes.emplace_back("amp_keycenter", absl::StrCat(ampKeycenter));
    }
    if (forceAll || ampKeytrack != Default::ampKeytrack.defaultInputValue) {
        retOpcodes.emplace_back("amp_keytrack", absl::StrCat(ampKeytrack));
    }
    
    if (forceAll || ampVeltrack*100.0f != Default::ampVeltrack.defaultInputValue) {
        retOpcodes.emplace_back("amp_veltrack", absl::StrCat(ampVeltrack*100.0f));
    }
    for (const auto & val : ampVeltrackCC) {
        if (val.data.modifier*100.0f != Default::ampVeltrackMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("amp_veltrack_oncc", val.cc), absl::StrCat(val.data.modifier*100.0f));
        if (val.data.curve != Default::curveCC.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("amp_veltrack_curvecc", val.cc), absl::StrCat(val.data.curve));
    }

    if (forceAll || ampRandom != Default::ampRandom.defaultInputValue) {
        retOpcodes.emplace_back("amp_random", absl::StrCat(ampRandom));
    }

    for (const auto & val : velocityPoints) {
        if (val.second != Default::ampVelcurve.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("amp_velcurve_", val.first), absl::StrCat(val.second));
    }
    
    if (forceAll || crossfadeKeyInRange.getStart() != Default::loKey.defaultInputValue) {
        retOpcodes.emplace_back("xfin_lokey", absl::StrCat(crossfadeKeyInRange.getStart()));
    }
    if (forceAll || crossfadeKeyInRange.getEnd() != Default::loKey.defaultInputValue) { // lokey for proper default
        retOpcodes.emplace_back("xfin_hikey", absl::StrCat(crossfadeKeyInRange.getEnd()));
    }
    if (forceAll || crossfadeKeyOutRange.getStart() != Default::hiKey.defaultInputValue) { // hikey for proper default
        retOpcodes.emplace_back("xfout_lokey", absl::StrCat(crossfadeKeyOutRange.getStart()));
    }
    if (forceAll || crossfadeKeyOutRange.getEnd() != Default::hiKey.defaultInputValue) {
        retOpcodes.emplace_back("xfout_hikey", absl::StrCat(crossfadeKeyOutRange.getEnd()));
    }
    
    if (forceAll || crossfadeVelInRange.getStart()*127.0f != Default::xfinLo.defaultInputValue) {
        retOpcodes.emplace_back("xfin_lovel", absl::StrCat((int)(crossfadeVelInRange.getStart()*127.0f)));
    }
    if (forceAll || crossfadeVelInRange.getEnd()*127.0f != Default::xfinHi.defaultInputValue) {
        retOpcodes.emplace_back("xfin_hivel", absl::StrCat((int)(crossfadeVelInRange.getEnd()*127.0f)));
    }
    if (forceAll || crossfadeVelOutRange.getStart()*127.0f != Default::xfoutLo.defaultInputValue) {
        retOpcodes.emplace_back("xfout_lovel", absl::StrCat((int)(crossfadeVelOutRange.getStart()*127.0f)));
    }
    if (forceAll || crossfadeVelOutRange.getEnd()*127.0f != Default::xfoutHi.defaultInputValue) {
        retOpcodes.emplace_back("xfout_hivel", absl::StrCat((int)(crossfadeVelOutRange.getEnd()*127.0f)));
    }

    if (forceAll || crossfadeKeyCurve != Default::crossfadeCurve.defaultInputValue) {
        std::string lmodestr;
        switch(crossfadeKeyCurve) {
            case CrossfadeCurve::gain: lmodestr ="gain"; break;
            case CrossfadeCurve::power: lmodestr = "power"; break;
        }
        retOpcodes.emplace_back("xf_keycurve", lmodestr);
    }
    if (forceAll || crossfadeVelCurve != Default::crossfadeCurve.defaultInputValue) {
        std::string lmodestr;
        switch(crossfadeVelCurve) {
            case CrossfadeCurve::gain: lmodestr ="gain"; break;
            case CrossfadeCurve::power: lmodestr = "power"; break;
        }
        retOpcodes.emplace_back("xf_velcurve", absl::StrCat(crossfadeVelCurve));
    }

    if (forceAll || crossfadeCCCurve != Default::crossfadeCurve.defaultInputValue) {
        std::string lmodestr;
        switch(crossfadeCCCurve) {
            case CrossfadeCurve::gain: lmodestr ="gain"; break;
            case CrossfadeCurve::power: lmodestr = "power"; break;
        }
        retOpcodes.emplace_back("xf_cccurve", absl::StrCat(crossfadeCCCurve));
    }

    for (const auto & val : crossfadeCCInRange) {
        if (forceAll || val.data.getStart()*127.0f != Default::xfinLo.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("xfin_locc", val.cc), absl::StrCat((int)(val.data.getStart()*127.0f)) );
        }
        if (forceAll || val.data.getEnd()*127.0f != Default::xfinHi.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("xfin_hicc", val.cc), absl::StrCat((int)(val.data.getEnd()*127.0f)) );
        }
    }
    for (const auto & val : crossfadeCCOutRange) {
        if (forceAll || val.data.getStart()*127.0f != Default::xfoutLo.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("xfout_locc", val.cc), absl::StrCat((int)(val.data.getStart()*127.0f)) );
        }
        if (forceAll || val.data.getEnd()*127.0f != Default::xfoutHi.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("xfout_hicc", val.cc), absl::StrCat((int)(val.data.getEnd()*127.0f)) );
        }
    }

    if (forceAll || rtDecay != Default::rtDecay.defaultInputValue) {
        retOpcodes.emplace_back("rt_decay", absl::StrCat(rtDecay));
    }
    
    if (forceAll || globalAmplitude*100.0f != Default::amplitude.defaultInputValue) {
        retOpcodes.emplace_back("global_amplitude", absl::StrCat(globalAmplitude*100.0f));
    }
    if (forceAll || masterAmplitude*100.0f != Default::amplitude.defaultInputValue) {
        retOpcodes.emplace_back("master_amplitude", absl::StrCat(masterAmplitude*100.0f));
    }
    if (forceAll || groupAmplitude*100.0f != Default::amplitude.defaultInputValue) {
        retOpcodes.emplace_back("group_amplitude", absl::StrCat(groupAmplitude*100.0f));
    }

    if (forceAll || globalVolume != Default::volume.defaultInputValue) {
        retOpcodes.emplace_back("global_volume", absl::StrCat(globalVolume));
    }
    if (forceAll || masterVolume != Default::volume.defaultInputValue) {
        retOpcodes.emplace_back("master_volume", absl::StrCat(masterVolume));
    }
    if (forceAll || groupVolume != Default::volume.defaultInputValue) {
        retOpcodes.emplace_back("group_volume", absl::StrCat(groupVolume));
    }

    for (size_t i=0; i < filters.size(); ++i) {
        if (forceAll || filters[i].cutoff != Default::filterCutoff.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("cutoff", i+1), absl::StrCat(filters[i].cutoff) );
        }
        if (forceAll || filters[i].resonance != Default::filterResonance.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("resonance", i+1), absl::StrCat(filters[i].resonance) );
        }
        if (forceAll || filters[i].keytrack != Default::filterKeytrack.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_keytrack"), absl::StrCat(filters[i].keytrack) );
        }
        if (forceAll || filters[i].keycenter != Default::key.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_keycenter"), absl::StrCat(filters[i].keycenter) );
        }
        if (forceAll || filters[i].veltrack != Default::filterVeltrack.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_veltrack"), absl::StrCat(filters[i].veltrack) );
        }
        for (const auto & val : filters[i].veltrackCC) {
            if (forceAll || val.data.modifier != Default::filterVeltrackMod.defaultInputValue) {
                retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_veltrack_oncc", val.cc), absl::StrCat(val.data.modifier) );
            }
            if (forceAll || val.data.curve != Default::curveCC.defaultInputValue) {
                retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_veltrack_curvecc", val.cc), absl::StrCat(val.data.curve) );
            }
        }
        if (forceAll || filters[i].random != Default::filterRandom.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_random"), absl::StrCat(filters[i].random) );
        }
        if (forceAll || filters[i].gain != Default::filterGain.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_gain"), absl::StrCat(filters[i].gain) );
        }
        if (forceAll || filters[i].type != Default::filter.defaultInputValue) {
            std::string lmodestr;
            switch (filters[i].type) {
                case FilterType::kFilterLpf1p: lmodestr = "lpf_1p"; break;
                case FilterType::kFilterHpf1p: lmodestr = "hpf_1p"; break;
                case FilterType::kFilterLpf2p: lmodestr = "lpf_2p"; break;
                case FilterType::kFilterHpf2p: lmodestr = "hpf_2p"; break;
                case FilterType::kFilterBpf2p: lmodestr = "bpf_2p"; break;
                case FilterType::kFilterBrf2p: lmodestr = "brf_2p"; break;
                case FilterType::kFilterBpf1p: lmodestr = "bpf_1p"; break;
                case FilterType::kFilterBrf1p: lmodestr = "brf_1p"; break;
                case FilterType::kFilterApf1p: lmodestr = "apf_1p"; break;
                case FilterType::kFilterLpf2pSv: lmodestr = "lpf_2p_sv"; break;
                case FilterType::kFilterHpf2pSv: lmodestr = "hpf_2p_sv"; break;
                case FilterType::kFilterBpf2pSv: lmodestr = "bpf_2p_sv"; break;
                case FilterType::kFilterBrf2pSv: lmodestr = "brf_2p_sv"; break;
                case FilterType::kFilterLpf4p: lmodestr = "lpf_4p"; break;
                case FilterType::kFilterHpf4p: lmodestr = "hpf_4p"; break;
                case FilterType::kFilterLpf6p: lmodestr = "lpf_6p"; break;
                case FilterType::kFilterHpf6p: lmodestr = "hpf_6p"; break;
                case FilterType::kFilterPink: lmodestr = "pink"; break;
                case FilterType::kFilterLsh: lmodestr = "lsh"; break;
                case FilterType::kFilterHsh: lmodestr = "hsh"; break;
                case FilterType::kFilterPeq: lmodestr = "peq"; break;
                case FilterType::kFilterBpf4p: lmodestr = "bpf_4p"; break;
                case FilterType::kFilterBpf6p: lmodestr = "bpf_6p"; break;
                case FilterType::kFilterNone: lmodestr = "none"; break;
            }
            retOpcodes.emplace_back( absl::StrCat("fil", i+1, "_type"), lmodestr );
        }
    }
    
    for (size_t i=0; i < equalizers.size(); ++i) {
        if (forceAll || equalizers[i].bandwidth != Default::eqBandwidth.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("eq", i+1, "_bw"), absl::StrCat(equalizers[i].bandwidth) );
        }
        if (forceAll || equalizers[i].frequency != Default::eqFrequency.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("eq", i+1, "_freq"), absl::StrCat(equalizers[i].frequency) );
        }
        if (forceAll || equalizers[i].vel2frequency != Default::eqVel2Frequency.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("eq", i+1, "_veltofreq"), absl::StrCat(equalizers[i].vel2frequency) );
        }
        if (forceAll || equalizers[i].gain != Default::eqGain.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("eq", i+1, "_freq"), absl::StrCat(equalizers[i].gain) );
        }
        if (forceAll || equalizers[i].vel2gain != Default::eqVel2Gain.defaultInputValue) {
            retOpcodes.emplace_back( absl::StrCat("eq", i+1, "_veltogain"), absl::StrCat(equalizers[i].vel2gain) );
        }
        
        if (forceAll || equalizers[i].type != Default::eq.defaultInputValue) {
            // jlc
            std::string lmodestr;
            switch (equalizers[i].type) {
                case EqType::kEqNone: lmodestr = "none"; break;
                case EqType::kEqPeak: lmodestr = "peak"; break;
                case EqType::kEqLshelf: lmodestr = "lshelf"; break;
                case EqType::kEqHshelf: lmodestr = "hshelf"; break;
            }
            retOpcodes.emplace_back( absl::StrCat("eq", i+1, "_type"), lmodestr );
        }
    }

    if (forceAll || pitchKeytrack != Default::pitchKeytrack.defaultInputValue) {
        retOpcodes.emplace_back("pitch_keytrack", absl::StrCat(pitchKeytrack));
    }
    
    if (forceAll || pitchVeltrack != Default::pitchVeltrack.defaultInputValue) {
        retOpcodes.emplace_back("pitch_veltrack", absl::StrCat(pitchVeltrack));
    }
    for (const auto & val : pitchVeltrackCC) {
        if (val.data.modifier != Default::pitchVeltrackMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("pitch_veltrack_oncc", val.cc), absl::StrCat(val.data.modifier));
        if (val.data.curve != Default::curveCC.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("pitch_veltrack_curvecc", val.cc), absl::StrCat(val.data.curve));
    }
    
    if (forceAll || pitchRandom != Default::pitchRandom.defaultInputValue) {
        retOpcodes.emplace_back("pitch_random", absl::StrCat(pitchRandom));
    }

    if (forceAll || pitch != Default::pitch.defaultInputValue) {
        retOpcodes.emplace_back("pitch", absl::StrCat(pitch));
    }
    // todo pitch cc


    if (forceAll || bendUp != Default::bendUp.defaultInputValue) {
        retOpcodes.emplace_back("bend_up", absl::StrCat(bendUp));
    }
    if (forceAll || bendDown != Default::bendDown.defaultInputValue) {
        retOpcodes.emplace_back("bend_down", absl::StrCat(bendDown));
    }
    if (forceAll || bendStep != Default::bendStep.defaultInputValue) {
        retOpcodes.emplace_back("bend_step", absl::StrCat(bendStep));
    }
    if (forceAll || bendSmooth != Default::smoothCC.defaultInputValue) {
        retOpcodes.emplace_back("bend_smooth", absl::StrCat(bendSmooth));
    }

    // skip first, which is main
    for (size_t i = 1; i < gainToEffect.size(); ++i) {
        if (gainToEffect[i]*100.0f != Default::effect.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat("effect", i), absl::StrCat(gainToEffect[i]*100.0f));
    }

    if (defaultSwitch) {
        if (forceAll || *defaultSwitch != Default::key.defaultInputValue) {
            retOpcodes.emplace_back("sw_default", absl::StrCat(*defaultSwitch));
        }
    }

    // ampeg_*
    generateEGOpcodes(retOpcodes, amplitudeEG, "ampeg_", forceAll);

    // pitcheg_*
    if (pitchEG)
        generateEGOpcodes(retOpcodes, *pitchEG, "pitcheg_", forceAll);

    // fileg_*
    if (filterEG)
        generateEGOpcodes(retOpcodes, *filterEG, "fileg_", forceAll);

    // TODO LFO

    if (amplitudeLFO)
        generateLFOOpcodes(retOpcodes, *amplitudeLFO, "amplfo_", forceAll);
    if (pitchLFO)
        generateLFOOpcodes(retOpcodes, *pitchLFO, "pitchlfo_", forceAll);
    if (filterLFO)
        generateLFOOpcodes(retOpcodes, *filterLFO, "fillfo_", forceAll);

    
    // mod matrix connections
    generateConnections(retOpcodes, forceAll);
    
    // put sample last
    retOpcodes.emplace_back("sample", sampleId->filename());

    
    return true;
}

bool sfz::Region::generateEGOpcodes(std::vector<Opcode> & retOpcodes, const EGDescription& eg, const std::string & prefix, bool forceAll) const
{
    
    if (forceAll || eg.attack != Default::egTime.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"attack"), absl::StrCat(eg.attack));
    }
    if (forceAll || eg.decay != Default::egTime.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"decay"), absl::StrCat(eg.decay));
    }
    if (forceAll || eg.delay != Default::egTime.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"delay"), absl::StrCat(eg.delay));
    }
    if (forceAll || eg.hold != Default::egTime.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"hold"), absl::StrCat(eg.hold));
    }
    if (forceAll || eg.release != Default::egRelease.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"release"), absl::StrCat(eg.release));
    }
    if (forceAll || eg.start*100.0f != Default::egPercent.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"start"), absl::StrCat(eg.start*100.0f));
    }
    if (forceAll || eg.sustain*100.0f != Default::egSustain.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"sustain"), absl::StrCat(eg.sustain*100.0f));
    }
    if (forceAll || eg.vel2attack != Default::egTimeMod.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"veltoattack"), absl::StrCat(eg.vel2attack));
    }
    if (forceAll || eg.vel2decay != Default::egTimeMod.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"veltodecay"), absl::StrCat(eg.vel2decay));
    }
    if (forceAll || eg.vel2delay != Default::egTimeMod.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"veltodelay"), absl::StrCat(eg.vel2delay));
    }
    if (forceAll || eg.vel2hold != Default::egTimeMod.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"veltohold"), absl::StrCat(eg.vel2hold));
    }
    if (forceAll || eg.vel2release != Default::egTimeMod.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"veltorelease"), absl::StrCat(eg.vel2release));
    }
    if (forceAll || eg.vel2sustain*100.0f != Default::egPercentMod.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"veltosustain"), absl::StrCat(eg.vel2sustain*100.0f));
    }
    if (forceAll || eg.dynamic != Default::egDynamic.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"dynamic"), absl::StrCat(eg.dynamic));
    }

    for (const auto & val : eg.ccAttack) {
        if (val.data != Default::egTimeMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "attack_oncc", val.cc), absl::StrCat(val.data));
    }
    for (const auto & val : eg.ccDecay) {
        if (val.data != Default::egTimeMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "decay_oncc", val.cc), absl::StrCat(val.data));
    }
    for (const auto & val : eg.ccDelay) {
        if (val.data != Default::egTimeMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "delay_oncc", val.cc), absl::StrCat(val.data));
    }
    for (const auto & val : eg.ccHold) {
        if (val.data != Default::egTimeMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "hold_oncc", val.cc), absl::StrCat(val.data));
    }
    for (const auto & val : eg.ccRelease) {
        if (val.data != Default::egTimeMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "release_oncc", val.cc), absl::StrCat(val.data));
    }
    for (const auto & val : eg.ccStart) {
        if (val.data*100.0f != Default::egPercentMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "start_oncc", val.cc), absl::StrCat(val.data*100.0f));
    }
    for (const auto & val : eg.ccSustain) {
        if (val.data*100.0f != Default::egPercentMod.defaultInputValue)
            retOpcodes.emplace_back(absl::StrCat(prefix, "sustain_oncc", val.cc), absl::StrCat(val.data*100.0f));
    }

    
    return true;
}


bool sfz::Region::generateLFOOpcodes(std::vector<Opcode> & retOpcodes, const LFODescription& lfo, const std::string & prefix, bool forceAll) const
{
    
    if (forceAll || lfo.delay != Default::lfoDelay.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"delay"), absl::StrCat(lfo.delay));
    }
    if (forceAll || lfo.fade != Default::lfoFade.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"fade"), absl::StrCat(lfo.fade));
    }
    if (forceAll || lfo.freq != Default::lfoFreq.defaultInputValue) {
        retOpcodes.emplace_back(absl::StrCat(prefix,"freq"), absl::StrCat(lfo.freq));
    }

    if (lfo.sub.size() > 0) {
        if (forceAll || lfo.sub[0].wave != Default::lfoWave.defaultInputValue) {
            retOpcodes.emplace_back(absl::StrCat(prefix,"wave"), absl::StrCat(lfo.sub[0].wave));
        }
    }

    return true;
}

bool sfz::Region::generateConnections(std::vector<Opcode> & retOpcodes, bool forceAll) const
{
    auto generateForSource = [&retOpcodes, forceAll] (const std::string & srcpostfix, const Connection & conn, const std::string & valueOverride = "") {
        
        auto useval = !valueOverride.empty() ? valueOverride : absl::StrCat(conn.sourceDepth);
        auto percentval = !valueOverride.empty() ? valueOverride : absl::StrCat(conn.sourceDepth*100.0f);

        if (conn.target.id() == ModId::AmpLFODepth) {
            if (forceAll || conn.sourceDepth != Default::volumeMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("amplfo_depth", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::PitchLFODepth) {
            if (forceAll || conn.sourceDepth != Default::pitchMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("pitchlfo_depth", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::FilLFODepth) {
            if (forceAll || conn.sourceDepth != Default::filterCutoffMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("fillfo_depth", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::AmpLFOFrequency) {
            if (forceAll || conn.sourceDepth != Default::lfoFreqMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("amplfo_freq", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::PitchLFOFrequency) {
            if (forceAll || conn.sourceDepth != Default::lfoFreqMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("pitchlfo_freq", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::FilLFOFrequency) {
            if (forceAll || conn.sourceDepth != Default::lfoFreqMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("fillfo_freq", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::Volume) {
            if (forceAll || conn.sourceDepth != Default::volumeMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("volume", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::Amplitude) {
            if (forceAll || conn.sourceDepth*100.0f != Default::amplitudeMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("amplitude", srcpostfix), percentval);
        }
        else if (conn.target.id() == ModId::Pan) {
            if (forceAll || conn.sourceDepth*100.0f != Default::panMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("pan", srcpostfix), percentval);
        }
        else if (conn.target.id() == ModId::Position) {
            if (forceAll || conn.sourceDepth*100.0f != Default::positionMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("position", srcpostfix), percentval);
        }
        else if (conn.target.id() == ModId::Width) {
            if (forceAll || conn.sourceDepth*100.0f != Default::widthMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("width", srcpostfix), percentval);
        }
        else if (conn.target.id() == ModId::FilCutoff) {
            if (forceAll || conn.sourceDepth != Default::filterCutoffMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("cutoff", conn.source.parameters().N+1, srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::FilResonance) {
            if (forceAll || conn.sourceDepth != Default::filterResonanceMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("resonance", conn.source.parameters().N+1, srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::EqBandwidth) {
            if (forceAll || conn.sourceDepth != Default::eqBandwidthMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("eq", conn.source.parameters().N+1, "_bw", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::EqFrequency) {
            if (forceAll || conn.sourceDepth != Default::eqFrequencyMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("eq", conn.source.parameters().N+1, "_freq", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::EqGain) {
            if (forceAll || conn.sourceDepth != Default::eqGainMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("eq", conn.source.parameters().N+1, "_gain", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::Pitch) {
            if (forceAll || conn.sourceDepth != Default::pitchMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("pitch", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::PitchEGDepth) {
            if (forceAll || conn.sourceDepth != Default::pitchMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("pitcheg_depth", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::FilEGDepth) {
            if (forceAll || conn.sourceDepth != Default::filterCutoffMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("fileg_depth", srcpostfix), useval);
        }

        else if (conn.target.id() == ModId::OscillatorDetune) {
            if (forceAll || conn.sourceDepth != Default::oscillatorDetuneMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("oscillator_detune", srcpostfix), useval);
        }
        else if (conn.target.id() == ModId::OscillatorModDepth) {
            if (forceAll || conn.sourceDepth*100.0f != Default::oscillatorModDepthMod.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("oscillator_mod_depth", srcpostfix), percentval);
        }
        
        // TODO v2 EG and LFO stuff
        
    };
    
    
    for (const auto & conn : connections) {
     
        if (conn.source.id() == ModId::AmpLFO) {
            if (forceAll || conn.sourceDepth != Default::ampLFODepth.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("amplfo_depth"), absl::StrCat(conn.sourceDepth));
            
        }
        else if (conn.source.id() == ModId::PitchLFO) {
            if (forceAll || conn.sourceDepth != Default::pitchLFODepth.defaultInputValue)
                retOpcodes.emplace_back(absl::StrCat("pitchlfo_depth"), absl::StrCat(conn.sourceDepth));
            
        }
        else if (conn.source.id() == ModId::PitchEG) {
            if (conn.target.id() == ModId::Pitch) {
                if (forceAll || conn.sourceDepth != Default::egDepth.defaultInputValue)
                    retOpcodes.emplace_back(absl::StrCat("pitcheg_depth"), absl::StrCat(conn.sourceDepth));
                if (forceAll || conn.velToDepth != Default::egVel2Depth.defaultInputValue)
                    retOpcodes.emplace_back(absl::StrCat("pitcheg_veltodepth"), absl::StrCat(conn.velToDepth));
            }
        }
        else if (conn.source.id() == ModId::FilEG) {
            if (conn.target.id() == ModId::FilCutoff) {
                if (forceAll || conn.sourceDepth != Default::egDepth.defaultInputValue)
                    retOpcodes.emplace_back(absl::StrCat("fileg_depth"), absl::StrCat(conn.sourceDepth));
                if (forceAll || conn.velToDepth != Default::egVel2Depth.defaultInputValue)
                    retOpcodes.emplace_back(absl::StrCat("fileg_veltodepth"), absl::StrCat(conn.velToDepth));
            }
        }
        else if (conn.source.id() == ModId::ChannelAftertouch) {
            generateForSource("chanaft", conn);
        }
        else if (conn.source.id() == ModId::PolyAftertouch) {
            generateForSource("polyaft", conn);
        }
        else if (conn.source.id() == ModId::Controller || conn.source.id() == ModId::PerVoiceController ) {
            const auto & params = conn.source.parameters();

            if (params.cc != Default::ccNumber) {
                generateForSource(absl::StrCat("_oncc", params.cc), conn);
            }
            if (params.curve != Default::curveCC) {
                generateForSource(absl::StrCat("_curvecc", params.cc), conn, absl::StrCat(params.curve));
            }
            if (params.smooth != Default::smoothCC) {
                generateForSource(absl::StrCat("_smoothcc", params.cc), conn, absl::StrCat(params.smooth));
            }
            if (params.step != 0.0f) {
                generateForSource(absl::StrCat("_stepcc", params.cc), conn, absl::StrCat(params.step));
            }
        }
    }
    
    return true;
}
