#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "dsp/GlobalGain.h"

namespace {

using guitarrackcraft::GlobalGain;

constexpr int32_t kBlockSize = 64;
constexpr int32_t kLongBlock = 480;
constexpr int32_t kSampleRate = 48000;

double linearOf(float db) {
	return std::pow(10.0, static_cast<double>(db) / 20.0);
}

double relativeTolerance(float db) {
	return 1e-6 * std::max(1.0, linearOf(db));
}

std::vector<float> filled(int32_t frames, float value) {
	return std::vector<float>(frames, value);
}

std::vector<float> noise(int32_t frames, uint32_t seed) {
	std::mt19937 rng(seed);
	std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);

	std::vector<float> buffer(static_cast<size_t>(frames));

	for (float& sample : buffer)
		sample = distribution(rng);

	return buffer;
}

bool isUnity(float gain) {
	return gain == 1.0f;
}

// The gain each frame received, read back by processing a block of ones.
std::vector<float> gainsOf(GlobalGain& gain, int32_t frames, float targetDb) {
	std::vector<float> block = filled(frames, 1.0f);

	gain.process(block.data(), frames, targetDb);

	return block;
}

void expectAllFramesFinite(const std::vector<float>& buffer) {
	for (size_t i = 0; i < buffer.size(); ++i) {
		ASSERT_TRUE(std::isfinite(buffer[i])) << "frame " << i;
	}
}

} // namespace

TEST(GlobalGain, PublicContractMatchesThePlan) {
	static_assert(GlobalGain::kMinDb == -24.0f, "global gain range is -24 dB");
	static_assert(GlobalGain::kMaxDb == 24.0f, "global gain range is +24 dB");
	static_assert(GlobalGain::kUnityDb == 0.0f, "unity is 0 dB");
	static_assert(GlobalGain::kSnapEpsilon == 1e-6f, "snap threshold is 1e-6 in the linear domain");

	EXPECT_EQ(1.0f, GlobalGain::dbToLinear(GlobalGain::kUnityDb));
	EXPECT_NEAR(GlobalGain::dbToLinear(GlobalGain::kMaxDb), 15.8489, 1e-4);
	EXPECT_NEAR(GlobalGain::dbToLinear(GlobalGain::kMinDb), 0.0630957, 1e-6);
	EXPECT_NEAR(GlobalGain::dbToLinear(24.0f), linearOf(24.0f), relativeTolerance(24.0f));
	EXPECT_NEAR(GlobalGain::dbToLinear(-24.0f), linearOf(-24.0f), relativeTolerance(-24.0f));
}

TEST(GlobalGain, UnityIsABitExactPassthrough) {
	GlobalGain gain;

	std::vector<float> buffer = noise(8192, 1);
	const std::vector<float> expected = buffer;

	for (int32_t offset = 0; offset < 8192; offset += kBlockSize)
		gain.process(buffer.data() + offset, kBlockSize, GlobalGain::kUnityDb);

	EXPECT_EQ(0, std::memcmp(expected.data(), buffer.data(), buffer.size() * sizeof(float)));
	EXPECT_TRUE(isUnity(gain.currentGain()));
}

TEST(GlobalGain, UnityLeavesEvenHostileBitPatternsUntouched) {
	// A multiply by 1.0f is not a byte no-op in general: it quiets signalling
	// NaNs and can change payloads. Zero dB must not touch the samples at all.
	const std::vector<uint32_t> patterns = {
		0x00000000u, // +0
		0x80000000u, // -0
		0x00000001u, // smallest denormal
		0x007FFFFFu, // largest denormal
		0x7F800000u, // +inf
		0xFF800000u, // -inf
		0x7F800001u, // signalling NaN with a payload
		0xFF800001u, // negative signalling NaN
		0x7FC00001u, // quiet NaN with a payload
	};

	std::vector<float> buffer(patterns.size() * 4);

	for (size_t i = 0; i < buffer.size(); ++i) {
		const uint32_t bits = patterns[i % patterns.size()];
		std::memcpy(&buffer[i], &bits, sizeof(float));
	}

	const std::vector<float> expected = buffer;
	GlobalGain gain;

	gain.process(buffer.data(), static_cast<int32_t>(buffer.size()), GlobalGain::kUnityDb);

	EXPECT_EQ(0, std::memcmp(expected.data(), buffer.data(), buffer.size() * sizeof(float)));
}

