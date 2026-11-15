#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <random>
#include <type_traits>
#include <vector>

#include "amp_controls.h"

#if defined(GRC_HAVE_NAM_PLUGIN_HEADER)
#include "nam_plugin.h"
#endif

#if defined(GRC_NAM_PLUGIN_LIBRARY)
#include <dlfcn.h>

#include <map>
#include <string>

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#endif

namespace {

constexpr float kSampleRate = 48000.0f;
constexpr uint32_t kBlockSize = 64;
// A 12 dB band that lands 0.5 dB off still passes a +-0.5 dB tolerance, so the
// tolerance is what pins the smoothing's settle threshold: 0.25 dB fails as
// soon as a band stops within 0.5 dB of its target and calls it done.
constexpr double kToleranceDb = 0.25;
// 128 blocks at 64 frames: well past the 41 blocks the 25 %/block smoothing
// needs for a full 12 dB step, so every measurement starts in steady state.
constexpr size_t kSettleSamples = 8192;

using BandGains = std::array<float, nam_dsp::AmpToneStack::kBandCount>;

size_t measureSamples(float sampleRate) {
	return static_cast<size_t>(sampleRate);
}

std::vector<float> sine(double frequencyHz, size_t count, float sampleRate = kSampleRate, float amplitude = 0.25f) {
	std::vector<float> buffer(count);
	const double w = 2.0 * M_PI * frequencyHz / static_cast<double>(sampleRate);

	for (size_t i = 0; i < count; ++i)
		buffer[i] = amplitude * static_cast<float>(std::sin(w * static_cast<double>(i)));

	return buffer;
}

std::vector<float> noise(size_t count, uint32_t seed = 4242) {
	std::mt19937 rng(seed);
	std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);

	std::vector<float> buffer(count);

	for (size_t i = 0; i < count; ++i)
		buffer[i] = distribution(rng);

	return buffer;
}

void processBlocks(nam_dsp::AmpToneStack& stack, std::vector<float>& buffer) {
	for (size_t offset = 0; offset < buffer.size(); offset += kBlockSize) {
		const uint32_t count = static_cast<uint32_t>(std::min<size_t>(kBlockSize, buffer.size() - offset));

		stack.process(buffer.data() + offset, count);
	}
}

double projectMagnitude(const std::vector<float>& buffer, double frequencyHz, float sampleRate) {
	double real = 0.0;
	double imaginary = 0.0;
	const double w = 2.0 * M_PI * frequencyHz / static_cast<double>(sampleRate);

	for (size_t i = 0; i < buffer.size(); ++i) {
		real += buffer[i] * std::cos(w * static_cast<double>(i));
		imaginary -= buffer[i] * std::sin(w * static_cast<double>(i));
	}

	return std::hypot(real, imaginary) / static_cast<double>(buffer.size());
}

// Probe where a single band reaches its full target gain: deep in the flat end
// of a shelf, and at the centre of the peaking band.
double bandProbeHz(int band) {
	switch (band) {
		case nam_dsp::AmpToneStack::kBass:
			return 25.0;
		case nam_dsp::AmpToneStack::kTreble:
		case nam_dsp::AmpToneStack::kPresence:
			return 16000.0;
		default:
			return 750.0;
	}
}

BandGains singleBand(int band, float gainDb) {
	BandGains gains = {};
	gains[band] = gainDb;
	return gains;
}

void applyTargets(nam_dsp::AmpToneStack& stack, const BandGains& gains) {
	stack.setTargets(gains[0], gains[1], gains[2], gains[3]);
}

// Steady-state gain of the cascade at one frequency: a sine is pushed through
// the stack so the smoothing settles, then the magnitude of the next second is
// projected at the probe frequency. Both projections see the same leakage (the
// window rarely holds an integer number of cycles), so the ratio of the two is
// accurate to a few hundredths of a dB.
double measureResponseDb(nam_dsp::AmpToneStack& stack, float sampleRate, double probeHz) {
	const size_t measure = measureSamples(sampleRate);
	const std::vector<float> full = sine(probeHz, kSettleSamples + measure, sampleRate);

	std::vector<float> settle(full.begin(), full.begin() + kSettleSamples);
	processBlocks(stack, settle);

	const std::vector<float> reference(full.begin() + kSettleSamples, full.end());
	std::vector<float> measured = reference;
	processBlocks(stack, measured);

	return 20.0 * std::log10(projectMagnitude(measured, probeHz, sampleRate) /
							  projectMagnitude(reference, probeHz, sampleRate));
}

double responseDbAt(float sampleRate, const BandGains& gains, double probeHz) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(sampleRate);
	applyTargets(stack, gains);

	const double measured = measureResponseDb(stack, sampleRate, probeHz);
	EXPECT_TRUE(stack.isSettled());

	return measured;
}

double responseDb(float bassDb, float midDb, float trebleDb, float presenceDb, double probeHz) {
	return responseDbAt(kSampleRate, {bassDb, midDb, trebleDb, presenceDb}, probeHz);
}

// Frequency inside [low, high] where the band has travelled half of its target
// gain. Located by bisection on the measured response instead of assumed, so a
// band that does not follow the sample rate cannot pass: coefficients designed
// for 48 kHz read a 280 Hz corner on a 44.1 kHz device, not 305 Hz.
double halfGainCrossingHz(float sampleRate, int band, float gainDb, double low, double high) {
	const BandGains gains = singleBand(band, gainDb);

	auto error = [&](double hz) { return responseDbAt(sampleRate, gains, hz) - 0.5 * gainDb; };

	double errorLow = error(low);

	EXPECT_LT(errorLow * error(high), 0.0) << "half-gain crossing of band " << band << " is not bracketed";

	for (int iteration = 0; iteration < 14; ++iteration) {
		const double middle = 0.5 * (low + high);
		const double errorMiddle = error(middle);

		if ((errorMiddle > 0.0) == (errorLow > 0.0)) {
			low = middle;
			errorLow = errorMiddle;
		}
		else {
			high = middle;
		}
	}

	return 0.5 * (low + high);
}

