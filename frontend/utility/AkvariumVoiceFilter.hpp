/******************************************************************************
    AkvariumOBS: audio filters and speech validation for voice commands.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace akvarium {

struct AudioSegmentStats {
	double rms = 0.0;        /* whole segment */
	double peak = 0.0;
	double activeMs = 0.0;   /* time above the speech gate */
	int frames = 0;
};

/* 20 ms frames (320 samples at 16 kHz) */
inline AudioSegmentStats AnalyzeAudioSegment(const float *data, size_t count, double gate)
{
	AudioSegmentStats stats;
	if (!data || count == 0)
		return stats;

	const size_t frameSize = 320;
	const size_t frames = count / frameSize;
	stats.frames = (int)frames;
	if (frames == 0)
		return stats;

	double globalEnergy = 0.0;
	size_t activeFrames = 0;

	for (size_t f = 0; f < frames; f++) {
		double frameEnergy = 0.0;
		for (size_t i = 0; i < frameSize; i++) {
			const double v = data[f * frameSize + i];
			frameEnergy += v * v;
			if (std::fabs(v) > stats.peak)
				stats.peak = std::fabs(v);
		}
		const double frameRms = std::sqrt(frameEnergy / (double)frameSize);
		globalEnergy += frameEnergy;

		if (frameRms > gate)
			activeFrames++;
	}

	stats.rms = std::sqrt(globalEnergy / (double)(frames * frameSize));
	stats.activeMs = (double)activeFrames * 20.0;
	return stats;
}

/* Removes DC / sub-60Hz rumble and applies controlled automatic gain control
   ONLY to genuine speech, never amplifying quiet ambient background. */
inline void PrepareForWhisper(std::vector<float> &pcm)
{
	if (pcm.empty())
		return;

	/* High-pass filter ~60 Hz */
	const double a = 0.9767; /* exp(-2 * pi * 60 / 16000) */
	float previousIn = pcm[0];
	float previousOut = 0.0f;
	for (float &sample : pcm) {
		const float in = sample;
		previousOut = (float)(a * (previousOut + in - previousIn));
		previousIn = in;
		sample = previousOut;
	}

	float peak = 0.0f;
	for (float sample : pcm)
		peak = std::max(peak, std::fabs(sample));

	/* Only apply moderate boost if signal is genuine speech (peak >= 0.030).
	   Never boost sub-threshold noise or background bleed. Max gain 2.5x (+8 dB). */
	if (peak >= 0.030f && peak < 0.25f) {
		const float gain = std::min(2.5f, 0.50f / peak);
		for (float &sample : pcm)
			sample *= gain;
	}
}

/* Rejects non-speech audio: pure silence, electrical clicks, breaths, keyboard clatter,
   and quiet background noise across the room. */
inline bool SegmentLooksLikeSpeech(const AudioSegmentStats &stats, int sensitivity = 3)
{
	/* Speech command duration bounds:
	   Minimum ~360 ms (18 frames at 20 ms), maximum 3.5 s (175 frames).
	   A click, pop or breath is < 200 ms; long conversation is > 3.5 s. */
	if (stats.frames < 18 || stats.frames > 175)
		return false;

	/* Active speech energy bounds based on sensitivity */
	double minPeak = 0.030;
	double minRms = 0.005;
	double minActiveMs = 180.0;

	switch (sensitivity) {
	case 1: /* Low (loud room / reject background voice & bleed) */
		minPeak = 0.050;
		minRms = 0.010;
		minActiveMs = 220.0;
		break;
	case 2: /* Medium-Low */
		minPeak = 0.038;
		minRms = 0.007;
		minActiveMs = 200.0;
		break;
	case 4: /* Medium-High */
		minPeak = 0.020;
		minRms = 0.0035;
		minActiveMs = 160.0;
		break;
	case 5: /* High (quiet voice) */
		minPeak = 0.012;
		minRms = 0.0020;
		minActiveMs = 140.0;
		break;
	case 3: /* Normal / recommended */
	default:
		minPeak = 0.030;
		minRms = 0.005;
		minActiveMs = 180.0;
		break;
	}

	if (stats.peak < minPeak)
		return false;
	if (stats.rms < minRms)
		return false;
	if (stats.activeMs < minActiveMs)
		return false;

	/* Active speech must make up at least 20% of the buffer (not a single spike in silence) */
	const double totalMs = (double)stats.frames * 20.0;
	if (totalMs > 0.0 && (stats.activeMs / totalMs) < 0.20)
		return false;

	return true;
}

/* Fast stationary noise floor estimator: returns the 15th percentile frame energy. */
inline double EstimateNoiseFloor(const std::vector<float> &pcm)
{
	const size_t frameSize = 320;
	const size_t frames = pcm.size() / frameSize;
	if (frames == 0)
		return 0.0;

	std::vector<double> values;
	values.reserve(frames);
	for (size_t f = 0; f < frames; f++) {
		double energy = 0.0;
		for (size_t i = 0; i < frameSize; i++) {
			const double v = pcm[f * frameSize + i];
			energy += v * v;
		}
		values.push_back(std::sqrt(energy / (double)frameSize));
	}

	std::sort(values.begin(), values.end());
	const size_t index = std::min(values.size() - 1, (size_t)(values.size() * 0.15));
	return values[index];
}

} // namespace akvarium
