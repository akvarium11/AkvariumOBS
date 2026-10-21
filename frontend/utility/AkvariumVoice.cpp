/******************************************************************************
    AkvariumOBS: voice commands implementation (whisper.cpp + WASAPI).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumVoice.hpp"

#include "AkvariumPhraseMatch.hpp"
#include "AkvariumVoiceFilter.hpp"

#include <OBSApp.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QStringList>
#include <QThread>

#include <curl/curl.h>
#include <whisper.h>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace akvarium {

static const char *kConfigSection = "AkvariumVoice";

/* ------------------------------------------------------------------------- */
/* Audio helpers                                                             */
/* ------------------------------------------------------------------------- */

static void AppendResampled16k(const float *data, size_t frames, int channels, int sampleRate,
			       double &phase, float &lastSample, std::vector<float> &out)
{
	if (channels < 1)
		channels = 1;

	std::vector<float> mono(frames);
	for (size_t i = 0; i < frames; i++) {
		float sum = 0.0f;
		for (int c = 0; c < channels; c++)
			sum += data[i * (size_t)channels + c];
		mono[i] = sum / channels;
	}

	if (sampleRate == 16000) {
		out.insert(out.end(), mono.begin(), mono.end());
		return;
	}

	const double ratio = 16000.0 / (double)sampleRate;
	const size_t outCount = (size_t)((mono.size() + phase) * ratio);
	const size_t start = out.size();
	out.resize(start + outCount);

	for (size_t i = 0; i < outCount; i++) {
		const double pos = (double)i / ratio - phase;
		const size_t i0 = (size_t)pos;
		const double frac = pos - (double)i0;
		const float a = i0 < mono.size() ? mono[i0] : lastSample;
		const float b = (i0 + 1) < mono.size() ? mono[i0 + 1] : a;
		out[start + i] = (float)(a * (1.0 - frac) + b * frac);
	}

	phase = (double)outCount / ratio - (double)mono.size() + phase;
	if (!mono.empty())
		lastSample = mono.back();
}

static DecodeConfidence MeasureDecode(whisper_context *ctx)
{
	DecodeConfidence confidence;
	double sum = 0.0;
	double minProb = 1.0;
	int count = 0;
	double noSpeech = 0.0;

	const int segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < segments; i++) {
		noSpeech = std::max(noSpeech, (double)whisper_full_get_segment_no_speech_prob(ctx, i));

		const int tokens = whisper_full_n_tokens(ctx, i);
		for (int t = 0; t < tokens; t++) {
			const char *text = whisper_full_get_token_text(ctx, i, t);
			if (!text || !*text || (text[0] == '<' && text[1] == '|'))
				continue;

			const double p = (double)whisper_full_get_token_p(ctx, i, t);
			sum += p;
			count++;
			minProb = std::min(minProb, p);
		}
	}

	confidence.tokens = count;
	confidence.meanTokenProb = count > 0 ? sum / (double)count : 0.0;
	confidence.minTokenProb = count > 0 ? minProb : 0.0;
	confidence.noSpeechProb = noSpeech;
	return confidence;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

VoiceConfig VoiceEngine::LoadConfig()
{
	VoiceConfig cfg;
	config_t *conf = App()->GetUserConfig();

	cfg.enabled = config_get_bool(conf, kConfigSection, "Enabled");
	cfg.micSource = QString::fromUtf8(config_get_string(conf, kConfigSection, "MicSource"));
	cfg.threads = (int)config_get_int(conf, kConfigSection, "Threads");
	if (cfg.threads <= 0 || cfg.threads > 8)
		cfg.threads = 0;

	cfg.sensitivity = (int)config_get_int(conf, kConfigSection, "Sensitivity");
	if (cfg.sensitivity < 1 || cfg.sensitivity > 5)
		cfg.sensitivity = 3;

	cfg.onlyWithReplayBuffer = config_get_bool(conf, kConfigSection, "OnlyWithReplayBuffer");
	cfg.model = AutoModel();

	const char *phrases = config_get_string(conf, kConfigSection, "Phrases");
	if (phrases && *phrases) {
		obs_data_t *root = obs_data_create_from_json(phrases);
		obs_data_array_t *arr = root ? obs_data_get_array(root, "phrases") : nullptr;
		if (arr) {
			const size_t count = obs_data_array_count(arr);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(arr, i);
				const char *text = obs_data_get_string(item, "text");
				if (text && *text) {
					const QString p = QString::fromUtf8(text).trimmed();
					if (!p.isEmpty() && !cfg.phrases.contains(p))
						cfg.phrases << p;
				}
				obs_data_release(item);
			}
			obs_data_array_release(arr);
		}
		if (root)
			obs_data_release(root);
	}

	if (cfg.phrases.isEmpty())
		cfg.phrases << QStringLiteral("save replay") << QStringLiteral("clip that");

	const char *aliases = config_get_string(conf, kConfigSection, "Aliases");
	if (aliases && *aliases) {
		obs_data_t *root = obs_data_create_from_json(aliases);
		obs_data_array_t *arr = root ? obs_data_get_array(root, "aliases") : nullptr;
		if (arr) {
			const size_t count = obs_data_array_count(arr);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(arr, i);
				const QString p = QString::fromUtf8(obs_data_get_string(item, "p"));
				const QString t = QString::fromUtf8(obs_data_get_string(item, "t"));
				if (!p.isEmpty() && !t.isEmpty())
					cfg.aliases.append(qMakePair(p, t));
				obs_data_release(item);
			}
			obs_data_array_release(arr);
		}
		if (root)
			obs_data_release(root);
	}

	const char *lang = config_get_string(conf, kConfigSection, "Language");
	if (lang && *lang)
		cfg.language = QString::fromUtf8(lang);
	else
		cfg.language = QStringLiteral("auto");

	return cfg;
}

