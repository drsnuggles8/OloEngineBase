#include <gtest/gtest.h>
#include "OloEngine/Audio/DSP/Denormals.h"
#include "OloEngine/Audio/DSP/AllpassFilter.h"
#include "OloEngine/Audio/DSP/CombFilter.h"
#include "OloEngine/Audio/DSP/DelayLine.h"
#include "OloEngine/Audio/DSP/ReverbModel.h"
#include <cmath>
#include <vector>
#include <numeric>

using namespace OloEngine::Audio::DSP;

//==============================================================================
/// Denormals Tests
//==============================================================================

TEST(AudioDSP_Denormals, FlushesSubnormalToZero)
{
	// Smallest subnormal float
	float subnormal = 1.0e-45f;
	Undenormalize(subnormal);
	EXPECT_EQ(subnormal, 0.0f);
}

TEST(AudioDSP_Denormals, PreservesNormalFloat)
{
	float normal = 1.0f;
	Undenormalize(normal);
	EXPECT_EQ(normal, 1.0f);

	float small = 1.0e-30f;
	Undenormalize(small);
	EXPECT_EQ(small, 1.0e-30f);
}

TEST(AudioDSP_Denormals, PreservesZero)
{
	float zero = 0.0f;
	Undenormalize(zero);
	EXPECT_EQ(zero, 0.0f);
}

TEST(AudioDSP_Denormals, PreservesNegative)
{
	float neg = -0.5f;
	Undenormalize(neg);
	EXPECT_FLOAT_EQ(neg, -0.5f);
}

//==============================================================================
/// DelayLine Tests
//==============================================================================

TEST(AudioDSP_DelayLine, ConstructsWithValidSize)
{
	DelayLine dl(100);
	EXPECT_EQ(dl.GetDelay(), 0.0f);
}

TEST(AudioDSP_DelayLine, SetAndGetDelay)
{
	DelayLine dl(100);
	dl.SetConfig(1, 44100.0);
	dl.SetDelay(50.0f);
	EXPECT_FLOAT_EQ(dl.GetDelay(), 50.0f);
}

TEST(AudioDSP_DelayLine, PushPopProducesDelayedSample)
{
	constexpr int delaySamples = 5;
	DelayLine dl(delaySamples + 2);
	dl.SetConfig(1, 44100.0);
	dl.SetDelay(static_cast<float>(delaySamples));

	// Push a sequence of samples: 0, 1, 2, 3, ...
	// First 'delaySamples' outputs should be 0 (buffer was cleared)
	for (int i = 0; i < delaySamples; ++i)
	{
		dl.PushSample(0, static_cast<float>(i + 1));
		float out = dl.PopSample(0);
		EXPECT_FLOAT_EQ(out, 0.0f) << "Sample at index " << i << " should be zero (delay not reached)";
	}

	// After delay, we should get the earlier samples back
	dl.PushSample(0, 100.0f);
	float delayed = dl.PopSample(0);
	EXPECT_FLOAT_EQ(delayed, 1.0f);
}

TEST(AudioDSP_DelayLine, MultiChannelIndependence)
{
	DelayLine dl(10);
	dl.SetConfig(2, 44100.0);
	dl.SetDelay(1.0f);

	dl.PushSample(0, 1.0f);
	dl.PushSample(1, 2.0f);

	// First pop returns 0 (initial state)
	float ch0 = dl.PopSample(0);
	float ch1 = dl.PopSample(1);
	EXPECT_FLOAT_EQ(ch0, 0.0f);
	EXPECT_FLOAT_EQ(ch1, 0.0f);

	dl.PushSample(0, 10.0f);
	dl.PushSample(1, 20.0f);

	ch0 = dl.PopSample(0);
	ch1 = dl.PopSample(1);
	EXPECT_FLOAT_EQ(ch0, 1.0f);
	EXPECT_FLOAT_EQ(ch1, 2.0f);
}

