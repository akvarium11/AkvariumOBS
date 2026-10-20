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

/* Removes DC / sub-60Hz rumble and applies clean automatic gain control
   so headset microphones deliver a healthy signal level to Whisper. */
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

	/* Boost quiet microphone input up to +18 dB (target peak ~0.65) */
	if (peak > 1e-4f && peak < 0.20f) {
		const float gain = std::min(8.0f, 0.65f / peak);
		for (float &sample : pcm)
			sample *= gain;
	}
}

/* Rejects pure silence, electrical clicks, and empty buffers.
   Does NOT reject natural speech based on arbitrary ZCR or duration limits. */
inline bool SegmentLooksLikeSpeech(const AudioSegmentStats &stats)
{
	if (stats.frames < 10) /* minimum 200 ms */
		return false;
	if (stats.peak < 0.003) /* pure silence */
		return false;
	if (stats.rms < 0.0008)
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
