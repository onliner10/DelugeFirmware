#include "CppUTest/TestHarness.h"
#include "modulation/params/param_node.h"
#include "modulation/params/param_node_vector.h"
#include <cstring>

TEST_GROUP(LazyParamNodeVectorTest){};

namespace {
void addNode(LazyParamNodeVector& nodes, int32_t pos, int32_t value) {
	int32_t i = nodes.insertAtKey(pos);
	CHECK(i >= 0);
	nodes.getElement(i)->value = value;
}
} // namespace

TEST(LazyParamNodeVectorTest, isJustAPointer) {
	CHECK_EQUAL(sizeof(void*), sizeof(LazyParamNodeVector));
}

TEST(LazyParamNodeVectorTest, unallocatedBehavesLikeEmpty) {
	LazyParamNodeVector nodes;
	ParamNodeVector reference;

	POINTERS_EQUAL(nullptr, nodes.get());
	CHECK_EQUAL(0, nodes.getNumElements());
	POINTERS_EQUAL(nullptr, nodes.getElement(0));
	POINTERS_EQUAL(nullptr, nodes.getFirst());
	POINTERS_EQUAL(nullptr, nodes.getLast());
	CHECK_EQUAL(reference.search(100, GREATER_OR_EQUAL), nodes.search(100, GREATER_OR_EQUAL));
	CHECK_EQUAL(reference.search(100, LESS), nodes.search(100, LESS));
	CHECK_EQUAL(reference.searchExact(100), nodes.searchExact(100));
	CHECK(nodes.generateRepeats(96, 192));

	int32_t terms[2] = {10, 20};
	int32_t expected[2] = {-1, -1};
	int32_t actual[2] = {-1, -1};
	reference.searchDual(terms, expected);
	nodes.searchDual(terms, actual);
	CHECK_EQUAL(expected[0], actual[0]);
	CHECK_EQUAL(expected[1], actual[1]);

	nodes.deleteAtIndex(0); // Must be harmless
	POINTERS_EQUAL(nullptr, nodes.get());
}

TEST(LazyParamNodeVectorTest, allocatesOnInsertAndFreesWhenEmptied) {
	LazyParamNodeVector nodes;
	addNode(nodes, 48, 5);
	addNode(nodes, 0, 3);

	CHECK(nodes.get() != nullptr);
	CHECK_EQUAL(2, nodes.getNumElements());
	CHECK_EQUAL(0, nodes.getFirst()->pos);
	CHECK_EQUAL(48, nodes.getLast()->pos);
	CHECK_EQUAL(1, nodes.searchExact(48));

	nodes.deleteAtIndex(0, 2);
	POINTERS_EQUAL(nullptr, nodes.get());

	addNode(nodes, 12, 1);
	nodes.empty();
	POINTERS_EQUAL(nullptr, nodes.get());
}

TEST(LazyParamNodeVectorTest, keepsVectorWhenNotAllowedToShorten) {
	LazyParamNodeVector nodes;
	addNode(nodes, 0, 1);
	nodes.deleteAtIndex(0, 1, false);
	CHECK(nodes.get() != nullptr);
	CHECK_EQUAL(0, nodes.getNumElements());
}

TEST(LazyParamNodeVectorTest, cloneFromIsDeep) {
	LazyParamNodeVector original;
	addNode(original, 0, 7);
	addNode(original, 24, 9);

	LazyParamNodeVector clone;
	CHECK(clone.cloneFrom(&original));
	CHECK(clone.get() != original.get());
	CHECK_EQUAL(2, clone.getNumElements());
	CHECK_EQUAL(9, clone.getElement(1)->value);

	clone.getElement(1)->value = 100;
	CHECK_EQUAL(9, original.getElement(1)->value);

	LazyParamNodeVector empty;
	LazyParamNodeVector cloneOfEmpty;
	CHECK(cloneOfEmpty.cloneFrom(&empty));
	POINTERS_EQUAL(nullptr, cloneOfEmpty.get());
}

