/******************************************************************************
    AkvariumOBS: offline voice-command self-test.

    Usage:
      akvarium-voice-selftest <model.bin> <audio.wav> "phrase one|phrase two"

    Prints the recognised transcript (UTF-8) and the matched phrase, or "NO MATCH".
    Used to verify the recognition pipeline without a microphone.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "utility/AkvariumPhraseMatch.hpp"
#include "utility/AkvariumVoiceFilter.hpp"

#include <whisper.h>

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool ReadWav(const char *path, std::vector<float> &out)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return false;

	char header[12] = {};
	if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		fclose(file);
		return false;
	}

	uint16_t channels = 1;
	uint32_t rate = 16000;
	uint16_t bits = 16;
	bool haveFormat = false;

	for (;;) {
		char chunk[8] = {};
		if (fread(chunk, 1, 8, file) != 8)
			break;

		uint32_t size = 0;
		memcpy(&size, chunk + 4, 4);

		if (memcmp(chunk, "fmt ", 4) == 0) {
			std::vector<unsigned char> fmt(size);
			if (fread(fmt.data(), 1, size, file) != size)
				break;
			if (size >= 16) {
				memcpy(&channels, fmt.data() + 2, 2);
				memcpy(&rate, fmt.data() + 4, 4);
				memcpy(&bits, fmt.data() + 14, 2);
				haveFormat = true;
			}
		} else if (memcmp(chunk, "data", 4) == 0) {
			if (!haveFormat || bits != 16)
				break;

			std::vector<int16_t> pcm(size / 2);
			const size_t read = fread(pcm.data(), 2, pcm.size(), file);
			pcm.resize(read);

			std::vector<float> mono;
			mono.reserve(pcm.size() / channels);
			for (size_t i = 0; i + channels <= pcm.size(); i += channels) {
				float sum = 0.0f;
				for (uint16_t c = 0; c < channels; c++)
					sum += pcm[i + c] / 32768.0f;
				mono.push_back(sum / channels);
			}

			if (rate != 16000) {
				const double ratio = 16000.0 / rate;
				const size_t newSize = (size_t)(mono.size() * ratio);
				out.resize(newSize);
				for (size_t i = 0; i < newSize; i++) {
					const double src = i / ratio;
					const size_t i0 = (size_t)src;
					const size_t i1 = (i0 + 1 < mono.size()) ? i0 + 1 : i0;
					const double frac = src - i0;
					out[i] = (float)(mono[i0] * (1.0 - frac) + mono[i1] * frac);
				}
			} else {
				out = std::move(mono);
			}

			fclose(file);
			return !out.empty();
		} else {
			if (fseek(file, (long)size + (size & 1), SEEK_CUR) != 0)
				break;
		}
	}

	fclose(file);
	return false;
}

int main(int argc, char *argv[])
{
	if (argc < 4) {
		printf("usage: %s <model.bin> <audio.wav> \"phrase one|phrase two\" [language]\n", argv[0]);
		return 2;
	}

	std::vector<float> audio;
	if (!ReadWav(argv[2], audio)) {
		printf("FAIL: cannot read wav\n");
		return 1;
	}

	/* Same gates the engine applies before the model runs. */
	const double noiseFloor = akvarium::EstimateNoiseFloor(audio);
	const double gate = std::max(0.0045, noiseFloor * 4.0);
	const akvarium::AudioSegmentStats stats =
		akvarium::AnalyzeAudioSegment(audio.data(), audio.size(), gate);
	printf("STATS: %.2f s, peak %.3f, rms %.4f, active %.0f ms, floor %.4f -> %s\n",
	       (double)audio.size() / 16000.0, stats.peak, stats.rms, stats.activeMs, noiseFloor,
	       akvarium::SegmentLooksLikeSpeech(stats) ? "speech-like" : "REJECTED AS NOISE");

	akvarium::PrepareForWhisper(audio);

	whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = argc > 6 && atoi(argv[6]) != 0;
	printf("USE_GPU: %d\n", (int)cparams.use_gpu);
	whisper_context *ctx = whisper_init_from_file_with_params(argv[1], cparams);
	if (!ctx) {
		printf("FAIL: cannot load model\n");
		return 1;
	}

	const QByteArray language = argc > 4 ? QByteArray(argv[4]) : QByteArray("ru");

	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = 4;
	params.translate = false;
	params.no_context = true;
	params.single_segment = true;
	params.print_special = false;
	params.print_progress = false;
	params.print_realtime = false;
	params.print_timestamps = false;
	params.suppress_blank = true;
	params.suppress_nst = true;
	params.temperature = 0.0f;
	params.max_tokens = 64;
	params.no_timestamps = true;
	if (argc > 5)
		params.audio_ctx = atoi(argv[5]);
	printf("AUDIO_CTX: %d\n", params.audio_ctx);
	params.language = language.constData();

	if (whisper_full(ctx, params, audio.data(), (int)audio.size()) != 0) {
		printf("FAIL: recognition failed\n");
		whisper_free(ctx);
		return 1;
	}

	QString text;
	double sumP = 0.0;
	double minP = 1.0;
	int tokens = 0;
	double noSpeech = 0.0;

	const int segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < segments; i++) {
		noSpeech = std::max(noSpeech, (double)whisper_full_get_segment_no_speech_prob(ctx, i));
		text += QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).trimmed() + QChar(' ');

		const int count = whisper_full_n_tokens(ctx, i);
		for (int t = 0; t < count; t++) {
			const char *tokenText = whisper_full_get_token_text(ctx, i, t);
			if (!tokenText || !*tokenText || (tokenText[0] == '<' && tokenText[1] == '|'))
				continue;
			const double p = (double)whisper_full_get_token_p(ctx, i, t);
			sumP += p;
			minP = std::min(minP, p);
			tokens++;
		}
	}
	text = text.trimmed();
	whisper_free(ctx);

	const double meanP = tokens > 0 ? sumP / (double)tokens : 0.0;

	printf("TRANSCRIPT: %s\n", text.toUtf8().constData());
	printf("CONFIDENCE: no_speech %.2f, avg_p %.2f, min_p %.2f, tokens %d\n", noSpeech, meanP,
	       tokens > 0 ? minP : 0.0, tokens);

	bool rejected = false;
	if (akvarium::IsLikelyHallucination(text)) {
		printf("REJECT: hallucination\n");
		rejected = true;
	}
	if (noSpeech > 0.45) {
		printf("REJECT: no-speech probability %.2f\n", noSpeech);
		rejected = true;
	}
	if (tokens >= 2 && meanP < 0.45) {
		printf("REJECT: low token confidence %.2f\n", meanP);
		rejected = true;
	}

	const QStringList phrases = QString::fromUtf8(argv[3]).split(QChar('|'), Qt::SkipEmptyParts);
	QString bestPhrase;
	double bestScore = 0.0;
	for (const QString &phrase : phrases) {
		const akvarium::PhraseMatchResult result = akvarium::MatchPhrase(phrase, text);
		printf("SCORE '%s': %.2f (extra %d)\n", phrase.toUtf8().constData(), result.score,
		       result.extraWords);
		if (result.score > bestScore) {
			bestScore = result.score;
			bestPhrase = phrase;
		}
	}

	if (rejected)
		return 4;

	if (bestScore >= akvarium::kPhraseMatchThreshold) {
		printf("MATCH: %s\n", bestPhrase.toUtf8().constData());
		return 0;
	}

	printf("NO MATCH\n");
	return 3;
}
