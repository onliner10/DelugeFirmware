/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "util/container/array/ordered_resizeable_array.h"
#include <cstdint>
#include <cstring>
#include <utility>

class ParamNode;

class ParamNodeVector : public OrderedResizeableArrayWith32bitKey {
public:
	ParamNodeVector();

	ParamNode* getElement(int32_t index);
	ParamNode* getFirst();
	ParamNode* getLast();
};

/// Where an AutoParam is in playing back its automation: how fast it's gliding towards the next node, and whether the
/// user has overridden the automation by turning the knob. All zeros for a param that's doing neither, which is nearly
/// all of them, so it's kept next to the nodes rather than in every AutoParam.
struct AutomationPlayState {
	// Which of these is in use depends on ParamCollection::shouldInterpolateWithFloat(), so anything reading the
	// increment has to be told which by the ParamCollection that owns the AutoParam.
	union {
		int32_t valueIncrementPerHalfTick;
		float value_increment_per_half_tick_float;
	};
	float interpolation_increment_remainder_float;
	uint32_t renewedOverridingAtTime; // If 0, it's off. If 1, it's latched until we hit some nodes / automation

	/// Whether every bit is zero. Bits rather than values, as a float increment of -0 still reads as "interpolating"
	/// through the integer view.
	[[nodiscard]] bool isNeutral() const {
		uint32_t words[3];
		memcpy(words, this, sizeof(words));
		return (words[0] | words[1] | words[2]) == 0;
	}
};
static_assert(sizeof(AutomationPlayState) == 3 * sizeof(uint32_t));

inline constexpr AutomationPlayState kNeutralAutomationPlayState{};

/// Owns a ParamNodeVector that is only allocated once the first node is added, and freed again once the last node is
/// removed. An un-automated AutoParam therefore costs one pointer instead of a whole (empty) ParamNodeVector, which
/// matters because every Sound has well over a hundred AutoParams per Clip.
///
/// The interface mirrors the subset of ParamNodeVector that AutoParam uses, including its "memcpy then init() or
/// beenCloned()" cloning convention, so read-only calls on an unallocated vector behave exactly as on an empty one.
///
/// The allocation also has room for an AutomationPlayState, which only AutoParamNodes uses. Here it stays neutral.
class LazyParamNodeVector {
public:
	LazyParamNodeVector() = default;
	~LazyParamNodeVector() { empty(); }
	LazyParamNodeVector(LazyParamNodeVector const&) = delete;
	LazyParamNodeVector& operator=(LazyParamNodeVector const&) = delete;

	/// Forget the current vector without freeing it - for when this object was memcpy'd from another one.
	void init() { block_ = nullptr; }
	/// Free the vector and all its nodes.
	void empty();
	/// Like ResizeableArray::cloneFrom(), this assumes there's currently nothing here to free.
	bool cloneFrom(LazyParamNodeVector const* other);
	/// Call after this object was memcpy'd from another one, to give it its own copy of the nodes.
	Error beenCloned();
	void swapStateWith(LazyParamNodeVector* other) { std::swap(block_, other->block_); }

	/// The underlying vector, or nullptr if it's not allocated.
	ParamNodeVector* get() { return block_ ? &block_->vector : nullptr; }

	int32_t getNumElements() const { return block_ ? block_->vector.getNumElements() : 0; }
	ParamNode* getElement(int32_t index) { return block_ ? block_->vector.getElement(index) : nullptr; }
	ParamNode* getFirst() { return block_ ? block_->vector.getFirst() : nullptr; }
	ParamNode* getLast() { return block_ ? block_->vector.getLast() : nullptr; }
	void* getElementAddress(int32_t index) { return block_->vector.getElementAddress(index); }

	int32_t search(int32_t key, int32_t comparison, int32_t rangeBegin = 0) {
		return block_ ? block_->vector.search(key, comparison, rangeBegin) : rangeBegin + comparison;
	}
	int32_t search(int32_t key, int32_t comparison, int32_t rangeBegin, int32_t rangeEnd) {
		return block_ ? block_->vector.search(key, comparison, rangeBegin, rangeEnd) : rangeBegin + comparison;
	}
	int32_t searchExact(int32_t key) { return block_ ? block_->vector.searchExact(key) : -1; }
	void searchDual(int32_t const* __restrict__ searchTerms, int32_t* __restrict__ resultingIndexes);
	void testSequentiality(char const* errorCode) {
		if (block_) {
			block_->vector.testSequentiality(errorCode);
		}
	}