TEST(GlobalGain, UnityStaysFreeAfterResetSnapAndAReturnToZero) {
	std::vector<float> expected = noise(4096, 2);

	GlobalGain afterReset;
	std::vector<float> warmUp = noise(4096, 3);
	for (int32_t offset = 0; offset < 4096; offset += kBlockSize)
		afterReset.process(warmUp.data() + offset, kBlockSize, 6.0f);
	afterReset.reset();
	ASSERT_TRUE(isUnity(afterReset.currentGain()));

	std::vector<float> buffer = expected;
	for (int32_t offset = 0; offset < 4096; offset += kBlockSize)
		afterReset.process(buffer.data() + offset, kBlockSize, GlobalGain::kUnityDb);
	EXPECT_EQ(0, std::memcmp(expected.data(), buffer.data(), buffer.size() * sizeof(float)));

	GlobalGain afterSnap;
	afterSnap.snapTo(GlobalGain::kUnityDb);
	std::vector<float> snapped = expected;
	for (int32_t offset = 0; offset < 4096; offset += kBlockSize)
		afterSnap.process(snapped.data() + offset, kBlockSize, GlobalGain::kUnityDb);
	EXPECT_EQ(0, std::memcmp(expected.data(), snapped.data(), snapped.size() * sizeof(float)));

	// Coming back from a boost costs one block that ends exactly on unity, and
	// from there the stage is a free passthrough again.
	GlobalGain returning;
	std::vector<float> ramped = filled(kBlockSize, 1.0f);
	returning.process(ramped.data(), kBlockSize, 12.0f);
	ASSERT_NE(1.0f, returning.currentGain());

	std::vector<float> landing = expected;
	returning.process(landing.data(), kBlockSize, GlobalGain::kUnityDb);
	EXPECT_TRUE(isUnity(returning.currentGain()));
	EXPECT_EQ(expected[static_cast<size_t>(kBlockSize - 1)], landing[static_cast<size_t>(kBlockSize - 1)]);

	std::vector<float> after = expected;
	for (int32_t offset = 0; offset < 4096; offset += kBlockSize)
		returning.process(after.data() + offset, kBlockSize, GlobalGain::kUnityDb);
	EXPECT_EQ(0, std::memcmp(expected.data(), after.data(), after.size() * sizeof(float)));
}

TEST(GlobalGain, EndpointsProduceTheExpectedLinearFactor) {
	for (float db : {-24.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 24.0f}) {
		SCOPED_TRACE(db);

		GlobalGain gain;
		const std::vector<float> gains = gainsOf(gain, kBlockSize, db);

		EXPECT_EQ(GlobalGain::dbToLinear(db), gains.back());
		EXPECT_EQ(GlobalGain::dbToLinear(db), gain.currentGain());
		EXPECT_NEAR(gains.back(), linearOf(db), relativeTolerance(db));
	}
}