// Corner of a shelf: the half-gain crossing, which is where the RBJ shelf puts
// its design frequency.
double shelfCornerHz(float sampleRate, int band, float gainDb) {
	const double nominal = nam_dsp::AmpToneStack::voicing(band).frequencyHz;

	return halfGainCrossingHz(sampleRate, band, gainDb, nominal * 0.5, nominal * 2.0);
}

// Centre of the peaking band: the response is symmetric in log frequency, so
// the geometric mean of the two half-gain crossings is the centre. Reading the
// peak amplitude alone is useless here - the peak is broad enough that a centre
// 9 % off still looks like a full 12 dB.
double peakCentreHz(float sampleRate, int band, float gainDb) {
	const double nominal = nam_dsp::AmpToneStack::voicing(band).frequencyHz;
	const double below = halfGainCrossingHz(sampleRate, band, gainDb, nominal * 0.25, nominal);
	const double above = halfGainCrossingHz(sampleRate, band, gainDb, nominal, nominal * 4.0);

	return std::sqrt(below * above);
}

} // namespace

TEST(AmpToneStack, DefaultsAreABitExactPassthrough) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);

	std::vector<float> buffer = noise(8192);
	const std::vector<float> expected = buffer;

	processBlocks(stack, buffer);

	EXPECT_EQ(0, std::memcmp(expected.data(), buffer.data(), buffer.size() * sizeof(float)));
	EXPECT_EQ(0.0f, stack.maxAbsState());
	EXPECT_TRUE(stack.isSettled());
}

TEST(AmpToneStack, ZeroDbTargetsAreABitExactPassthroughFromAnyState) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);
	stack.setTargets(12.0f, -12.0f, 12.0f, 12.0f);

	std::vector<float> warmUp = noise(4096, 7);
	processBlocks(stack, warmUp);
	ASSERT_GT(stack.maxAbsState(), 0.0f);

	stack.setTargets(0.0f, 0.0f, 0.0f, 0.0f);

	for (int block = 0; block < 2000 && !stack.isSettled(); ++block)
		stack.processSilence(kBlockSize);
	stack.processSilence(8 * kBlockSize);

	ASSERT_TRUE(stack.isSettled());
	ASSERT_EQ(0.0f, stack.maxAbsState());

	std::vector<float> buffer = noise(4096, 11);
	const std::vector<float> expected = buffer;

	processBlocks(stack, buffer);

	EXPECT_EQ(0, std::memcmp(expected.data(), buffer.data(), buffer.size() * sizeof(float)));
}

TEST(AmpToneStack, EachBandReachesItsTargetInItsBand) {
	// The probe is where the band reaches its full gain: the corner for the
	// peaking band, the flat end of the shelf for the others (see
	// ShelfCornerSitsAtHalfTheTargetGain for the response at the corner).
	struct Case {
		const char* name;
		float bassDb;
		float midDb;
		float trebleDb;
		float presenceDb;
		double probeHz;
		double expectedDb;
	} cases[] = {
		{"bass+12@25Hz", 12.0f, 0.0f, 0.0f, 0.0f, 25.0, 12.0},
		{"bass-12@25Hz", -12.0f, 0.0f, 0.0f, 0.0f, 25.0, -12.0},
		{"mid+12@750Hz", 0.0f, 12.0f, 0.0f, 0.0f, 750.0, 12.0},
		{"mid-12@750Hz", 0.0f, -12.0f, 0.0f, 0.0f, 750.0, -12.0},
		{"treble+12@16kHz", 0.0f, 0.0f, 12.0f, 0.0f, 16000.0, 12.0},
		{"treble-12@16kHz", 0.0f, 0.0f, -12.0f, 0.0f, 16000.0, -12.0},
		{"presence+12@16kHz", 0.0f, 0.0f, 0.0f, 12.0f, 16000.0, 12.0},
		{"presence-12@16kHz", 0.0f, 0.0f, 0.0f, -12.0f, 16000.0, -12.0},
	};

	for (const Case& testCase : cases) {
		SCOPED_TRACE(testCase.name);

		const double gainDb = responseDb(testCase.bassDb, testCase.midDb, testCase.trebleDb,
			testCase.presenceDb, testCase.probeHz);

		EXPECT_NEAR(gainDb, testCase.expectedDb, kToleranceDb);
	}
}

TEST(AmpToneStack, ShelfCornerSitsAtHalfTheTargetGain) {
	EXPECT_NEAR(responseDb(12.0f, 0.0f, 0.0f, 0.0f, 305.0), 6.0, kToleranceDb);
	EXPECT_NEAR(responseDb(-12.0f, 0.0f, 0.0f, 0.0f, 305.0), -6.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 0.0f, 12.0f, 0.0f, 2000.0), 6.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 0.0f, 0.0f, 12.0f, 900.0), 6.0, kToleranceDb);
}

