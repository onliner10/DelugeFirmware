/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include "definitions_cxx.hpp"
#include "modulation/params/param_collection.h"
#include "modulation/patch/patch_cable.h"

class Song;
class ModelStackWithParamCollection;
class ModelStackWithThreeMainThings;
class LearnedMIDI;
class MIDICable;

struct CableGroup {
	uint8_t first;
	uint8_t end;
};

struct Destination {
	ParamDescriptor destinationParamDescriptor;
	uint32_t sources;
	uint8_t firstCable;
	uint8_t endCable;
};

/// A PatchCableSet's cables. The first few - all most sounds ever have - are kept inline; the rest go in one array that
/// is allocated the first time it's needed, at full size, and then stays where it is. So cables never move, and nothing
/// holding a pointer into them can be left dangling. (32 inline cables made every kit row's and clip's param
/// collections a 4 KB block, nearly half of it unused.)
class PatchCableStorage {
public:
	/// As many as fit, with the rest of PatchCableSet, its allocator size class
	static constexpr int32_t kNumInline = 6;
	static constexpr int32_t kNumOverflow = kMaxNumPatchCables - kNumInline;

	PatchCable& operator[](int32_t c) { return (c < kNumInline) ? inline_[c] : overflow_[c - kNumInline]; }
	PatchCable const& operator[](int32_t c) const { return (c < kNumInline) ? inline_[c] : overflow_[c - kNumInline]; }

	/// How many cables there's room for without allocating
	[[nodiscard]] int32_t capacity() const { return (overflow_ != nullptr) ? kMaxNumPatchCables : kNumInline; }

	/// Makes room for numCables cables, up to kMaxNumPatchCables. False if they don't fit or there isn't the RAM.
	[[nodiscard]] bool reserve(int32_t numCables);

	/// For cloning: memory for a copy's overflow cables, if other has any. False if they're needed but there isn't the
	/// RAM. Pass the result to adoptClone() on the copy.
	[[nodiscard]] static bool allocOverflowForClone(PatchCableStorage const& other, PatchCable** overflow);

	/// After a PatchCableSet was memcpy'd, take over the overflow from allocOverflowForClone() and copy the cables in.
	/// Like the inline ones, they're shallow copies until their params are told they've been cloned.
	void adoptClone(PatchCable* overflow);

	PatchCableStorage() = default;
	PatchCableStorage(PatchCableStorage const&) = delete;
	PatchCableStorage& operator=(PatchCableStorage const&) = delete;
	~PatchCableStorage();

private:
	PatchCable inline_[kNumInline];
	PatchCable* overflow_ = nullptr;
};

class PatchCableSet final : public ParamCollection {
public:
	PatchCableSet(ParamCollectionSummary* summary);
	~PatchCableSet() override;

	void setupPatching(ModelStackWithParamCollection const* modelStack);
	bool doesDestinationDescriptorHaveAnyCables(ParamDescriptor destinationParamDescriptor);
	uint8_t getPatchCableIndex(PatchSource from, ParamDescriptor destinationParamDescriptor,
	                           ModelStackWithParamCollection const* modelStack = nullptr,
	                           bool createIfNotFound = false);
	void deletePatchCable(ModelStackWithParamCollection const* modelStack, uint8_t c);
	bool patchCableIsUsable(uint8_t c, ModelStackWithThreeMainThings const* modelStack);
	int32_t getModifiedPatchCableAmount(int32_t c, int32_t p);
	void removeAllPatchingToParam(ModelStackWithParamCollection* modelStack, uint8_t p);
	bool isSourcePatchedToSomething(PatchSource s);
	bool isSourcePatchedToSomethingManuallyCheckCables(PatchSource s);
	bool doesParamHaveSomethingPatchedToIt(int32_t p);