TEST(GlobalGain, OutOfRangeAndNonFiniteTargetsClampToTheEndpoints) {
	EXPECT_EQ(GlobalGain::kMinDb, GlobalGain::clampDb(-200.0f));
	EXPECT_EQ(GlobalGain::kMaxDb, GlobalGain::clampDb(200.0f));
	EXPECT_EQ(GlobalGain::kMaxDb, GlobalGain::clampDb(std::numeric_limits<float>::infinity()));
	EXPECT_EQ(GlobalGain::kMinDb, GlobalGain::clampDb(-std::numeric_limits<float>::infinity()));
	EXPECT_EQ(GlobalGain::kMinDb, GlobalGain::clampDb(std::numeric_limits<float>::quiet_NaN()));
	EXPECT_EQ(GlobalGain::dbToLinear(GlobalGain::kMinDb),
		GlobalGain::dbToLinear(std::numeric_limits<float>::quiet_NaN()));

	struct Case {
		const char* name;
		float db;
		float expectedDb;
	} cases[] = {
		{"-200dB clamps to -24", -200.0f, GlobalGain::kMinDb},
		{"+200dB clamps to +24", 200.0f, GlobalGain::kMaxDb},
		{"+inf clamps to +24", std::numeric_limits<float>::infinity(), GlobalGain::kMaxDb},
		{"-inf clamps to -24", -std::numeric_limits<float>::infinity(), GlobalGain::kMinDb},
		{"NaN maps to -24", std::numeric_limits<float>::quiet_NaN(), GlobalGain::kMinDb},
	};

	for (const Case& testCase : cases) {
		SCOPED_TRACE(testCase.name);

		GlobalGain gain;
		const std::vector<float> gains = gainsOf(gain, kBlockSize, testCase.db);

		expectAllFramesFinite(gains);
		EXPECT_EQ(GlobalGain::dbToLinear(testCase.expectedDb), gains.back());
		EXPECT_EQ(GlobalGain::dbToLinear(testCase.expectedDb), gain.currentGain());
	}
}

TEST(GlobalGain, RampIsMonotonicAndNeverOvershoots) {
	struct Case {
		const char* name;
		float fromDb;
		float toDb;
	} cases[] = {
		{"unity to +18", 0.0f, 18.0f},
		{"unity to -18", 0.0f, -18.0f},
		{"+12 to -12", 12.0f, -12.0f},
		{"-24 to +24", GlobalGain::kMinDb, GlobalGain::kMaxDb},
		{"+24 to 0", GlobalGain::kMaxDb, 0.0f},
	};

	for (const Case& testCase : cases) {
		for (int32_t frames : {kBlockSize, kLongBlock}) {
			SCOPED_TRACE(testCase.name);
			SCOPED_TRACE(frames);

			GlobalGain gain;
			gain.snapTo(testCase.fromDb);

			const float start = GlobalGain::dbToLinear(testCase.fromDb);
			const float target = GlobalGain::dbToLinear(testCase.toDb);
			const std::vector<float> gains = gainsOf(gain, frames, testCase.toDb);

			ASSERT_EQ(static_cast<size_t>(frames), gains.size());
			EXPECT_EQ(target, gains.back());

			const float low = std::min(start, target);
			const float high = std::max(start, target);

			for (int32_t i = 0; i < frames; ++i) {
				ASSERT_GE(gains[static_cast<size_t>(i)], low) << "frame " << i;
				ASSERT_LE(gains[static_cast<size_t>(i)], high) << "frame " << i;

				const float previous = i == 0 ? start : gains[static_cast<size_t>(i - 1)];

				if (target > start)
					ASSERT_GE(gains[static_cast<size_t>(i)], previous) << "frame " << i;
				else
					ASSERT_LE(gains[static_cast<size_t>(i)], previous) << "frame " << i;
			}

			if (target > start)
				EXPECT_GT(gains.front(), start);
			else
				EXPECT_LT(gains.front(), start);
		}
	}
}

TEST(GlobalGain, RampFollowsTheDocumentedLinearTrajectory) {
	GlobalGain gain;
	gain.snapTo(-6.0f);

	const float start = GlobalGain::dbToLinear(-6.0f);
	const float target = GlobalGain::dbToLinear(9.0f);
	const double step = (static_cast<double>(target) - static_cast<double>(start)) / kLongBlock;

	const std::vector<float> gains = gainsOf(gain, kLongBlock, 9.0f);

	for (int32_t i = 0; i < kLongBlock - 1; ++i) {
		const double expected = static_cast<double>(start) + step * static_cast<double>(i + 1);

		ASSERT_NEAR(gains[static_cast<size_t>(i)], expected, 1e-6 * std::max(1.0, std::fabs(expected)))
			<< "frame " << i;
	}

	EXPECT_EQ(target, gains[static_cast<size_t>(kLongBlock - 1)]);
}

