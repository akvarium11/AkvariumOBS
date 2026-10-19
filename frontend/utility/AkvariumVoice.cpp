/******************************************************************************
    AkvariumOBS: voice commands implementation (whisper.cpp + WASAPI).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumVoice.hpp"

#include "AkvariumPhraseMatch.hpp"

#include <OBSApp.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QMetaObject>
#include <QDateTime>
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
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

VoiceConfig VoiceEngine::LoadConfig()
{
	VoiceConfig cfg;
	config_t *conf = App()->GetUserConfig();

	cfg.enabled = config_get_bool(conf, kConfigSection, "Enabled");
	cfg.model = QString::fromUtf8(config_get_string(conf, kConfigSection, "Model"));
	if (cfg.model.isEmpty())
		cfg.model = QStringLiteral("base");
	cfg.micSource = QString::fromUtf8(config_get_string(conf, kConfigSection, "MicSource"));
	cfg.threads = (int)config_get_int(conf, kConfigSection, "Threads");
	if (cfg.threads <= 0 || cfg.threads > 8)
		cfg.threads = 0; /* auto */
	cfg.onlyWithReplayBuffer = config_get_bool(conf, kConfigSection, "OnlyWithReplayBuffer");

	const char *phrases = config_get_string(conf, kConfigSection, "Phrases");
	if (phrases && *phrases) {
		obs_data_t *root = obs_data_create_from_json(phrases);
		obs_data_array_t *arr = root ? obs_data_get_array(root, "phrases") : nullptr;
		if (arr) {
			const size_t count = obs_data_array_count(arr);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(arr, i);
				const char *text = obs_data_get_string(item, "text");
				if (text && *text)
					cfg.phrases << QString::fromUtf8(text);
				obs_data_release(item);
			}
			obs_data_array_release(arr);
		}
		if (root)
			obs_data_release(root);
	}

	if (cfg.phrases.isEmpty())
		cfg.phrases << QStringLiteral("реплей") << QStringLiteral("сделай реплей") << QStringLiteral("save replay");

	return cfg;
}

void VoiceEngine::SaveConfig(const VoiceConfig &cfg)
{
	config_t *conf = App()->GetUserConfig();

	config_set_bool(conf, kConfigSection, "Enabled", cfg.enabled);
	config_set_string(conf, kConfigSection, "Model", cfg.model.toUtf8().constData());
	config_set_string(conf, kConfigSection, "MicSource", cfg.micSource.toUtf8().constData());
	config_set_int(conf, kConfigSection, "Threads", cfg.threads);
	config_set_bool(conf, kConfigSection, "OnlyWithReplayBuffer", cfg.onlyWithReplayBuffer);

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

	config_save_safe(conf, "tmp", nullptr);
}

/* ------------------------------------------------------------------------- */
/* Model paths                                                               */
/* ------------------------------------------------------------------------- */

QString VoiceEngine::ModelPath(const QString &model)
{
	char path[512];
	const QString relative = QStringLiteral("AkvariumOBS/voice/ggml-%1.bin").arg(model);
	if (GetAppConfigPath(path, sizeof(path), relative.toUtf8().constData()) <= 0)
		return QString();

	return QString::fromUtf8(path);
}

QString VoiceEngine::ModelUrl(const QString &model)
{
	return QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%1.bin").arg(model);
}

bool VoiceEngine::ModelReady(const QString &model)
{
	const QString path = ModelPath(model);
	if (path.isEmpty())
		return false;

	return os_file_exists(path.toUtf8().constData());
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

	QString model;
	{
		config_t *conf = App()->GetUserConfig();
		model = QString::fromUtf8(config_get_string(conf, kConfigSection, "Model"));
	}
	if (model.isEmpty())
		model = QStringLiteral("base");

	const QString path = ModelPath(model);
	whisper_context_params contextParams = whisper_context_default_params();
	contextParams.use_gpu = false;
	whisper_context *ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), contextParams);
	if (!ctx)
		return QString();

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

	QString text;
	if (whisper_full(ctx, params, audio.data(), (int)audio.size()) == 0) {
		const int segments = whisper_full_n_segments(ctx);
		for (int i = 0; i < segments; i++) {
			const char *segmentText = whisper_full_get_segment_text(ctx, i);
			if (segmentText)
				text += QString::fromUtf8(segmentText).trimmed() + QChar(' ');
		}
	}
	whisper_free(ctx);

	text = text.trimmed();
	if (transcriptOut)
		*transcriptOut = text;

	for (const QString &phrase : phrases) {
		if (PhraseMatches(phrase, text))
			return phrase;
	}
	return QString();
}

