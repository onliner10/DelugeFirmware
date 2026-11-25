#include "dsp/reverb/mutable.hpp"

#include "cppspec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <vector>

namespace rv = deluge::dsp::reverb;

namespace {

/// Mutable::process() as it was before it learned to run four frames at a time, kept as the
/// reference the vectorised one has to reproduce.
class ScalarMutable : public rv::Mutable {
public:
	void process(std::span<int32_t> in, std::span<StereoSample> output) override {
		using namespace rv;
		typename FxEngine::AllPass ap1(150);
		typename FxEngine::AllPass ap2(214);
		typename FxEngine::AllPass ap3(319);
		typename FxEngine::AllPass ap4(527);

		typename FxEngine::AllPass dap1a(2182);
		typename FxEngine::AllPass dap1b(2690);
		typename FxEngine::AllPass del1(4501);

		typename FxEngine::AllPass dap2a(2525);
		typename FxEngine::AllPass dap2b(2197);
		typename FxEngine::AllPass del2(6312);

		typename FxEngine::Context c;
		FxEngine::ConstructTopology(engine_, //<
		                            {
		                                &ap1, &ap2, &ap3, &ap4, //<
		                                &dap1a, &dap1b, &del1,  //<
		                                &dap2a, &dap2b, &del2,  //<
		                            });

		const float kap = diffusion_;
		const float klp = lp_;
		const float krt = reverb_time_;

		float lp_1 = lp_decay_1_;
		float lp_2 = lp_decay_2_;

		for (size_t frame = 0; frame < in.size(); frame++) {
			StereoSample& s = output[frame];
			float wet = 0;
			float apout = 0.0f;
			engine_.Advance();

			const float input_sample = in[frame] / static_cast<float>(std::numeric_limits<int32_t>::max());

			c.Set(input_sample);

			ap1.Process(c, kap);
			ap2.Process(c, kap);
			ap3.Process(c, kap);
			ap4.Process(c, kap);
			apout = c.Get();

			c.Set(apout);
			del2.Interpolate(c, 6261.0f, LFO_2, 50.0f, krt);
			c.Lp(lp_1, klp);
			dap1a.Process(c, -kap);
			dap1b.Process(c, kap);
			del1.Write(c, 2.0f);
			wet = c.Get();
			wet = wet - deluge::dsp::OnePole(hp_r_, wet, hp_cutoff_);
			wet = deluge::dsp::OnePole(lp_r_, wet, lp_cutoff_);

			auto output_right = deluge::dsp::toQ31Saturating(wet * kOutputGain);

			c.Set(apout);
			del1.Interpolate(c, 4460.0f, LFO_1, 40.0f, krt);
			c.Lp(lp_2, klp);
			dap2a.Process(c, -kap);
			dap2b.Process(c, kap);
			del2.Write(c, 2.0f);
			wet = c.Get();
			wet = wet - deluge::dsp::OnePole(hp_l_, wet, hp_cutoff_);
			wet = deluge::dsp::OnePole(lp_l_, wet, lp_cutoff_);

			auto output_left = deluge::dsp::toQ31Saturating(wet * kOutputGain);

			s.l += multiply_32x32_rshift32_rounded(output_left, getPanLeft());
			s.r += multiply_32x32_rshift32_rounded(output_right, getPanRight());
		}

		lp_decay_1_ = lp_1;
		lp_decay_2_ = lp_2;
	}

