/******************************************************************************
    AkvariumOBS: voice commands for the replay buffer.

    Speech recognition runs locally on whisper.cpp. Audio is captured straight
    from the microphone that the active scene is using (WASAPI), gated by a
    cheap energy VAD, so the neural model only ever runs on short speech
    segments. Idle cost is the capture thread alone.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

class OBSBasic;

struct whisper_context;

namespace akvarium {

struct VoiceConfig {
	bool enabled = false;
	QStringList phrases;
	QString model = QStringLiteral("base");
	QString micSource; /* empty = follow the active scene */
	int threads = 0;   /* 0 = auto from CPU count */
	bool onlyWithReplayBuffer = true;
	bool downloadOnDemand = false;
};

class VoiceEngine : public QObject {
	Q_OBJECT

public:
	static void Init(OBSBasic *main);
	static VoiceEngine *Instance();

	static VoiceConfig LoadConfig();
	static void SaveConfig(const VoiceConfig &config);

	static QString ModelPath(const QString &model);
	static QString ModelUrl(const QString &model);
	static bool ModelReady(const QString &model);

	/* Offline self-test: recognise a WAV file and match it against phrases.
	   Returns the matched phrase (empty if none) and fills transcriptOut. */
	static QString TestRecognize(const QString &wavPath, const QStringList &phrases, QString *transcriptOut);

	void Apply();
	void Stop();
	void ReapplyConfig();

	bool Listening() const;
	QString LastTranscript() const;
	QString StatusText() const;

	void SetStatus(const QString &status);
	void SetTranscript(const QString &text);

signals:
	void StatusChanged();
	void TranscriptChanged(const QString &text);
	void Triggered(const QString &phrase, const QString &transcript);

private:
	explicit VoiceEngine(OBSBasic *main);
	~VoiceEngine() override;

	void CaptureLoop();
	void InferenceLoop();
	bool OpenCapture(const QString &deviceSpec, void **device, void **client, void **capture,
			 int *sampleRate);
	void Feed(const float *data, size_t frames, int channels);
	void Shutdown();
	void FinalizeSegment(std::vector<float> &segment);
	void HandleTranscript(const QString &text);
	QString ResolveDeviceSpec() const;

	OBSBasic *m_main = nullptr;

	std::thread m_captureThread;
	std::thread m_inferThread;
	std::atomic<bool> m_running{false};
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::vector<float> m_pending;
	bool m_hasPending = false;

	whisper_context *m_ctx = nullptr;
	QString m_modelLoaded;

	/* Capture format / resampler state (capture thread only) */
	int m_sampleRate = 16000;
	int m_channels = 1;
	double m_phase = 0.0;
	double m_peakWindow = 0.0;
	size_t m_framesWindow = 0;
	float m_lastSample = 0.0f;
	bool m_loopback = false;

	/* VAD state (capture thread only) */
	std::vector<float> m_residual;
	std::vector<float> m_history;
	std::vector<float> m_segment;
	double m_noiseFloor = 0.0015;
	int m_silenceMs = 0;
	bool m_inSpeech = false;

	VoiceConfig m_config;
	QString m_status;
	QString m_transcript;
	std::atomic<bool> m_listening{false};
};

class VoiceModelDownloader : public QObject {
	Q_OBJECT

public:
	explicit VoiceModelDownloader(QObject *parent = nullptr);

	static QString TargetPath(const QString &model);

	void Start(const QString &model);
	void Cancel();

signals:
	void Progress(int percent);
	void Finished(bool ok, const QString &error);

private:
	void Run(const QString &model);

	std::atomic<bool> m_cancel{false};
	std::thread m_thread;
};

} // namespace akvarium