void VoiceEngine::SaveConfig(const VoiceConfig &cfg)
{
	config_t *conf = App()->GetUserConfig();

	config_set_bool(conf, kConfigSection, "Enabled", cfg.enabled);
	config_set_string(conf, kConfigSection, "Model", cfg.model.toUtf8().constData());
	config_set_string(conf, kConfigSection, "MicSource", cfg.micSource.toUtf8().constData());
	config_set_int(conf, kConfigSection, "Threads", cfg.threads);
	config_set_int(conf, kConfigSection, "Sensitivity", cfg.sensitivity);
	config_set_bool(conf, kConfigSection, "OnlyWithReplayBuffer", cfg.onlyWithReplayBuffer);
	config_set_string(conf, kConfigSection, "Language", cfg.language.toUtf8().constData());

	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const QString &phrase : cfg.phrases) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "text", phrase.toUtf8().constData());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "phrases", arr);
	obs_data_array_release(arr);

	const char *json = obs_data_get_json(root);
	config_set_string(conf, kConfigSection, "Phrases", json ? json : "{}");
	obs_data_release(root);

	obs_data_t *aliasRoot = obs_data_create();
	obs_data_array_t *aliasArr = obs_data_array_create();
	for (const auto &alias : cfg.aliases) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "p", alias.first.toUtf8().constData());
		obs_data_set_string(item, "t", alias.second.toUtf8().constData());
		obs_data_array_push_back(aliasArr, item);
		obs_data_release(item);
	}
	obs_data_set_array(aliasRoot, "aliases", aliasArr);
	obs_data_array_release(aliasArr);

	const char *aliasJson = obs_data_get_json(aliasRoot);
	config_set_string(conf, kConfigSection, "Aliases", aliasJson ? aliasJson : "{}");
	obs_data_release(aliasRoot);

	config_save_safe(conf, "tmp", nullptr);
}

/* ------------------------------------------------------------------------- */
/* Model paths & resolution                                                  */
/* ------------------------------------------------------------------------- */

QString VoiceEngine::ModelPath(const QString &model)
{
	const QString fileName = QStringLiteral("ggml-%1.bin").arg(model);

	char path[512];
	const QString relative = QStringLiteral("AkvariumOBS/voice/%1").arg(fileName);
	if (GetAppConfigPath(path, sizeof(path), relative.toUtf8().constData()) > 0) {
		const QString userPath = QString::fromUtf8(path);
		if (os_file_exists(userPath.toUtf8().constData()))
			return userPath;
	}

	const QString appDir = QCoreApplication::applicationDirPath();
	for (const QString &candidate :
	     {appDir + QStringLiteral("/../../data/voice/") + fileName,
	      appDir + QStringLiteral("/../../../data/voice/") + fileName,
	      appDir + QStringLiteral("/data/voice/") + fileName}) {
		if (os_file_exists(candidate.toUtf8().constData()))
			return candidate;
	}

	if (GetAppConfigPath(path, sizeof(path), relative.toUtf8().constData()) > 0)
		return QString::fromUtf8(path);

	return QString();
}

QString VoiceEngine::ModelUrl(const QString &model)
{
	return QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%1.bin").arg(model);
}

bool VoiceEngine::ModelReady(const QString &model)
{
	const QString path = ModelPath(model);
	return !path.isEmpty() && os_file_exists(path.toUtf8().constData());
}

QString VoiceEngine::AutoModel()
{
	/* base-q8_0 is ~82 MB, fast and accurate for command words */
	if (ModelReady(QStringLiteral("base-q8_0")))
		return QStringLiteral("base-q8_0");
	if (ModelReady(QStringLiteral("base")))
		return QStringLiteral("base");
	if (ModelReady(QStringLiteral("tiny")))
		return QStringLiteral("tiny");
	return QStringLiteral("base-q8_0");
}

QString VoiceEngine::BuildPrompt() const
{
	QStringList words;
	for (const QString &p : m_config.phrases) {
		const QString t = p.trimmed();
		if (!t.isEmpty())
			words << t;
	}
	for (const auto &alias : m_config.aliases) {
		const QString t = alias.second.trimmed();
		if (!t.isEmpty())
			words << t;
	}
	words.removeDuplicates();
	if (words.isEmpty())
		return QString();
	return words.join(QStringLiteral(", "));
}

QString VoiceEngine::AutoLanguage(const QStringList &phrases)
{
	int cyrillic = 0;
	int latin = 0;
	for (const QString &phrase : phrases) {
		for (QChar ch : phrase) {
			const ushort c = ch.unicode();
			if (c >= 0x0400 && c <= 0x04FF)
				cyrillic++;
			else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
				latin++;
		}
	}
	return (cyrillic >= latin) ? QStringLiteral("ru") : QStringLiteral("en");
}

/* ------------------------------------------------------------------------- */
/* Offline self-test                                                         */
/* ------------------------------------------------------------------------- */

static bool ReadWavMono16k(const QString &path, std::vector<float> &out)
{
	FILE *file = fopen(path.toUtf8().constData(), "rb");
	if (!file)
		return false;

	char riff[12];
	if (fread(riff, 1, 12, file) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
		fclose(file);
		return false;
	}

	uint16_t channels = 1;
	uint32_t rate = 16000;
	uint16_t bits = 16;
	bool haveFormat = false;

	for (;;) {
		char chunk[8];
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
					const size_t i1 = std::min(i0 + 1, mono.size() - 1);
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

QString VoiceEngine::TestRecognize(const QString &wavPath, const QStringList &phrases, QString *transcriptOut)
{
	std::vector<float> audio;
	if (!ReadWavMono16k(wavPath, audio))
		return QString();

	const QString model = AutoModel();
	const QString path = ModelPath(model);
	whisper_context_params contextParams = whisper_context_default_params();
	contextParams.use_gpu = false;
	whisper_context *ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), contextParams);
	if (!ctx)
		return QString();

	PrepareForWhisper(audio);

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
	params.max_tokens = 32;
	params.audio_ctx = 0;
	params.initial_prompt = nullptr;
	const QByteArray lang = AutoLanguage(phrases).toUtf8();
	params.language = lang.constData();

	QString text;
	DecodeConfidence confidence;
	if (whisper_full(ctx, params, audio.data(), (int)audio.size()) == 0) {
		const int segments = whisper_full_n_segments(ctx);
		for (int i = 0; i < segments; i++) {
			const char *segmentText = whisper_full_get_segment_text(ctx, i);
			if (segmentText)
				text += QString::fromUtf8(segmentText).trimmed() + QChar(' ');
		}
		confidence = MeasureDecode(ctx);
	}
	whisper_free(ctx);

	text = text.trimmed();
	if (transcriptOut)
		*transcriptOut = text;

	if (IsLikelyHallucination(text) || confidence.noSpeechProb > 0.45)
		return QString();

	for (const QString &phrase : phrases) {
		if (PhraseMatches(phrase, text))
			return phrase;
	}
	return QString();
}