TEST(AudioDSP_DelayLine, ResetClearsBuffer)
{
	DelayLine dl(10);
	dl.SetConfig(1, 44100.0);
	dl.SetDelay(1.0f);

	dl.PushSample(0, 42.0f);
	dl.Reset();

	dl.PushSample(0, 0.0f);
	float out = dl.PopSample(0);
	EXPECT_FLOAT_EQ(out, 0.0f);
}

//==============================================================================
/// AllpassFilter Tests
//==============================================================================

TEST(AudioDSP_Allpass, SetFeedback)
{
	AllpassFilter ap;
	ap.SetFeedback(0.5f);
	EXPECT_FLOAT_EQ(ap.GetFeedback(), 0.5f);
}

TEST(AudioDSP_Allpass, ProcessOutputsDifferentFromInput)
{
	AllpassFilter ap;
	std::vector<float> buf(32, 0.0f);
	ap.SetBuffer(buf);
	ap.SetFeedback(0.5f);

	float out = ap.Process(1.0f);
	// First sample: buf[0] was 0, so output = -1.0 + 0 = -1.0
	EXPECT_FLOAT_EQ(out, -1.0f);

	// After processing, buf[0] should be 1.0 + 0*0.5 = 1.0
	// Next process with 0 input: buf[1] = 0, output = -0+0 = 0
	// but buf stored at index 0 was 1.0, so next cycle through buffer
	// will reflect it
}

TEST(AudioDSP_Allpass, AllpassPreservesEnergy)
{
	// An allpass filter should roughly preserve signal energy over many samples
	AllpassFilter ap;
	std::vector<float> buf(64, 0.0f);
	ap.SetBuffer(buf);
	ap.SetFeedback(0.5f);

	double inputEnergy = 0.0;
	double outputEnergy = 0.0;
	constexpr int numSamples = 1000;

	for (int i = 0; i < numSamples; ++i)
	{
		float input = (i < 100) ? 1.0f : 0.0f; // impulse burst
		inputEnergy += static_cast<double>(input * input);
		float output = ap.Process(input);
		outputEnergy += static_cast<double>(output * output);
	}

	// Energy should be roughly preserved (within tolerance — allpass spreads energy over time,
	// so a finite observation window may capture more or less)
	double ratio = outputEnergy / inputEnergy;
	EXPECT_GT(ratio, 0.5);
	EXPECT_LT(ratio, 3.0);
}

TEST(AudioDSP_Allpass, MuteClearsBuffer)
{
	AllpassFilter ap;
	std::vector<float> buf(16, 0.0f);
	ap.SetBuffer(buf);
	ap.SetFeedback(0.5f);

	// Process some samples to populate buffer
	for (int i = 0; i < 16; ++i)
	{
		ap.Process(1.0f);
	}

	ap.Mute();

	// After muting, buffer should be all zeros
	for (float v : buf)
	{
		EXPECT_FLOAT_EQ(v, 0.0f);
	}
}

//==============================================================================
/// CombFilter Tests
//==============================================================================

TEST(AudioDSP_Comb, SetDampAndFeedback)
{
	CombFilter comb;
	comb.SetDamp(0.4f);
	comb.SetFeedback(0.7f);
	EXPECT_FLOAT_EQ(comb.GetDamp(), 0.4f);
	EXPECT_FLOAT_EQ(comb.GetFeedback(), 0.7f);
}

TEST(AudioDSP_Comb, ProcessProducesOutput)
{
	CombFilter comb;
	std::vector<float> buf(32, 0.0f);
	comb.SetBuffer(buf);
	comb.SetFeedback(0.7f);
	comb.SetDamp(0.2f);

	// First output should be from buffer (zero initially)
	float out = comb.Process(1.0f);
	EXPECT_FLOAT_EQ(out, 0.0f);

	// Process more and we should eventually see non-zero output
	// (after wrapping around the buffer)
	for (int i = 0; i < 31; ++i)
	{
		comb.Process(0.0f);
	}

	// Buffer index has wrapped, should read back the stored value
	float delayed = comb.Process(0.0f);
	EXPECT_NE(delayed, 0.0f);
}