	[[nodiscard]] const auto& memory() const { return buffer_; }
};

class VectorMutable : public rv::Mutable {
public:
	[[nodiscard]] const auto& memory() const { return buffer_; }
};

struct Settings {
	float room_size;
	float damping;
	float width;
	float hpf;
	float lpf;
	int32_t noise_level;
};

template <typename Model>
std::unique_ptr<Model> makeModel(const Settings& settings) {
	auto model = std::make_unique<Model>();
	model->setRoomSize(settings.room_size);
	model->setDamping(settings.damping);
	model->setWidth(settings.width);
	model->setHPF(settings.hpf);
	model->setLPF(settings.lpf);
	model->setPanLevels(1 << 30, 0x5a827999);
	return model;
}

struct Comparison {
	/// Largest output difference, in q31 steps.
	int64_t max_output_error;
	/// Largest difference anywhere in the delay memory at the end.
	float max_memory_error;
	/// Largest thing the reverb added to its output, so a silent pair can't pass for a matching one.
	int64_t peak;
};

/// Feeds both implementations the same noise, impulses and gaps through the same ragged run of
/// block sizes, so the four-frame path, its leftover frames, and every alignment of the LFO steps
/// and the buffer wrap against a block all get hit.
Comparison compare(const Settings& settings, double seconds, uint32_t seed) {
	auto reference = makeModel<ScalarMutable>(settings);
	auto vectorised = makeModel<VectorMutable>(settings);

	std::mt19937 rng(seed);
	constexpr std::array<size_t, 12> block_sizes{1, 2, 3, 4, 5, 7, 8, 31, 64, 125, 127, 128};
	std::uniform_int_distribution<size_t> pick_block(0, block_sizes.size() - 1);
	std::uniform_int_distribution<int32_t> noise(-settings.noise_level, settings.noise_level);
	std::uniform_int_distribution<int32_t> bed(-(1 << 29), 1 << 29);

	Comparison result{.max_output_error = 0, .max_memory_error = 0.f, .peak = 0};
	const auto total = static_cast<size_t>(seconds * kSampleRate);
	std::vector<int32_t> input;
	std::vector<StereoSample> bedding;
	std::vector<StereoSample> out_reference;
	std::vector<StereoSample> out_vectorised;

	for (size_t done = 0; done < total;) {
		const size_t size = block_sizes[pick_block(rng)];
		input.resize(size);
		bedding.resize(size);
		for (size_t i = 0; i < size; ++i) {
			const size_t t = done + i;
			// Half a second of noise, a quarter of impulses, a quarter of silence for the tail.
			const size_t phase = t % kSampleRate;
			if (phase < kSampleRate / 2) {
				input[i] = noise(rng);
			}
			else if (phase < 3 * kSampleRate / 4) {
				input[i] = (t % 997 == 0) ? std::numeric_limits<int32_t>::max() : 0;
			}
			else {
				input[i] = 0;
			}
			// The reverb mixes onto what's already there.
			bedding[i] = StereoSample{.l = bed(rng), .r = bed(rng)};
		}
		out_reference = bedding;
		out_vectorised = bedding;

		reference->process(input, out_reference);
		vectorised->process(input, out_vectorised);

		for (size_t i = 0; i < size; ++i) {
			const int64_t l = out_reference[i].l;
			const int64_t r = out_reference[i].r;
			result.max_output_error = std::max(
			    {result.max_output_error, std::abs(l - out_vectorised[i].l), std::abs(r - out_vectorised[i].r)});
			result.peak = std::max({result.peak, std::abs(l - bedding[i].l), std::abs(r - bedding[i].r)});
		}
		done += size;
	}

	const auto& a = reference->memory();
	const auto& b = vectorised->memory();
	for (size_t i = 0; i < a.size(); ++i) {
		result.max_memory_error = std::max(result.max_memory_error, std::abs(a[i] - b[i]));
	}
	return result;
}

constexpr std::array<Settings, 7> kSettings{{
    {0.5f, 0.5f, 0.5f, 0.f, 1.f, 1 << 27},     // the defaults the other specs use
    {1.0f, 0.0f, 1.0f, 0.f, 0.05f, 1 << 27},   // longest, brightest, widest, LPF nearly closed
    {0.0f, 1.0f, 0.0f, 1.f, 1.f, 1 << 27},     // shortest, darkest, narrowest, HPF fully up
    {0.8f, 0.2f, 0.7f, 0.3f, 0.6f, 1 << 24},   //
    {0.3f, 0.9f, 0.2f, 0.6f, 0.2f, 1 << 30},   //
    {0.95f, 0.05f, 0.9f, 0.1f, 0.9f, 1 << 20}, //
    {1.0f, 0.0f, 1.0f, 0.f, 1.f, 0x7fffffff},  // driven into the output clamp
}};

} // namespace

// clang-format off
describe mutable_reverb("Mutable reverb, four frames at a time", $ {
	it("renders what the frame-at-a-time loop did", _ {
		uint32_t seed = 1;
		for (const Settings& settings : kSettings) {
			const Comparison result = compare(settings, 6.0, seed++);
			expect(result.peak).to_be_greater_than(int64_t{1} << 20);
			expect(result.max_output_error).to_be_less_than(int64_t{2});
			expect(result.max_memory_error).to_be_less_than(1e-6f);
		}
	});

	it("keeps every read inside its line's reserved region", _ {
		rv::debug::out_of_bounds_access = false;
		compare(kSettings[1], 2.0, 99);
		expect(rv::debug::out_of_bounds_access).to_be_false();
	});
});
// clang-format on

CPPSPEC_SPEC(mutable_reverb)