TEST(GlobalGain, ReachesALargeStepTargetInsideOneBlock) {
	GlobalGain gain;
	gain.snapTo(GlobalGain::kMinDb);
	ASSERT_EQ(GlobalGain::dbToLinear(GlobalGain::kMinDb), gain.currentGain());

	const std::vector<float> gains = gainsOf(gain, kBlockSize, GlobalGain::kMaxDb);

	EXPECT_EQ(GlobalGain::dbToLinear(GlobalGain::kMaxDb), gain.currentGain());
	EXPECT_EQ(GlobalGain::dbToLinear(GlobalGain::kMaxDb), gains.back());

	const std::vector<float> next = gainsOf(gain, kBlockSize, GlobalGain::kMaxDb);

	for (float value : next)
		EXPECT_EQ(GlobalGain::dbToLinear(GlobalGain::kMaxDb), value);
}

TEST(GlobalGain, TargetChangeMidStreamStartsFromThePreviousTarget) {
	GlobalGain gain;

	const std::vector<float> first = gainsOf(gain, kLongBlock, 6.0f);
	const float previous = gain.currentGain();
	ASSERT_EQ(GlobalGain::dbToLinear(6.0f), previous);

	const std::vector<float> second = gainsOf(gain, kLongBlock, 12.0f);

	const float target = GlobalGain::dbToLinear(12.0f);
	const float expectedBoundaryStep = (target - previous) / static_cast<float>(kLongBlock);

	EXPECT_GT(second.front(), previous);
	EXPECT_LT(second.front(), target);
	EXPECT_NEAR(second.front(), previous + expectedBoundaryStep, 1e-6f * target);
	EXPECT_EQ(target, second.back());

	const float inBlockStep = second[1] - second[0];
	EXPECT_NEAR(second.front() - previous, inBlockStep, 1e-6f * target);

	const std::vector<float> steadyA = gainsOf(gain, kLongBlock, 12.0f);
	const std::vector<float> steadyB = gainsOf(gain, kLongBlock, 12.0f);

	EXPECT_EQ(steadyA, steadyB);
	for (float value : steadyA)
		EXPECT_EQ(target, value);
}

TEST(GlobalGain, SubEpsilonChangesSnapInsteadOfCreeping) {
	GlobalGain gain;
	gain.snapTo(12.0f);

	const float start = gain.currentGain();
	const float tinyDb = 12.0f + 1e-6f;
	const float target = GlobalGain::dbToLinear(tinyDb);

	ASSERT_LE(std::fabs(target - start), GlobalGain::kSnapEpsilon);

	const std::vector<float> gains = gainsOf(gain, kBlockSize, tinyDb);

	for (float value : gains)
		EXPECT_EQ(target, value);
	EXPECT_EQ(target, gains.front());
	EXPECT_EQ(target, gain.currentGain());

	const std::vector<float> again = gainsOf(gain, kBlockSize, tinyDb);

	EXPECT_EQ(gains, again);

	// Sanity check on the other side of the threshold: a real change does ramp,
	// so the constant-block assertion above is not vacuous.
	GlobalGain ramping;
	const std::vector<float> ramped = gainsOf(ramping, kBlockSize, 0.5f);

	EXPECT_NE(ramped.front(), ramped.back());
	EXPECT_LT(ramped.front(), ramped.back());
}

TEST(GlobalGain, SnapToSetsTheStateImmediately) {
	GlobalGain gain;

	gain.snapTo(12.0f);
	EXPECT_EQ(GlobalGain::dbToLinear(12.0f), gain.currentGain());

	const std::vector<float> gains = gainsOf(gain, kBlockSize, 12.0f);

	for (float value : gains)
		EXPECT_EQ(GlobalGain::dbToLinear(12.0f), value);

	gain.snapTo(-9.0f);
	EXPECT_EQ(GlobalGain::dbToLinear(-9.0f), gain.currentGain());

	gain.snapTo(GlobalGain::kUnityDb);
	EXPECT_TRUE(isUnity(gain.currentGain()));
}