/* ------------------------------------------------------------------------- */
/* VoiceEngine                                                               */
/* ------------------------------------------------------------------------- */

static VoiceEngine *engineInstance = nullptr;

VoiceEngine::VoiceEngine(OBSBasic *main) : m_main(main) {}

VoiceEngine::~VoiceEngine()
{
	Stop();
}

static void whisper_log_callback(enum ggml_log_level level, const char *text, void *)
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

	/* Route ggml/whisper logs (including Vulkan device info) into the OBS log. */
	whisper_log_set(whisper_log_callback, nullptr);

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
				engineInstance->SetStatus(QStringLiteral("Услышал «%1», но Replay Buffer не запущен")
								  .arg(transcript));
				return;
			}
			obs_frontend_replay_buffer_save();
			engineInstance->SetStatus(QStringLiteral("Сохранён реплей по команде (услышано: «%1»)")
						  .arg(transcript));
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
		SetStatus(QStringLiteral("Выключено"));
		return;
	}

	if (!ModelReady(m_config.model)) {
		SetStatus(QStringLiteral("Модель ggml-%1.bin не скачана").arg(m_config.model));
		return;
	}

	if (m_config.onlyWithReplayBuffer && !obs_frontend_replay_buffer_active()) {
		SetStatus(QStringLiteral("Ожидание запуска Replay Buffer"));
		return;
	}

	m_running = true;
	m_listening = true;
	m_captureThread = std::thread(&VoiceEngine::CaptureLoop, this);
	m_inferThread = std::thread(&VoiceEngine::InferenceLoop, this);

	SetStatus(QStringLiteral("Слушаю микрофон"));
}

void VoiceEngine::Shutdown()
{
	Stop();

	if (m_ctx) {
		whisper_free(m_ctx);
		m_ctx = nullptr;
		m_modelLoaded.clear();
	}
}

void VoiceEngine::Stop()
{
	if (!m_running.exchange(false))
		return;

	m_cv.notify_all();

	if (m_captureThread.joinable())
		m_captureThread.join();
	if (m_inferThread.joinable())
		m_inferThread.join();

	m_listening = false;
}

/* Resolves the microphone selection into a device spec:
   ""                            -> active mic of the current scene (or default)
   "wasapi:<endpoint id>"        -> a Windows capture device
   "loopback:<endpoint id>"      -> a Windows render device captured via loopback
   "source:<OBS source name>"    -> device used by an OBS audio source          */
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
			/* Global audio devices are private sources: look through mixer channels. */
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
		/* Follow the active scene: first active audio capture source wins. */
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
		} else if (sourceId && strstr(sourceId, "output_capture")) {
			spec = QStringLiteral("loopback:default");
		}
		obs_data_release(settings);
		obs_source_release(source);
	}

	return spec;
}