/* ------------------------------------------------------------------------- */
/* VoiceEngine lifecycle                                                     */
/* ------------------------------------------------------------------------- */

static VoiceEngine *engineInstance = nullptr;

VoiceEngine::VoiceEngine(OBSBasic *main) : m_main(main)
{
	m_downloader = new VoiceModelDownloader(this);

	connect(m_downloader, &VoiceModelDownloader::Progress, this,
		[this](int percent) { emit ModelDownloadProgress(percent); });

	connect(m_downloader, &VoiceModelDownloader::Finished, this, [this](bool ok, const QString &error) {
		emit ModelDownloadFinished(ok, error);
		if (ok) {
			SetStatus(QStringLiteral("Model ready"));
			ReapplyConfig();
		} else {
			SetStatus(QStringLiteral("Model download failed: %1").arg(error));
		}
	});
}

VoiceEngine::~VoiceEngine()
{
	Stop();
}

static void whisper_log_cb(enum ggml_log_level level, const char *text, void *)
{
	if (!text)
		return;
	const int obsLevel = (level == GGML_LOG_LEVEL_ERROR) ? LOG_ERROR : LOG_DEBUG;
	blog(obsLevel, "[whisper] %s", text);
}

void VoiceEngine::Init(OBSBasic *main)
{
	if (engineInstance)
		return;

	whisper_log_set(whisper_log_cb, nullptr);

	engineInstance = new VoiceEngine(main);
	engineInstance->m_config = LoadConfig();

	obs_frontend_add_event_callback(
		[](enum obs_frontend_event event, void *data) {
			auto *self = static_cast<VoiceEngine *>(data);
			if (!self)
				return;
			if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED ||
			    event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED)
				QMetaObject::invokeMethod(self, [self]() { self->Apply(); }, Qt::QueuedConnection);
			else if (event == OBS_FRONTEND_EVENT_EXIT)
				self->Shutdown();
		},
		engineInstance);

	connect(engineInstance, &VoiceEngine::Triggered, engineInstance,
		[](const QString &phrase, const QString &transcript) {
			Q_UNUSED(phrase);
			if (!obs_frontend_replay_buffer_active()) {
				engineInstance->SetStatus(
					QStringLiteral("Heard \"%1\" but Replay Buffer is not running").arg(transcript));
				return;
			}
			obs_frontend_replay_buffer_save();
			engineInstance->SetStatus(
				QStringLiteral("Replay saved by voice command (heard: \"%1\")").arg(transcript));
		});

	engineInstance->Apply();
}

VoiceEngine *VoiceEngine::Instance()
{
	return engineInstance;
}

bool VoiceEngine::Listening() const
{
	return m_listening.load();
}

QString VoiceEngine::LastTranscript() const
{
	return m_transcript;
}

QString VoiceEngine::StatusText() const
{
	return m_status;
}

void VoiceEngine::SetStatus(const QString &status)
{
	m_status = status;
	emit StatusChanged();
}

void VoiceEngine::SetTranscript(const QString &text)
{
	m_transcript = text;
	emit TranscriptChanged(text);
}

void VoiceEngine::ReapplyConfig()
{
	m_config = LoadConfig();
	Apply();
}

void VoiceEngine::Apply()
{
	m_config = LoadConfig();
	Stop();

	if (!m_config.enabled) {
		SetStatus(QStringLiteral("Disabled"));
		return;
	}

	if (!ModelReady(m_config.model)) {
		SetStatus(QStringLiteral("Model ggml-%1.bin is not downloaded").arg(m_config.model));
		return;
	}

	if (m_config.onlyWithReplayBuffer && !obs_frontend_replay_buffer_active()) {
		SetStatus(QStringLiteral("Waiting for Replay Buffer to start"));
		return;
	}

	m_noiseFloor = 0.0015;
	m_running = true;
	m_listening = true;
	m_captureThread = std::thread(&VoiceEngine::CaptureLoop, this);
	m_inferThread = std::thread(&VoiceEngine::InferenceLoop, this);

	SetStatus(QStringLiteral("Listening to microphone"));
}

void VoiceEngine::Shutdown()
{
	Stop();
}

void VoiceEngine::Stop()
{
	const bool wasRunning = m_running.exchange(false);
	m_interactiveCancel = true;

	if (wasRunning) {
		m_cv.notify_all();
		if (m_captureThread.joinable())
			m_captureThread.join();
		if (m_inferThread.joinable())
			m_inferThread.join();
	}

	if (m_interactiveThread.joinable())
		m_interactiveThread.join();

	m_listening = false;
	ReleaseContexts();
}

void VoiceEngine::ReleaseContexts()
{
	if (m_ctx) {
		whisper_free(m_ctx);
		m_ctx = nullptr;
		m_modelLoaded.clear();
	}
}

void VoiceEngine::DownloadMissingModels()
{
	if (DownloadingModels())
		return;

	const QString model = AutoModel();
	if (ModelReady(model)) {
		emit ModelDownloadFinished(true, QString());
		return;
	}

	blog(LOG_INFO, "[AkvariumVoice] downloading model '%s'", model.toUtf8().constData());
	SetStatus(QStringLiteral("Downloading model %1...").arg(model));
	m_downloader->Start(model);
}

bool VoiceEngine::DownloadingModels() const
{
	return m_downloader && m_downloader->Downloading();
}

/* ------------------------------------------------------------------------- */
/* Voice gate thresholds                                                     */
/* ------------------------------------------------------------------------- */

double VoiceEngine::SpeechStartThreshold() const
{
	/* Sensitivity 1 (low) .. 5 (high) */
	switch (m_config.sensitivity) {
	case 1: /* Low (loud room / close voice: rejects background speech & bleed) */
		return std::max(0.040, m_noiseFloor * 4.5);
	case 2: /* Medium-Low */
		return std::max(0.024, m_noiseFloor * 3.5);
	case 4: /* Medium-High */
		return std::max(0.009, m_noiseFloor * 2.0);
	case 5: /* High (quiet voice) */
		return std::max(0.004, m_noiseFloor * 1.5);
	case 3: /* Normal (recommended) */
	default:
		return std::max(0.016, m_noiseFloor * 2.8);
	}
}

double VoiceEngine::SpeechEndThreshold() const
{
	const double start = SpeechStartThreshold();
	return std::max(0.0025, start * 0.50);
}