TEST(GlobalGain, ResetRestoresUnityFromAnyState) {
	const std::vector<float> expected = noise(1024, 5);

	for (float db : {24.0f, -24.0f, 3.0f}) {
		SCOPED_TRACE(db);

		GlobalGain gain;
		std::vector<float> warmUp = noise(1024, 6);
		for (int32_t offset = 0; offset < 1024; offset += kBlockSize)
			gain.process(warmUp.data() + offset, kBlockSize, db);

		gain.reset();
		ASSERT_TRUE(isUnity(gain.currentGain()));

		std::vector<float> buffer = expected;
		for (int32_t offset = 0; offset < 1024; offset += kBlockSize)
			gain.process(buffer.data() + offset, kBlockSize, GlobalGain::kUnityDb);

		EXPECT_EQ(0, std::memcmp(expected.data(), buffer.data(), buffer.size() * sizeof(float)));
	}

	GlobalGain fresh;

	EXPECT_TRUE(isUnity(fresh.currentGain()));
}

TEST(GlobalGain, StereoChannelsShareOneRamp) {
	const std::vector<float> source = noise(4096, 21);

	std::vector<float> left = source;
	std::vector<float> right = source;

	GlobalGain stereo;
	for (int32_t offset = 0; offset < 4096; offset += kBlockSize)
		stereo.process(left.data() + offset, right.data() + offset, kBlockSize, 12.0f);

	EXPECT_EQ(0, std::memcmp(left.data(), right.data(), left.size() * sizeof(float)));

	std::vector<float> mono = source;
	GlobalGain reference;
	for (int32_t offset = 0; offset < 4096; offset += kBlockSize)
		reference.process(mono.data() + offset, kBlockSize, 12.0f);

	EXPECT_EQ(mono, left);
	EXPECT_EQ(reference.currentGain(), stereo.currentGain());
}

TEST(GlobalGain, StereoRampAppliesTheSameGainToBothChannels) {
	std::vector<float> source = noise(1024, 33);

	// Scaling by two is exact, so an identical gain on both channels keeps
	// outRight == 2 * outLeft bit for bit; a desynced pair would break that.
	std::vector<float> left = source;
	std::vector<float> right(static_cast<size_t>(1024));

	for (int32_t i = 0; i < 1024; ++i)
		right[static_cast<size_t>(i)] = 2.0f * source[static_cast<size_t>(i)];

	GlobalGain stereo;
	for (int32_t offset = 0; offset < 1024; offset += kBlockSize)
		stereo.process(left.data() + offset, right.data() + offset, kBlockSize, -24.0f);

	for (int32_t i = 0; i < 1024; ++i)
		ASSERT_EQ(2.0f * left[static_cast<size_t>(i)], right[static_cast<size_t>(i)]) << "frame " << i;

	EXPECT_EQ(source.back() * GlobalGain::dbToLinear(-24.0f), left.back());
}

TEST(GlobalGain, EdgeFrameCountsAndNullBuffersAreSafe) {
	GlobalGain gain;

	float sample = 1.0f;
	gain.process(&sample, 0, 12.0f);

	EXPECT_EQ(1.0f, sample);
	EXPECT_TRUE(isUnity(gain.currentGain()));

	gain.process(&sample, -7, 12.0f);

	EXPECT_EQ(1.0f, sample);
	EXPECT_TRUE(isUnity(gain.currentGain()));

	gain.process(&sample, 0, 12.0f);
	gain.process(nullptr, kBlockSize, 12.0f);

	EXPECT_EQ(GlobalGain::dbToLinear(12.0f), gain.currentGain());

	GlobalGain single;
	float only = 0.5f;
	single.process(&only, 1, 6.0f);

	EXPECT_EQ(0.5f * GlobalGain::dbToLinear(6.0f), only);
	EXPECT_EQ(GlobalGain::dbToLinear(6.0f), single.currentGain());

	GlobalGain singleStereo;
	float leftOnly = 1.0f;
	singleStereo.process(&leftOnly, nullptr, 1, -6.0f);

	EXPECT_EQ(GlobalGain::dbToLinear(-6.0f), leftOnly);
	EXPECT_EQ(GlobalGain::dbToLinear(-6.0f), singleStereo.currentGain());
}

