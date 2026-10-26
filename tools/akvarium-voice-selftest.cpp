/******************************************************************************
    AkvariumOBS: offline voice-command self-test (Vosk engine).

    Usage:
      akvarium-voice-selftest <model_dir> <audio.wav> "phrase one|phrase two"

    Prints the recognised transcript (UTF-8) and the matched phrase, or "NO MATCH".
    Used to verify the recognition pipeline without a microphone.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "utility/AkvariumPhraseMatch.hpp"
#include "utility/AkvariumVoiceFilter.hpp"
#include "utility/AkvariumVosk.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
	QCoreApplication app(argc, argv);

	if (argc < 4) {
		printf("usage: %s <model_dir> <audio.wav> \"phrase one|phrase two\"\n", argv[0]);
		return 2;
	}

	std::vector<float> audio;
	if (!ReadWav(argv[2], audio)) {
		printf("FAIL: cannot read wav\n");
		return 1;
	}

	const double noiseFloor = akvarium::EstimateNoiseFloor(audio);
	const double gate = std::max(0.0045, noiseFloor * 4.0);
	const akvarium::AudioSegmentStats stats =
		akvarium::AnalyzeAudioSegment(audio.data(), audio.size(), gate);
	printf("STATS: %.2f s, peak %.3f, rms %.4f, active %.0f ms, floor %.4f -> %s\n",
	       (double)audio.size() / 16000.0, stats.peak, stats.rms, stats.activeMs, noiseFloor,
	       akvarium::SegmentLooksLikeSpeech(stats) ? "speech-like" : "REJECTED AS NOISE");

	akvarium::PrepareAudio(audio);

	auto &api = akvarium::VoskApi::Instance();
	if (!api.Load()) {
		printf("FAIL: cannot load libvosk.dll\n");
		return 1;
	}

	akvarium::VoskModel *model = api.model_new(argv[1]);
	if (!model) {
		printf("FAIL: cannot load model from %s\n", argv[1]);
		return 1;
	}

	akvarium::VoskRecognizer *rec = api.recognizer_new(model, 16000.0f);
	if (!rec) {
		printf("FAIL: cannot create recognizer\n");
		api.model_free(model);
		return 1;
	}

	api.recognizer_set_words(rec, 1);

	const auto pcm = akvarium::FloatToPcm16(audio);
	const int chunkSize = 4000;
	const char *byteData = reinterpret_cast<const char *>(pcm.data());
	const int totalBytes = static_cast<int>(pcm.size() * sizeof(int16_t));

	for (int offset = 0; offset < totalBytes; offset += chunkSize) {
		const int bytes = std::min(chunkSize, totalBytes - offset);
		api.recognizer_accept_waveform(rec, byteData + offset, bytes);
	}

	const char *jsonStr = api.recognizer_final_result(rec);
	QString text;
	int tokens = 0;
	double sumP = 0.0;
	double minP = 1.0;

	if (jsonStr && *jsonStr) {
		const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(jsonStr));
		const QJsonObject obj = doc.object();
		text = obj.value(QStringLiteral("text")).toString().trimmed();

		const QJsonArray words = obj.value(QStringLiteral("result")).toArray();
		tokens = words.size();
		for (const auto &wVal : words) {
			const double c = wVal.toObject().value(QStringLiteral("conf")).toDouble(1.0);
			sumP += c;
			minP = std::min(minP, c);
		}
	}

	api.recognizer_free(rec);
	api.model_free(model);

	const double meanP = tokens > 0 ? sumP / (double)tokens : 1.0;
	printf("TRANSCRIPT: %s\n", text.toUtf8().constData());
	printf("CONFIDENCE: avg_p %.2f, min_p %.2f, tokens %d\n", meanP, tokens > 0 ? minP : 1.0, tokens);

	bool rejected = false;
	if (text.isEmpty()) {
		printf("REJECT: empty transcript\n");
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