TEST(LazyParamNodeVectorTest, beenClonedAfterMemcpyMakesIndependentCopy) {
	LazyParamNodeVector original;
	addNode(original, 0, 7);
	addNode(original, 24, 9);

	alignas(LazyParamNodeVector) unsigned char storage[sizeof(LazyParamNodeVector)];
	memcpy(storage, &original, sizeof(LazyParamNodeVector));
	auto* copy = reinterpret_cast<LazyParamNodeVector*>(storage);
	CHECK(copy->beenCloned() == Error::NONE);

	CHECK(copy->get() != nullptr);
	CHECK(copy->get() != original.get());
	CHECK_EQUAL(2, copy->getNumElements());
	CHECK_EQUAL(7, copy->getElement(0)->value);
	CHECK_EQUAL(2, original.getNumElements());
	copy->empty();
}

TEST(LazyParamNodeVectorTest, swapStateWithSwapsOwnership) {
	LazyParamNodeVector a;
	LazyParamNodeVector b;
	addNode(a, 0, 1);
	ParamNodeVector* aVector = a.get();

	a.swapStateWith(&b);
	POINTERS_EQUAL(nullptr, a.get());
	POINTERS_EQUAL(aVector, b.get());
}

TEST_GROUP(AutoParamNodesTest){};

namespace {
void addNode(AutoParamNodes& nodes, int32_t pos, int32_t value) {
	int32_t i = nodes.insertAtKey(pos);
	CHECK(i >= 0);
	nodes.getElement(i)->value = value;
}

AutomationPlayState gliding(int32_t increment, uint32_t overridingAtTime = 0) {
	AutomationPlayState state = kNeutralAutomationPlayState;
	state.valueIncrementPerHalfTick = increment;
	state.renewedOverridingAtTime = overridingAtTime;
	return state;
}

void setPlayState(AutoParamNodes& nodes, AutomationPlayState const& state) {
	AutomationPlayState* stored = nodes.playStateForWrite();
	CHECK(stored != nullptr);
	*stored = state;
}

void checkPlayState(AutomationPlayState const& expected, AutoParamNodes const& nodes) {
	MEMCMP_EQUAL(&expected, &nodes.playState(), sizeof(AutomationPlayState));
}
} // namespace

TEST(AutoParamNodesTest, isJustAPointer) {
	CHECK_EQUAL(sizeof(void*), sizeof(AutoParamNodes));
}

TEST(AutoParamNodesTest, neutralPlayStateNeedsNoMemory) {
	AutoParamNodes nodes;
	CHECK(nodes.playState().isNeutral());
	POINTERS_EQUAL(nullptr, nodes.playStateIfAny());
	POINTERS_EQUAL(nullptr, nodes.get());
	CHECK_EQUAL(0, nodes.getNumElements());

	addNode(nodes, 0, 1);
	CHECK(nodes.playState().isNeutral());
	nodes.deleteAtIndex(0);
	POINTERS_EQUAL(nullptr, nodes.get());
}

TEST(AutoParamNodesTest, negativeZeroIncrementIsNotNeutral) {
	AutomationPlayState state = kNeutralAutomationPlayState;
	state.value_increment_per_half_tick_float = -0.0f;
	CHECK(!state.isNeutral());
	CHECK(state.valueIncrementPerHalfTick != 0);
}

TEST(AutoParamNodesTest, playStateOutlivesTheNodes) {
	AutoParamNodes nodes;
	addNode(nodes, 0, 1);
	addNode(nodes, 24, 2);
	setPlayState(nodes, gliding(5, 1));

	nodes.deleteAtIndex(0, 2);
	CHECK_EQUAL(0, nodes.getNumElements());
	checkPlayState(gliding(5, 1), nodes);

	addNode(nodes, 12, 3);
	nodes.deleteAllNodes();
	CHECK_EQUAL(0, nodes.getNumElements());
	POINTERS_EQUAL(nullptr, nodes.getElement(0));
	checkPlayState(gliding(5, 1), nodes);

	// Once neutral again, there's nothing left to keep
	*nodes.playStateIfAny() = kNeutralAutomationPlayState;
	nodes.freeIfUnused();
	POINTERS_EQUAL(nullptr, nodes.get());
}

