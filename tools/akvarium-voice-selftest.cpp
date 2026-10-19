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

#include <whisper.h>

#include <QString>
#include <QStringList>

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
		printf("usage: %s <model.bin> <audio.wav> \"phrase one|phrase two\"\n", argv[0]);
		return 2;
	}

	std::vector<float> audio;
	if (!ReadWav(argv[2], audio)) {
		printf("FAIL: cannot read wav\n");
		return 1;
	}

	whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = false;
	whisper_context *ctx = whisper_init_from_file_with_params(argv[1], cparams);
	if (!ctx) {
		printf("FAIL: cannot load model\n");
		return 1;
	}

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
	params.temperature = 0.0f;
	params.max_tokens = 64;
	params.language = "auto";

	if (whisper_full(ctx, params, audio.data(), (int)audio.size()) != 0) {
		printf("FAIL: recognition failed\n");
		whisper_free(ctx);
		return 1;
	}

	QString text;
	const int segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < segments; i++) {
		const char *segmentText = whisper_full_get_segment_text(ctx, i);
		if (segmentText)
			text += QString::fromUtf8(segmentText).trimmed() + QChar(' ');
	}
	text = text.trimmed();
	whisper_free(ctx);

	printf("TRANSCRIPT: %s\n", text.toUtf8().constData());

	const QStringList phrases = QString::fromUtf8(argv[3]).split(QChar('|'), Qt::SkipEmptyParts);
	for (const QString &phrase : phrases) {
		if (akvarium::PhraseMatches(phrase, text)) {
			printf("MATCH: %s\n", phrase.toUtf8().constData());
			return 0;
		}
	}

	printf("NO MATCH\n");
	return 3;
}