TEST(AmpToneStack, ShelvesOnlyMoveTheirOwnEndOfTheSpectrum) {
	EXPECT_NEAR(responseDb(12.0f, 0.0f, 0.0f, 0.0f, 8000.0), 0.0, kToleranceDb);
	EXPECT_NEAR(responseDb(-12.0f, 0.0f, 0.0f, 0.0f, 8000.0), 0.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 0.0f, 12.0f, 0.0f, 60.0), 0.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 0.0f, -12.0f, 0.0f, 60.0), 0.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 0.0f, 0.0f, 12.0f, 60.0), 0.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 0.0f, 0.0f, -12.0f, 60.0), 0.0, kToleranceDb);
	EXPECT_NEAR(responseDb(0.0f, 12.0f, 0.0f, 0.0f, 10000.0), 0.0, kToleranceDb);
}

TEST(AmpToneStack, TenSecondsOfAudioStayFiniteAndBounded) {
	constexpr size_t kSamples = 480000;
	constexpr size_t kBlocks = kSamples / kBlockSize;

	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);

	std::mt19937 rng(99);
	std::uniform_real_distribution<float> signal(-0.5f, 0.5f);
	std::uniform_real_distribution<float> targets(-12.0f, 12.0f);

	unsigned int nonFinite = 0;
	float peak = 0.0f;
	float peakState = 0.0f;

	for (size_t block = 0; block < kBlocks; ++block) {
		if (block % 47 == 0)
			stack.setTargets(targets(rng), targets(rng), targets(rng), targets(rng));

		float samples[kBlockSize];

		for (uint32_t i = 0; i < kBlockSize; ++i)
			samples[i] = signal(rng);

		stack.process(samples, kBlockSize);

		for (uint32_t i = 0; i < kBlockSize; ++i) {
			if (!std::isfinite(samples[i]))
				nonFinite++;
			peak = std::max(peak, std::fabs(samples[i]));
		}

		peakState = std::max(peakState, stack.maxAbsState());
	}

	EXPECT_EQ(0u, nonFinite);
	EXPECT_TRUE(std::isfinite(peak));
	EXPECT_TRUE(std::isfinite(stack.maxAbsState()));
	// +12 dB on all four bands overlaps to roughly +25 dB, i.e. a gain of 18
	// over the 0.5 amplitude input: anything beyond that is a runaway filter.
	EXPECT_LT(peak, 18.0f);
	EXPECT_LT(peakState, 40.0f);
}

TEST(AmpToneStack, SilenceDecaysTheFilterStateToZero) {
	constexpr size_t kSamples = 48000;

	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);
	stack.setTargets(12.0f, 12.0f, 12.0f, 12.0f);

	std::vector<float> buffer = noise(kSamples);
	processBlocks(stack, buffer);
	ASSERT_GT(stack.maxAbsState(), 1e-6f);

	std::vector<float> silence(kSamples, 0.0f);
	processBlocks(stack, silence);

	// The ring-down is over well before the end of the second...
	for (size_t i = silence.size() - 1024; i < silence.size(); ++i)
		EXPECT_LT(std::fabs(silence[i]), 1e-12f);

	// ...and the state is flushed to exactly zero (not parked in denormals),
	ASSERT_EQ(0.0f, stack.maxAbsState());

	// so the next block is an exact digital silence.
	std::vector<float> afterFlush(kBlockSize, 0.0f);
	processBlocks(stack, afterFlush);

	for (float sample : afterFlush)
		EXPECT_EQ(0.0f, sample);
}

TEST(AmpToneStack, TargetStepConvergesWellWithinOneHundredMilliseconds) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);

	std::vector<float> buffer = noise(kBlockSize);
	processBlocks(stack, buffer);
	ASSERT_TRUE(stack.isSettled());

	stack.setTargets(12.0f, -12.0f, 12.0f, -12.0f);

	size_t blocks = 0;

	while (!stack.isSettled() && blocks < 10000) {
		stack.processSilence(kBlockSize);
		++blocks;
	}

	ASSERT_TRUE(stack.isSettled());
	const double elapsedMs = static_cast<double>(blocks * kBlockSize) * 1000.0 / kSampleRate;
	EXPECT_LT(elapsedMs, 100.0);
	EXPECT_NEAR(stack.gainDb(nam_dsp::AmpToneStack::kBass), 12.0f, 1e-5);
}

TEST(AmpToneStack, TargetStepsAreSmoothedRatherThanSnapped) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);
	ASSERT_TRUE(stack.isSettled());

	stack.setTargets(12.0f, -12.0f, 12.0f, -12.0f);
	stack.processSilence(kBlockSize);

	// One block must land strictly between the old and the new value: the
	// filter coefficients are recomputed while the audio is running, so jumping
	// to the target in one block is what clicks.
	for (int band = 0; band < nam_dsp::AmpToneStack::kBandCount; ++band) {
		SCOPED_TRACE(band);

		const float stepped = stack.gainDb(static_cast<nam_dsp::AmpToneStack::Band>(band));
		const float target = stack.targetDb(static_cast<nam_dsp::AmpToneStack::Band>(band));

		EXPECT_TRUE(std::isfinite(stepped));
		EXPECT_GT(std::fabs(stepped), 0.0f);
		EXPECT_LT(std::fabs(stepped), std::fabs(target));
		EXPECT_NEAR(stepped, 0.25f * target, 1e-5);
	}

	EXPECT_FALSE(stack.isSettled());
}

