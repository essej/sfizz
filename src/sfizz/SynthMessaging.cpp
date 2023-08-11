// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "SynthPrivate.h"
#include "FilePool.h"
#include "Curve.h"
#include "MidiState.h"
#include "SynthConfig.h"
#include "utility/StringViewHelpers.h"
#include <absl/strings/ascii.h>
#include <cstring>

// TODO: `ccModDepth` and `ccModParameters` are O(N), need better implementation

namespace sfz {
static constexpr unsigned maxIndices = 8;

static bool extractMessage(const char* pattern, const char* path, unsigned* indices);
static uint64_t hashMessagePath(const char* path, const char* sig);

void sfz::Synth::dispatchMessage(Client& client, int delay, const char* path, const char* sig, const sfizz_arg_t* args)
{
    UNUSED(args);
    Impl& impl = *impl_;
    unsigned indices[maxIndices];

    switch (hashMessagePath(path, sig)) {
        #define MATCH(p, s) case hash(p "," s): \
            if (extractMessage(p, path, indices) && !strcmp(sig, s))

        #define GET_REGION_OR_BREAK(idx)            \
            if (idx >= impl.layers_.size())         \
                break;                              \
            Layer& layer = *impl.layers_[idx];      \
            Region& region = layer.getRegion();

        #define GET_FILTER_OR_BREAK(idx)                \
            if (idx >= region.filters.size())           \
                break;                                  \
            auto& filter = region.filters[idx];

        #define GET_EQ_OR_BREAK(idx)                    \
            if (idx >= region.equalizers.size())        \
                break;                                  \
            auto& eq = region.equalizers[idx];

        #define GET_LFO_OR_BREAK(idx)             \
            if (idx >= region.lfos.size())        \
                break;                            \
            auto& lfo = region.lfos[idx];

        #define GET_EG_OR_BREAK(idx)              \
            if (idx >= region.flexEGs.size())     \
                break;                            \
            auto& eg = region.flexEGs[idx];

        #define GET_EG_POINT_OR_BREAK(idx)        \
            if (idx >= eg.points.size())          \
                break;                            \
            auto& point = eg.points[idx];

        #define GET_LFO_SUB_OR_BREAK(idx)         \
            if (idx >= lfo.sub.size())            \
                break;                            \
            auto& sub = lfo.sub[idx];


        MATCH("/hello", "") {
            client.receive(delay, "/hello", "", nullptr);
        } break;

        //----------------------------------------------------------------------

        MATCH("/num_regions", "") {
            client.receive<'i'>(delay, path, int(impl.layers_.size()));
        } break;

        MATCH("/num_groups", "") {
            client.receive<'i'>(delay, path, impl.numGroups_);
        } break;

        MATCH("/num_masters", "") {
            client.receive<'i'>(delay, path, impl.numMasters_);
        } break;

        MATCH("/num_curves", "") {
            client.receive<'i'>(delay, path, int(impl.resources_.getCurves().getNumCurves()));
        } break;

        MATCH("/num_samples", "") {
            client.receive<'i'>(delay, path, int(impl.resources_.getFilePool().getNumPreloadedSamples()));
        } break;

        MATCH("/octave_offset", "") {
            client.receive<'i'>(delay, path, impl.octaveOffset_);
        } break;

        MATCH("/note_offset", "") {
            client.receive<'i'>(delay, path, impl.noteOffset_);
        } break;

        MATCH("/num_outputs", "") {
            client.receive<'i'>(delay, path, impl.numOutputs_);
        } break;

        //----------------------------------------------------------------------

        MATCH("/key/slots", "") {
            const BitArray<128>& keys = impl.keySlots_;
            sfizz_blob_t blob { keys.data(), static_cast<uint32_t>(keys.byte_size()) };
            client.receive<'b'>(delay, path, &blob);
        } break;

        MATCH("/key&/label", "") {
            if (indices[0] >= 128)
                break;
            const std::string* label = impl.getKeyLabel(indices[0]);
            client.receive<'s'>(delay, path, label ? label->c_str() : "");
        } break;

        //----------------------------------------------------------------------

        MATCH("/root_path", "") {
            client.receive<'s'>(delay, path, impl.rootPath_.c_str());
        } break;

        MATCH("/image", "") {
            client.receive<'s'>(delay, path, impl.image_.c_str());
        } break;

        MATCH("/image_controls", "") {
            client.receive<'s'>(delay, path, impl.image_controls_.c_str());
        } break;

        //----------------------------------------------------------------------

        MATCH("/sw/last/slots", "") {
            const BitArray<128>& switches = impl.swLastSlots_;
            sfizz_blob_t blob { switches.data(), static_cast<uint32_t>(switches.byte_size()) };
            client.receive<'b'>(delay, path, &blob);
        } break;

        MATCH("/sw/last/current", "") {
            if (impl.currentSwitch_)
                client.receive<'i'>(delay, path, *impl.currentSwitch_);
            else
                client.receive<'N'>(delay, path, {});
        } break;

        MATCH("/sw/last/&/label", "") {
            if (indices[0] >= 128)
                break;
            const std::string* label = impl.getKeyswitchLabel(indices[0]);
            client.receive<'s'>(delay, path, label ? label->c_str() : "");
        } break;

        //----------------------------------------------------------------------

        MATCH("/cc/slots", "") {
            const BitArray<config::numCCs>& ccs = impl.currentUsedCCs_;
            sfizz_blob_t blob { ccs.data(), static_cast<uint32_t>(ccs.byte_size()) };
            client.receive<'b'>(delay, path, &blob);
        } break;

        MATCH("/cc&/default", "") {
            if (indices[0] >= config::numCCs)
                break;
            client.receive<'f'>(delay, path, impl.defaultCCValues_[indices[0]]);
        } break;

        MATCH("/cc&/value", "") {
            if (indices[0] >= config::numCCs)
                break;
            // Note: result value is not frame-exact
            client.receive<'f'>(delay, path, impl.resources_.getMidiState().getCCValue(indices[0]));
        } break;

        MATCH("/cc&/value", "f") {
            if (indices[0] >= config::numCCs)
                break;
            impl.resources_.getMidiState().ccEvent(delay, indices[0], args[0].f);
        } break;

        MATCH("/cc&/label", "") {
            if (indices[0] >= config::numCCs)
                break;
            const std::string* label = impl.getCCLabel(indices[0]);
            client.receive<'s'>(delay, path, label ? label->c_str() : "");
        } break;

        MATCH("/cc/changed", "") {
            const BitArray<config::numCCs>& changedCCs = impl.changedCCsThisCycle_;
            sfizz_blob_t blob { changedCCs.data(), static_cast<uint32_t>(changedCCs.byte_size()) };
            client.receive<'b'>(delay, path, &blob);
        } break;

        MATCH("/cc/changed~", "") {
            const BitArray<config::numCCs>& changedCCs = impl.changedCCsLastCycle_;
            sfizz_blob_t blob { changedCCs.data(), static_cast<uint32_t>(changedCCs.byte_size()) };
            client.receive<'b'>(delay, path, &blob);
        } break;

        MATCH("/sustain_or_sostenuto/slots", "") {
            const BitArray<128>& sustainOrSostenuto = impl.sustainOrSostenuto_;
            sfizz_blob_t blob { sustainOrSostenuto.data(),
                static_cast<uint32_t>(sustainOrSostenuto.byte_size()) };
            client.receive<'b'>(delay, path, &blob);
        } break;

        MATCH("/aftertouch", "") {
            client.receive<'f'>(delay, path, impl.resources_.getMidiState().getChannelAftertouch());
        } break;

        MATCH("/poly_aftertouch/&", "") {
            if (indices[0] > 127)
                break;
            // Note: result value is not frame-exact
            client.receive<'f'>(delay, path, impl.resources_.getMidiState().getPolyAftertouch(indices[0]));
        } break;

        MATCH("/pitch_bend", "") {
            // Note: result value is not frame-exact
            client.receive<'f'>(delay, path, impl.resources_.getMidiState().getPitchBend());
        } break;

        //----------------------------------------------------------------------

        MATCH("/mem/buffers", "") {
            uint64_t total = BufferCounter::counter().getTotalBytes();
            client.receive<'h'>(delay, path, total);
        } break;

        //----------------------------------------------------------------------

        MATCH("/region&/delay", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.delay);
        } break;
        MATCH("/region&/delay", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.delay = Opcode::transform(Default::delay, args[0].f);
        } break;

        MATCH("/region&/sample", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'s'>(delay, path, region.sampleId->filename().c_str());
        } break;

        MATCH("/region&/direction", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.sampleId->isReverse())
                client.receive<'s'>(delay, path, "reverse");
            else
                client.receive<'s'>(delay, path, "forward");
        } break;

