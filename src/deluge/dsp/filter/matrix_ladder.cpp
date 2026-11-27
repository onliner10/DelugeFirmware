/*
 * Copyright © 2026 Synthstrom Audible Limited
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
#include "dsp/filter/matrix_ladder.h"
#include <array>

namespace deluge::dsp::filter {

// Each stage is a trapezoidal one-pole: with G = g / (1 + g), its lowpass output is lp = G in + (1 - G) s and its
// memory becomes s' = 2 lp - s; the highpass output is in - lp. A stage's output is linear in its input and memory, so
// the ladder's output is linear in the loop input u and the memories, and the feedback u = c x - k y can be solved for
// u directly: no delay in the loop. Substituting gives the four matrices. c = 1 + k keeps the passband at unity gain (a
// ladder's passband otherwise drops by 1 / (1 + k) as resonance goes up).
void MatrixLadder::configureReference(Topology topology, float g, float k) {
	lastG_ = -1.f; // configure() mustn't take these for its own
	bool const highpass = (topology == Topology::HP4 || topology == Topology::HP2);
	int32_t const n = (topology == Topology::LP4 || topology == Topology::HP4) ? 4 : 2;
	float const G = g / (1.f + g);
	float const beta = 1.f - G;
	float const c = 1.f + k;

	std::array<std::array<float, 4>, 4> A{}; // A[row j][column i]
	std::array<float, 4> B{};
	std::array<float, 4> C{};
	float D;

	if (!highpass) {
		std::array<float, 5> Gp{1.f, G, G * G, G * G * G, G * G * G * G}; // G^j
		// y_n = G^n u + sum_i w_i s_i, w_i = G^(n-i) beta
		std::array<float, 4> w{};
		for (int32_t i = 0; i < n; i++) {
			w[i] = Gp[n - 1 - i] * beta;
		}
		float const d0 = 1.f / (1.f + k * Gp[n]);
		float const a = d0 * c;
		std::array<float, 4> r{};
		for (int32_t i = 0; i < n; i++) {
			r[i] = -d0 * k * w[i];
		}
		// y_j = G^j u + sum_{i <= j} G^(j-i) beta s_i (stages and memories 1-based in the maths, 0-based here)
		for (int32_t j = 0; j < n; j++) {
			for (int32_t i = 0; i < n; i++) {
				float y = Gp[j + 1] * r[i] + ((i <= j) ? Gp[j - i] * beta : 0.f);
				A[j][i] = 2.f * y - ((i == j) ? 1.f : 0.f);
			}
			B[j] = 2.f * Gp[j + 1] * a;
		}
		for (int32_t i = 0; i < n; i++) {
			C[i] = Gp[n] * r[i] + w[i];
		}
		D = Gp[n] * a;
	}
	else {
		std::array<float, 5> Bp{1.f, beta, beta * beta, beta * beta * beta, beta * beta * beta * beta}; // beta^j
		// in_j = beta^(j-1) u - sum_{i < j} beta^(j-i) s_i; y = beta^n u + sum_i v_i s_i, v_i = -beta^(n-i+1)
		std::array<float, 4> v{};
		for (int32_t i = 0; i < n; i++) {
			v[i] = -Bp[n - i];
		}
		float const d0 = 1.f / (1.f + k * Bp[n]);
		float const a = d0 * c;
		std::array<float, 4> r{};
		for (int32_t i = 0; i < n; i++) {
			r[i] = -d0 * k * v[i];
		}
		// s_j' = 2 G in_j + (2 beta - 1) s_j
		for (int32_t j = 0; j < n; j++) {
			for (int32_t i = 0; i < n; i++) {
				float in = Bp[j] * r[i] - ((i < j) ? Bp[j - i] : 0.f);
				A[j][i] = 2.f * G * in + ((i == j) ? (2.f * beta - 1.f) : 0.f);
			}
			B[j] = 2.f * G * Bp[j] * a;
		}
		for (int32_t i = 0; i < n; i++) {
			C[i] = Bp[n] * r[i] + v[i];
		}
		D = Bp[n] * a;
	}

	for (int32_t i = 0; i < 4; i++) {
		float column[4] = {A[0][i], A[1][i], A[2][i], A[3][i]};
		a_[i] = vld1q_f32(column);
	}
	b_ = vld1q_f32(B.data());
	c_ = vld1q_f32(C.data());
	d_ = D;
	configureFourSteps();
}

namespace {
[[gnu::always_inline]] inline float32x4_t matVec(float32x4_t const (&m)[4], float32x4_t v) {
	float32x4_t t0 = vmulq_lane_f32(m[0], vget_low_f32(v), 0);
	float32x4_t t1 = vmulq_lane_f32(m[1], vget_low_f32(v), 1);
	t0 = vmlaq_lane_f32(t0, m[2], vget_high_f32(v), 0);
	t1 = vmlaq_lane_f32(t1, m[3], vget_high_f32(v), 1);
	return vaddq_f32(t0, t1);
}
// out = m * n, all as columns
[[gnu::always_inline]] inline void matMul(float32x4_t const (&m)[4], float32x4_t const (&n)[4], float32x4_t (&out)[4]) {
	for (int32_t i = 0; i < 4; i++) {
		out[i] = matVec(m, n[i]);
	}
}
// Columns <-> rows
[[gnu::always_inline]] inline void transpose(float32x4_t const (&in)[4], float32x4_t (&out)[4]) {
	float32x4x2_t const t01 = vtrnq_f32(in[0], in[1]);
	float32x4x2_t const t23 = vtrnq_f32(in[2], in[3]);
	out[0] = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
	out[1] = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
	out[2] = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
	out[3] = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
}
[[gnu::always_inline]] inline float dot(float32x4_t a, float32x4_t b) {
	float32x4_t const m = vmulq_f32(a, b);
	float32x2_t const s = vadd_f32(vget_low_f32(m), vget_high_f32(m));
	return vget_lane_f32(vpadd_f32(s, s), 0);
}
} // namespace

void MatrixLadder::configureFourSteps() {
	float32x4_t a2[4];
	float32x4_t a3[4];
	matMul(a_, a_, a2);
	matMul(a2, a_, a3);
	matMul(a3, a_, a4_);
	float32x4_t const ab = matVec(a_, b_);
	float32x4_t const a2b = matVec(a2, b_);
	float32x4_t const a3b = matVec(a3, b_);
	p_[0] = a3b;
	p_[1] = a2b;
	p_[2] = ab;
	p_[3] = b_;
	// Row k of O is C A^k, i.e. (A^k)^T C
	float32x4_t rows[4];
	float32x4_t r[4];
	r[0] = c_;
	transpose(a_, rows);
	r[1] = matVec(rows, c_);
	transpose(a2, rows);
	r[2] = matVec(rows, c_);
	transpose(a3, rows);
	r[3] = matVec(rows, c_);
	// o_[i] holds element i of each row, so O s = sum_i o_[i] s_i
	transpose(r, o_);
	float const markov[4] = {d_, dot(c_, b_), dot(c_, ab), dot(c_, a2b)};
	markov_ = vld1q_f32(markov);
}

namespace {
// One step of the recursion: A s + B x, as four independent products added in a tree
[[gnu::always_inline]] inline float32x4_t stepTree(float32x4_t const (&a)[4], float32x4_t b, float32x4_t s,
                                                   float32x2_t x, auto xLane) {
	float32x2_t const lo = vget_low_f32(s);
	float32x2_t const hi = vget_high_f32(s);
	float32x4_t t0 = vmulq_lane_f32(a[0], lo, 0);
	float32x4_t t1 = vmulq_lane_f32(a[1], lo, 1);
	float32x4_t t2 = vmulq_lane_f32(a[2], hi, 0);
	float32x4_t t3 = vmulq_lane_f32(a[3], hi, 1);
	t0 = vmlaq_lane_f32(t0, b, x, decltype(xLane)::value);
	return vaddq_f32(vaddq_f32(t0, t1), vaddq_f32(t2, t3));
}

[[gnu::always_inline]] inline float32x4_t stepChain(float32x4_t const (&a)[4], float32x4_t b, float32x4_t s,
                                                    float32x2_t x, auto xLane) {
	float32x2_t const lo = vget_low_f32(s);
	float32x2_t const hi = vget_high_f32(s);
	float32x4_t t = vmulq_lane_f32(b, x, decltype(xLane)::value);
	t = vmlaq_lane_f32(t, a[0], lo, 0);
	t = vmlaq_lane_f32(t, a[1], lo, 1);
	t = vmlaq_lane_f32(t, a[2], hi, 0);
	t = vmlaq_lane_f32(t, a[3], hi, 1);
	return t;
}

template <int L>
using Lane = std::integral_constant<int, L>;

template <bool kTree>
[[gnu::always_inline]] inline void run(float32x4_t const (&a)[4], float32x4_t b, float32x4_t c, float d,
                                       float32x4_t& state, q31_t* buffer, int32_t numSamples, int32_t stride) {
	auto step = [&](float32x4_t s, float32x2_t x, auto lane) {
		if constexpr (kTree) {
			return stepTree(a, b, s, x, lane);
		}
		else {
			return stepChain(a, b, s, x, lane);
		}
	};
	float32x4_t s = state;
	int32_t i = 0;
	for (; i + 4 <= numSamples; i += 4) {
		q31_t* p = buffer + i * stride;
		int32x4_t in;
		int32x4x2_t pair;
		if (stride == 1) {
			in = vld1q_s32(p);
		}
		else {
			pair = vld2q_s32(p);
			in = pair.val[0];
		}
		float32x4_t const xs = vcvtq_n_f32_s32(in, 31);
		float32x2_t const x01 = vget_low_f32(xs);
		float32x2_t const x23 = vget_high_f32(xs);
		// The state before each of the four samples - the output is worked out from those after, outside the recursion
		float32x4_t const s0 = s;
		s = step(s, x01, Lane<0>{});
		float32x4_t const s1 = s;
		s = step(s, x01, Lane<1>{});
		float32x4_t const s2 = s;
		s = step(s, x23, Lane<0>{});
		float32x4_t const s3 = s;
		s = step(s, x23, Lane<1>{});
		// y = C s + D x for the four samples at once: transpose so each vector holds one memory across the samples
		float32x4x2_t const t01 = vtrnq_f32(s0, s1);
		float32x4x2_t const t23 = vtrnq_f32(s2, s3);
		float32x4_t const m0 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
		float32x4_t const m1 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
		float32x4_t const m2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
		float32x4_t const m3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
		float32x4_t y = vmulq_n_f32(xs, d);
		y = vmlaq_lane_f32(y, m0, vget_low_f32(c), 0);
		y = vmlaq_lane_f32(y, m1, vget_low_f32(c), 1);
		y = vmlaq_lane_f32(y, m2, vget_high_f32(c), 0);
		y = vmlaq_lane_f32(y, m3, vget_high_f32(c), 1);
		int32x4_t const out = vcvtq_n_s32_f32(y, 31); // saturates
		if (stride == 1) {
			vst1q_s32(p, out);
		}
		else {
			pair.val[0] = out;
			vst2q_s32(p, pair);
		}
	}
	// The last few, one at a time
	for (; i < numSamples; i++) {
		q31_t* p = buffer + i * stride;
		float32x2_t const x = vdup_n_f32(static_cast<float>(*p) * (1.f / 2147483648.f));
		float32x4_t const sc = vmulq_f32(s, c);
		float32x2_t const sum2 = vadd_f32(vget_low_f32(sc), vget_high_f32(sc));
		float32x2_t const y = vmla_n_f32(vpadd_f32(sum2, sum2), x, d);
		*p = vget_lane_s32(vcvt_n_s32_f32(y, 31), 0);
		s = step(s, x, Lane<0>{});
	}
	state = s;
}
} // namespace

void MatrixLadder::processTree(q31_t* buffer, int32_t numSamples, int32_t stride) {
	run<true>(a_, b_, c_, d_, state_, buffer, numSamples, stride);
}

void MatrixLadder::process(q31_t* buffer, int32_t numSamples, int32_t stride) {
	float32x4_t s = state_;
	float32x4_t const zero = vdupq_n_f32(0.f);
	float32x2_t const mk01 = vget_low_f32(markov_);
	float32x2_t const mk23 = vget_high_f32(markov_);
	int32_t i = 0;
	for (; i + 4 <= numSamples; i += 4) {
		q31_t* p = buffer + i * stride;
		int32x4_t in;
		int32x4x2_t pair;
		if (stride == 1) {
			in = vld1q_s32(p);
		}
		else {
			pair = vld2q_s32(p);
			in = pair.val[0];
		}
		float32x4_t const x = vcvtq_n_f32_s32(in, 31);
		float32x2_t const x01 = vget_low_f32(x);
		float32x2_t const x23 = vget_high_f32(x);
		float32x2_t const s01 = vget_low_f32(s);
		float32x2_t const s23 = vget_high_f32(s);

		// The four outputs: O s, plus the inputs through the Toeplitz T (each diagonal is x shifted down)
		float32x4_t y0 = vmulq_lane_f32(o_[0], s01, 0);
		float32x4_t y1 = vmulq_lane_f32(o_[1], s01, 1);
		y0 = vmlaq_lane_f32(y0, o_[2], s23, 0);
		y1 = vmlaq_lane_f32(y1, o_[3], s23, 1);
		y0 = vmlaq_lane_f32(y0, x, mk01, 0);
		y1 = vmlaq_lane_f32(y1, vextq_f32(zero, x, 3), mk01, 1);
		y0 = vmlaq_lane_f32(y0, vextq_f32(zero, x, 2), mk23, 0);
		y1 = vmlaq_lane_f32(y1, vextq_f32(zero, x, 1), mk23, 1);

		// The state after them: A^4 s + P x
		float32x4_t n0 = vmulq_lane_f32(a4_[0], s01, 0);
		float32x4_t n1 = vmulq_lane_f32(a4_[1], s01, 1);
		n0 = vmlaq_lane_f32(n0, a4_[2], s23, 0);
		n1 = vmlaq_lane_f32(n1, a4_[3], s23, 1);
		n0 = vmlaq_lane_f32(n0, p_[0], x01, 0);
		n1 = vmlaq_lane_f32(n1, p_[1], x01, 1);
		n0 = vmlaq_lane_f32(n0, p_[2], x23, 0);
		n1 = vmlaq_lane_f32(n1, p_[3], x23, 1);
		s = vaddq_f32(n0, n1);

		int32x4_t const out = vcvtq_n_s32_f32(vaddq_f32(y0, y1), 31); // saturates
		if (stride == 1) {
			vst1q_s32(p, out);
		}
		else {
			pair.val[0] = out;
			vst2q_s32(p, pair);
		}
	}
	state_ = s;
	if (i < numSamples) {
		// The last 1-3, a sample at a time
		run<true>(a_, b_, c_, d_, state_, buffer + i * stride, numSamples - i, stride);
	}
}

void MatrixLadder::processChain(q31_t* buffer, int32_t numSamples, int32_t stride) {
	run<false>(a_, b_, c_, d_, state_, buffer, numSamples, stride);
}

// The same matrices as configureReference(), built a column at a time in NEON registers, and the four-step ones with
// two matrix products (A^2, A^4) and a few matrix-vector ones. Nothing to do if nothing changed.
void MatrixLadder::configure(Topology topology, float g, float k) {
	if (topology == lastTopology_ && g == lastG_ && k == lastK_) {
		return;
	}
	lastTopology_ = topology;
	lastG_ = g;
	lastK_ = k;
	bool const highpass = (topology == Topology::HP4 || topology == Topology::HP2);
	bool const four = (topology == Topology::LP4 || topology == Topology::HP4);
	float const G = g / (1.f + g);
	float const beta = 1.f - G;
	float const c = 1.f + k;
	float32x4_t const zero = vdupq_n_f32(0.f);
	// Lanes 2 and 3 of the 2-pole ladders stay 0
	float32x4_t const live = four ? vdupq_n_f32(1.f) : (float32x4_t){1.f, 1.f, 0.f, 0.f};
	float32x4_t const e0 = {1.f, 0.f, 0.f, 0.f};
	float32x4_t const e1 = {0.f, 1.f, 0.f, 0.f};
	float32x4_t const e2 = {0.f, 0.f, 1.f, 0.f};
	float32x4_t const e3 = {0.f, 0.f, 0.f, 1.f};
	if (!highpass) {
		float const G2 = G * G;
		float const G3 = G2 * G;
		float const G4 = G2 * G2;
		float const Gn = four ? G4 : G2;
		float32x4_t const gv = vmulq_f32((float32x4_t){G, G2, G3, G4}, live);  // G^(j+1)
		float32x4_t const pv = vmulq_f32((float32x4_t){1.f, G, G2, G3}, live); // G^j
		// w_i = G^(n-1-i) beta
		float32x4_t const w = vmulq_n_f32(four ? (float32x4_t){G3, G2, G, 1.f} : (float32x4_t){G, 1.f, 0.f, 0.f}, beta);
		float const d0 = 1.f / (1.f + k * Gn);
		float const a = d0 * c;
		float32x4_t const r = vmulq_n_f32(w, -d0 * k);
		// Column i: 2 (G^(j+1) r_i + [j >= i] G^(j-i) beta) - [j == i]
		float32x4_t const twoBetaPv = vmulq_n_f32(pv, 2.f * beta);
		float32x4_t const twoGv = vaddq_f32(gv, gv);
		a_[0] = vmulq_f32(vsubq_f32(vmlaq_lane_f32(twoBetaPv, twoGv, vget_low_f32(r), 0), e0), live);
		a_[1] =
		    vmulq_f32(vsubq_f32(vmlaq_lane_f32(vextq_f32(zero, twoBetaPv, 3), twoGv, vget_low_f32(r), 1), e1), live);
		a_[2] =
		    vmulq_f32(vsubq_f32(vmlaq_lane_f32(vextq_f32(zero, twoBetaPv, 2), twoGv, vget_high_f32(r), 0), e2), live);
		a_[3] =
		    vmulq_f32(vsubq_f32(vmlaq_lane_f32(vextq_f32(zero, twoBetaPv, 1), twoGv, vget_high_f32(r), 1), e3), live);
		b_ = vmulq_n_f32(gv, 2.f * a);
		c_ = vmlaq_n_f32(w, r, Gn);
		d_ = Gn * a;
	}
	else {
		float const B2 = beta * beta;
		float const B3 = B2 * beta;
		float const B4 = B2 * B2;
		float const Bn = four ? B4 : B2;
		float32x4_t const bv = vmulq_f32((float32x4_t){1.f, beta, B2, B3}, live); // beta^j
		// v_i = -beta^(n-i)
		float32x4_t const v = four ? (float32x4_t){-B4, -B3, -B2, -beta} : (float32x4_t){-B2, -beta, 0.f, 0.f};
		float const d0 = 1.f / (1.f + k * Bn);
		float const a = d0 * c;
		float32x4_t const r = vmulq_n_f32(v, -d0 * k);
		// Column i: 2G (beta^j r_i - [j > i] beta^(j-i)) + [j == i] (2 beta - 1)
		float32x4_t const twoGbv = vmulq_n_f32(bv, 2.f * G);
		// [0, 2G beta, 2G beta^2, 2G beta^3]: shifted down by i lanes, the [j > i] terms of column i
		float32x4_t const sub = vmulq_n_f32(vextq_f32(zero, (float32x4_t){beta, B2, B3, 0.f}, 3), 2.f * G);
		float const diag = 2.f * beta - 1.f;
		a_[0] = vmulq_f32(vmlaq_n_f32(vsubq_f32(vmulq_lane_f32(twoGbv, vget_low_f32(r), 0), sub), e0, diag), live);
		a_[1] = vmulq_f32(
		    vmlaq_n_f32(vsubq_f32(vmulq_lane_f32(twoGbv, vget_low_f32(r), 1), vextq_f32(zero, sub, 3)), e1, diag),
		    live);
		a_[2] = vmulq_f32(
		    vmlaq_n_f32(vsubq_f32(vmulq_lane_f32(twoGbv, vget_high_f32(r), 0), vextq_f32(zero, sub, 2)), e2, diag),
		    live);
		a_[3] = vmulq_f32(
		    vmlaq_n_f32(vsubq_f32(vmulq_lane_f32(twoGbv, vget_high_f32(r), 1), vextq_f32(zero, sub, 1)), e3, diag),
		    live);
		b_ = vmulq_n_f32(twoGbv, a);
		c_ = vmlaq_n_f32(v, r, Bn);
		d_ = Bn * a;
	}
	configureFourStepsFast();
}

void MatrixLadder::configureFourStepsFast() {
	float32x4_t a2[4];
	matMul(a_, a_, a2);
	matMul(a2, a2, a4_);
	float32x4_t const ab = matVec(a_, b_);
	float32x4_t const a2b = matVec(a2, b_);
	p_[0] = matVec(a_, a2b);
	p_[1] = a2b;
	p_[2] = ab;
	p_[3] = b_;
	float32x4_t at[4];
	transpose(a_, at);
	float32x4_t r[4];
	r[0] = c_;
	r[1] = matVec(at, c_);
	r[2] = matVec(at, r[1]);
	r[3] = matVec(at, r[2]);
	transpose(r, o_);
	float const markov[4] = {d_, dot(c_, b_), dot(r[1], b_), dot(r[2], b_)};
	markov_ = vld1q_f32(markov);
}

// Like process(), with every product its own multiply and the sums as trees (no chains of multiply-accumulates), and
// the coefficients in locals
void MatrixLadder::processMul(q31_t* buffer, int32_t numSamples, int32_t stride) {
	float32x4_t s = state_;
	float32x4_t const zero = vdupq_n_f32(0.f);
	float32x4_t const o0 = o_[0], o1 = o_[1], o2 = o_[2], o3 = o_[3];
	float32x4_t const f0 = a4_[0], f1 = a4_[1], f2 = a4_[2], f3 = a4_[3];
	float32x4_t const q0 = p_[0], q1 = p_[1], q2 = p_[2], q3 = p_[3];
	float32x2_t const mk01 = vget_low_f32(markov_);
	float32x2_t const mk23 = vget_high_f32(markov_);
	int32_t i = 0;
	for (; i + 4 <= numSamples; i += 4) {
		q31_t* p = buffer + i * stride;
		int32x4_t in;
		int32x4x2_t pair;
		if (stride == 1) {
			in = vld1q_s32(p);
		}
		else {
			pair = vld2q_s32(p);
			in = pair.val[0];
		}
		float32x4_t const x = vcvtq_n_f32_s32(in, 31);
		float32x2_t const x01 = vget_low_f32(x);
		float32x2_t const x23 = vget_high_f32(x);
		float32x2_t const s01 = vget_low_f32(s);
		float32x2_t const s23 = vget_high_f32(s);
		float32x4_t const n0 = vmulq_lane_f32(f0, s01, 0);
		float32x4_t const n1 = vmulq_lane_f32(f1, s01, 1);
		float32x4_t const n2 = vmulq_lane_f32(f2, s23, 0);
		float32x4_t const n3 = vmulq_lane_f32(f3, s23, 1);
		float32x4_t const n4 = vmulq_lane_f32(q0, x01, 0);
		float32x4_t const n5 = vmulq_lane_f32(q1, x01, 1);
		float32x4_t const n6 = vmulq_lane_f32(q2, x23, 0);
		float32x4_t const n7 = vmulq_lane_f32(q3, x23, 1);
		float32x4_t const y0 = vmulq_lane_f32(o0, s01, 0);
		float32x4_t const y1 = vmulq_lane_f32(o1, s01, 1);
		float32x4_t const y2 = vmulq_lane_f32(o2, s23, 0);
		float32x4_t const y3 = vmulq_lane_f32(o3, s23, 1);
		float32x4_t const y4 = vmulq_lane_f32(x, mk01, 0);
		float32x4_t const y5 = vmulq_lane_f32(vextq_f32(zero, x, 3), mk01, 1);
		float32x4_t const y6 = vmulq_lane_f32(vextq_f32(zero, x, 2), mk23, 0);
		float32x4_t const y7 = vmulq_lane_f32(vextq_f32(zero, x, 1), mk23, 1);
		s = vaddq_f32(vaddq_f32(vaddq_f32(n0, n1), vaddq_f32(n2, n3)), vaddq_f32(vaddq_f32(n4, n5), vaddq_f32(n6, n7)));
		float32x4_t const y =
		    vaddq_f32(vaddq_f32(vaddq_f32(y0, y1), vaddq_f32(y2, y3)), vaddq_f32(vaddq_f32(y4, y5), vaddq_f32(y6, y7)));
		int32x4_t const out = vcvtq_n_s32_f32(y, 31);
		if (stride == 1) {
			vst1q_s32(p, out);
		}
		else {
			pair.val[0] = out;
			vst2q_s32(p, pair);
		}
	}
	state_ = s;
	if (i < numSamples) {
		processTree(buffer + i * stride, numSamples - i, stride);
	}
}

// Both channels of interleaved stereo with one set of coefficients, the two recursions side by side in one loop
void MatrixLadder::processStereo(q31_t* buffer, int32_t numFrames) {
	float32x4_t sl = state_;
	float32x4_t sr = stateR_;
	float32x4_t const zero = vdupq_n_f32(0.f);
	float32x2_t const mk01 = vget_low_f32(markov_);
	float32x2_t const mk23 = vget_high_f32(markov_);
	auto block = [&](float32x4_t& s, float32x4_t x) {
		float32x2_t const x01 = vget_low_f32(x);
		float32x2_t const x23 = vget_high_f32(x);
		float32x2_t const s01 = vget_low_f32(s);
		float32x2_t const s23 = vget_high_f32(s);
		float32x4_t y0 = vmulq_lane_f32(o_[0], s01, 0);
		float32x4_t y1 = vmulq_lane_f32(o_[1], s01, 1);
		y0 = vmlaq_lane_f32(y0, o_[2], s23, 0);
		y1 = vmlaq_lane_f32(y1, o_[3], s23, 1);
		y0 = vmlaq_lane_f32(y0, x, mk01, 0);
		y1 = vmlaq_lane_f32(y1, vextq_f32(zero, x, 3), mk01, 1);
		y0 = vmlaq_lane_f32(y0, vextq_f32(zero, x, 2), mk23, 0);
		y1 = vmlaq_lane_f32(y1, vextq_f32(zero, x, 1), mk23, 1);
		float32x4_t n0 = vmulq_lane_f32(a4_[0], s01, 0);
		float32x4_t n1 = vmulq_lane_f32(a4_[1], s01, 1);
		n0 = vmlaq_lane_f32(n0, a4_[2], s23, 0);
		n1 = vmlaq_lane_f32(n1, a4_[3], s23, 1);
		n0 = vmlaq_lane_f32(n0, p_[0], x01, 0);
		n1 = vmlaq_lane_f32(n1, p_[1], x01, 1);
		n0 = vmlaq_lane_f32(n0, p_[2], x23, 0);
		n1 = vmlaq_lane_f32(n1, p_[3], x23, 1);
		s = vaddq_f32(n0, n1);
		return vaddq_f32(y0, y1);
	};
	int32_t i = 0;
	for (; i + 4 <= numFrames; i += 4) {
		q31_t* p = buffer + i * 2;
		int32x4x2_t lr = vld2q_s32(p);
		float32x4_t const yl = block(sl, vcvtq_n_f32_s32(lr.val[0], 31));
		float32x4_t const yr = block(sr, vcvtq_n_f32_s32(lr.val[1], 31));
		lr.val[0] = vcvtq_n_s32_f32(yl, 31);
		lr.val[1] = vcvtq_n_s32_f32(yr, 31);
		vst2q_s32(p, lr);
	}
	if (i < numFrames) {
		// The last 1-3 frames a sample at a time, each channel with its own state
		state_ = sr;
		processTree(buffer + i * 2 + 1, numFrames - i, 2);
		sr = state_;
		state_ = sl;
		processTree(buffer + i * 2, numFrames - i, 2);
		sl = state_;
	}
	state_ = sl;
	stateR_ = sr;
}

} // namespace deluge::dsp::filter