QString VoiceEngine::ResolveLanguage() const
{
	const QString lang = m_config.language.trimmed().toLower();
	if (lang.startsWith(QStringLiteral("ru")))
		return QStringLiteral("ru");
	if (lang.startsWith(QStringLiteral("en")))
		return QStringLiteral("en");
	return AutoLanguage(m_config.phrases);
}

/* ------------------------------------------------------------------------- */
/* Audio capture & VAD                                                       */
/* ------------------------------------------------------------------------- */

QString VoiceEngine::ResolveDeviceSpec() const
{
	const QString selection = m_config.micSource.trimmed();
	if (selection.startsWith(QStringLiteral("wasapi:")) || selection.startsWith(QStringLiteral("loopback:")))
		return selection;

	QString sourceName = selection;
	if (sourceName.startsWith(QStringLiteral("source:")))
		sourceName = sourceName.mid(7);

	obs_source_t *source = nullptr;
	if (!sourceName.isEmpty()) {
		source = obs_get_source_by_name(sourceName.toUtf8().constData());
		if (!source) {
			for (int channel = 1; channel < 7 && !source; channel++) {
				obs_source_t *candidate = obs_get_output_source(channel);
				if (!candidate)
					continue;
				const char *name = obs_source_get_name(candidate);
				if (name && sourceName == QString::fromUtf8(name))
					source = obs_source_get_ref(candidate);
				obs_source_release(candidate);
			}
		}
	} else {
		obs_source_t *sceneSource = obs_frontend_get_current_scene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (scene) {
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					obs_source_t **out = static_cast<obs_source_t **>(param);
					obs_source_t *src = obs_sceneitem_get_source(item);
					if (!src || !obs_source_active(src))
						return true;
					if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
						return true;
					const char *id = obs_source_get_id(src);
					if (id && strstr(id, "input_capture")) {
						*out = obs_source_get_ref(src);
						return false;
					}
					return true;
				},
				&source);
		}
		if (sceneSource)
			obs_source_release(sceneSource);
	}

	QString spec;
	if (source) {
		obs_data_t *settings = obs_source_get_settings(source);
		const char *id = obs_data_get_string(settings, "device_id");
		const char *sourceId = obs_source_get_id(source);
		if (id && *id) {
			const bool loopback = sourceId && strstr(sourceId, "output_capture");
			spec = loopback ? QStringLiteral("loopback:%1").arg(QString::fromUtf8(id))
					: QStringLiteral("wasapi:%1").arg(QString::fromUtf8(id));
		}
		obs_data_release(settings);
		obs_source_release(source);
	}
	return spec;
}

bool VoiceEngine::OpenCapture(const QString &deviceSpec, void **outDevice, void **outClient, void **outCapture,
			      int *outSampleRate)
{
	const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
	const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

	IMMDeviceEnumerator *enumerator = nullptr;
	if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
				    (void **)&enumerator)))
		return false;

	const bool loopback = deviceSpec.startsWith(QStringLiteral("loopback:"));
	QString deviceId = deviceSpec;
	const int colon = deviceSpec.indexOf(QChar(':'));
	if (colon > 0)
		deviceId = deviceSpec.mid(colon + 1);

	IMMDevice *device = nullptr;
	if (!deviceId.isEmpty() && deviceId != QStringLiteral("default")) {
		const std::wstring wide = deviceId.toStdWString();
		enumerator->GetDevice(wide.c_str(), &device);
	}

	if (!device) {
		const EDataFlow flow = loopback ? eRender : eCapture;
		if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
			enumerator->Release();
			return false;
		}
	}

	enumerator->Release();

	IAudioClient *client = nullptr;
	if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) {
		device->Release();
		return false;
	}

	WAVEFORMATEX format = {};
	format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	format.nChannels = 1;
	format.nSamplesPerSec = 16000;
	format.wBitsPerSample = 32;
	format.nBlockAlign = sizeof(float);
	format.nAvgBytesPerSec = 16000 * sizeof(float);

	DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (loopback)
		flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

	HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, &format, nullptr);
	int sampleRate = 16000;
	int channels = 1;

	if (FAILED(hr)) {
		WAVEFORMATEX *mixFormat = nullptr;
		if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
			client->Release();
			device->Release();
			return false;
		}
		if (mixFormat->wBitsPerSample != 32 || mixFormat->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
			CoTaskMemFree(mixFormat);
			client->Release();
			device->Release();
			return false;
		}
		sampleRate = (int)mixFormat->nSamplesPerSec;
		channels = (int)mixFormat->nChannels;
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, mixFormat, nullptr);
		CoTaskMemFree(mixFormat);
	}

	if (FAILED(hr)) {
		client->Release();
		device->Release();
		return false;
	}

	IAudioCaptureClient *capture = nullptr;
	if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture))) {
		client->Release();
		device->Release();
		return false;
	}

	if (FAILED(client->Start())) {
		capture->Release();
		client->Release();
		device->Release();
		return false;
	}

	*outDevice = device;
	*outClient = client;
	*outCapture = capture;
	*outSampleRate = sampleRate;

	m_sampleRate = sampleRate;
	m_channels = channels;
	m_phase = 0.0;
	m_lastSample = 0.0f;
	return true;
}

void VoiceEngine::CaptureLoop()
{
	const QString spec = ResolveDeviceSpec();
	m_loopback = spec.startsWith(QStringLiteral("loopback:"));

	IMMDevice *device = nullptr;
	IAudioClient *client = nullptr;
	IAudioCaptureClient *capture = nullptr;
	int sampleRate = 0;

	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return;

	if (!OpenCapture(spec, (void **)&device, (void **)&client, (void **)&capture, &sampleRate)) {
		CoUninitialize();
		return;
	}

	while (m_running) {
		Sleep(10);

		UINT32 packetSize = 0;
		if (FAILED(capture->GetNextPacketSize(&packetSize)))
			break;

		while (packetSize > 0) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;

			if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
				break;

			if (frames > 0) {
				if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
					std::vector<float> silence((size_t)frames * m_channels, 0.0f);
					Feed(silence.data(), frames, m_channels);
				} else {
					Feed(reinterpret_cast<const float *>(data), frames, m_channels);
				}
			}

			capture->ReleaseBuffer(frames);
			if (FAILED(capture->GetNextPacketSize(&packetSize)))
				break;
		}
	}

	client->Stop();
	capture->Release();
	client->Release();
	device->Release();
	CoUninitialize();
}