TEST(GlobalGain, BlocksNeverWriteOutsideTheRequestedRange) {
	constexpr int32_t kGuard = 16;
	constexpr float kGuardValue = -1234.5f;

	for (int32_t frames : {0, 1, 2, 3, 7, 63, kBlockSize, 65, kLongBlock, 4096}) {
		for (float db : {-24.0f, -3.0f, 0.0f, 0.5f, 24.0f}) {
			SCOPED_TRACE(frames);
			SCOPED_TRACE(db);

			std::vector<float> mono(static_cast<size_t>(frames) + 2 * kGuard, 0.75f);
			for (int32_t i = 0; i < kGuard; ++i) {
				mono[static_cast<size_t>(i)] = kGuardValue;
				mono[static_cast<size_t>(frames) + kGuard + i] = kGuardValue;
			}

			GlobalGain gain;
			gain.process(mono.data() + kGuard, frames, db);

			for (int32_t i = 0; i < kGuard; ++i) {
				ASSERT_EQ(kGuardValue, mono[static_cast<size_t>(i)]) << "leading guard " << i;
				ASSERT_EQ(kGuardValue, mono[static_cast<size_t>(frames) + kGuard + i])
					<< "trailing guard " << i;
			}

			std::vector<float> left(static_cast<size_t>(frames) + 2 * kGuard, 0.75f);
			std::vector<float> right(static_cast<size_t>(frames) + 2 * kGuard, -0.75f);
			for (int32_t i = 0; i < kGuard; ++i) {
				left[static_cast<size_t>(i)] = kGuardValue;
				left[static_cast<size_t>(frames) + kGuard + i] = kGuardValue;
				right[static_cast<size_t>(i)] = kGuardValue;
				right[static_cast<size_t>(frames) + kGuard + i] = kGuardValue;
			}

			GlobalGain stereo;
			stereo.process(left.data() + kGuard, right.data() + kGuard, frames, db);

			for (int32_t i = 0; i < kGuard; ++i) {
				ASSERT_EQ(kGuardValue, left[static_cast<size_t>(i)]) << "left leading guard " << i;
				ASSERT_EQ(kGuardValue, left[static_cast<size_t>(frames) + kGuard + i])
					<< "left trailing guard " << i;
				ASSERT_EQ(kGuardValue, right[static_cast<size_t>(i)]) << "right leading guard " << i;
				ASSERT_EQ(kGuardValue, right[static_cast<size_t>(frames) + kGuard + i])
					<< "right trailing guard " << i;
			}
		}
	}
}

TEST(GlobalGain, TenSecondsOfMovingTargetsStayFiniteAndBounded) {
	constexpr int32_t kBlocks = kSampleRate * 10 / kBlockSize;
	const float peakCeiling = 0.5f * GlobalGain::dbToLinear(GlobalGain::kMaxDb) * 1.0001f;

	std::mt19937 rng(2024);
	std::uniform_real_distribution<float> signal(-0.5f, 0.5f);
	std::uniform_real_distribution<float> targets(GlobalGain::kMinDb, GlobalGain::kMaxDb);

	GlobalGain gain;
	std::vector<float> left(static_cast<size_t>(kBlockSize));
	std::vector<float> right(static_cast<size_t>(kBlockSize));

	unsigned int nonFinite = 0;
	float peak = 0.0f;
	float targetDb = 0.0f;
	float lowest = std::numeric_limits<float>::max();
	float highest = 0.0f;

	for (int32_t block = 0; block < kBlocks; ++block) {
		if (block % 37 == 0)
			targetDb = targets(rng);
		if (block % 101 == 0)
			targetDb = block % 202 == 0 ? GlobalGain::kMinDb : GlobalGain::kMaxDb;

		for (int32_t i = 0; i < kBlockSize; ++i) {
			left[static_cast<size_t>(i)] = signal(rng);
			right[static_cast<size_t>(i)] = signal(rng);
		}

		gain.process(left.data(), right.data(), kBlockSize, targetDb);

		for (int32_t i = 0; i < kBlockSize; ++i) {
			for (float sample : {left[static_cast<size_t>(i)], right[static_cast<size_t>(i)]}) {
				if (!std::isfinite(sample))
					nonFinite++;
				peak = std::max(peak, std::fabs(sample));
			}
		}

		lowest = std::min(lowest, gain.currentGain());
		highest = std::max(highest, gain.currentGain());
	}

	EXPECT_EQ(0u, nonFinite);
	EXPECT_TRUE(std::isfinite(peak));
	EXPECT_LE(peak, peakCeiling);
	EXPECT_NEAR(gain.currentGain(), GlobalGain::dbToLinear(targetDb), relativeTolerance(targetDb));
	EXPECT_GE(lowest, GlobalGain::dbToLinear(GlobalGain::kMinDb));
	EXPECT_LE(highest, GlobalGain::dbToLinear(GlobalGain::kMaxDb));
}