void VoiceEngine::CaptureLoop()
{
	const QString spec = ResolveDeviceSpec();
	m_loopback = spec.startsWith(QStringLiteral("loopback:"));

	IMMDevice *device = nullptr;
	IAudioClient *client = nullptr;
	IAudioCaptureClient *capture = nullptr;
	int sampleRate = 0;

	const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(comResult)) {
		SetStatus(QStringLiteral("COM init failed"));
		return;
	}

	if (!OpenCapture(spec, (void **)&device, (void **)&client, (void **)&capture, &sampleRate)) {
		blog(LOG_WARNING, "[AkvariumVoice] cannot open audio device '%s'", spec.toUtf8().constData());
		SetStatus(QStringLiteral("Cannot open microphone"));
		CoUninitialize();
		return;
	}

	blog(LOG_INFO, "[AkvariumVoice] capturing '%s' at %d Hz, %d ch%s", spec.toUtf8().constData(), m_sampleRate,
	     m_channels, m_loopback ? " (loopback)" : "");

	ULONGLONG lastReport = GetTickCount64();
	UINT64 totalPackets = 0;
	UINT64 totalFrames = 0;

	while (m_running) {
		Sleep(10);

		UINT32 packetSize = 0;
		HRESULT nextResult = capture->GetNextPacketSize(&packetSize);
		if (FAILED(nextResult)) {
			blog(LOG_WARNING, "[AkvariumVoice] GetNextPacketSize failed 0x%08lX", (unsigned long)nextResult);
			break;
		}

		const ULONGLONG now = GetTickCount64();
		if (now - lastReport >= 3000) {
			blog(LOG_DEBUG, "[AkvariumVoice] capture stats: packets=%llu frames=%llu",
			     (unsigned long long)totalPackets, (unsigned long long)totalFrames);
			lastReport = now;
		}

		while (packetSize > 0) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;

			if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
				break;

			totalPackets++;
			totalFrames += frames;
			if (frames > 0) {
				if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
					std::vector<float> silence((size_t)frames * m_channels, 0.0f);
					Feed(silence.data(), frames, m_channels);
				} else {
					Feed(reinterpret_cast<const float *>(data), frames, m_channels);
				}
			}

			capture->ReleaseBuffer(frames);

			nextResult = capture->GetNextPacketSize(&packetSize);
			if (FAILED(nextResult))
				break;
		}
	}

	client->Stop();
	capture->Release();
	client->Release();
	device->Release();
	CoUninitialize();

	m_listening = false;
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
		/* OBS itself uses the console role for "Default" devices. */
		const EDataFlow flow = loopback ? eRender : eCapture;
		if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
			enumerator->Release();
			return false;
		}
	}

	if (device) {
		IPropertyStore *props = nullptr;
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
			PROPVARIANT value;
			PropVariantInit(&value);
			if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
				blog(LOG_INFO, "[AkvariumVoice] device: '%s'",
				     QString::fromWCharArray(value.pwszVal).toUtf8().constData());
			PropVariantClear(&value);
			props->Release();
		}
	}

	enumerator->Release();

	IAudioClient *client = nullptr;
	if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) {
		device->Release();
		return false;
	}

	/* Ask WASAPI to convert to 16 kHz mono float. If the device refuses, fall
	   back to its mix format and resample ourselves. */
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

		if (mixFormat->wBitsPerSample != 32 ||
		    mixFormat->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
			/* Only float32 mix formats are handled; most Windows devices use them. */
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

void VoiceEngine::Feed(const float *data, size_t frames, int channels)
{
	if (channels < 1)
		channels = 1;

	/* Downmix to mono. */
	std::vector<float> mono(frames);
	for (size_t i = 0; i < frames; i++) {
		float sum = 0.0f;
		for (int c = 0; c < channels; c++)
			sum += data[i * (size_t)channels + c];
		mono[i] = sum / channels;
	}

	/* Linear resample to 16 kHz, keeping phase across packets. */
	std::vector<float> pcm16k;
	if (m_sampleRate == 16000) {
		pcm16k = std::move(mono);
	} else {
		const double ratio = 16000.0 / (double)m_sampleRate;
		const size_t outCount = (size_t)((mono.size() + m_phase) * ratio);
		pcm16k.resize(outCount);

		for (size_t i = 0; i < outCount; i++) {
			const double pos = (double)i / ratio - m_phase;
			const size_t i0 = (size_t)pos;
			const double frac = pos - i0;
			const float a = i0 < mono.size() ? mono[i0] : m_lastSample;
			const float b = (i0 + 1) < mono.size() ? mono[i0 + 1] : a;
			pcm16k[i] = (float)(a * (1.0 - frac) + b * frac);
		}
		m_phase = (double)outCount / ratio - (double)mono.size() + m_phase;
		if (!mono.empty())
			m_lastSample = mono.back();
	}

	const size_t frameSize = 320; /* 20 ms at 16 kHz */

	/* Packets are often shorter than one analysis frame (10 ms), so keep the
	   remainder instead of dropping it. */
	m_residual.insert(m_residual.end(), pcm16k.begin(), pcm16k.end());

	size_t offset = 0;

	while (offset + frameSize <= m_residual.size()) {
		double sum = 0.0;
		for (size_t i = 0; i < frameSize; i++) {
			const double v = m_residual[offset + i];
			sum += v * v;
		}
		const double rms = std::sqrt(sum / frameSize);

		/* Adaptive gate: speech starts well above the ambient floor and ends
		   close to it, with hysteresis so steady noise never holds the gate
		   open. No user-facing sensitivity knob. */
		const double startThreshold = std::max(0.0045, m_noiseFloor * 3.0);
		const double endThreshold = std::max(0.0025, m_noiseFloor * 1.8);

		if (!m_inSpeech) {
			m_history.insert(m_history.end(), m_residual.begin() + (long)offset,
					 m_residual.begin() + (long)(offset + frameSize));
			if (m_history.size() > 3200)
				m_history.erase(m_history.begin(), m_history.begin() + (long)(m_history.size() - 3200));

			if (rms > startThreshold) {
				m_inSpeech = true;
				m_silenceMs = 0;
				m_segment = m_history;
				blog(LOG_DEBUG, "[AkvariumVoice] speech start (rms %.4f, gate %.4f)", rms, startThreshold);
			} else {
				m_noiseFloor = 0.97 * m_noiseFloor + 0.03 * std::min(rms, 0.03);
			}
		}

		if (m_inSpeech) {
			if (m_segment.size() < 16000 * 4) {
				m_segment.insert(m_segment.end(), m_residual.begin() + (long)offset,
						 m_residual.begin() + (long)(offset + frameSize));
			}

			if (rms <= endThreshold)
				m_silenceMs += 20;
			else
				m_silenceMs = 0;

			/* 350 ms tail: enough to keep the last word, short enough that the
			   command fires quickly. */
			if (m_silenceMs >= 350 || m_segment.size() >= 16000 * 4)
				FinalizeSegment(m_segment);
		}

		offset += frameSize;
	}

	if (offset > 0)
		m_residual.erase(m_residual.begin(), m_residual.begin() + (long)offset);
}

void VoiceEngine::FinalizeSegment(std::vector<float> &segment)
{
	const size_t minSamples = 16000 * 3 / 10; /* 300 ms */

	double peak = 0.0;
	for (float sample : segment) {
		const double magnitude = std::fabs((double)sample);
		if (magnitude > peak)
			peak = magnitude;
	}

	/* Noise that briefly crossed the gate must not cost a whisper pass. */
	if (segment.size() >= minSamples && segment.size() <= 16000 * 6 && peak >= 0.005) {
		blog(LOG_DEBUG, "[AkvariumVoice] speech segment %.2f s", (double)segment.size() / 16000.0);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_pending = segment;
		m_hasPending = true;
		m_cv.notify_one();
	}

	segment.clear();
	m_inSpeech = false;
	m_silenceMs = 0;
}

void VoiceEngine::InferenceLoop()
{
	/* Recognition must win against a busy game, otherwise the reply lags. */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	const QString model = m_config.model;
	const QString path = ModelPath(model);
	if (path.isEmpty())
		return;

	/* The model is expensive to load; keep it between replay buffer runs. */
	if (!m_ctx || m_modelLoaded != model) {
		if (m_ctx) {
			whisper_free(m_ctx);
			m_ctx = nullptr;
		}

		whisper_context_params contextParams = whisper_context_default_params();
		/* GPU (Vulkan) when the build has it and a device exists; ggml falls
		   back to CPU otherwise. */
		contextParams.use_gpu = true;
		m_ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), contextParams);
		if (!m_ctx) {
			QMetaObject::invokeMethod(this, [this]() { SetStatus(QStringLiteral("Failed to load model")); },
						  Qt::QueuedConnection);
			return;
		}
		m_modelLoaded = model;
		blog(LOG_INFO, "[AkvariumVoice] system info: %s", whisper_print_system_info());
	}

	QMetaObject::invokeMethod(this, [this]() { SetStatus(QStringLiteral("Слушаю микрофон")); },
				  Qt::QueuedConnection);

	/* Thread count is not user-configurable: half of the logical cores keeps
	   the latency low without starving OBS or the game. */
	const int threadCount = std::max(2, std::min(6, QThread::idealThreadCount() / 2));
	blog(LOG_INFO, "[AkvariumVoice] model '%s', %d threads, audio_ctx 768", model.toUtf8().constData(),
	     threadCount);

	const QString prompt = QStringLiteral("Команды: %1.").arg(m_config.phrases.join(QStringLiteral(", ")));
	const QByteArray promptUtf8 = prompt.toUtf8();

	while (m_running) {
		std::vector<float> audio;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait_for(lock, std::chrono::milliseconds(200), [this]() { return m_hasPending || !m_running; });
			if (!m_running)
				break;
			if (!m_hasPending)
				continue;
			audio = m_pending;
			m_hasPending = false;
		}

		whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		params.n_threads = threadCount;
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
		params.language = "auto";
		params.no_timestamps = true;
		params.audio_ctx = 768; /* commands are short: halves encoder cost */
		params.initial_prompt = promptUtf8.constData();

		const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
		if (whisper_full(m_ctx, params, audio.data(), (int)audio.size()) != 0)
			continue;
		const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;

		const int segments = whisper_full_n_segments(m_ctx);
		QString text;
		for (int i = 0; i < segments; i++) {
			const char *segmentText = whisper_full_get_segment_text(m_ctx, i);
			if (segmentText)
				text += QString::fromUtf8(segmentText).trimmed() + QChar(' ');
		}
		text = text.trimmed();

		blog(LOG_INFO, "[AkvariumVoice] heard in %lld ms (%.2f s audio): '%s'", (long long)elapsed,
		     (double)audio.size() / 16000.0, text.toUtf8().constData());

		if (text.isEmpty())
			continue;

		QMetaObject::invokeMethod(this, [this, text]() { HandleTranscript(text); }, Qt::QueuedConnection);
	}

	/* Context stays loaded for the next run; freed in Shutdown(). */
}