void VoiceEngine::Feed(const float *data, size_t frames, int channels)
{
	std::vector<float> pcm16k;
	AppendResampled16k(data, frames, channels, m_sampleRate, m_phase, m_lastSample, pcm16k);

	const size_t frameSize = 320; /* 20 ms */
	m_residual.insert(m_residual.end(), pcm16k.begin(), pcm16k.end());

	size_t offset = 0;
	while (offset + frameSize <= m_residual.size()) {
		double sum = 0.0;
		for (size_t i = 0; i < frameSize; i++) {
			const double v = m_residual[offset + i];
			sum += v * v;
		}
		const double rms = std::sqrt(sum / (double)frameSize);

		const double startThold = SpeechStartThreshold();
		const double endThold = SpeechEndThreshold();

		if (!m_inSpeech) {
			/* Maintain rolling 250 ms pre-speech buffer (4000 samples) */
			m_history.insert(m_history.end(), m_residual.begin() + (long)offset,
					 m_residual.begin() + (long)(offset + frameSize));
			if (m_history.size() > 4000)
				m_history.erase(m_history.begin(), m_history.begin() + (long)(m_history.size() - 4000));

			if (rms > startThold) {
				m_inSpeech = true;
				m_silenceMs = 0;
				m_segment = m_history;
			} else {
				/* Adapt background noise floor smoothly */
				m_noiseFloor = 0.98 * m_noiseFloor + 0.02 * std::min(rms, 0.02);
			}
		} else {
			if (m_segment.size() < 16000 * 4) {
				m_segment.insert(m_segment.end(), m_residual.begin() + (long)offset,
						 m_residual.begin() + (long)(offset + frameSize));
			}

			if (rms <= endThold)
				m_silenceMs += 20;
			else
				m_silenceMs = 0;

			/* 450 ms of silence ends speech cleanly without truncating multi-word commands */
			if (m_silenceMs >= 450 || m_segment.size() >= 16000 * 4)
				FinalizeSegment(m_segment);
		}

		offset += frameSize;
	}

	if (offset > 0)
		m_residual.erase(m_residual.begin(), m_residual.begin() + (long)offset);
}

void VoiceEngine::FinalizeSegment(std::vector<float> &segment)
{
	const AudioSegmentStats stats = AnalyzeAudioSegment(segment.data(), segment.size(), SpeechEndThreshold());
	if (SegmentLooksLikeSpeech(stats, m_config.sensitivity)) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_pendingQueue.size() >= 3)
			m_pendingQueue.erase(m_pendingQueue.begin());
		m_pendingQueue.push_back(segment);
		m_cv.notify_one();
	}
	segment.clear();
	m_inSpeech = false;
	m_silenceMs = 0;
}

/* ------------------------------------------------------------------------- */
/* Whisper inference                                                         */
/* ------------------------------------------------------------------------- */

QString VoiceEngine::DecodeWith(whisper_context *ctx, const std::vector<float> &audio,
				DecodeConfidence *confidence)
{
	if (!ctx || audio.empty())
		return QString();

	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = std::max(2, std::min(6, QThread::idealThreadCount() / 2));
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
	params.max_tokens = 32;
	params.no_timestamps = true;
	params.no_speech_thold = 0.40f;
	params.logprob_thold = -1.0f;
	params.entropy_thold = 2.4f;
	params.audio_ctx = 0;         /* Default full context (1500) preserves maximum accuracy */

	/* Do NOT use initial_prompt in live recognition - causes phantom hallucinations on silence */
	params.initial_prompt = nullptr;

	const QByteArray lang = ResolveLanguage().toUtf8();
	params.language = lang.constData();

	if (whisper_full(ctx, params, audio.data(), (int)audio.size()) != 0)
		return QString();

	QString text;
	const int segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < segments; i++) {
		const char *segmentText = whisper_full_get_segment_text(ctx, i);
		if (segmentText)
			text += QString::fromUtf8(segmentText).trimmed() + QChar(' ');
	}

	if (confidence)
		*confidence = MeasureDecode(ctx);

	return text.trimmed();
}

void VoiceEngine::InferenceLoop()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	const QString model = m_config.model;
	const QString path = ModelPath(model);
	if (path.isEmpty())
		return;

	whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = false; /* CPU is fast (~100 ms) and free of Vulkan driver quirks */
	m_ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), cparams);
	if (!m_ctx) {
		QMetaObject::invokeMethod(this, [this]() { SetStatus(QStringLiteral("Failed to load model")); },
					  Qt::QueuedConnection);
		return;
	}
	m_modelLoaded = model;

	QMetaObject::invokeMethod(this, [this]() { SetStatus(QStringLiteral("Listening to microphone")); },
				  Qt::QueuedConnection);

	while (m_running) {
		std::vector<float> audio;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait_for(lock, std::chrono::milliseconds(200),
				      [this]() { return !m_pendingQueue.empty() || !m_running; });
			if (!m_running)
				break;
			if (m_pendingQueue.empty())
				continue;
			audio = std::move(m_pendingQueue.front());
			m_pendingQueue.erase(m_pendingQueue.begin());
		}

		PrepareForWhisper(audio);

		DecodeConfidence confidence;
		const QString text = DecodeWith(m_ctx, audio, &confidence);

		/* Drop silence, low-confidence decodes, and hallucinations */
		if (text.isEmpty() ||
		    confidence.noSpeechProb > 0.45 ||
		    (confidence.tokens >= 2 && confidence.meanTokenProb < 0.55) ||
		    (confidence.tokens >= 1 && confidence.minTokenProb < 0.15) ||
		    IsLikelyHallucination(text)) {
			continue;
		}

		QMetaObject::invokeMethod(this, [this, text]() { HandleTranscript(text); }, Qt::QueuedConnection);
	}
}