TEST(GlobalGain, SnapToClampsOutOfRangeAndNonFiniteTargets) {
	struct Case {
		const char* name;
		float db;
		float expectedDb;
	} cases[] = {
		{"+200dB snaps to +24", 200.0f, GlobalGain::kMaxDb},
		{"-200dB snaps to -24", -200.0f, GlobalGain::kMinDb},
		{"+inf snaps to +24", std::numeric_limits<float>::infinity(), GlobalGain::kMaxDb},
		{"-inf snaps to -24", -std::numeric_limits<float>::infinity(), GlobalGain::kMinDb},
		{"NaN snaps to -24", std::numeric_limits<float>::quiet_NaN(), GlobalGain::kMinDb},
	};

	for (const Case& testCase : cases) {
		SCOPED_TRACE(testCase.name);

		GlobalGain gain;
		gain.snapTo(testCase.db);

		EXPECT_TRUE(std::isfinite(gain.currentGain()));
		EXPECT_GE(gain.currentGain(), GlobalGain::dbToLinear(GlobalGain::kMinDb));
		EXPECT_LE(gain.currentGain(), GlobalGain::dbToLinear(GlobalGain::kMaxDb));
		EXPECT_EQ(GlobalGain::dbToLinear(testCase.expectedDb), gain.currentGain());
	}
}

TEST(GlobalGain, SnapThenProcessStaysInsideTheClampedRange) {
	// OfflineProcessor::applyConstantGainDb snaps and then processes the very same
	// dB request, so a snap that skipped the clamp would amplify the carrier by the
	// raw request before the ramp dragged it back to the endpoint. These cases pin
	// that composite contract at the helper level.
	const float lowest = GlobalGain::dbToLinear(GlobalGain::kMinDb);
	const float highest = GlobalGain::dbToLinear(GlobalGain::kMaxDb);
	const float slack = 1e-4f;

	struct Case {
		const char* name;
		float snapDb;
		float processDb;
	} cases[] = {
		{"+200dB snap, same target", 200.0f, 200.0f},
		{"+200dB snap, unity target", 200.0f, GlobalGain::kUnityDb},
		{"-200dB snap, same target", -200.0f, -200.0f},
		{"+inf snap, -inf target", std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity()},
		{"NaN snap, NaN target", std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::quiet_NaN()},
	};

	for (const Case& testCase : cases) {
		SCOPED_TRACE(testCase.name);

		GlobalGain gain;
		gain.snapTo(testCase.snapDb);

		ASSERT_TRUE(std::isfinite(gain.currentGain()));
		ASSERT_GE(gain.currentGain(), lowest);
		ASSERT_LE(gain.currentGain(), highest);

		std::vector<float> left = filled(kBlockSize, 1.0f);
		std::vector<float> right = filled(kBlockSize, 1.0f);

		gain.process(left.data(), right.data(), kBlockSize, testCase.processDb);

		expectAllFramesFinite(left);
		expectAllFramesFinite(right);

		for (int32_t i = 0; i < kBlockSize; ++i) {
			ASSERT_GE(left[static_cast<size_t>(i)], lowest - slack) << "left frame " << i;
			ASSERT_LE(left[static_cast<size_t>(i)], highest + slack) << "left frame " << i;
		}

		EXPECT_EQ(left, right);
		EXPECT_EQ(GlobalGain::dbToLinear(testCase.processDb), left.back());
		EXPECT_EQ(GlobalGain::dbToLinear(testCase.processDb), gain.currentGain());
	}
}