void VoiceEngine::HandleTranscript(const QString &text)
{
	SetTranscript(text);

	bool matched = false;
	QString matchedPhrase;
	for (const QString &phrase : m_config.phrases) {
		if (PhraseMatches(phrase, text)) {
			matched = true;
			matchedPhrase = phrase;
			break;
		}
	}

	blog(LOG_INFO, "[AkvariumVoice] match: %s", matched ? matchedPhrase.toUtf8().constData() : "none");

	if (matched)
		emit Triggered(matchedPhrase, text);
}

/* ------------------------------------------------------------------------- */
/* Model downloader                                                          */
/* ------------------------------------------------------------------------- */

VoiceModelDownloader::VoiceModelDownloader(QObject *parent) : QObject(parent) {}

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
	curl_off_t total = 0;
	int percent = -1;
	VoiceModelDownloader *self = nullptr;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *state = static_cast<DownloadState *>(userdata);
	const size_t written = fwrite(ptr, size, nmemb, state->file);
	const curl_off_t bytes = (curl_off_t)(size * nmemb);
	state->total += bytes;

	curl_off_t length = 0;
	if (state->self && state->total > 0) {
		/* Content length is unknown here; percentage is computed in Run(). */
		length = state->total;
	}
	UNUSED_PARAMETER(length);
	return written;
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	auto *state = static_cast<DownloadState *>(clientp);
	UNUSED_PARAMETER(ultotal);
	UNUSED_PARAMETER(ulnow);

	if (dltotal > 0 && state->self) {
		const int percent = (int)((dlnow * 100) / dltotal);
		if (percent != state->percent) {
			state->percent = percent;
			QMetaObject::invokeMethod(state->self, [state, percent]() { emit state->self->Progress(percent); },
						  Qt::QueuedConnection);
		}
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
		QMetaObject::invokeMethod(this, [this]() { emit Finished(false, QStringLiteral("Нет пути конфигурации")); },
					  Qt::QueuedConnection);
		return;
	}

	FILE *file = fopen(partial.toUtf8().constData(), "wb");
	if (!file) {
		QMetaObject::invokeMethod(this, [this]() { emit Finished(false, QStringLiteral("Не удалось создать файл")); },
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

		const CURLcode result = curl_easy_perform(curl);
		ok = result == CURLE_OK;
		curl_easy_cleanup(curl);
	}

	fclose(file);

	if (!ok || m_cancel) {
		os_unlink(partial.toUtf8().constData());
		const QString error = m_cancel ? QStringLiteral("Отменено") : QStringLiteral("Ошибка загрузки");
		QMetaObject::invokeMethod(this, [this, error]() { emit Finished(false, error); }, Qt::QueuedConnection);
		return;
	}

	if (os_safe_replace(target.toUtf8().constData(), partial.toUtf8().constData(), nullptr) != 0) {
		os_unlink(partial.toUtf8().constData());
		QMetaObject::invokeMethod(this, [this]() { emit Finished(false, QStringLiteral("Не удалось сохранить")); },
					  Qt::QueuedConnection);
		return;
	}

	QMetaObject::invokeMethod(this, [this]() { emit Finished(true, QString()); }, Qt::QueuedConnection);
}

} // namespace akvarium