TEST(AmpToneStack, EveryBlockClosesAtMostAQuarterOfTheRemainingDistance) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);

	const BandGains target = {12.0f, -12.0f, 6.0f, -3.0f};

	stack.setTargets(target[0], target[1], target[2], target[3]);

	// Not read from the header on purpose: the small terminal snap that lands on
	// the target is legitimate, anything coarser (a settle threshold of 0.5 dB
	// would jump from 0.38 dB away) is the click this test exists to catch.
	constexpr float kTerminalSnapDb = 1e-4f;

	float largestFraction = 0.0f;
	float largestLateStep = 0.0f;
	int blocks = 0;

	while (!stack.isSettled() && blocks < 10000) {
		std::array<float, nam_dsp::AmpToneStack::kBandCount> before;

		for (int band = 0; band < nam_dsp::AmpToneStack::kBandCount; ++band)
			before[band] = stack.gainDb(static_cast<nam_dsp::AmpToneStack::Band>(band));

		stack.processSilence(kBlockSize);

		for (int band = 0; band < nam_dsp::AmpToneStack::kBandCount; ++band) {
			const auto which = static_cast<nam_dsp::AmpToneStack::Band>(band);
			const float remaining = target[band] - before[band];
			const float step = stack.gainDb(which) - before[band];

			if (std::fabs(remaining) > kTerminalSnapDb) {
				largestFraction = std::max(largestFraction, std::fabs(step / remaining));
				EXPECT_LE(std::fabs(step), 0.26f * std::fabs(remaining)) << "band " << band << " block " << blocks;
			}

			if (std::fabs(remaining) < 1.0f)
				largestLateStep = std::max(largestLateStep, std::fabs(step));
		}

		++blocks;
	}

	// The last dB of a step must still be approached in 0.25 dB fractions, which
	// is what keeps the coefficient change per block inaudible.
	EXPECT_LE(largestLateStep, 0.26f);

	ASSERT_TRUE(stack.isSettled());
	EXPECT_LE(largestFraction, 0.26f);
}

TEST(AmpToneStack, SettledParametersKeepTheResponseIdentical) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);
	stack.setTargets(6.0f, -3.0f, 9.0f, -6.0f);

	for (int block = 0; block < 1000 && !stack.isSettled(); ++block) {
		std::vector<float> warmUp = noise(kBlockSize, static_cast<uint32_t>(block));
		processBlocks(stack, warmUp);
	}
	ASSERT_TRUE(stack.isSettled());

	// In steady state the coefficients are not recomputed, so the response is a
	// pure function of the input and the (reset) filter state.
	const std::vector<float> buffer = noise(kBlockSize);

	stack.reset();
	std::vector<float> firstRun = buffer;
	processBlocks(stack, firstRun);

	stack.reset();
	std::vector<float> secondRun = buffer;
	processBlocks(stack, secondRun);

	EXPECT_EQ(firstRun, secondRun);
	EXPECT_NE(firstRun, buffer);
	EXPECT_TRUE(stack.isSettled());
	EXPECT_FLOAT_EQ(6.0f, stack.gainDb(nam_dsp::AmpToneStack::kBass));
	EXPECT_FLOAT_EQ(-3.0f, stack.gainDb(nam_dsp::AmpToneStack::kMid));
	EXPECT_FLOAT_EQ(9.0f, stack.gainDb(nam_dsp::AmpToneStack::kTreble));
	EXPECT_FLOAT_EQ(-6.0f, stack.gainDb(nam_dsp::AmpToneStack::kPresence));
}

TEST(AmpToneStack, DenormalScaleInputNeverParksInTheState) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);
	stack.setTargets(12.0f, 0.0f, 12.0f, 0.0f);

	std::vector<float> buffer(kBlockSize * 16, 1e-30f);
	processBlocks(stack, buffer);

	EXPECT_EQ(0.0f, stack.maxAbsState());
}

TEST(AmpToneStack, EmptyBuffersAreIgnored) {
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(kSampleRate);

	float sample = 1.0f;
	stack.process(&sample, 0);
	stack.process(nullptr, 32);
	stack.processSilence(0);

	EXPECT_EQ(1.0f, sample);
	EXPECT_EQ(0.0f, stack.maxAbsState());
}

TEST(AmpToneStack, CornersAndCentresFollowTheSampleRate) {
	// The voicings are physical frequencies: 305 Hz bass corner, 750 Hz mid
	// centre, 2 kHz treble corner, 900 Hz presence corner at every rate. A
	// setSampleRate() that ignores its argument leaves the coefficients designed
	// for 48 kHz, which puts the "305 Hz" corner at 280 Hz on a 44.1 kHz device -
	// every EQ band 8.8 % out with no audible test covering it.
	for (float sampleRate : {44100.0f, 96000.0f}) {
		SCOPED_TRACE(sampleRate);

		const double bassCorner = shelfCornerHz(sampleRate, nam_dsp::AmpToneStack::kBass, 12.0f);
		EXPECT_NEAR(bassCorner, 305.0, 305.0 * 0.01);

		const double trebleCorner = shelfCornerHz(sampleRate, nam_dsp::AmpToneStack::kTreble, 12.0f);
		EXPECT_NEAR(trebleCorner, 2000.0, 2000.0 * 0.01);

		const double presenceCorner = shelfCornerHz(sampleRate, nam_dsp::AmpToneStack::kPresence, 12.0f);
		EXPECT_NEAR(presenceCorner, 900.0, 900.0 * 0.01);

		const double midCentre = peakCentreHz(sampleRate, nam_dsp::AmpToneStack::kMid, 12.0f);
		EXPECT_NEAR(midCentre, 750.0, 750.0 * 0.01);
	}
}