void VoiceEngine::HandleTranscript(const QString &text)
{
	SetTranscript(text);

	double bestScore = 0.0;
	const QString bestPhrase = MatchCommand(text, &bestScore);

	if (bestPhrase.isEmpty()) {
		blog(LOG_INFO, "[AkvariumVoice] transcript '%s' did not match commands (score %.2f)",
		     text.toUtf8().constData(), bestScore);
		return;
	}

	/* 2.5 second cooldown against double triggers */
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (bestPhrase == m_lastTriggeredPhrase && (now - m_lastTriggerMs) < 2500)
		return;

	m_lastTriggerMs = now;
	m_lastTriggeredPhrase = bestPhrase;

	blog(LOG_INFO, "[AkvariumVoice] triggered command '%s' (score %.2f, text: '%s')",
	     bestPhrase.toUtf8().constData(), bestScore, text.toUtf8().constData());
	emit Triggered(bestPhrase, text);
}

void VoiceEngine::HandleVerifiedCommand(const QString &phrase, const QString &text, double score)
{
	Q_UNUSED(phrase);
	Q_UNUSED(score);
	HandleTranscript(text);
}

QString VoiceEngine::MatchCommand(const QString &text, double *scoreOut) const
{
	QString bestPhrase;
	double bestScore = 0.0;

	for (const QString &phrase : m_config.phrases) {
		double score = MatchPhrase(phrase, text).score;

		for (const auto &alias : m_config.aliases) {
			if (alias.first == phrase)
				score = std::max(score, MatchPhrase(alias.second, text).score);
		}

		if (score > bestScore) {
			bestScore = score;
			bestPhrase = phrase;
		}
	}

	if (scoreOut)
		*scoreOut = bestScore;

	return bestScore >= kPhraseMatchThreshold ? bestPhrase : QString();
}

/* ------------------------------------------------------------------------- */
/* Interactive recording and live phrase test                                */
/* ------------------------------------------------------------------------- */

void VoiceEngine::StartPhraseRecording(int seconds)
{
	if (m_recordingPhrase.exchange(true))
		return;

	Stop();
	m_interactiveCancel = false;
	m_interactiveThread = std::thread(&VoiceEngine::RunInteractiveRecord, this, seconds, false);
}

void VoiceEngine::CancelPhraseRecording()
{
	m_interactiveCancel = true;
}

bool VoiceEngine::RecordingPhrase() const
{
	return m_recordingPhrase.load();
}

void VoiceEngine::StartSampleCollection(const QString &phrase, int totalSamples)
{
	if (m_collectingSamples.exchange(true))
		return;

	Stop();
	m_interactiveCancel = false;
	m_interactiveThread = std::thread(&VoiceEngine::RunSampleCollection, this, phrase.trimmed(),
					  std::max(1, totalSamples));
}

void VoiceEngine::CancelSampleCollection()
{
	m_interactiveCancel = true;
}

bool VoiceEngine::CollectingSamples() const
{
	return m_collectingSamples.load();
}

void VoiceEngine::StartPhraseTest(int seconds)
{
	if (m_testingPhrase.exchange(true))
		return;

	Stop();
	m_interactiveCancel = false;
	m_interactiveThread = std::thread(&VoiceEngine::RunInteractiveRecord, this, seconds, true);
}

bool VoiceEngine::TestingPhrase() const
{
	return m_testingPhrase.load();
}

void VoiceEngine::RunInteractiveRecord(int seconds, bool isTest)
{
	const QString spec = ResolveDeviceSpec();

	IMMDevice *device = nullptr;
	IAudioClient *client = nullptr;
	IAudioCaptureClient *capture = nullptr;
	int sampleRate = 0;

	std::vector<float> recorded;
	bool ok = false;

	if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		if (OpenCapture(spec, (void **)&device, (void **)&client, (void **)&capture, &sampleRate)) {
			double phase = 0.0;
			float lastSample = 0.0f;
			const ULONGLONG started = GetTickCount64();
			const int maxWaitMs = std::max(3, seconds) * 1000;

			std::vector<float> preBuffer;
			std::vector<float> residual;
			bool inSpeech = false;
			int silenceMs = 0;

			while (!m_interactiveCancel && (GetTickCount64() - started) < (ULONGLONG)maxWaitMs) {
				Sleep(10);

				UINT32 packetSize = 0;
				if (FAILED(capture->GetNextPacketSize(&packetSize)))
					break;

				while (packetSize > 0) {
					BYTE *data = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;

					if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
						break;

					if (frames > 0) {
						std::vector<float> pcm;
						if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
							std::vector<float> silence((size_t)frames * m_channels, 0.0f);
							AppendResampled16k(silence.data(), frames, m_channels, m_sampleRate,
									   phase, lastSample, pcm);
						} else {
							AppendResampled16k(reinterpret_cast<const float *>(data), frames,
									   m_channels, m_sampleRate, phase, lastSample,
									   pcm);
						}
						residual.insert(residual.end(), pcm.begin(), pcm.end());
					}

					capture->ReleaseBuffer(frames);
					if (FAILED(capture->GetNextPacketSize(&packetSize)))
						break;
				}

				const size_t frameSize = 320;
				size_t offset = 0;
				bool speechEnded = false;

				while (offset + frameSize <= residual.size()) {
					double sum = 0.0;
					for (size_t i = 0; i < frameSize; i++) {
						const double v = residual[offset + i];
						sum += v * v;
					}
					const double rms = std::sqrt(sum / (double)frameSize);
					const double startThold = SpeechStartThreshold();
					const double endThold = SpeechEndThreshold();

					if (!inSpeech) {
						preBuffer.insert(preBuffer.end(), residual.begin() + (long)offset,
								 residual.begin() + (long)(offset + frameSize));
						if (preBuffer.size() > 4800)
							preBuffer.erase(preBuffer.begin(),
									preBuffer.begin() + (long)(preBuffer.size() - 4800));

						if (rms > startThold) {
							inSpeech = true;
							silenceMs = 0;
							recorded = preBuffer;
						}
					} else {
						recorded.insert(recorded.end(), residual.begin() + (long)offset,
								residual.begin() + (long)(offset + frameSize));

						if (rms <= endThold)
							silenceMs += 20;
						else
							silenceMs = 0;

						if (silenceMs >= 500 || recorded.size() >= 16000 * 4) {
							speechEnded = true;
							offset += frameSize;
							break;
						}
					}

					offset += frameSize;
				}

				if (offset > 0)
					residual.erase(residual.begin(), residual.begin() + (long)std::min(offset, residual.size()));

				if (speechEnded)
					break;
			}

			client->Stop();
			capture->Release();
			client->Release();
			device->Release();
			ok = !recorded.empty() && inSpeech && !m_interactiveCancel;
		}
		CoUninitialize();
	}

	QString text;
	double score = 0.0;
	QString matchedPhrase;
	bool matched = false;

	if (ok) {
		PrepareForWhisper(recorded);

		const QString model = AutoModel();
		const QString path = ModelPath(model);
		whisper_context_params cparams = whisper_context_default_params();
		cparams.use_gpu = false;
		whisper_context *ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), cparams);

		if (ctx) {
			DecodeConfidence conf;
			text = DecodeWith(ctx, recorded, &conf);
			whisper_free(ctx);

			text = NormalizePhraseText(text);

			if (isTest) {
				matchedPhrase = MatchCommand(text, &score);
				matched = !matchedPhrase.isEmpty();
			}
		}
	}

	QMetaObject::invokeMethod(
		this,
		[this, isTest, text, matched, matchedPhrase, score]() {
			if (isTest) {
				m_testingPhrase = false;
				emit PhraseTestFinished(matched, text, matchedPhrase, score);
			} else {
				m_recordingPhrase = false;
				emit PhraseRecorded(text);
			}
			ReapplyConfig();
		},
		Qt::QueuedConnection);
}