	void tickSamples(int32_t numSamples, ModelStackWithParamCollection* modelStack) override;
	void tickTicks(int32_t numSamples, ModelStackWithParamCollection* modelStack) override {};
	void setPlayPos(uint32_t pos, ModelStackWithParamCollection* modelStack, bool reversed) override;
	void playbackHasEnded(ModelStackWithParamCollection* modelStack) override;
	void grabValuesFromPos(uint32_t pos, ModelStackWithParamCollection* modelStack) override;
	void generateRepeats(ModelStackWithParamCollection* modelStack, uint32_t oldLength, uint32_t newLength,
	                     bool shouldPingpong) override;
	void appendParamCollection(ModelStackWithParamCollection* modelStack,
	                           ModelStackWithParamCollection* otherModelStack, int32_t oldLength,
	                           int32_t reverseThisRepeatWithLength, bool pingpongingGenerally) override;
	void trimToLength(uint32_t newLength, ModelStackWithParamCollection* modelStack, Action* action,
	                  bool maySetupPatching) override;
	void shiftHorizontally(ModelStackWithParamCollection* modelStack, int32_t amount, int32_t effectiveLength) override;
	void processCurrentPos(ModelStackWithParamCollection* modelStack, int32_t ticksSkipped, bool reversed,
	                       bool didPingpong, bool mayInterpolate) override;
	void beenCloned(bool copyAutomation, int32_t reverseDirectionWithLength) override;
	ParamManagerForTimeline* getParamManager();

	void writePatchCablesToFile(Serializer& writer, bool writeAutomation);
	void readPatchCablesFromFile(Deserializer& reader, int32_t readAutomationUpToPos);
	void deleteAllAutomation(Action* action, ModelStackWithParamCollection* modelStack) override;
	void nudgeNonInterpolatingNodesAtPos(int32_t pos, int32_t offset, int32_t lengthBeforeLoop, Action* action,
	                                     ModelStackWithParamCollection* modelStack) override;

	void remotelySwapParamState(AutoParamState* state, ModelStackWithParamId* modelStack) override;
	AutoParam* getParam(ModelStackWithParamCollection const* modelStack, PatchSource s,
	                    ParamDescriptor destinationParamDescriptor, bool allowCreation = false);
	ModelStackWithAutoParam* getAutoParamFromId(ModelStackWithParamId* modelStack, bool allowCreation = false) override;
	static int32_t getParamId(ParamDescriptor destinationParamDescriptor, PatchSource s);

	AutoParam* getParam(int32_t paramId);

	void notifyParamModifiedInSomeWay(ModelStackWithAutoParam const* modelStack, int32_t oldValue,
	                                  bool automationChanged, bool automatedBefore, bool automatedNow) override;
	void notifyPingpongOccurred(ModelStackWithParamCollection* modelStack) override;

	int32_t paramValueToKnobPos(int32_t paramValue, ModelStackWithAutoParam* modelStack) override;
	int32_t knobPosToParamValue(int32_t knobPos, ModelStackWithAutoParam* modelStack) override;
	bool isSourcePatchedToDestinationDescriptorVolumeInspecific(PatchSource s,
	                                                            ParamDescriptor destinationParamDescriptor);
	bool isAnySourcePatchedToParamVolumeInspecific(ParamDescriptor destinationParamDescriptor);
	void grabVelocityToLevelFromMIDIInput(LearnedMIDI* midiInput);
	void grabVelocityToLevelFromMIDICable(MIDICable& cable);
	PatchCable* getPatchCableFromVelocityToLevel();

	Destination* getDestinationForParam(int32_t p);

	deluge::modulation::params::Kind getParamKind() override { return deluge::modulation::params::Kind::PATCH_CABLE; }

	uint32_t sourcesPatchedToAnything[2]; // Only valid after setupPatching()

	PatchCableStorage patchCables;
	uint8_t numUsablePatchCables;
	uint8_t numPatchCables;

	Destination* destinations[2];

	bool shouldParamIndicateMiddleValue(ModelStackWithParamId const* modelStack) override { return true; };

	/// Makes room for numCables cables. False if over kMaxNumPatchCables or there isn't the RAM
	[[nodiscard]] bool reserveCables(int32_t numCables) { return patchCables.reserve(numCables); }

	static void dissectParamId(uint32_t paramId, ParamDescriptor* destinationParamDescriptor, PatchSource* s);

private:
	void swapCables(int32_t c1, int32_t c2);
	void freeDestinationMemory(bool destructing);
};