TEST(AmpToneStack, CornerGainsAreUnchangedAtEveryRealRate) {
	// The Nyquist clamp must not move any voicing at a rate a device can use:
	// 0.45 * 8000 Hz is still well above the highest band (2 kHz).
	struct Case {
		int band;
		double cornerHz;
	} cases[] = {
		{nam_dsp::AmpToneStack::kBass, 305.0},
		{nam_dsp::AmpToneStack::kTreble, 2000.0},
		{nam_dsp::AmpToneStack::kPresence, 900.0},
	};

	for (float sampleRate : {8000.0f, 22050.0f, 44100.0f, 48000.0f, 96000.0f}) {
		SCOPED_TRACE(sampleRate);

		for (const Case& testCase : cases) {
			EXPECT_NEAR(responseDbAt(sampleRate, singleBand(testCase.band, 12.0f), testCase.cornerHz),
				6.0, kToleranceDb);
			EXPECT_NEAR(responseDbAt(sampleRate, singleBand(testCase.band, -12.0f), testCase.cornerHz),
				-6.0, kToleranceDb);
		}

		EXPECT_NEAR(responseDbAt(sampleRate, singleBand(nam_dsp::AmpToneStack::kMid, 12.0f), 750.0),
			12.0, kToleranceDb);
	}
}

TEST(AmpToneStack, BandsAreKeptBelowNyquistSoLowRatesStayStable) {
	// 3 kHz puts the 2 kHz treble corner above Nyquist, where the RBJ formulas
	// produce a pole outside the unit circle (radius 1.72 for +12 dB): the
	// clamp moves it to 0.45 * 3000 Hz and the filter stays stable.
	nam_dsp::AmpToneStack stack;
	stack.setSampleRate(3000.0f);
	stack.setTargets(12.0f, 12.0f, 12.0f, 12.0f);

	std::vector<float> buffer = noise(kBlockSize * 512);

	processBlocks(stack, buffer);

	float peak = 0.0f;
	unsigned int nonFinite = 0;

	for (float sample : buffer) {
		if (!std::isfinite(sample))
			nonFinite++;
		peak = std::max(peak, std::fabs(sample));
	}

	EXPECT_EQ(0u, nonFinite);
	EXPECT_TRUE(std::isfinite(peak));
	EXPECT_LT(peak, 30.0f);
	EXPECT_LT(stack.maxAbsState(), 40.0f);
}

TEST(AmpToneStack, NonFiniteControlsFallBackToUnityAndCannotPoisonTheState) {
	// A corrupt or hand-edited preset can hand the plugin NaN/Inf. A NaN that
	// reaches the RBJ formulas leaves every coefficient - and the whole filter
	// state - NaN, and the audio never recovers.
	const float badValues[] = {std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};

	for (int band = 0; band < nam_dsp::AmpToneStack::kBandCount; ++band) {
		for (float badValue : badValues) {
			SCOPED_TRACE(band);

			nam_dsp::AmpToneStack stack;
			stack.setSampleRate(kSampleRate);

			const BandGains healthy = singleBand(band, 12.0f);
			applyTargets(stack, healthy);

			std::vector<float> warmUp = noise(4096, 5);
			processBlocks(stack, warmUp);
			ASSERT_GT(stack.maxAbsState(), 1e-6f);

			BandGains poisoned = healthy;
			poisoned[band] = badValue;
			applyTargets(stack, poisoned);

			std::vector<float> block = noise(kBlockSize, 13);
			stack.process(block.data(), kBlockSize);

			for (float sample : block) {
				EXPECT_TRUE(std::isfinite(sample));
				EXPECT_LT(std::fabs(sample), 10.0f);
			}

			EXPECT_TRUE(std::isfinite(stack.maxAbsState()));
			EXPECT_TRUE(std::isfinite(stack.gainDb(static_cast<nam_dsp::AmpToneStack::Band>(band))));
			EXPECT_EQ(0.0f, stack.targetDb(static_cast<nam_dsp::AmpToneStack::Band>(band)));

			// Back to a sane setting: the band must reach its steady state again,
			// which is impossible while a poisoned coefficient or state survives.
			applyTargets(stack, healthy);

			EXPECT_NEAR(measureResponseDb(stack, kSampleRate, bandProbeHz(band)), 12.0, kToleranceDb);
		}
	}
}