TEST(AudioDSP_Comb, CombDecays)
{
	CombFilter comb;
	std::vector<float> buf(16, 0.0f);
	comb.SetBuffer(buf);
	comb.SetDamp(0.3f);
	comb.SetFeedback(0.5f);

	// Send a single impulse
	comb.Process(1.0f);
	for (int i = 0; i < 15; ++i)
	{
		comb.Process(0.0f);
	}

	// First echo
	float first = std::abs(comb.Process(0.0f));
	EXPECT_GT(first, 0.0f);

	// Skip another buffer cycle
	for (int i = 0; i < 15; ++i)
	{
		comb.Process(0.0f);
	}

	// Second echo should be smaller due to feedback < 1
	float second = std::abs(comb.Process(0.0f));
	EXPECT_LT(second, first);
}

TEST(AudioDSP_Comb, MuteClearsBuffer)
{
	CombFilter comb;
	std::vector<float> buf(16, 0.0f);
	comb.SetBuffer(buf);
	comb.SetFeedback(0.8f);
	comb.SetDamp(0.1f);

	for (int i = 0; i < 32; ++i)
	{
		comb.Process(1.0f);
	}

	comb.Mute();

	for (float v : buf)
	{
		EXPECT_FLOAT_EQ(v, 0.0f);
	}
}

//==============================================================================
/// ReverbModel Tests
//==============================================================================

TEST(AudioDSP_Reverb, ConstructsWithSampleRate)
{
	ReverbModel reverb(44100.0);
	// Should not throw, default values set
	EXPECT_GE(reverb.GetRoomSize(), 0.0f);
	EXPECT_LE(reverb.GetRoomSize(), 1.0f);
}

TEST(AudioDSP_Reverb, SettersAndGetters)
{
	ReverbModel reverb(44100.0);

	reverb.SetRoomSize(0.8f);
	EXPECT_NEAR(reverb.GetRoomSize(), 0.8f, 0.01f);

	reverb.SetDamp(0.5f);
	EXPECT_NEAR(reverb.GetDamp(), 0.5f, 0.01f);

	reverb.SetWet(0.3f);
	EXPECT_NEAR(reverb.GetWet(), 0.3f, 0.01f);

	reverb.SetDry(0.7f);
	EXPECT_NEAR(reverb.GetDry(), 0.7f, 0.01f);

	reverb.SetWidth(1.0f);
	EXPECT_NEAR(reverb.GetWidth(), 1.0f, 0.01f);
}

TEST(AudioDSP_Reverb, ProcessReplaceSilenceProducesSilence)
{
	ReverbModel reverb(44100.0);
	reverb.SetWet(1.0f);
	reverb.SetDry(0.0f);

	constexpr int numSamples = 512;
	std::vector<float> inL(numSamples, 0.0f);
	std::vector<float> inR(numSamples, 0.0f);
	std::vector<float> outL(numSamples, 999.0f);
	std::vector<float> outR(numSamples, 999.0f);

	reverb.ProcessReplace(inL.data(), inR.data(), outL.data(), outR.data(), numSamples, 1);

	// With zero input the output should remain essentially 0
	for (int i = 0; i < numSamples; ++i)
	{
		EXPECT_NEAR(outL[i], 0.0f, 1e-6f) << "outL[" << i << "]";
		EXPECT_NEAR(outR[i], 0.0f, 1e-6f) << "outR[" << i << "]";
	}
}