        MATCH("/region&/delay_random", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.delayRandom);
        } break;
        MATCH("/region&/delay_random", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.delayRandom = Opcode::transform(Default::delayRandom, args[0].f);
        } break;

        MATCH("/region&/delay_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.delayCC.getWithDefault(indices[1]));
        } break;
        MATCH("/region&/delay_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.delayCC[indices[1]] = Opcode::transform(Default::delayMod, args[0].f);
        } break;

        MATCH("/region&/offset", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.offset);
        } break;
        MATCH("/region&/offset", "h") {
            GET_REGION_OR_BREAK(indices[0])
            region.offset = Opcode::transform(Default::offset, args[0].h);
        } break;

        MATCH("/region&/offset_random", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.offsetRandom);
        } break;
        MATCH("/region&/offset_random", "h") {
            GET_REGION_OR_BREAK(indices[0])
            region.offsetRandom = Opcode::transform(Default::offsetRandom, args[0].h);
        } break;
            
        MATCH("/region&/offset_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.offsetCC.getWithDefault(indices[1]));
        } break;
        MATCH("/region&/offset_cc&", "h") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.offsetCC[indices[1]] = Opcode::transform(Default::offsetMod, args[0].h);
        } break;
            
        MATCH("/region&/end", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.sampleEnd);
        } break;
        MATCH("/region&/end", "h") {
            GET_REGION_OR_BREAK(indices[0])
            region.sampleEnd = Opcode::transform(Default::sampleEnd, args[0].h);
        } break;

        MATCH("/region&/end_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.endCC.getWithDefault(indices[1]));
        } break;
        MATCH("/region&/end_cc&", "h") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.endCC[indices[1]] = Opcode::transform(Default::sampleEndMod, args[0].h);
        } break;

        MATCH("/region&/enabled", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.disabled()) {
                client.receive<'F'>(delay, path, {});
            } else {
                client.receive<'T'>(delay, path, {});
            }
        } break;

        MATCH("/region&/trigger_on_note", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.triggerOnNote) {
                client.receive<'T'>(delay, path, {});
            } else {
                client.receive<'F'>(delay, path, {});
            }
        } break;

        MATCH("/region&/trigger_on_cc", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.triggerOnCC) {
                client.receive<'T'>(delay, path, {});
            } else {
                client.receive<'F'>(delay, path, {});
            }
        } break;

        MATCH("/region&/count", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.sampleCount)
                client.receive<'h'>(delay, path, *region.sampleCount);
            else
                client.receive<'N'>(delay, path, {});
        } break;

        MATCH("/region&/loop_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].h = region.loopRange.getStart();
            args[1].h = region.loopRange.getEnd();
            client.receive(delay, path, "hh", args);
        } break;
        MATCH("/region&/loop_range", "hh") {
            GET_REGION_OR_BREAK(indices[0])
            region.loopRange.setStart(Opcode::transform(Default::loopStart, args[0].h));
            region.loopRange.setEnd(Opcode::transform(Default::loopEnd, args[1].h));
        } break;
            
        MATCH("/region&/loop_start_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.loopStartCC.getWithDefault(indices[1]));
        } break;
        MATCH("/region&/loop_start_cc&", "h") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.loopStartCC[indices[1]] = Opcode::transform(Default::loopMod, args[0].h);
        } break;

            
        MATCH("/region&/loop_end_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.loopEndCC.getWithDefault(indices[1]));
        } break;
        MATCH("/region&/loop_end_cc&", "h") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.loopEndCC[indices[1]] = Opcode::transform(Default::loopMod, args[0].h);
        } break;

        MATCH("/region&/loop_mode", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (!region.loopMode) {
                client.receive<'s'>(delay, path, "no_loop");
                break;
            }

            switch (*region.loopMode) {
            case LoopMode::no_loop:
                client.receive<'s'>(delay, path, "no_loop");
                break;
            case LoopMode::loop_continuous:
                client.receive<'s'>(delay, path, "loop_continuous");
                break;
            case LoopMode::loop_sustain:
                client.receive<'s'>(delay, path, "loop_sustain");
                break;
            case LoopMode::one_shot:
                client.receive<'s'>(delay, path, "one_shot");
                break;
            }
        } break;
        MATCH("/region&/loop_mode", "s") {
            GET_REGION_OR_BREAK(indices[0])
            region.loopMode = Opcode::readOptional(Default::loopMode, args[0].s);
        } break;
            
        MATCH("/region&/loop_crossfade", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.loopCrossfade);
        } break;
        MATCH("/region&/loop_crossfade", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.loopCrossfade = Opcode::transform(Default::loopCrossfade, args[0].f);
        } break;

        MATCH("/region&/loop_count", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.loopCount)
                client.receive<'h'>(delay, path, *region.loopCount);
            else
                client.receive<'N'>(delay, path, {});
        } break;
        MATCH("/region&/loop_count", "h") {
            GET_REGION_OR_BREAK(indices[0])
            if (args[0].h > 0)
                region.loopCount = Opcode::transform(Default::loopCount, args[0].h);
            else
                region.loopCount = {};
        } break;

            
        MATCH("/region&/output", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.output);
        } break;
        MATCH("/region&/output", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.output = Opcode::transform(Default::output, args[0].i);
        } break;
            
        MATCH("/region&/group", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.group);
        } break;
        MATCH("/region&/group", "h") {
            GET_REGION_OR_BREAK(indices[0])
            region.group = Opcode::transform(Default::group, args[0].h);
        } break;
            
        MATCH("/region&/off_by", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (!region.offBy) {
                client.receive<'N'>(delay, path, {});
            } else {
                client.receive<'h'>(delay, path, *region.offBy);
            }
        } break;
        MATCH("/region&/off_by", "h") {
            GET_REGION_OR_BREAK(indices[0])
            if (args[0].h > 0)
                region.offBy = Opcode::transform(Default::group, args[0].h);
            else
                region.offBy = {};
        } break;

            
        MATCH("/region&/off_mode", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch (region.offMode) {
            case OffMode::time:
                client.receive<'s'>(delay, path, "time");
                break;
            case OffMode::normal:
                client.receive<'s'>(delay, path, "normal");
                break;
            case OffMode::fast:
                client.receive<'s'>(delay, path, "fast");
                break;
            }
        } break;
        MATCH("/region&/off_mode", "s") {
            GET_REGION_OR_BREAK(indices[0])
            region.offMode = Opcode::read(Default::offMode, args[0].s);
        } break;
            
        MATCH("/region&/key_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].i = region.keyRange.getStart();
            args[1].i = region.keyRange.getEnd();
            client.receive(delay, path, "ii", args);
        } break;
        MATCH("/region&/key_range", "ii") {
            GET_REGION_OR_BREAK(indices[0])
            region.keyRange.setStart(Opcode::transform(Default::loKey, args[0].i));
            region.keyRange.setEnd(Opcode::transform(Default::hiKey, args[1].i));
        } break;
            
        MATCH("/region&/off_time", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.offTime);
        } break;
        MATCH("/region&/off_time", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.offTime = Opcode::transform(Default::offTime, args[0].f);
        } break;
            
        MATCH("/region&/pitch_keycenter", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.pitchKeycenter);
        } break;
        MATCH("/region&/pitch_keycenter", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.pitchKeycenter = Opcode::transform(Default::key, args[0].i);
        } break;
            
        MATCH("/region&/vel_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.velocityRange.getStart();
            args[1].f = region.velocityRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/vel_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.velocityRange.setStart(Opcode::transform(Default::loVel, args[0].f));
            region.velocityRange.setEnd(Opcode::transform(Default::hiVel, args[1].f));
        } break;
            
        MATCH("/region&/bend_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.bendRange.getStart();
            args[1].f = region.bendRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/bend_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.bendRange.setStart(Opcode::transform(Default::loBend, args[0].f));
            region.bendRange.setEnd(Opcode::transform(Default::hiBend, args[1].f));
        } break;

        MATCH("/region&/program_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].i = region.programRange.getStart();
            args[1].i = region.programRange.getEnd();
            client.receive(delay, path, "ii", args);
        } break;
        MATCH("/region&/program_range", "ii") {
            GET_REGION_OR_BREAK(indices[0])
            region.programRange.setStart(Opcode::transform(Default::loProgram, args[0].i));
            region.programRange.setEnd(Opcode::transform(Default::hiProgram, args[1].i));
        } break;

        MATCH("/region&/cc_range&", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            const auto& conditions = region.ccConditions.getWithDefault(indices[1]);
            args[0].f = conditions.getStart();
            args[1].f = conditions.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/cc_range&", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs) {
                region.ccConditions[indices[1]].setStart(Opcode::transform(Default::loCC, args[0].f));
                region.ccConditions[indices[1]].setEnd(Opcode::transform(Default::hiCC, args[0].f));
            }
        } break;

            
        MATCH("/region&/sw_last", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.lastKeyswitch) {
                client.receive<'i'>(delay, path, *region.lastKeyswitch);
            } else if (region.lastKeyswitchRange) {
                sfizz_arg_t args[2];
                args[0].i = region.lastKeyswitchRange->getStart();
                args[1].i = region.lastKeyswitchRange->getEnd();
                client.receive(delay, path, "ii", args);
            } else {
                client.receive<'N'>(delay, path, {});
            }

        } break;
        MATCH("/region&/sw_last", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.lastKeyswitch = Opcode::transform(Default::key, args[0].i);
            region.lastKeyswitchRange = {};
        } break;
        MATCH("/region&/sw_last", "ii") {
            GET_REGION_OR_BREAK(indices[0])
            auto start = Opcode::transform(Default::key, args[0].i);
            auto end = Opcode::transform(Default::key, args[1].i);

            if (!region.lastKeyswitchRange)
                region.lastKeyswitchRange.emplace(start, end);
            else {
                region.lastKeyswitchRange->setStart(start);
                region.lastKeyswitchRange->setEnd(end);
            }
        } break;

            
        MATCH("/region&/sw_label", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.keyswitchLabel) {
                client.receive<'s'>(delay, path, region.keyswitchLabel->c_str());
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/sw_label", "s") {
            GET_REGION_OR_BREAK(indices[0])
            if (strlen(args[0].s) > 0)
                region.keyswitchLabel = args[0].s;
            else
                region.keyswitchLabel = {};
        } break;
            
        MATCH("/region&/sw_up", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.upKeyswitch) {
                client.receive<'i'>(delay, path, *region.upKeyswitch);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/sw_up", "i") {
            GET_REGION_OR_BREAK(indices[0])
            if (args[0].i >= 0)
                region.upKeyswitch = Opcode::transform(Default::key, args[0].i);
            else
                region.upKeyswitch = {};
        } break;
            
        MATCH("/region&/sw_down", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.downKeyswitch) {
                client.receive<'i'>(delay, path, *region.downKeyswitch);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/sw_down", "i") {
            GET_REGION_OR_BREAK(indices[0])
            if (args[0].i >= 0)
                region.downKeyswitch = Opcode::transform(Default::key, args[0].i);
            else
                region.downKeyswitch = {};
            
            region.usesKeySwitches = region.downKeyswitch.has_value();
        } break;

        MATCH("/region&/sw_previous", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.previousKeyswitch) {
                client.receive<'i'>(delay, path, *region.previousKeyswitch);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/sw_previous", "i") {
            GET_REGION_OR_BREAK(indices[0])
            if (args[0].i >= 0)
                region.previousKeyswitch = Opcode::transform(Default::key, args[0].i);
            else
                region.previousKeyswitch = {};
        } break;

            
        MATCH("/region&/sw_vel", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch (region.velocityOverride) {
            case VelocityOverride::current:
                client.receive<'s'>(delay, path, "current");
                break;
            case VelocityOverride::previous:
                client.receive<'s'>(delay, path, "previous");
                break;
            }
        } break;

        MATCH("/region&/chanaft_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.aftertouchRange.getStart();
            args[1].f = region.aftertouchRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/chanaft_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.aftertouchRange.setStart(Opcode::transform(Default::loChannelAftertouch, args[0].f));
            region.aftertouchRange.setEnd(Opcode::transform(Default::hiChannelAftertouch, args[1].f));
        } break;

        MATCH("/region&/polyaft_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.polyAftertouchRange.getStart();
            args[1].f = region.polyAftertouchRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/polyaft_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.polyAftertouchRange.setStart(Opcode::transform(Default::loPolyAftertouch, args[0].f));
            region.polyAftertouchRange.setEnd(Opcode::transform(Default::hiPolyAftertouch, args[1].f));
        } break;

            
        MATCH("/region&/bpm_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.bpmRange.getStart();
            args[1].f = region.bpmRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/bpm_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.bpmRange.setStart(Opcode::transform(Default::loBPM, args[0].f));
            region.bpmRange.setEnd(Opcode::transform(Default::hiBPM, args[1].f));
        } break;

        MATCH("/region&/rand_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.randRange.getStart();
            args[1].f = region.randRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/rand_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.randRange.setStart(Opcode::transform(Default::loNormalized, args[0].f));
            region.randRange.setEnd(Opcode::transform(Default::hiNormalized, args[1].f));
        } break;

        MATCH("/region&/seq_length", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.sequenceLength);
        } break;
        MATCH("/region&/seq_length", "h") {
            GET_REGION_OR_BREAK(indices[0])
            region.sequenceLength = Opcode::transform(Default::sequence, args[0].h);
        } break;

        MATCH("/region&/seq_position", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'h'>(delay, path, region.sequencePosition);
        } break;
        MATCH("/region&/seq_position", "h") {
            GET_REGION_OR_BREAK(indices[0])
            region.sequencePosition = Opcode::transform(Default::sequence, args[0].h);
        } break;
            
        MATCH("/region&/trigger", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch (region.trigger) {
            case Trigger::attack:
                client.receive<'s'>(delay, path, "attack");
                break;
            case Trigger::first:
                client.receive<'s'>(delay, path, "first");
                break;
            case Trigger::release:
                client.receive<'s'>(delay, path, "release");
                break;
            case Trigger::release_key:
                client.receive<'s'>(delay, path, "release_key");
                break;
            case Trigger::legato:
                client.receive<'s'>(delay, path, "legato");
                break;
            }
        } break;
        MATCH("/region&/trigger", "s") {
            GET_REGION_OR_BREAK(indices[0])
            region.trigger = Opcode::read(Default::trigger, args[0].s);
        } break;
            
        MATCH("/region&/start_cc_range&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto trigger = region.ccTriggers.get(indices[1]);
            if (trigger) {
                sfizz_arg_t args[2];
                args[0].f = trigger->getStart();
                args[1].f = trigger->getEnd();
                client.receive(delay, path, "ff", args);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/start_cc_range&", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs) {                
                region.ccTriggers[indices[1]].setStart(Opcode::transform(Default::loCC, args[0].f));
                region.ccTriggers[indices[1]].setEnd(Opcode::transform(Default::hiCC, args[0].f));
                region.triggerOnCC = true;
            }
        } break;
            
        MATCH("/region&/volume", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.volume);
        } break;
        MATCH("/region&/volume", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.volume = Opcode::transform(Default::volume, args[0].f);
        } break;
            
        MATCH("/region&/volume_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto value = region.ccModDepth(indices[1], ModId::Volume);
            if (value) {
                client.receive<'f'>(delay, path, *value);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/volume_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Volume);
            if (params) {
                client.receive<'f'>(delay, path, params->step);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/volume_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Volume);
            if (params) {
                client.receive<'i'>(delay, path, params->smooth);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/volume_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Volume);
            if (params) {
                client.receive<'i'>(delay, path, params->curve);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/pan", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.pan * 100.0f);
        } break;
        MATCH("/region&/pan", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.pan = Opcode::transform(Default::pan, args[0].f); // correct?
        } break;

        // TODO: value set for ccModDepth and ccModParameter stuff below

        MATCH("/region&/pan_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto value = region.ccModDepth(indices[1], ModId::Pan);
            if (value) {
                client.receive<'f'>(delay, path, *value * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/pan_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Pan);
            if (params) {
                client.receive<'f'>(delay, path, params->step * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/pan_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Pan);
            if (params) {
                client.receive<'i'>(delay, path, params->smooth);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/pan_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Pan);
            if (params) {
                client.receive<'i'>(delay, path, params->curve);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/width", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.width * 100.0f);
        } break;
        MATCH("/region&/width", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.width = Opcode::transform(Default::width, args[0].f); // correct?
        } break;

        MATCH("/region&/width_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto value = region.ccModDepth(indices[1], ModId::Width);
            if (value) {
                client.receive<'f'>(delay, path, *value * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/width_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Width);
            if (params) {
                client.receive<'f'>(delay, path, params->step * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/width_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Width);
            if (params) {
                client.receive<'i'>(delay, path, params->smooth);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/width_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Width);
            if (params) {
                client.receive<'i'>(delay, path, params->curve);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/position", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.position * 100.0f);
        } break;
        MATCH("/region&/position", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.position = Opcode::transform(Default::position, args[0].f); // correct?
        } break;

        MATCH("/region&/position_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto value = region.ccModDepth(indices[1], ModId::Position);
            if (value) {
                client.receive<'f'>(delay, path, *value * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/position_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Position);
            if (params) {
                client.receive<'f'>(delay, path, params->step * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/position_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Position);
            if (params) {
                client.receive<'i'>(delay, path, params->smooth);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/position_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Position);
            if (params) {
                client.receive<'i'>(delay, path, params->curve);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/amplitude", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitude * 100.0f);
        } break;
        MATCH("/region&/amplitude", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitude = Opcode::transform(Default::amplitude, args[0].f); // correct?
        } break;

        MATCH("/region&/amplitude_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto value = region.ccModDepth(indices[1], ModId::Amplitude);
            if (value) {
                client.receive<'f'>(delay, path, *value * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/amplitude_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Amplitude);
            if (params) {
                client.receive<'f'>(delay, path, params->step * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/amplitude_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Amplitude);
            if (params) {
                client.receive<'i'>(delay, path, params->smooth);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/amplitude_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Amplitude);
            if (params) {
                client.receive<'i'>(delay, path, params->curve);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;

        MATCH("/region&/amp_keycenter", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.ampKeycenter);
        } break;
        MATCH("/region&/amp_keycenter", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.ampKeycenter = Opcode::transform(Default::key, args[0].i);
        } break;

        MATCH("/region&/amp_keytrack", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.ampKeytrack);
        } break;
        MATCH("/region&/amp_keytrack", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.ampKeytrack = Opcode::transform(Default::ampKeytrack, args[0].f);
        } break;

        MATCH("/region&/amp_veltrack", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.ampVeltrack * 100.0f);
        } break;
        MATCH("/region&/amp_veltrack", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.ampVeltrack = Opcode::transform(Default::ampVeltrack, args[0].f); // correct?
        } break;

        MATCH("/region&/amp_veltrack_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.ampVeltrackCC.contains(indices[1])) {
                const auto& cc = region.ampVeltrackCC.getWithDefault(indices[1]);
                client.receive<'f'>(delay, path, cc.modifier * 100.0f);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/amp_veltrack_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.ampVeltrackCC[indices[1]].modifier = Opcode::transform(Default::ampVeltrackMod, args[0].f);
        } break;
            
        MATCH("/region&/amp_veltrack_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.ampVeltrackCC.contains(indices[1])) {
                const auto& cc = region.ampVeltrackCC.getWithDefault(indices[1]);
                client.receive<'i'>(delay, path, cc.curve );
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/amp_veltrack_curvecc&", "i") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.ampVeltrackCC[indices[1]].curve = Opcode::transform(Default::curveCC, args[0].i);
        } break;

        MATCH("/region&/amp_random", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.ampRandom);
        } break;
        MATCH("/region&/amp_random", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.ampRandom = Opcode::transform(Default::ampRandom, args[0].f);
        } break;

        MATCH("/region&/xfin_key_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].i = region.crossfadeKeyInRange.getStart();
            args[1].i = region.crossfadeKeyInRange.getEnd();
            client.receive(delay, path, "ii", args);
        } break;
        MATCH("/region&/xfin_key_range", "ii") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeKeyInRange.setStart(Opcode::transform(Default::loKey, args[0].i));
            region.crossfadeKeyInRange.setEnd(Opcode::transform(Default::hiKey, args[1].i));
        } break;

        MATCH("/region&/xfout_key_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].i = region.crossfadeKeyOutRange.getStart();
            args[1].i = region.crossfadeKeyOutRange.getEnd();
            client.receive(delay, path, "ii", args);
        } break;
        MATCH("/region&/xfout_key_range", "ii") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeKeyOutRange.setStart(Opcode::transform(Default::loKey, args[0].i));
            region.crossfadeKeyOutRange.setEnd(Opcode::transform(Default::hiKey, args[1].i));
        } break;

        MATCH("/region&/xfin_vel_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.crossfadeVelInRange.getStart();
            args[1].f = region.crossfadeVelInRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/xfin_vel_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeVelInRange.setStart(Opcode::transform(Default::xfinLo, args[0].f));
            region.crossfadeVelInRange.setEnd(Opcode::transform(Default::xfinHi, args[1].f));
        } break;

        MATCH("/region&/xfout_vel_range", "") {
            GET_REGION_OR_BREAK(indices[0])
            sfizz_arg_t args[2];
            args[0].f = region.crossfadeVelOutRange.getStart();
            args[1].f = region.crossfadeVelOutRange.getEnd();
            client.receive(delay, path, "ff", args);
        } break;
        MATCH("/region&/xfout_vel_range", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeVelOutRange.setStart(Opcode::transform(Default::xfoutLo, args[0].f));
            region.crossfadeVelOutRange.setEnd(Opcode::transform(Default::xfoutHi, args[1].f));
        } break;

        MATCH("/region&/xfin_cc_range&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto range = region.crossfadeCCInRange.get(indices[1]);
            if (range) {
                sfizz_arg_t args[2];
                args[0].f = range->getStart();
                args[1].f = range->getEnd();
                client.receive(delay, path, "ff", args);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/xfin_cc_range&", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs) {
                region.crossfadeCCInRange[indices[1]].setStart(Opcode::transform(Default::xfinLo, args[0].f));
                region.crossfadeCCInRange[indices[1]].setEnd(Opcode::transform(Default::xfinHi, args[0].f));
            }
        } break;
            
        MATCH("/region&/xfout_cc_range&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto range = region.crossfadeCCOutRange.get(indices[1]);
            if (range) {
                sfizz_arg_t args[2];
                args[0].f = range->getStart();
                args[1].f = range->getEnd();
                client.receive(delay, path, "ff", args);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/xfout_cc_range&", "ff") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs) {
                region.crossfadeCCOutRange[indices[1]].setStart(Opcode::transform(Default::xfoutLo, args[0].f));
                region.crossfadeCCOutRange[indices[1]].setEnd(Opcode::transform(Default::xfoutHi, args[0].f));
            }
        } break;

        MATCH("/region&/xf_keycurve", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch (region.crossfadeKeyCurve) {
            case CrossfadeCurve::gain:
                client.receive<'s'>(delay, path, "gain");
                break;
            case CrossfadeCurve::power:
                client.receive<'s'>(delay, path, "power");
                break;
            }
        } break;
        MATCH("/region&/xf_keycurve", "s") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeKeyCurve = Opcode::read(Default::crossfadeCurve, args[0].s);
        } break;
            
        MATCH("/region&/xf_velcurve", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch (region.crossfadeVelCurve) {
            case CrossfadeCurve::gain:
                client.receive<'s'>(delay, path, "gain");
                break;
            case CrossfadeCurve::power:
                client.receive<'s'>(delay, path, "power");
                break;
            }
        } break;
        MATCH("/region&/xf_velcurve", "s") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeVelCurve = Opcode::read(Default::crossfadeCurve, args[0].s);
        } break;

        MATCH("/region&/xf_cccurve", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch (region.crossfadeCCCurve) {
            case CrossfadeCurve::gain:
                client.receive<'s'>(delay, path, "gain");
                break;
            case CrossfadeCurve::power:
                client.receive<'s'>(delay, path, "power");
                break;
            }
        } break;
        MATCH("/region&/xf_cccurve", "s") {
            GET_REGION_OR_BREAK(indices[0])
            region.crossfadeCCCurve = Opcode::read(Default::crossfadeCurve, args[0].s);
        } break;

            
        MATCH("/region&/global_volume", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.globalVolume);
        } break;
        MATCH("/region&/global_volume", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.globalVolume = Opcode::transform(Default::volume, args[0].f);
        } break;

        MATCH("/region&/master_volume", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.masterVolume);
        } break;
        MATCH("/region&/master_volume", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.masterVolume = Opcode::transform(Default::volume, args[0].f);
        } break;

        MATCH("/region&/group_volume", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.groupVolume);
        } break;
        MATCH("/region&/group_volume", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.groupVolume = Opcode::transform(Default::volume, args[0].f);
        } break;

        MATCH("/region&/global_amplitude", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.globalAmplitude * 100.0f);
        } break;
        MATCH("/region&/global_amplitude", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.globalAmplitude = Opcode::transform(Default::amplitude, args[0].f); // correct?
        } break;

        MATCH("/region&/master_amplitude", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.masterAmplitude * 100.0f);
        } break;
        MATCH("/region&/master_amplitude", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.masterAmplitude = Opcode::transform(Default::amplitude, args[0].f); // correct?
        } break;

        MATCH("/region&/group_amplitude", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.groupAmplitude * 100.0f);
        } break;
        MATCH("/region&/group_amplitude", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.groupAmplitude = Opcode::transform(Default::amplitude, args[0].f); // correct?
        } break;

        MATCH("/region&/pitch_keytrack", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.pitchKeytrack);
        } break;
        MATCH("/region&/pitch_keytrack", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.pitchKeytrack = Opcode::transform(Default::pitchKeytrack, args[0].i);
        } break;
            
        MATCH("/region&/pitch_veltrack", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.pitchVeltrack);
        } break;
        MATCH("/region&/pitch_veltrack", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.pitchVeltrack = Opcode::transform(Default::pitchVeltrack, args[0].i);
        } break;

        MATCH("/region&/pitch_veltrack_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.pitchVeltrackCC.contains(indices[1])) {
                const auto& cc = region.pitchVeltrackCC.getWithDefault(indices[1]);
                client.receive<'f'>(delay, path, cc.modifier);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/pitch_veltrack_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.pitchVeltrackCC[indices[1]].modifier = Opcode::transform(Default::pitchVeltrackMod, args[0].f);
        } break;
            
        MATCH("/region&/pitch_veltrack_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.pitchVeltrackCC.contains(indices[1])) {
                const auto& cc = region.pitchVeltrackCC.getWithDefault(indices[1]);
                client.receive<'i'>(delay, path, cc.curve );
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/pitch_veltrack_curvecc&", "i") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.pitchVeltrackCC[indices[1]].curve = Opcode::transform(Default::curveCC, args[0].i);
        } break;

        MATCH("/region&/pitch_random", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.pitchRandom);
        } break;
        MATCH("/region&/pitch_random", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.pitchRandom = Opcode::transform(Default::pitchRandom, args[0].f);
        } break;

        MATCH("/region&/transpose", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.transpose);
        } break;
        MATCH("/region&/transpose", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.transpose = Opcode::transform(Default::transpose, args[0].i);
        } break;

        MATCH("/region&/pitch", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.pitch);
        } break;
        MATCH("/region&/pitch", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.pitch = Opcode::transform(Default::pitch, args[0].f);
        } break;

        MATCH("/region&/pitch_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto value = region.ccModDepth(indices[1], ModId::Pitch);
            if (value) {
                client.receive<'f'>(delay, path, *value);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        // TODO: value set

        MATCH("/region&/pitch_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Pitch);
            if (params) {
                client.receive<'f'>(delay, path, params->step);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        // TODO: value set

        MATCH("/region&/pitch_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Pitch);
            if (params) {
                client.receive<'i'>(delay, path, params->smooth);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        // TODO: value set

        MATCH("/region&/pitch_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto params = region.ccModParameters(indices[1], ModId::Pitch);
            if (params) {
                client.receive<'i'>(delay, path, params->curve);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        // TODO: value set

        MATCH("/region&/bend_up", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.bendUp);
        } break;
        MATCH("/region&/bend_up", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.bendUp = Opcode::transform(Default::bendUp, args[0].f);
        } break;
            
        MATCH("/region&/bend_down", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.bendDown);
        } break;
        MATCH("/region&/bend_down", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.bendDown = Opcode::transform(Default::bendDown, args[0].f);
        } break;

        MATCH("/region&/bend_step", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.bendStep);
        } break;
        MATCH("/region&/bend_step", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.bendStep = Opcode::transform(Default::bendStep, args[0].f);
        } break;

        MATCH("/region&/bend_smooth", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.bendSmooth);
        } break;
        MATCH("/region&/bend_smooth", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.bendSmooth = Opcode::transform(Default::smoothCC, args[0].i);
        } break;

        MATCH("/region&/ampeg_attack", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.attack);
        } break;
        MATCH("/region&/ampeg_attack", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.attack = Opcode::transform(Default::egTime, args[0].f);
        } break;

        MATCH("/region&/ampeg_delay", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.delay);
        } break;
        MATCH("/region&/ampeg_delay", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.delay = Opcode::transform(Default::egTime, args[0].f);
        } break;

        MATCH("/region&/ampeg_decay", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.decay);
        } break;
        MATCH("/region&/ampeg_decay", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.decay = Opcode::transform(Default::egTime, args[0].f);
        } break;

        MATCH("/region&/ampeg_hold", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.hold);
        } break;
        MATCH("/region&/ampeg_hold", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.hold = Opcode::transform(Default::egTime, args[0].f);
        } break;

        MATCH("/region&/ampeg_release", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.release);
        } break;
        MATCH("/region&/ampeg_release", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.release = Opcode::transform(Default::egRelease, args[0].f);
        } break;

            
        MATCH("/region&/ampeg_start", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.start * 100.0f);
        } break;
        MATCH("/region&/ampeg_start", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.start = Opcode::transform(Default::egPercent, args[0].f);
        } break;

        MATCH("/region&/ampeg_sustain", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.sustain * 100.0f);
        } break;
        MATCH("/region&/ampeg_sustain", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.sustain = Opcode::transform(Default::egPercent, args[0].f);
        } break;

        MATCH("/region&/ampeg_depth", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.amplitudeEG.depth);
        } break;
        MATCH("/region&/ampeg_depth", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.depth = Opcode::transform(Default::egDepth, args[0].f);
        } break;

        MATCH("/region&/ampeg_vel&attack", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2attack);
        } break;
        MATCH("/region&/ampeg_vel&attack", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2attack = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_vel&delay", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2delay);
        } break;
        MATCH("/region&/ampeg_vel&delay", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2delay = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_vel&decay", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2decay);
        } break;
        MATCH("/region&/ampeg_vel&decay", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2decay = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_vel&hold", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2hold);
        } break;
        MATCH("/region&/ampeg_vel&hold", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2hold = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;
            
        MATCH("/region&/ampeg_vel&release", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2release);
        } break;
        MATCH("/region&/ampeg_vel&release", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2release = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;
            
        MATCH("/region&/ampeg_vel&sustain", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2sustain * 100.0f);
        } break;
        MATCH("/region&/ampeg_vel&sustain", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2sustain = Opcode::transform(Default::egPercentMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_vel&depth", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            client.receive<'f'>(delay, path, region.amplitudeEG.vel2depth);
        } break;
        MATCH("/region&/ampeg_vel&depth", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] != 2)
                break;
            region.amplitudeEG.vel2depth = Opcode::transform(Default::egVel2Depth, args[0].f);
        } break;

        MATCH("/region&/ampeg_dynamic", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.amplitudeEG.dynamic) {
                client.receive<'T'>(delay, path, {});
            } else {
                client.receive<'F'>(delay, path, {});
            }
        } break;
        MATCH("/region&/ampeg_dynamic", "T") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.dynamic = true;
        } break;
        MATCH("/region&/ampeg_dynamic", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.amplitudeEG.dynamic = false;
        } break;

        MATCH("/region&/fileg_dynamic", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.filterEG && region.filterEG->dynamic) {
                client.receive<'T'>(delay, path, {});
            } else {
                client.receive<'F'>(delay, path, {});
            }
        } break;
        MATCH("/region&/fileg_dynamic", "T") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.filterEG) {
                region.filterEG->dynamic = true;
            }
        } break;
        MATCH("/region&/fileg_dynamic", "F") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.filterEG) {
                region.filterEG->dynamic = false;
            }
        } break;

        MATCH("/region&/pitcheg_dynamic", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.pitchEG && region.pitchEG->dynamic) {
                client.receive<'T'>(delay, path, {});
            } else {
                client.receive<'F'>(delay, path, {});
            }
        } break;
        MATCH("/region&/pitcheg_dynamic", "T") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.pitchEG) {
                region.pitchEG->dynamic = true;
            }
        } break;
        MATCH("/region&/pitcheg_dynamic", "F") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.pitchEG) {
                region.pitchEG->dynamic = false;
            }
        } break;

            
        MATCH("/region&/note_polyphony", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.notePolyphony) {
                client.receive<'i'>(delay, path, *region.notePolyphony);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/note_polyphony", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.notePolyphony = Opcode::transform(Default::notePolyphony, args[0].i);
        } break;
        MATCH("/region&/note_polyphony", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.notePolyphony = {};
        } break;

            
        MATCH("/region&/note_selfmask", "") {
            GET_REGION_OR_BREAK(indices[0])
            switch(region.selfMask) {
            case SelfMask::mask:
                client.receive(delay, path, "T", nullptr);
                break;
            case SelfMask::dontMask:
                client.receive(delay, path, "F", nullptr);
                break;
            }
        } break;
        MATCH("/region&/note_selfmask", "T") {
            GET_REGION_OR_BREAK(indices[0])
            region.selfMask = SelfMask::mask;
        } break;
        MATCH("/region&/note_selfmask", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.selfMask = SelfMask::dontMask;
        } break;

        MATCH("/region&/rt_dead", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.rtDead) {
                client.receive(delay, path, "T", nullptr);
            } else {
                client.receive(delay, path, "F", nullptr);
            }
        } break;
        MATCH("/region&/rt_dead", "T") {
            GET_REGION_OR_BREAK(indices[0])
            region.rtDead = true;
        } break;
        MATCH("/region&/rt_dead", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.rtDead = false;
        } break;

        MATCH("/region&/sustain_sw", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.checkSustain) {
                client.receive(delay, path, "T", nullptr);
            } else {
                client.receive(delay, path, "F", nullptr);
            }
        } break;
        MATCH("/region&/sustain_sw", "T") {
            GET_REGION_OR_BREAK(indices[0])
            region.checkSustain = true;
        } break;
        MATCH("/region&/sustain_sw", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.checkSustain = false;
        } break;

        MATCH("/region&/sostenuto_sw", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.checkSostenuto) {
                client.receive(delay, path, "T", nullptr);
            } else {
                client.receive(delay, path, "F", nullptr);
            }
        } break;
        MATCH("/region&/sostenuto_sw", "T") {
            GET_REGION_OR_BREAK(indices[0])
            region.checkSostenuto = true;
        } break;
        MATCH("/region&/sostenuto_sw", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.checkSostenuto = false;
        } break;

        MATCH("/region&/sustain_cc", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.sustainCC);
        } break;
        MATCH("/region&/sustain_cc", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.sustainCC = Opcode::transform(Default::sustainCC, args[0].i);
        } break;

        MATCH("/region&/sostenuto_cc", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.sostenutoCC);
        } break;
        MATCH("/region&/sostenuto_cc", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.sostenutoCC = Opcode::transform(Default::sostenutoCC, args[0].i);
        } break;

        MATCH("/region&/sustain_lo", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.sustainThreshold);
        } break;
        MATCH("/region&/sustain_lo", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.sustainThreshold = Opcode::transform(Default::sustainThreshold, args[0].f);
        } break;

        MATCH("/region&/sostenuto_lo", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.sostenutoThreshold);
        } break;
        MATCH("/region&/sostenuto_lo", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.sostenutoThreshold = Opcode::transform(Default::sostenutoThreshold, args[0].f);
        } break;

        MATCH("/region&/oscillator_phase", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.oscillatorPhase);
        } break;
        MATCH("/region&/oscillator_phase", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorPhase = Opcode::transform(Default::oscillatorPhase, args[0].f);
        } break;

        MATCH("/region&/oscillator_quality", "") {
            GET_REGION_OR_BREAK(indices[0])
            if (region.oscillatorQuality) {
                client.receive<'i'>(delay, path, *region.oscillatorQuality);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/oscillator_quality", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorQuality = Opcode::transform(Default::oscillatorQuality, args[0].i);
        } break;
        MATCH("/region&/oscillator_quality", "F") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorQuality = {};
        } break;

        MATCH("/region&/oscillator_mode", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.oscillatorMode);
        } break;
        MATCH("/region&/oscillator_mode", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorMode = Opcode::transform(Default::oscillatorMode, args[0].i);
        } break;

        MATCH("/region&/oscillator_multi", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, region.oscillatorMulti);
        } break;
        MATCH("/region&/oscillator_multi", "i") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorMulti = Opcode::transform(Default::oscillatorMulti, args[0].i);
        } break;

        MATCH("/region&/oscillator_detune", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.oscillatorDetune);
        } break;
        MATCH("/region&/oscillator_detune", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorDetune = Opcode::transform(Default::oscillatorDetune, args[0].f);
        } break;

        MATCH("/region&/oscillator_mod_depth", "") {
            GET_REGION_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, region.oscillatorModDepth * 100.0f);
        } break;
        MATCH("/region&/oscillator_mod_depth", "f") {
            GET_REGION_OR_BREAK(indices[0])
            region.oscillatorModDepth = Opcode::transform(Default::oscillatorModDepth, args[0].f);
        } break;

        // TODO: detune cc, mod depth cc

        MATCH("/region&/effect&", "") {
            GET_REGION_OR_BREAK(indices[0])
            auto effectIdx = indices[1];
            if (indices[1] == 0)
                break;

            if (effectIdx < region.gainToEffect.size())
                client.receive<'f'>(delay, path, region.gainToEffect[effectIdx] * 100.0f);
        } break;
        MATCH("/region&/effect&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            auto effectIdx = indices[1];
            if (effectIdx > 0 && effectIdx < region.gainToEffect.size())
                region.gainToEffect[effectIdx] = Opcode::transform(Default::effect, args[0].f);
        } break;

        MATCH("/region&/ampeg_attack_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccAttack.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value);
        } break;
        MATCH("/region&/ampeg_attack_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccAttack[indices[1]] = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;
            
        MATCH("/region&/ampeg_decay_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccDecay.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value);
        } break;
        MATCH("/region&/ampeg_decay_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccDecay[indices[1]] = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_delay_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccDelay.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value);
        } break;
        MATCH("/region&/ampeg_delay_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccDelay[indices[1]] = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_hold_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccHold.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value);
        } break;
        MATCH("/region&/ampeg_hold_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccHold[indices[1]] = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

            
        MATCH("/region&/ampeg_release_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccRelease.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value);
        } break;
        MATCH("/region&/ampeg_release_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccRelease[indices[1]] = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_start_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccStart.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value * 100.0f);
        } break;
        MATCH("/region&/ampeg_start_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccStart[indices[1]] = Opcode::transform(Default::egTimeMod, args[0].f);
        } break;

        MATCH("/region&/ampeg_sustain_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            float value = region.amplitudeEG.ccSustain.getWithDefault(indices[1]);
            client.receive<'f'>(delay, path, value * 100.0f);
        } break;
        MATCH("/region&/ampeg_sustain_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            if (indices[1] < config::numCCs)
                region.amplitudeEG.ccSustain[indices[1]] = Opcode::transform(Default::egPercentMod, args[0].f);
        } break;

        MATCH("/region&/filter&/cutoff", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            client.receive<'f'>(delay, path, filter.cutoff);
        } break;
        MATCH("/region&/filter&/cutoff", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.cutoff = Opcode::transform(Default::filterCutoff, args[0].f);
        } break;

        // TODO: setters for ccModDepth or ccModParameters
        MATCH("/region&/filter&/cutoff_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            const auto depth = region.ccModDepth(indices[2], ModId::FilCutoff, indices[1]);
            if (depth)
                client.receive<'f'>(delay, path, *depth);
        } break;

        MATCH("/region&/filter&/cutoff_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            const auto params = region.ccModParameters(indices[2], ModId::FilCutoff, indices[1]);
            if (params)
                client.receive<'i'>(delay, path, params->curve);
        } break;

        MATCH("/region&/filter&/cutoff_stepcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            const auto params = region.ccModParameters(indices[2], ModId::FilCutoff, indices[1]);
            if (params)
                client.receive<'i'>(delay, path, params->step);
        } break;

        MATCH("/region&/filter&/cutoff_smoothcc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            const auto params = region.ccModParameters(indices[2], ModId::FilCutoff, indices[1]);
            if (params)
                client.receive<'i'>(delay, path, params->smooth);
        } break;

        MATCH("/region&/filter&/resonance", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            client.receive<'f'>(delay, path, filter.resonance);
        } break;
        MATCH("/region&/filter&/resonance", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.resonance = Opcode::transform(Default::filterResonance, args[0].f);
        } break;

        MATCH("/region&/filter&/gain", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            client.receive<'f'>(delay, path, filter.gain);
        } break;
        MATCH("/region&/filter&/gain", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.gain = Opcode::transform(Default::filterGain, args[0].f);
        } break;

        MATCH("/region&/filter&/keycenter", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            client.receive<'i'>(delay, path, filter.keycenter);
        } break;
        MATCH("/region&/filter&/keycenter", "i") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.keycenter = Opcode::transform(Default::key, args[0].i);
        } break;

        MATCH("/region&/filter&/keytrack", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            client.receive<'i'>(delay, path, filter.keytrack);
        } break;
        MATCH("/region&/filter&/keytrack", "i") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.keytrack = Opcode::transform(Default::filterKeytrack, args[0].i);
        } break;

        MATCH("/region&/filter&/veltrack", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            client.receive<'i'>(delay, path, filter.veltrack);
        } break;
        MATCH("/region&/filter&/veltrack", "i") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.veltrack = Opcode::transform(Default::filterVeltrack, args[0].i);
        } break;

        MATCH("/region&/filter&/veltrack_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            if (filter.veltrackCC.contains(indices[2])) {
                const auto& cc = filter.veltrackCC.getWithDefault(indices[2]);
                client.receive<'f'>(delay, path, cc.modifier);
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/filter&/veltrack_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            if (indices[2] < config::numCCs)
                filter.veltrackCC[indices[2]].modifier = Opcode::transform(Default::ampVeltrackMod, args[0].f);
        } break;
            
        MATCH("/region&/filter&/veltrack_curvecc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            if (filter.veltrackCC.contains(indices[2])) {
                const auto& cc = filter.veltrackCC.getWithDefault(indices[2]);
                client.receive<'i'>(delay, path, cc.curve );
            } else {
                client.receive<'N'>(delay, path, {});
            }
        } break;
        MATCH("/region&/filter&/veltrack_cc&", "i") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            if (indices[2] < config::numCCs)
                filter.veltrackCC[indices[2]].curve = Opcode::transform(Default::curveCC, args[0].i);
        } break;

        MATCH("/region&/filter&/type", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            switch (filter.type) {
            case FilterType::kFilterLpf1p: client.receive<'s'>(delay, path, "lpf_1p"); break;
            case FilterType::kFilterHpf1p: client.receive<'s'>(delay, path, "hpf_1p"); break;
            case FilterType::kFilterLpf2p: client.receive<'s'>(delay, path, "lpf_2p"); break;
            case FilterType::kFilterHpf2p: client.receive<'s'>(delay, path, "hpf_2p"); break;
            case FilterType::kFilterBpf2p: client.receive<'s'>(delay, path, "bpf_2p"); break;
            case FilterType::kFilterBrf2p: client.receive<'s'>(delay, path, "brf_2p"); break;
            case FilterType::kFilterBpf1p: client.receive<'s'>(delay, path, "bpf_1p"); break;
            case FilterType::kFilterBrf1p: client.receive<'s'>(delay, path, "brf_1p"); break;
            case FilterType::kFilterApf1p: client.receive<'s'>(delay, path, "apf_1p"); break;
            case FilterType::kFilterLpf2pSv: client.receive<'s'>(delay, path, "lpf_2p_sv"); break;
            case FilterType::kFilterHpf2pSv: client.receive<'s'>(delay, path, "hpf_2p_sv"); break;
            case FilterType::kFilterBpf2pSv: client.receive<'s'>(delay, path, "bpf_2p_sv"); break;
            case FilterType::kFilterBrf2pSv: client.receive<'s'>(delay, path, "brf_2p_sv"); break;
            case FilterType::kFilterLpf4p: client.receive<'s'>(delay, path, "lpf_4p"); break;
            case FilterType::kFilterHpf4p: client.receive<'s'>(delay, path, "hpf_4p"); break;
            case FilterType::kFilterLpf6p: client.receive<'s'>(delay, path, "lpf_6p"); break;
            case FilterType::kFilterHpf6p: client.receive<'s'>(delay, path, "hpf_6p"); break;
            case FilterType::kFilterPink: client.receive<'s'>(delay, path, "pink"); break;
            case FilterType::kFilterLsh: client.receive<'s'>(delay, path, "lsh"); break;
            case FilterType::kFilterHsh: client.receive<'s'>(delay, path, "hsh"); break;
            case FilterType::kFilterPeq: client.receive<'s'>(delay, path, "peq"); break;
            case FilterType::kFilterBpf4p: client.receive<'s'>(delay, path, "bpf_4p"); break;
            case FilterType::kFilterBpf6p: client.receive<'s'>(delay, path, "bpf_6p"); break;
            case FilterType::kFilterNone: client.receive<'s'>(delay, path, "none"); break;
            }
        } break;
        MATCH("/region&/filter&/type", "s") {
            GET_REGION_OR_BREAK(indices[0])
            GET_FILTER_OR_BREAK(indices[1])
            filter.type = Opcode::read(Default::filter, args[0].s);
        } break;
            
        MATCH("/region&/eq&/gain", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            client.receive<'f'>(delay, path, eq.gain);
        } break;
        MATCH("/region&/eq&/gain", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            eq.gain = Opcode::transform(Default::eqGain, args[0].f);
        } break;

        MATCH("/region&/eq&/bandwidth", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            client.receive<'f'>(delay, path, eq.bandwidth);
        } break;
        MATCH("/region&/eq&/bandwidth", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            eq.bandwidth = Opcode::transform(Default::eqBandwidth, args[0].f);
        } break;

        MATCH("/region&/eq&/frequency", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            client.receive<'f'>(delay, path, eq.frequency);
        } break;
        MATCH("/region&/eq&/frequency", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            eq.frequency = Opcode::transform(Default::eqFrequency, args[0].f);
        } break;

        MATCH("/region&/eq&/vel&freq", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            if (indices[2] != 2)
                break;
            client.receive<'f'>(delay, path, eq.vel2frequency);
        } break;
        MATCH("/region&/eq&/vel&freq", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            if (indices[2] != 2)
                break;
            eq.vel2frequency = Opcode::transform(Default::eqVel2Frequency, args[0].f);
        } break;

        MATCH("/region&/eq&/vel&gain", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            if (indices[2] != 2)
                break;
            client.receive<'f'>(delay, path, eq.vel2gain);
        } break;
        MATCH("/region&/eq&/vel&gain", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            if (indices[2] != 2)
                break;
            eq.vel2gain = Opcode::transform(Default::eqVel2Gain, args[0].f);
        } break;

        MATCH("/region&/eq&/type", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            switch (eq.type) {
            case EqType::kEqNone: client.receive<'s'>(delay, path, "none"); break;
            case EqType::kEqPeak: client.receive<'s'>(delay, path, "peak"); break;
            case EqType::kEqLshelf: client.receive<'s'>(delay, path, "lshelf"); break;
            case EqType::kEqHshelf: client.receive<'s'>(delay, path, "hshelf"); break;
            }
        } break;
        MATCH("/region&/eq&/type", "s") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EQ_OR_BREAK(indices[1])
            eq.type = Opcode::read(Default::eq, args[0].s);
        } break;

        MATCH("/region&/lfo&/wave", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_LFO_OR_BREAK(indices[1])
            if (lfo.sub.size() == 0)
                break;

            client.receive<'i'>(delay, path, static_cast<int32_t>(lfo.sub[0].wave));
        } break;
        MATCH("/region&/lfo&/wave", "i") {
            indices[2] = 0;
            goto set_lfoN_wave;
        } break;
            
        MATCH("/region&/lfo&/wave&", "i") {
        set_lfoN_wave:
            GET_REGION_OR_BREAK(indices[0])
            GET_LFO_OR_BREAK(indices[1])
            GET_LFO_SUB_OR_BREAK(indices[2])
            sub.wave = Opcode::transform(Default::lfoWave, args[0].i);
        } break;
            
        MATCH("/region&/eg&/point&/time", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)

            client.receive<'f'>(delay, path, point.time);
        } break;
        MATCH("/region&/eg&/point&/time", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)
            point.time = Opcode::transform(Default::flexEGPointTime, args[0].f);
        } break;

        MATCH("/region&/eg&/point&/time_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)

            client.receive<'f'>(delay, path, point.ccTime.getWithDefault(indices[3]));
        } break;
        MATCH("/region&/eg&/point&/time_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)
            if (indices[3] < config::numCCs)
                point.ccTime[indices[3]] = Opcode::transform(Default::flexEGPointTimeMod, args[0].f);
        } break;

        MATCH("/region&/eg&/point&/level", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)

            client.receive<'f'>(delay, path, point.level);
        } break;
        MATCH("/region&/eg&/point&/level", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)
            point.level = Opcode::transform(Default::flexEGPointLevel, args[0].f);
        } break;

            
        MATCH("/region&/eg&/point&/level_cc&", "") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)

            client.receive<'f'>(delay, path, point.ccLevel.getWithDefault(indices[3]));
        } break;
        MATCH("/region&/eg&/point&/level_cc&", "f") {
            GET_REGION_OR_BREAK(indices[0])
            GET_EG_OR_BREAK(indices[1])
            GET_EG_POINT_OR_BREAK(indices[2] + 1)
            if (indices[3] < config::numCCs)
                point.ccLevel[indices[3]] = Opcode::transform(Default::flexEGPointLevelMod, args[0].f);
        } break;


        //----------------------------------------------------------------------
        // Setting other values
        // Note: all these must be rt-safe within the parseOpcode method in region

        MATCH("/sample_quality", "i") {
            impl.resources_.getSynthConfig().liveSampleQuality =
                Opcode::transform(Default::sampleQuality, static_cast<int>(args[0].i));
        } break;

        MATCH("/oscillator_quality", "i") {
            impl.resources_.getSynthConfig().liveOscillatorQuality =
                Opcode::transform(Default::oscillatorQuality, static_cast<int>(args[0].i));
        } break;

        MATCH("/freewheeling_sample_quality", "i") {
            impl.resources_.getSynthConfig().freeWheelingSampleQuality =
                Opcode::transform(Default::freewheelingSampleQuality, static_cast<int>(args[0].i));
        } break;

        MATCH("/freewheeling_oscillator_quality", "i") {
            impl.resources_.getSynthConfig().freeWheelingOscillatorQuality =
                Opcode::transform(Default::freewheelingOscillatorQuality, static_cast<int>(args[0].i));
        } break;

        MATCH("/sustain_cancels_release", "T") {
            impl.resources_.getSynthConfig().sustainCancelsRelease = true;
        } break;

        MATCH("/sustain_cancels_release", "F") {
            impl.resources_.getSynthConfig().sustainCancelsRelease = false;
        } break;


        //----------------------------------------------------------------------
        // Voices

        MATCH("/num_active_voices", "") {
            client.receive<'i'>(delay, path, static_cast<int>(impl.voiceManager_.getNumActiveVoices()));
        } break;

        #define GET_VOICE_OR_BREAK(idx)                     \
            if (static_cast<int>(idx) >= impl.numVoices_)   \
                break;                                      \
            const auto& voice = impl.voiceManager_[idx];    \
            if (voice.isFree())                             \
                break;

        MATCH("/voice&/trigger_value", "") {
            GET_VOICE_OR_BREAK(indices[0])
            client.receive<'f'>(delay, path, voice.getTriggerEvent().value);
        } break;

        MATCH("/voice&/trigger_number", "") {
            GET_VOICE_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, voice.getTriggerEvent().number);
        } break;

        MATCH("/voice&/trigger_type", "") {
            GET_VOICE_OR_BREAK(indices[0])
            const auto& event = voice.getTriggerEvent();
            switch (event.type) {
            case TriggerEventType::CC:
                client.receive<'s'>(delay, path, "cc");
                break;
            case TriggerEventType::NoteOn:
                client.receive<'s'>(delay, path, "note_on");
                break;
            case TriggerEventType::NoteOff:
                client.receive<'s'>(delay, path, "note_on");
                break;
            }

        } break;

        MATCH("/voice&/remaining_delay", "") {
            GET_VOICE_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, voice.getRemainingDelay());
        } break;

        MATCH("/voice&/source_position", "") {
            GET_VOICE_OR_BREAK(indices[0])
            client.receive<'i'>(delay, path, voice.getSourcePosition());
        } break;

        #undef MATCH
        // TODO...
    }
}

static bool extractMessage(const char* pattern, const char* path, unsigned* indices)
{
    unsigned nthIndex = 0;

    while (const char *endp = strchr(pattern, '&')) {
        if (nthIndex == maxIndices)
            return false;

        size_t length = endp - pattern;
        if (strncmp(pattern, path, length))
            return false;
        pattern += length;
        path += length;

        length = 0;
        while (absl::ascii_isdigit(path[length]))
            ++length;

        if (!absl::SimpleAtoi(absl::string_view(path, length), &indices[nthIndex++]))
            return false;

        pattern += 1;
        path += length;
    }

    return !strcmp(path, pattern);
}

static uint64_t hashMessagePath(const char* path, const char* sig)
{
    uint64_t h = Fnv1aBasis;
    while (unsigned char c = *path++) {
        if (!absl::ascii_isdigit(c))
            h = hashByte(c, h);
        else {
            h = hashByte('&', h);
            while (absl::ascii_isdigit(*path))
                ++path;
        }
    }
    h = hashByte(',', h);
    while (unsigned char c = *sig++)
        h = hashByte(c, h);
    return h;
}

} // namespace sfz