TEST(AmpToneStack, NonFiniteInputSamplesDoNotStickInTheState) {
	// Garbage in has to stay one sample of garbage out: a NaN or Inf that the
	// filter state latches would silence the plugin for the rest of the session.
	for (float poison : {std::numeric_limits<float>::quiet_NaN(),
			 std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
		SCOPED_TRACE(poison);

		nam_dsp::AmpToneStack stack;
		stack.setSampleRate(kSampleRate);
		stack.setTargets(12.0f, 12.0f, 12.0f, 12.0f);

		std::vector<float> warmUp = noise(4096, 21);
		processBlocks(stack, warmUp);

		std::vector<float> poisoned = noise(kBlockSize, 23);
		poisoned[7] = poison;
		stack.process(poisoned.data(), kBlockSize);

		std::vector<float> after = sine(305.0, kBlockSize * 4);
		processBlocks(stack, after);

		for (float sample : after)
			EXPECT_TRUE(std::isfinite(sample));

		EXPECT_TRUE(std::isfinite(stack.maxAbsState()));
	}
}

TEST(AmpToneStack, AccumulatorOverflowDoesNotStickInTheState) {
	// A hot sample can overflow the biquad accumulator into +-Inf before the
	// output does. An Inf state is not cleaned up by the denormal flush (its
	// magnitude is huge, not tiny), so without an explicit check the band stays
	// infinite - and silent - for the rest of the session.
	for (int band = 0; band < nam_dsp::AmpToneStack::kBandCount; ++band) {
		SCOPED_TRACE(band);

		nam_dsp::AmpToneStack stack;
		stack.setSampleRate(kSampleRate);
		applyTargets(stack, singleBand(band, 12.0f));

		std::vector<float> warmUp(4096, 0.25f);
		processBlocks(stack, warmUp);

		std::vector<float> hot(kBlockSize, 0.25f);
		hot[7] = 3.0e38f;
		stack.process(hot.data(), kBlockSize);

		std::vector<float> after(kBlockSize * 8, 0.25f);
		processBlocks(stack, after);

		unsigned int nonFinite = 0;

		for (float sample : after) {
			if (!std::isfinite(sample))
				nonFinite++;
			EXPECT_LT(std::fabs(sample), 20.0f);
		}

		EXPECT_EQ(0u, nonFinite);
		EXPECT_TRUE(std::isfinite(stack.maxAbsState()));
	}
}

TEST(AmpBiquad, NonFiniteGainAndSampleRateAreSanitized) {
	nam_dsp::Biquad identity;
	identity.set(nam_dsp::AmpToneStack::voicing(nam_dsp::AmpToneStack::kBass), kSampleRate,
		std::numeric_limits<float>::quiet_NaN());

	const std::vector<float> buffer = noise(1024, 31);
	std::vector<float> throughIdentity = buffer;

	for (float& sample : throughIdentity)
		sample = identity.process(sample);

	EXPECT_EQ(buffer, throughIdentity);

	nam_dsp::Biquad defaultRate;
	nam_dsp::Biquad explicitRate;
	defaultRate.set(nam_dsp::AmpToneStack::voicing(nam_dsp::AmpToneStack::kBass),
		std::numeric_limits<float>::quiet_NaN(), 12.0f);
	explicitRate.set(nam_dsp::AmpToneStack::voicing(nam_dsp::AmpToneStack::kBass), kSampleRate, 12.0f);

	std::vector<float> fromDefault = buffer;
	std::vector<float> fromExplicit = buffer;

	for (size_t i = 0; i < buffer.size(); ++i) {
		fromDefault[i] = defaultRate.process(fromDefault[i]);
		fromExplicit[i] = explicitRate.process(fromExplicit[i]);
	}

	EXPECT_EQ(fromExplicit, fromDefault);
	EXPECT_TRUE(std::isfinite(defaultRate.maxAbsState()));
}

TEST(AmpToneStack, NonFiniteSampleRateFallsBackToTheDefault) {
	for (float badRate : {std::numeric_limits<float>::quiet_NaN(),
			 std::numeric_limits<float>::infinity(), -1.0f, 0.0f}) {
		SCOPED_TRACE(badRate);

		nam_dsp::AmpToneStack stack;
		stack.setSampleRate(badRate);

		EXPECT_FLOAT_EQ(nam_dsp::AmpToneStack::kDefaultSampleRate, stack.sampleRate());
		EXPECT_NEAR(responseDbAt(stack.sampleRate(), singleBand(nam_dsp::AmpToneStack::kBass, 12.0f), 305.0),
			6.0, kToleranceDb);
	}
}

#if defined(GRC_HAVE_NAM_PLUGIN_HEADER)

TEST(NamPluginPorts, PortIndicesMatchThePublishedTable) {
	using Ports = NAM::Plugin::Ports;

	static_assert(std::is_standard_layout<Ports>::value, "connect_port relies on a flat, standard layout struct");
	static_assert(offsetof(Ports, control) == 0 * sizeof(void*), "port 0 must stay control");
	static_assert(offsetof(Ports, notify) == 1 * sizeof(void*), "port 1 must stay notify");
	static_assert(offsetof(Ports, audio_in) == 2 * sizeof(void*), "port 2 must stay audio_in");
	static_assert(offsetof(Ports, audio_out) == 3 * sizeof(void*), "port 3 must stay audio_out");
	static_assert(offsetof(Ports, input_level) == 4 * sizeof(void*), "port 4 must stay input_level");
	static_assert(offsetof(Ports, output_level) == 5 * sizeof(void*), "port 5 must stay output_level");
	static_assert(offsetof(Ports, quality_scale) == 6 * sizeof(void*), "port 6 must stay quality_scale");
	static_assert(offsetof(Ports, gain) == 7 * sizeof(void*), "port 7 must stay gain");
	static_assert(offsetof(Ports, bass) == 8 * sizeof(void*), "port 8 must stay bass");
	static_assert(offsetof(Ports, mid) == 9 * sizeof(void*), "port 9 must stay mid");
	static_assert(offsetof(Ports, treble) == 10 * sizeof(void*), "port 10 must stay treble");
	static_assert(offsetof(Ports, presence) == 11 * sizeof(void*), "port 11 must stay presence");
	static_assert(sizeof(Ports) == 12 * sizeof(void*), "no padding: connect_port indexes the struct by port number");

	// Appending is what keeps hosts' presets valid: a port that moves changes
	// the meaning of every stored index.
	static_assert(std::is_same<decltype(Ports::gain), float*>::value, "tone ports are audio-rate float controls");
	static_assert(std::is_same<decltype(Ports::presence), float*>::value, "tone ports are audio-rate float controls");

	SUCCEED();
}

#endif

#if defined(GRC_NAM_PLUGIN_LIBRARY)

namespace {

// Drives the built plugin through its exported LV2 descriptor: the integration
// layer (port -> tone stack -> gain staging) is where a regression would hide
// from the header-only tests above. No model is loaded, so `Plugin::process`
// takes its no-model path, which still runs the tone cascade, the gain fold and
// the output ramp - exactly the code the amp controls added.
class NamPlugin {
public:
	~NamPlugin() {
		close();
	}

	bool open(double sampleRate) {
		library_ = dlopen(GRC_NAM_PLUGIN_LIBRARY, RTLD_NOW | RTLD_LOCAL);
		if (library_ == nullptr)
			return false;

		const auto entry = reinterpret_cast<const LV2_Descriptor* (*)(uint32_t)>(dlsym(library_, "lv2_descriptor"));
		if (entry == nullptr)
			return false;

		descriptor_ = entry(0);
		if (descriptor_ == nullptr)
			return false;

		const LV2_Feature* features[] = {&mapFeature_, &scheduleFeature_, nullptr};
		instance_ = descriptor_->instantiate(descriptor_, sampleRate, "/tmp/grc-nam-host-tests.lv2", features);
		if (instance_ == nullptr)
			return false;

		controlStorage_.assign(sizeof(LV2_Atom) + sizeof(LV2_Atom_Sequence_Body) + 64, 0);
		notifyStorage_.assign(sizeof(LV2_Atom) + 4096, 0);
		control_ = reinterpret_cast<LV2_Atom_Sequence*>(controlStorage_.data());
		control_->atom.type = mapUrid(LV2_ATOM__Sequence);
		control_->atom.size = sizeof(LV2_Atom_Sequence_Body);
		notify_ = reinterpret_cast<LV2_Atom_Sequence*>(notifyStorage_.data());
		notify_->atom.type = mapUrid(LV2_ATOM__Sequence);
		notify_->atom.size = 4096;

		input_.assign(kBlockSize, 0.0f);
		output_.assign(kBlockSize, 0.0f);

		descriptor_->connect_port(instance_, 0, control_);
		descriptor_->connect_port(instance_, 1, notify_);
		descriptor_->connect_port(instance_, 2, input_.data());
		descriptor_->connect_port(instance_, 3, output_.data());
		descriptor_->connect_port(instance_, 4, &inputLevel_);
		descriptor_->connect_port(instance_, 5, &outputLevel_);
		descriptor_->connect_port(instance_, 6, &qualityScale_);
		descriptor_->connect_port(instance_, 7, &gain_);
		descriptor_->connect_port(instance_, 8, &bass_);
		descriptor_->connect_port(instance_, 9, &mid_);
		descriptor_->connect_port(instance_, 10, &treble_);
		descriptor_->connect_port(instance_, 11, &presence_);
		descriptor_->activate(instance_);

		return true;
	}

	std::vector<float> render(const std::vector<float>& input) {
		std::vector<float> result(input.size());

		for (size_t offset = 0; offset < input.size(); offset += kBlockSize) {
			const uint32_t count = static_cast<uint32_t>(std::min<size_t>(kBlockSize, input.size() - offset));

			std::copy(input.begin() + static_cast<long>(offset),
				input.begin() + static_cast<long>(offset + count), input_.begin());

			descriptor_->run(instance_, count);

			std::copy(output_.begin(), output_.begin() + count, result.begin() + static_cast<long>(offset));
		}

		return result;
	}

	float& gain() {
		return gain_;
	}

	float& bass() {
		return bass_;
	}

	float& outputLevel() {
		return outputLevel_;
	}

	void close() {
		if (instance_ != nullptr) {
			descriptor_->deactivate(instance_);
			descriptor_->cleanup(instance_);
			instance_ = nullptr;
		}

		if (library_ != nullptr) {
			dlclose(library_);
			library_ = nullptr;
		}
	}

private:
	static LV2_URID mapUrid(const char* uri) {
		static std::map<std::string, LV2_URID> table;
		static LV2_URID next = 1;

		auto found = table.find(uri);

		if (found != table.end())
			return found->second;

		const LV2_URID id = next++;
		table.emplace(uri, id);

		return id;
	}

	static LV2_URID mapCallback(LV2_URID_Map_Handle, const char* uri) {
		return mapUrid(uri);
	}

	static LV2_Worker_Status scheduleCallback(LV2_Worker_Schedule_Handle, uint32_t, const void*) {
		return LV2_WORKER_SUCCESS;
	}

	void* library_ = nullptr;
	const LV2_Descriptor* descriptor_ = nullptr;
	LV2_Handle instance_ = nullptr;
	LV2_URID_Map map_ = {nullptr, mapCallback};
	LV2_Worker_Schedule schedule_ = {nullptr, scheduleCallback};
	LV2_Feature mapFeature_ = {LV2_URID__map, &map_};
	LV2_Feature scheduleFeature_ = {LV2_WORKER__schedule, &schedule_};
	std::vector<uint8_t> controlStorage_;
	std::vector<uint8_t> notifyStorage_;
	LV2_Atom_Sequence* control_ = nullptr;
	LV2_Atom_Sequence* notify_ = nullptr;
	std::vector<float> input_;
	std::vector<float> output_;
	float inputLevel_ = 0.0f;
	float outputLevel_ = 0.0f;
	float qualityScale_ = 1.0f;
	float gain_ = 0.0f;
	float bass_ = 0.0f;
	float mid_ = 0.0f;
	float treble_ = 0.0f;
	float presence_ = 0.0f;
};

double pluginResponseDb(double sampleRate, float bassDb, double probeHz) {
	NamPlugin plugin;

	if (!plugin.open(sampleRate)) {
		ADD_FAILURE() << "cannot load " << GRC_NAM_PLUGIN_LIBRARY;

		return std::numeric_limits<double>::quiet_NaN();
	}

	plugin.bass() = bassDb;

	const size_t measure = measureSamples(static_cast<float>(sampleRate));
	const std::vector<float> reference = sine(probeHz, kSettleSamples + measure, static_cast<float>(sampleRate));
	const std::vector<float> measured = plugin.render(reference);

	return 20.0 * std::log10(projectMagnitude(std::vector<float>(measured.begin() + static_cast<long>(kSettleSamples),
											measured.end()),
								   probeHz, static_cast<float>(sampleRate)) /
							  projectMagnitude(std::vector<float>(reference.begin() + static_cast<long>(kSettleSamples),
													reference.end()),
								  probeHz, static_cast<float>(sampleRate)));
}

} // namespace

TEST(NamPlugin, RunsTheToneStackOnTheRateItWasInstantiatedAt) {
	NamPlugin probe;

	// GRC_NAM_PLUGIN_LIBRARY is only defined once CMake has verified the built
	// plugin exists, so a load failure here is a real defect and not an
	// unavailable dependency. A GTEST_SKIP() would hide it: ctest exits 0 when a
	// test skips itself, so the test step stays green and only the JUnit
	// assertion downstream would notice.
	if (!probe.open(48000.0)) {
		ADD_FAILURE() << "cannot load " << GRC_NAM_PLUGIN_LIBRARY;

		return;
	}

	probe.close();

	for (double sampleRate : {48000.0, 44100.0, 96000.0}) {
		SCOPED_TRACE(sampleRate);

		// The plugin's no-model path is a passthrough apart from the tone stack,
		// so a missing toneStack.process() reads 0 dB here and a setSampleRate()
		// that never saw the instantiate rate reads 5.1 dB at 44.1 kHz.
		EXPECT_NEAR(pluginResponseDb(sampleRate, 12.0f, 305.0), 6.0, kToleranceDb);
		EXPECT_NEAR(pluginResponseDb(sampleRate, 12.0f, 25.0), 12.0, kToleranceDb);
	}
}

TEST(NamPlugin, FoldsAmpGainIntoTheInputLevel) {
	NamPlugin plugin;

	// See the load-failure note above.
	if (!plugin.open(48000.0)) {
		ADD_FAILURE() << "cannot load " << GRC_NAM_PLUGIN_LIBRARY;

		return;
	}

	constexpr double kProbeHz = 1000.0;
	const size_t measure = measureSamples(kSampleRate);

	auto measuredDb = [&]() {
		const std::vector<float> reference = sine(kProbeHz, kSettleSamples + measure);
		const std::vector<float> measured = plugin.render(reference);

		return 20.0 * std::log10(projectMagnitude(std::vector<float>(
												measured.begin() + static_cast<long>(kSettleSamples), measured.end()),
										  kProbeHz, kSampleRate) /
								 projectMagnitude(std::vector<float>(
													 reference.begin() + static_cast<long>(kSettleSamples), reference.end()),
									 kProbeHz, kSampleRate));
	};

	EXPECT_NEAR(measuredDb(), 0.0, kToleranceDb) << "defaults must stay a passthrough";

	plugin.gain() = 12.0f;

	EXPECT_NEAR(measuredDb(), 12.0, kToleranceDb);
}

TEST(NamPlugin, OutputLevelRampsPerSampleInsteadOfSteppingPerBlock) {
	NamPlugin plugin;

	// See the load-failure note above.
	if (!plugin.open(48000.0)) {
		ADD_FAILURE() << "cannot load " << GRC_NAM_PLUGIN_LIBRARY;

		return;
	}

	// From a fresh instance the output smoother climbs from 0 to -12 dB. Using
	// the end-of-block level for the whole block stairs the gain in 47 % jumps
	// (the first block is entirely silent); the ramped level moves it by at most
	// ~1 % of the remaining distance per sample.
	plugin.outputLevel() = -12.0f;

	const std::vector<float> measured = plugin.render(std::vector<float>(kBlockSize * 8, 1.0f));

	float largestStep = 0.0f;

	for (size_t i = 1; i < measured.size(); ++i)
		largestStep = std::max(largestStep, std::fabs(measured[i] - measured[i - 1]));

	EXPECT_TRUE(std::isfinite(largestStep));
	EXPECT_LT(largestStep, 0.05f);
	EXPECT_GT(measured.back(), 0.2f);
	EXPECT_LT(measured.back(), 0.26f);
}

TEST(NamPlugin, NonFiniteControlValuesDoNotReachTheGainStages) {
	NamPlugin plugin;

	// See the load-failure note above.
	if (!plugin.open(48000.0)) {
		ADD_FAILURE() << "cannot load " << GRC_NAM_PLUGIN_LIBRARY;

		return;
	}

	// A NaN amp gain used to be folded straight into powf(): desiredInputLevel
	// goes NaN, the input smoother latches it and the plugin is silent-with-NaN
	// for the rest of the session.
	plugin.gain() = std::numeric_limits<float>::quiet_NaN();
	plugin.bass() = std::numeric_limits<float>::infinity();

	const std::vector<float> measured = plugin.render(std::vector<float>(kBlockSize * 64, 0.5f));

	for (float sample : measured)
		EXPECT_TRUE(std::isfinite(sample));

	EXPECT_NEAR(std::fabs(measured.back()), 0.5f, 0.01f);
}

#endif

int main(int argc, char** argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}