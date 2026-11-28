#include "CppUTest/TestHarness.h"
#include "dsp/delay/feedback_safety.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace df = deluge::dsp::delay_feedback;

namespace {
constexpr int32_t kUnity = 1 << 30;
constexpr int32_t kKnobMax = 2147483647;
constexpr int32_t kSaturation = 1 << 29; // where the modelled loop tops out
constexpr int32_t kBlocksPerSecond = 344;

// The knob amount for a position 0-50, as the param system makes it (linear, 0 to 2.0)
int32_t knob(int32_t position) {
	return static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(kKnobMax) * position / 50, kKnobMax));
}

double dB(double ratio) {
	return 20 * std::log10(ratio);
}

// A delay loop, block by block: what comes out is what went in `delayBlocks` ago, and what goes back in is that times
// the feedback, plus the input, saturating like the delay does
struct Loop {
	std::vector<int64_t> line = std::vector<int64_t>(86, 0); // 250 ms
	size_t pos = 0;
	df::Leveler leveler;
	bool safe = true;

	// Returns the peak coming out of the delay this block
	int32_t block(int32_t feedback, int32_t input) {
		auto const wet = static_cast<int32_t>(line[pos]);
		int32_t const used = safe ? leveler.process(feedback, input, wet, 128) : feedback;
		line[pos] = std::min<int64_t>(static_cast<int64_t>(wet) * used / kUnity + input, kSaturation);
		pos = (pos + 1) % line.size();
		return wet;
	}

	// Runs for `blocks`, returning the loudest peak in the last second of them
	int32_t run(int32_t feedback, int32_t blocks, int32_t input = 0) {
		int32_t loudest = 0;
		for (int32_t i = 0; i < blocks; i++) {
			int32_t wet = block(feedback, input);
			if (i >= blocks - kBlocksPerSecond) {
				loudest = std::max(loudest, wet);
			}
		}
		return loudest;
	}

	// Plays a 50 ms note at `level`
	void note(int32_t feedback, int32_t level) {
		for (int32_t i = 0; i < 17; i++) {
			block(feedback, level);
		}
	}
};
} // namespace

TEST_GROUP(DelayFeedbackTests){};

TEST(DelayFeedbackTests, knobIsUnchangedUpTo22) {
	for (int32_t position = 0; position <= 22; position++) {
		CHECK_EQUAL(knob(position), df::shape(knob(position)));
	}
}

TEST(DelayFeedbackTests, knobReachesUnityAt45AndTopsOutAt1Point3) {
	DOUBLES_EQUAL(1.0, static_cast<double>(df::shape(knob(45))) / kUnity, 0.001);
	DOUBLES_EQUAL(1.3, static_cast<double>(df::shape(knob(50))) / kUnity, 0.001);
	CHECK(df::shape(knob(44)) < kUnity);
	CHECK(df::shape(knob(46)) > kUnity);
}

TEST(DelayFeedbackTests, knobStaysSmoothAndRising) {
	int32_t last = -1;
	for (int64_t amount = 0; amount <= kKnobMax; amount += 1 << 20) {
		int32_t const shaped = df::shape(static_cast<int32_t>(amount));
		CHECK(shaped >= last);
		last = shaped;
	}
	CHECK(df::shape(kKnobMax) >= last);
}

// Without the leveler, a loop past unity grows until it saturates, far above what was played into it
TEST(DelayFeedbackTests, unleveledLoopRunsAway) {
	Loop loop;
	loop.safe = false;
	int32_t const played = kSaturation / 30; // ~30 dB under where the loop saturates
	loop.note(knob(50), played);
	int32_t peak = loop.run(knob(50), 5 * kBlocksPerSecond);
	CHECK_EQUAL(kSaturation, peak);
}

// With it, it self-oscillates at the level of what was played, for as long as the knob stays up
TEST(DelayFeedbackTests, leveledLoopHoldsAtThePlayedLevel) {
	Loop loop;
	int32_t const played = kSaturation / 30;
	int32_t const feedback = df::shape(knob(50));
	loop.note(feedback, played);
	int32_t peak = loop.run(feedback, 10 * kBlocksPerSecond);
	double const level = dB(static_cast<double>(peak) / played);
	CHECK(level < 4.0);
	CHECK(level > -3.0); // still going, not cut off
	peak = loop.run(feedback, 30 * kBlocksPerSecond);
	CHECK(dB(static_cast<double>(peak) / played) > -3.0);
}