TEST(AutoParamNodesTest, playStateWithoutEverHavingNodes) {
	AutoParamNodes nodes;
	setPlayState(nodes, gliding(0, 1234));
	CHECK_EQUAL(0, nodes.getNumElements());
	POINTERS_EQUAL(nullptr, nodes.getFirst());
	CHECK_EQUAL(-1, nodes.searchExact(0));
	checkPlayState(gliding(0, 1234), nodes);

	nodes.freeIfUnused(); // Not unused
	checkPlayState(gliding(0, 1234), nodes);

	addNode(nodes, 0, 9);
	checkPlayState(gliding(0, 1234), nodes);
	CHECK_EQUAL(9, nodes.getFirst()->value);
}

TEST(AutoParamNodesTest, deleteAllNodesFreesWhenNeutral) {
	AutoParamNodes nodes;
	addNode(nodes, 0, 1);
	nodes.deleteAllNodes();
	POINTERS_EQUAL(nullptr, nodes.get());
	nodes.deleteAllNodes(); // Must be harmless
	POINTERS_EQUAL(nullptr, nodes.get());
}

TEST(AutoParamNodesTest, swapNodesWithLeavesPlayStateHere) {
	AutoParamNodes param;
	addNode(param, 0, 1);
	addNode(param, 24, 2);
	setPlayState(param, gliding(7, 99));

	LazyParamNodeVector snapshot;
	int32_t i = snapshot.insertAtKey(48);
	snapshot.getElement(i)->value = 3;

	param.swapNodesWith(&snapshot);
	CHECK_EQUAL(1, param.getNumElements());
	CHECK_EQUAL(3, param.getFirst()->value);
	checkPlayState(gliding(7, 99), param);
	CHECK_EQUAL(2, snapshot.getNumElements());
	CHECK_EQUAL(2, snapshot.getLast()->value);

	// And back again, as undo then redo does
	param.swapNodesWith(&snapshot);
	CHECK_EQUAL(2, param.getNumElements());
	checkPlayState(gliding(7, 99), param);
	CHECK_EQUAL(1, snapshot.getNumElements());
}

TEST(AutoParamNodesTest, swapNodesWithNothingOnOneSide) {
	// The param has nodes and a play state, the snapshot nothing
	AutoParamNodes param;
	addNode(param, 0, 1);
	setPlayState(param, gliding(7));
	LazyParamNodeVector snapshot;

	param.swapNodesWith(&snapshot);
	CHECK_EQUAL(0, param.getNumElements());
	checkPlayState(gliding(7), param);
	CHECK_EQUAL(1, snapshot.getNumElements());

	// The param has only a play state, the snapshot the nodes
	param.swapNodesWith(&snapshot);
	CHECK_EQUAL(1, param.getNumElements());
	checkPlayState(gliding(7), param);
	POINTERS_EQUAL(nullptr, snapshot.get());

	// Neither has a play state
	AutoParamNodes plain;
	addNode(plain, 0, 5);
	ParamNodeVector* plainVector = plain.get();
	plain.swapNodesWith(&snapshot);
	POINTERS_EQUAL(nullptr, plain.get());
	POINTERS_EQUAL(plainVector, snapshot.get());
	plain.swapNodesWith(&snapshot);
	POINTERS_EQUAL(plainVector, plain.get());
	CHECK(plain.playState().isNeutral());
}

TEST(AutoParamNodesTest, cloningNodesLeavesPlayStateBehind) {
	AutoParamNodes original;
	addNode(original, 0, 7);
	addNode(original, 24, 9);
	setPlayState(original, gliding(3, 1));

	LazyParamNodeVector snapshot;
	CHECK(original.cloneNodesTo(&snapshot));
	CHECK_EQUAL(2, snapshot.getNumElements());
	snapshot.getElement(1)->value = 100;
	CHECK_EQUAL(9, original.getElement(1)->value);

	alignas(AutoParamNodes) unsigned char storage[sizeof(AutoParamNodes)];
	memset(storage, 0xAA, sizeof(storage)); // Nothing there to free, or to believe
	auto* clone = reinterpret_cast<AutoParamNodes*>(storage);
	CHECK(clone->becomeCloneOfNodes(&original));
	CHECK_EQUAL(2, clone->getNumElements());
	CHECK(clone->get() != original.get());
	CHECK(clone->playState().isNeutral());
	checkPlayState(gliding(3, 1), original);
	clone->deleteAllNodes();

	// Nothing to clone: no allocation, even with a play state on the original
	AutoParamNodes stateOnly;
	setPlayState(stateOnly, gliding(3));
	memset(storage, 0xAA, sizeof(storage));
	CHECK(clone->becomeCloneOfNodes(&stateOnly));
	POINTERS_EQUAL(nullptr, clone->get());
}