void VoiceEngine::RunSampleCollection(const QString &phrase, int totalSamples)
{
	const QString spec = ResolveDeviceSpec();

	IMMDevice *device = nullptr;
	IAudioClient *client = nullptr;
	IAudioCaptureClient *capture = nullptr;
	int sampleRate = 0;

	QStringList learnedVariants;

	const QString model = AutoModel();
	const QString path = ModelPath(model);
	whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = false;
	whisper_context *ctx = nullptr;
	if (ModelReady(model)) {
		ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), cparams);
	}

	if (!ctx) {
		QMetaObject::invokeMethod(
			this,
			[this, phrase]() {
				m_collectingSamples = false;
				emit SampleCollectionFinished(phrase, QStringList());
				ReapplyConfig();
			},
			Qt::QueuedConnection);
		return;
	}

	if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		if (OpenCapture(spec, (void **)&device, (void **)&client, (void **)&capture, &sampleRate)) {
			double phase = 0.0;
			float lastSample = 0.0f;

			int sampleIdx = 1;
			while (sampleIdx <= totalSamples && !m_interactiveCancel) {
				UINT32 stalePackets = 0;
				while (SUCCEEDED(capture->GetNextPacketSize(&stalePackets)) && stalePackets > 0) {
					BYTE *pData = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;
					if (FAILED(capture->GetBuffer(&pData, &frames, &flags, nullptr, nullptr)))
						break;
					capture->ReleaseBuffer(frames);
				}

				QMetaObject::invokeMethod(
					this,
					[this, sampleIdx, totalSamples, phrase]() {
						emit SampleProgress(sampleIdx, totalSamples,
								    QStringLiteral("Sample %1 of %2: Say \"%3\" into microphone...")
									    .arg(sampleIdx)
									    .arg(totalSamples)
									    .arg(phrase));
					},
					Qt::QueuedConnection);

				std::vector<float> sampleAudio;
				std::vector<float> preBuffer;
				std::vector<float> residual;
				bool inSpeech = false;
				int silenceMs = 0;
				const ULONGLONG sampleWaitStart = GetTickCount64();
				const ULONGLONG maxWaitMs = 25000;

				while (!m_interactiveCancel) {
					if (!inSpeech && (GetTickCount64() - sampleWaitStart) > maxWaitMs)
						break;

					Sleep(10);

					UINT32 packetSize = 0;
					if (FAILED(capture->GetNextPacketSize(&packetSize)))
						break;

					while (packetSize > 0) {
						BYTE *data = nullptr;
						UINT32 frames = 0;
						DWORD flags = 0;

						if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
							break;

						if (frames > 0) {
							std::vector<float> pcm;
							if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
								std::vector<float> silence((size_t)frames * m_channels, 0.0f);
								AppendResampled16k(silence.data(), frames, m_channels, m_sampleRate,
										   phase, lastSample, pcm);
							} else {
								AppendResampled16k(reinterpret_cast<const float *>(data), frames,
										   m_channels, m_sampleRate, phase, lastSample,
										   pcm);
							}
							residual.insert(residual.end(), pcm.begin(), pcm.end());
						}

						capture->ReleaseBuffer(frames);
						if (FAILED(capture->GetNextPacketSize(&packetSize)))
							break;
					}

					const size_t frameSize = 320;
					size_t offset = 0;
					bool speechEnded = false;

					while (offset + frameSize <= residual.size()) {
						double sum = 0.0;
						for (size_t i = 0; i < frameSize; i++) {
							const double v = residual[offset + i];
							sum += v * v;
						}
						const double rms = std::sqrt(sum / (double)frameSize);
						const double startThold = SpeechStartThreshold();
						const double endThold = SpeechEndThreshold();

						if (!inSpeech) {
							preBuffer.insert(preBuffer.end(), residual.begin() + (long)offset,
									 residual.begin() + (long)(offset + frameSize));
							if (preBuffer.size() > 4800)
								preBuffer.erase(preBuffer.begin(),
										preBuffer.begin() + (long)(preBuffer.size() - 4800));

							if (rms > startThold) {
								inSpeech = true;
								silenceMs = 0;
								sampleAudio = preBuffer;
								QMetaObject::invokeMethod(
									this,
									[this, sampleIdx, totalSamples, phrase]() {
										emit SampleProgress(sampleIdx, totalSamples,
												    QStringLiteral("🔴 Listening to sample %1 of %2...")
													    .arg(sampleIdx)
													    .arg(totalSamples));
									},
									Qt::QueuedConnection);
							}
						} else {
							sampleAudio.insert(sampleAudio.end(), residual.begin() + (long)offset,
									   residual.begin() + (long)(offset + frameSize));

							if (rms <= endThold)
								silenceMs += 20;
							else
								silenceMs = 0;

							if (silenceMs >= 500 || sampleAudio.size() >= 16000 * 4) {
								speechEnded = true;
								offset += frameSize;
								break;
							}
						}

						offset += frameSize;
					}

					if (offset > 0)
						residual.erase(residual.begin(), residual.begin() + (long)std::min(offset, residual.size()));

					if (speechEnded || m_interactiveCancel)
						break;
				}

				if (m_interactiveCancel)
					break;

				QString heard;
				DecodeConfidence conf;
				if (!sampleAudio.empty() && inSpeech) {
					PrepareForWhisper(sampleAudio);
					heard = DecodeWith(ctx, sampleAudio, &conf);
					heard = NormalizePhraseText(heard);
				}

				const bool valid = !heard.isEmpty() && conf.noSpeechProb <= 0.65 && !IsLikelyHallucination(heard);

				if (valid) {
					if (!learnedVariants.contains(heard, Qt::CaseInsensitive))
						learnedVariants.append(heard);

					QMetaObject::invokeMethod(
						this,
						[this, sampleIdx, heard]() {
							emit SampleCollected(sampleIdx, heard);
						},
						Qt::QueuedConnection);

					sampleIdx++;

					if (sampleIdx <= totalSamples && !m_interactiveCancel) {
						QMetaObject::invokeMethod(
							this,
							[this, sampleIdx, totalSamples]() {
								emit SampleProgress(sampleIdx, totalSamples,
										    QStringLiteral("Got it! Preparing for next sample..."));
							},
							Qt::QueuedConnection);

						const ULONGLONG pauseStart = GetTickCount64();
						while (!m_interactiveCancel && (GetTickCount64() - pauseStart) < 600) {
							Sleep(20);
						}
					}
				} else {
					QMetaObject::invokeMethod(
						this,
						[this, sampleIdx, totalSamples, phrase]() {
							emit SampleProgress(sampleIdx, totalSamples,
									    QStringLiteral("⚠️ Could not hear clearly. Please repeat \"%1\"...")
										    .arg(phrase));
						},
						Qt::QueuedConnection);

					const ULONGLONG pauseStart = GetTickCount64();
					while (!m_interactiveCancel && (GetTickCount64() - pauseStart) < 400) {
						Sleep(20);
					}
				}
			}

			client->Stop();
			capture->Release();
			client->Release();
			device->Release();
		}
		CoUninitialize();
	}

	whisper_free(ctx);

	if (!m_interactiveCancel && !phrase.isEmpty() && !learnedVariants.isEmpty()) {
		VoiceConfig cfg = LoadConfig();
		if (!cfg.phrases.contains(phrase, Qt::CaseInsensitive)) {
			cfg.phrases.append(phrase);
		}
		for (const QString &v : learnedVariants) {
			if (v.compare(phrase, Qt::CaseInsensitive) != 0) {
				bool exists = false;
				for (const auto &p : cfg.aliases) {
					if (p.first.compare(phrase, Qt::CaseInsensitive) == 0 &&
					    p.second.compare(v, Qt::CaseInsensitive) == 0) {
						exists = true;
						break;
					}
				}
				if (!exists) {
					cfg.aliases.append(qMakePair(phrase, v));
				}
			}
		}
		SaveConfig(cfg);
	}

	QMetaObject::invokeMethod(
		this,
		[this, phrase, learnedVariants]() {
			m_collectingSamples = false;
			emit SampleCollectionFinished(phrase, learnedVariants);
			ReapplyConfig();
		},
		Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */
/* Model downloader                                                          */
/* ------------------------------------------------------------------------- */

VoiceModelDownloader::VoiceModelDownloader(QObject *parent) : QObject(parent) {}

VoiceModelDownloader::~VoiceModelDownloader()
{
	m_cancel = true;
	if (m_thread.joinable())
		m_thread.join();
}

QString VoiceModelDownloader::TargetPath(const QString &model)
{
	const QString path = VoiceEngine::ModelPath(model);
	if (path.isEmpty())
		return QString();

	const int slash = path.lastIndexOf(QChar('/'));
	if (slash > 0)
		os_mkdirs(path.left(slash).toUtf8().constData());

	return path;
}

struct DownloadState {
	FILE *file = nullptr;
	VoiceModelDownloader *self = nullptr;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *state = static_cast<DownloadState *>(userdata);
	return fwrite(ptr, size, nmemb, state->file);
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
	auto *state = static_cast<DownloadState *>(clientp);
	if (state->self && state->self->Cancelled())
		return 1;

	if (dltotal > 0 && state->self) {
		const int percent = (int)((dlnow * 100) / dltotal);
		QMetaObject::invokeMethod(state->self, [state, percent]() { emit state->self->Progress(percent); },
					  Qt::QueuedConnection);
	}
	return 0;
}

void VoiceModelDownloader::Start(const QString &model)
{
	m_cancel = false;
	if (m_thread.joinable())
		m_thread.join();
	m_thread = std::thread(&VoiceModelDownloader::Run, this, model);
}

void VoiceModelDownloader::Cancel()
{
	m_cancel = true;
}

void VoiceModelDownloader::Run(const QString &model)
{
	const QString target = TargetPath(model);
	const QString partial = target + QStringLiteral(".part");

	if (target.isEmpty()) {
		QMetaObject::invokeMethod(this, [this]() { emit Finished(false, QStringLiteral("No configuration path")); },
					  Qt::QueuedConnection);
		return;
	}

	FILE *file = fopen(partial.toUtf8().constData(), "wb");
	if (!file) {
		QMetaObject::invokeMethod(this, [this]() { emit Finished(false, QStringLiteral("Cannot create file")); },
					  Qt::QueuedConnection);
		return;
	}

	DownloadState state;
	state.file = file;
	state.self = this;

	CURL *curl = curl_easy_init();
	bool ok = false;
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, VoiceEngine::ModelUrl(model).toUtf8().constData());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "AkvariumOBS");

		ok = (curl_easy_perform(curl) == CURLE_OK);
		curl_easy_cleanup(curl);
	}

	fclose(file);

	if (!ok || m_cancel) {
		os_unlink(partial.toUtf8().constData());
		const QString error = m_cancel ? QStringLiteral("Cancelled") : QStringLiteral("Download failed");
		QMetaObject::invokeMethod(this, [this, error]() { emit Finished(false, error); }, Qt::QueuedConnection);
		return;
	}

	if (os_safe_replace(target.toUtf8().constData(), partial.toUtf8().constData(), nullptr) != 0) {
		os_unlink(partial.toUtf8().constData());
		QMetaObject::invokeMethod(this, [this]() { emit Finished(false, QStringLiteral("Cannot save model")); },
					  Qt::QueuedConnection);
		return;
	}

	QMetaObject::invokeMethod(this, [this]() { emit Finished(true, QString()); }, Qt::QueuedConnection);
}

} // namespace akvarium