	Error insertAtIndex(int32_t i, int32_t numToInsert = 1, void* thingNotToStealFrom = nullptr);
	int32_t insertAtKey(int32_t key, bool isDefinitelyLast = false);
	bool ensureEnoughSpaceAllocated(int32_t numAdditionalElementsNeeded);
	void deleteAtIndex(int32_t i, int32_t numToDelete = 1, bool mayShortenMemoryAfter = true);

	void shiftHorizontal(int32_t amount, int32_t effectiveLength) {
		if (block_) {
			block_->vector.shiftHorizontal(amount, effectiveLength);
		}
	}
	bool generateRepeats(int32_t wrapPoint, int32_t endPos) {
		return block_ ? block_->vector.generateRepeats(wrapPoint, endPos) : true;
	}

protected:
	struct Block {
		ParamNodeVector vector;
		AutomationPlayState state = kNeutralAutomationPlayState;
	};

	bool allocate();
	/// Frees the allocation if it holds no nodes and no play state.
	void freeIfUnused();

	Block* block_ = nullptr;
};

/// An AutoParam's nodes, plus its AutomationPlayState, in the one allocation - which exists while there's a node or a
/// play state that isn't neutral, and possibly for a while after.
///
/// The play state is the AutoParam's own. It's untouched by anything that only concerns nodes - emptying them, swapping
/// them with a snapshot's, forgetting them after a memcpy - just as when it was fields of AutoParam itself. Hence no
/// init(), empty(), swapStateWith() or beenCloned() here: each of those has to say what becomes of the play state.
///
/// If there's no memory to hold a play state, it stays neutral: the param doesn't glide, or its automation isn't
/// overridden.
class AutoParamNodes : private LazyParamNodeVector {
public:
	using LazyParamNodeVector::deleteAtIndex;
	using LazyParamNodeVector::ensureEnoughSpaceAllocated;
	using LazyParamNodeVector::generateRepeats;
	using LazyParamNodeVector::get;
	using LazyParamNodeVector::getElement;
	using LazyParamNodeVector::getElementAddress;
	using LazyParamNodeVector::getFirst;
	using LazyParamNodeVector::getLast;
	using LazyParamNodeVector::getNumElements;
	using LazyParamNodeVector::insertAtIndex;
	using LazyParamNodeVector::insertAtKey;
	using LazyParamNodeVector::search;
	using LazyParamNodeVector::searchDual;
	using LazyParamNodeVector::searchExact;
	using LazyParamNodeVector::shiftHorizontal;
	using LazyParamNodeVector::testSequentiality;

	/// No nodes and a neutral play state, without freeing anything - for memory that isn't a live AutoParamNodes: not
	/// yet constructed, or a memcpy'd copy of one that another object now owns.
	void forget() { block_ = nullptr; }

	/// Frees all nodes. The play state stays.
	void deleteAllNodes();

	/// Swaps just the nodes with other's. The play state stays here.
	void swapNodesWith(LazyParamNodeVector* other);

	/// Gives other, which must hold nothing, a copy of the nodes. Returns false if there wasn't the memory.
	bool cloneNodesTo(LazyParamNodeVector* other) const { return other->cloneFrom(this); }

	/// Makes this, which must hold nothing to free, a copy of other's nodes, with a neutral play state. Returns false
	/// if there wasn't the memory.
	bool becomeCloneOfNodes(AutoParamNodes const* other) { return cloneFrom(other); }

	/// The allocation of the AutoParamNodes that this one was memcpy'd from.
	class Original {
		friend class AutoParamNodes;
		Block* block;
	};
	/// Call first thing after this was memcpy'd from another AutoParamNodes: this stops referring to that one's
	/// allocation, and keeps a copy of its play state - like the fields did - but none of its nodes.
	[[nodiscard]] Original detachFromOriginal();
	/// The original's nodes, or nullptr if it had none.
	[[nodiscard]] static ParamNodeVector* nodesOf(Original original) {
		return (original.block && original.block->vector.getNumElements()) ? &original.block->vector : nullptr;
	}
	/// Copies the original's nodes here, where there must be none. If there isn't the memory, there are still none.
	Error cloneNodesFrom(Original original);

	[[nodiscard]] AutomationPlayState const& playState() const {
		return block_ ? block_->state : kNeutralAutomationPlayState;
	}
	/// For changing a play state that's already somewhere: nullptr if it's neutral for having nowhere to live.
	[[nodiscard]] AutomationPlayState* playStateIfAny() { return block_ ? &block_->state : nullptr; }
	/// For making the play state something other than neutral. nullptr if there wasn't the memory for one.
	[[nodiscard]] AutomationPlayState* playStateForWrite() { return allocate() ? &block_->state : nullptr; }
	/// Frees the allocation if it's no longer holding anything.
	void freeIfUnused() { LazyParamNodeVector::freeIfUnused(); }
};