TEST(AudioDSP_Reverb, ProcessReplaceImpulseProducesReverbTail)
{
	ReverbModel reverb(44100.0);
	reverb.SetRoomSize(0.8f);
	reverb.SetWet(1.0f);
	reverb.SetDry(0.0f);
	reverb.SetDamp(0.5f);
	reverb.SetWidth(1.0f);

	constexpr int numSamples = 4096;
	std::vector<float> inL(numSamples, 0.0f);
	std::vector<float> inR(numSamples, 0.0f);
	std::vector<float> outL(numSamples, 0.0f);
	std::vector<float> outR(numSamples, 0.0f);

	// Single impulse
	inL[0] = 1.0f;
	inR[0] = 1.0f;

	reverb.ProcessReplace(inL.data(), inR.data(), outL.data(), outR.data(), numSamples, 1);

	// There should be non-zero output well past sample 0 (reverb tail)
	float tailEnergy = 0.0f;
	for (int i = numSamples / 2; i < numSamples; ++i)
	{
		tailEnergy += outL[i] * outL[i] + outR[i] * outR[i];
	}
	EXPECT_GT(tailEnergy, 0.0f) << "Reverb should produce a decaying tail";
}

TEST(AudioDSP_Reverb, MuteSilencesOutput)
{
	ReverbModel reverb(44100.0);
	reverb.SetRoomSize(0.9f);
	reverb.SetWet(1.0f);
	reverb.SetDry(0.0f);

	// Feed some signal
	constexpr int blockSize = 512;
	std::vector<float> inL(blockSize, 0.5f);
	std::vector<float> inR(blockSize, 0.5f);
	std::vector<float> outL(blockSize, 0.0f);
	std::vector<float> outR(blockSize, 0.0f);

	reverb.ProcessReplace(inL.data(), inR.data(), outL.data(), outR.data(), blockSize, 1);

	// Mute the reverb
	reverb.Mute();

	// Process silence — output should now be near-zero
	std::fill(inL.begin(), inL.end(), 0.0f);
	std::fill(inR.begin(), inR.end(), 0.0f);
	reverb.ProcessReplace(inL.data(), inR.data(), outL.data(), outR.data(), blockSize, 1);

	float totalEnergy = 0.0f;
	for (int i = 0; i < blockSize; ++i)
	{
		totalEnergy += outL[i] * outL[i] + outR[i] * outR[i];
	}
	EXPECT_NEAR(totalEnergy, 0.0f, 1e-6f);
}

TEST(AudioDSP_Reverb, DifferentSampleRates)
{
	// Should construct and operate at different sample rates without crashing
	ReverbModel reverb48k(48000.0);
	ReverbModel reverb96k(96000.0);

	// Must use enough samples for the impulse to travel through comb filter buffers (>1200 samples)
	constexpr int numSamples = 8192;
	std::vector<float> inL(numSamples, 0.0f);
	std::vector<float> inR(numSamples, 0.0f);
	std::vector<float> outL(numSamples, 0.0f);
	std::vector<float> outR(numSamples, 0.0f);
	inL[0] = 1.0f;
	inR[0] = 1.0f;

	reverb48k.ProcessReplace(inL.data(), inR.data(), outL.data(), outR.data(), numSamples, 1);

	float energy48k = 0.0f;
	for (int i = 0; i < numSamples; ++i)
	{
		energy48k += outL[i] * outL[i] + outR[i] * outR[i];
	}

	std::fill(outL.begin(), outL.end(), 0.0f);
	std::fill(outR.begin(), outR.end(), 0.0f);

	reverb96k.ProcessReplace(inL.data(), inR.data(), outL.data(), outR.data(), numSamples, 1);

	float energy96k = 0.0f;
	for (int i = 0; i < numSamples; ++i)
	{
		energy96k += outL[i] * outL[i] + outR[i] * outR[i];
	}

	// Both should produce output; exact values differ but both should be non-zero
	EXPECT_GT(energy48k, 0.0f);
	EXPECT_GT(energy96k, 0.0f);
}

TEST(AudioDSP_Reverb, FreezeMode)
{
	ReverbModel reverb(44100.0);
	reverb.SetMode(1.0f); // Freeze
	EXPECT_GE(reverb.GetMode(), 0.5f);

	reverb.SetMode(0.0f); // Normal
	EXPECT_LT(reverb.GetMode(), 0.5f);
}
