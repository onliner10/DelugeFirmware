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

#include "modulation/params/param_node_vector.h"

#include "memory/general_memory_allocator.h"
#include "modulation/params/param_node.h"
#include <cstdint>
#include <new>
#include <string.h>

ParamNodeVector::ParamNodeVector() : OrderedResizeableArrayWith32bitKey(sizeof(ParamNode)) {
}

ParamNode* ParamNodeVector::getElement(int32_t index) {
	if (index < 0 || index >= getNumElements()) {
		return nullptr;
	}
	return (ParamNode*)getElementAddress(index);
}

ParamNode* ParamNodeVector::getFirst() {
	return getElement(0);
}

ParamNode* ParamNodeVector::getLast() {
	return getElement(getNumElements() - 1);
}

bool LazyParamNodeVector::allocate() {
	if (block_) {
		return true;
	}
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Block));
	if (!memory) {
		return false;
	}
	block_ = new (memory) Block();
	return true;
}

void LazyParamNodeVector::empty() {
	if (block_) {
		block_->~Block();
		delugeDealloc(block_);
		block_ = nullptr;
	}
}

void LazyParamNodeVector::freeIfUnused() {
	if (block_ && !block_->vector.getNumElements() && block_->state.isNeutral()) {
		empty();
	}
}

bool LazyParamNodeVector::cloneFrom(LazyParamNodeVector const* other) {
	block_ = nullptr;
	if (!other->getNumElements()) {
		return true;
	}
	if (!allocate()) {
		return false;
	}
	bool success = block_->vector.cloneFrom(&other->block_->vector);
	freeIfUnused();
	return success;
}

Error LazyParamNodeVector::beenCloned() {
	Block* source = block_; // Still belongs to the object we were memcpy'd from
	block_ = nullptr;
	if (!source || !source->vector.getNumElements()) {
		return Error::NONE;
	}
	if (!allocate()) {
		return Error::INSUFFICIENT_RAM;
	}
	if (!block_->vector.cloneFrom(&source->vector)) {
		empty();
		return Error::INSUFFICIENT_RAM;
	}
	return Error::NONE;
}

void LazyParamNodeVector::searchDual(int32_t const* __restrict__ searchTerms, int32_t* __restrict__ resultingIndexes) {
	if (block_) {
		block_->vector.searchDual(searchTerms, resultingIndexes);
	}
	else {
		resultingIndexes[0] = 0;
		resultingIndexes[1] = 0;
	}
}

Error LazyParamNodeVector::insertAtIndex(int32_t i, int32_t numToInsert, void* thingNotToStealFrom) {
	if (!allocate()) {
		return Error::INSUFFICIENT_RAM;
	}
	Error error = block_->vector.insertAtIndex(i, numToInsert, thingNotToStealFrom);
	freeIfUnused();
	return error;
}

int32_t LazyParamNodeVector::insertAtKey(int32_t key, bool isDefinitelyLast) {
	if (!allocate()) {
		return -1;
	}
	int32_t i = block_->vector.insertAtKey(key, isDefinitelyLast);
	freeIfUnused();
	return i;
}

bool LazyParamNodeVector::ensureEnoughSpaceAllocated(int32_t numAdditionalElementsNeeded) {
	if (!allocate()) {
		return false;
	}
	bool success = block_->vector.ensureEnoughSpaceAllocated(numAdditionalElementsNeeded);
	if (!success) {
		freeIfUnused();
	}
	return success;
}

void LazyParamNodeVector::deleteAtIndex(int32_t i, int32_t numToDelete, bool mayShortenMemoryAfter) {
	if (!block_) {
		return;
	}
	block_->vector.deleteAtIndex(i, numToDelete, mayShortenMemoryAfter);
	if (mayShortenMemoryAfter) {
		freeIfUnused();
	}
}

void AutoParamNodes::deleteAllNodes() {
	if (!block_) {
		return;
	}
	if (block_->state.isNeutral()) {
		empty();
	}
	else {
		block_->vector.empty();
	}
}

void AutoParamNodes::swapNodesWith(LazyParamNodeVector* other) {
	AutomationPlayState state = playState();
	if (block_) {
		// other gets the nodes only - or nothing at all, if there aren't any
		block_->state = kNeutralAutomationPlayState;
		freeIfUnused();
	}
	LazyParamNodeVector::swapStateWith(other);
	if (!state.isNeutral() && allocate()) {
		block_->state = state;
	}
}

AutoParamNodes::Original AutoParamNodes::detachFromOriginal() {
	Original original;
	original.block = block_;
	block_ = nullptr;
	if (original.block && !original.block->state.isNeutral() && allocate()) {
		block_->state = original.block->state;
	}
	return original;
}

Error AutoParamNodes::cloneNodesFrom(Original original) {
	ParamNodeVector* source = nodesOf(original);
	if (!source) {
		return Error::NONE;
	}
	if (!allocate()) {
		return Error::INSUFFICIENT_RAM;
	}
	if (!block_->vector.cloneFrom(source)) {
		block_->vector.empty();
		freeIfUnused();
		return Error::INSUFFICIENT_RAM;
	}
	return Error::NONE;
}