TEST(AutoParamNodesTest, detachFromOriginalKeepsPlayStateNotNodes) {
	AutoParamNodes original;
	addNode(original, 0, 7);
	addNode(original, 24, 9);
	setPlayState(original, gliding(3, 1));

	alignas(AutoParamNodes) unsigned char storage[sizeof(AutoParamNodes)];
	memcpy(storage, &original, sizeof(AutoParamNodes));
	auto* copy = reinterpret_cast<AutoParamNodes*>(storage);

	AutoParamNodes::Original from = copy->detachFromOriginal();
	CHECK(copy->get() != original.get());
	CHECK_EQUAL(0, copy->getNumElements());
	checkPlayState(gliding(3, 1), *copy);
	POINTERS_EQUAL(original.get(), AutoParamNodes::nodesOf(from));

	CHECK(copy->cloneNodesFrom(from) == Error::NONE);
	CHECK_EQUAL(2, copy->getNumElements());
	CHECK_EQUAL(9, copy->getElement(1)->value);
	copy->getElement(1)->value = 100;
	*copy->playStateIfAny() = gliding(4);

	// The original is as it was
	CHECK_EQUAL(2, original.getNumElements());
	CHECK_EQUAL(9, original.getElement(1)->value);
	checkPlayState(gliding(3, 1), original);

	copy->deleteAllNodes();
	*copy->playStateIfAny() = kNeutralAutomationPlayState;
	copy->freeIfUnused();
	POINTERS_EQUAL(nullptr, copy->get());
}

TEST(AutoParamNodesTest, detachFromOriginalWithNothingToKeep) {
	// Nodes but a neutral play state: nothing allocated until the nodes get cloned
	AutoParamNodes original;
	addNode(original, 0, 7);

	alignas(AutoParamNodes) unsigned char storage[sizeof(AutoParamNodes)];
	memcpy(storage, &original, sizeof(AutoParamNodes));
	auto* copy = reinterpret_cast<AutoParamNodes*>(storage);
	AutoParamNodes::Original from = copy->detachFromOriginal();
	POINTERS_EQUAL(nullptr, copy->get());
	CHECK(copy->playState().isNeutral());
	CHECK(copy->cloneNodesFrom(from) == Error::NONE);
	CHECK_EQUAL(1, copy->getNumElements());
	copy->deleteAllNodes();

	// Nothing at all
	AutoParamNodes nothing;
	memcpy(storage, &nothing, sizeof(AutoParamNodes));
	from = copy->detachFromOriginal();
	POINTERS_EQUAL(nullptr, AutoParamNodes::nodesOf(from));
	CHECK(copy->cloneNodesFrom(from) == Error::NONE);
	POINTERS_EQUAL(nullptr, copy->get());

	// A play state but no nodes
	AutoParamNodes stateOnly;
	setPlayState(stateOnly, gliding(0, 55));
	memcpy(storage, &stateOnly, sizeof(AutoParamNodes));
	from = copy->detachFromOriginal();
	POINTERS_EQUAL(nullptr, AutoParamNodes::nodesOf(from));
	CHECK(copy->get() != stateOnly.get());
	checkPlayState(gliding(0, 55), *copy);
	*copy->playStateIfAny() = kNeutralAutomationPlayState;
	copy->freeIfUnused();
}

TEST(AutoParamNodesTest, forgetLeavesTheAllocationToItsOwner) {
	AutoParamNodes owner;
	addNode(owner, 0, 7);
	setPlayState(owner, gliding(3));

	alignas(AutoParamNodes) unsigned char storage[sizeof(AutoParamNodes)];
	memcpy(storage, &owner, sizeof(AutoParamNodes));
	auto* copy = reinterpret_cast<AutoParamNodes*>(storage);
	copy->forget();
	POINTERS_EQUAL(nullptr, copy->get());
	CHECK(copy->playState().isNeutral());

	CHECK_EQUAL(1, owner.getNumElements());
	checkPlayState(gliding(3), owner);
}