// Played into louder, it follows - but never past the loudest thing played
TEST(DelayFeedbackTests, leveledLoopFollowsLouderPlaying) {
	Loop loop;
	int32_t const feedback = df::shape(knob(50));
	loop.note(feedback, kSaturation / 30);
	loop.run(feedback, 2 * kBlocksPerSecond);
	int32_t const louder = kSaturation / 8;
	loop.note(feedback, louder);
	int32_t peak = loop.run(feedback, 5 * kBlocksPerSecond);
	CHECK(dB(static_cast<double>(peak) / louder) < 4.0);
	CHECK(dB(static_cast<double>(peak) / louder) > -3.0);
}

// A dub throw: play, let it echo, then turn the feedback up after the playing has stopped. The loop carries on at
// about the level that was played, rather than being cut off for having nothing coming in.
TEST(DelayFeedbackTests, dubThrowAfterPlayingKeepsGoing) {
	Loop loop;
	int32_t const played = kSaturation / 10;
	loop.note(knob(18), played);
	loop.run(knob(18), kBlocksPerSecond / 3);
	int32_t peak = loop.run(df::shape(knob(50)), 4 * kBlocksPerSecond);
	CHECK(dB(static_cast<double>(peak) / played) < 4.0);
	CHECK(dB(static_cast<double>(peak) / played) > -9.0);
}

// Turning the knob up must never make the echo quieter: a long tail at 44, turned up past unity after it has been
// ringing for a few seconds, carries on from where it was
TEST(DelayFeedbackTests, turningUpPastUnityNeverMakesItQuieter) {
	Loop loop;
	loop.note(df::shape(knob(44)), kSaturation / 10);
	int32_t const before = loop.run(df::shape(knob(44)), 4 * kBlocksPerSecond);
	// The last second before turning up, against the first and the third after
	int32_t const justAfter = loop.run(df::shape(knob(46)), kBlocksPerSecond);
	CHECK(dB(static_cast<double>(justAfter) / before) > -0.5);
	int32_t const later = loop.run(df::shape(knob(46)), 2 * kBlocksPerSecond);
	CHECK(dB(static_cast<double>(later) / before) > -0.5);
	CHECK(dB(static_cast<double>(later) / before) < 4.0);
}

// The same with repeats piled up from playing into a long tail all along
TEST(DelayFeedbackTests, turningUpWhilePlayingNeverMakesItQuieter) {
	Loop loop;
	int32_t const played = kSaturation / 20;
	int32_t before = 0;
	for (int32_t i = 0; i < 8; i++) {
		loop.note(df::shape(knob(44)), played);
		before = loop.run(df::shape(knob(44)), kBlocksPerSecond / 2);
	}
	int32_t const after = loop.run(df::shape(knob(50)), 2 * kBlocksPerSecond);
	CHECK(dB(static_cast<double>(after) / before) > -0.5);
}

// Renders come in all sizes, often far smaller than 128 frames: the timing has to hold regardless
TEST(DelayFeedbackTests, dubThrowWorksWithSmallRenders) {
	df::Leveler leveler;
	int32_t const played = kSaturation / 10;
	// Half a second at 29 frames a render after a note, with the repeats dying away
	leveler.process(knob(18), played, 0, 29);
	int32_t wet = played;
	for (int32_t i = 0; i < 44100 / 2 / 29; i++) {
		leveler.process(knob(18), 0, wet, 29);
	}
	// Turned up with the echo at -6 dB: within 150 ms the loop is let grow back towards what was played, not choked
	int32_t fed = 0;
	for (int32_t i = 0; i < 44100 * 15 / 100 / 29; i++) {
		fed = leveler.process(df::shape(knob(50)), 0, played / 2, 29);
	}
	CHECK(fed > kUnity);
}

// Below unity nothing changes: the repeats die away on their own
TEST(DelayFeedbackTests, belowUnityTheLevelerLeavesFeedbackAlone) {
	df::Leveler leveler;
	for (int32_t position : {5, 15, 22, 30, 44}) {
		int32_t const feedback = df::shape(knob(position));
		CHECK_EQUAL(feedback, leveler.process(feedback, kSaturation / 4, kSaturation, 128));
	}
}
