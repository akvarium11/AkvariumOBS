/******************************************************************************
    AkvariumOBS: voice-controlled Replay Buffer integration.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class OBSBasic;

struct whisper_context;

namespace akvarium {

struct VoiceConfig {
	bool enabled = false;
	QStringList phrases;
	QString model;
	QString micSource;
	QString language;
	int threads = 0;
	int sensitivity = 3; /* 1 (low) .. 5 (high), default 3 */
	bool onlyWithReplayBuffer = true;
	QList<QPair<QString, QString>> aliases; /* phrase, recognized variant */
};

struct DecodeConfidence {
	int tokens = 0;
	double meanTokenProb = 0.0;
	double minTokenProb = 0.0;
	double noSpeechProb = 0.0;
};

class VoiceModelDownloader;

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

	static QString AutoModel();
	static QString AutoLanguage(const QStringList &phrases);

	static QString TestRecognize(const QString &wavPath, const QStringList &phrases, QString *transcriptOut);

	void Apply();
	void Stop();
	void ReapplyConfig();

	void DownloadMissingModels();
	bool DownloadingModels() const;

	/* Interactive phrase recording from the microphone */
	void StartPhraseRecording(int seconds = 3);
	void CancelPhraseRecording();
	bool RecordingPhrase() const;

	/* Multi-sample phrase enrollment: prompts user to say phrase N times */
	void StartSampleCollection(const QString &phrase, int totalSamples = 5);
	void CancelSampleCollection();
	bool CollectingSamples() const;

	/* Test recognition against current phrases */
	void StartPhraseTest(int seconds = 3);
	bool TestingPhrase() const;

	bool Listening() const;
	QString LastTranscript() const;
	QString StatusText() const;

	void SetStatus(const QString &status);
	void SetTranscript(const QString &text);

signals:
	void StatusChanged();
	void TranscriptChanged(const QString &text);
	void Triggered(const QString &phrase, const QString &transcript);
	void PhraseRecordProgress(int secondsLeft);
	void PhraseRecorded(const QString &text);
	void PhraseTestFinished(bool matched, const QString &heardText, const QString &matchedPhrase, double score);
	void SampleProgress(int currentSample, int totalSamples, const QString &statusText);
	void SampleCollected(int sampleIndex, const QString &heardText);
	void SampleCollectionFinished(const QString &phrase, const QStringList &learnedVariants);
	void ModelDownloadProgress(int percent);
	void ModelDownloadFinished(bool ok, const QString &message);

private:
	explicit VoiceEngine(OBSBasic *main);
	~VoiceEngine() override;

	void CaptureLoop();
	void InferenceLoop();
	bool OpenCapture(const QString &deviceSpec, void **device, void **client, void **capture, int *sampleRate);
	void Feed(const float *data, size_t frames, int channels);
	void Shutdown();
	void FinalizeSegment(std::vector<float> &segment);
	void HandleTranscript(const QString &text);
	void HandleVerifiedCommand(const QString &phrase, const QString &text, double score);
	void RunInteractiveRecord(int seconds, bool isTest);
	void RunSampleCollection(const QString &phrase, int totalSamples);
	double SpeechStartThreshold() const;
	double SpeechEndThreshold() const;
	QString ResolveLanguage() const;
	QString BuildPrompt() const;
	QString DecodeWith(whisper_context *ctx, const std::vector<float> &audio, DecodeConfidence *confidence);
	QString MatchCommand(const QString &text, double *scoreOut) const;
	void ReleaseContexts();
	QString ResolveDeviceSpec() const;

	OBSBasic *m_main = nullptr;

	std::thread m_captureThread;
	std::thread m_inferThread;
	std::thread m_interactiveThread;
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_recordingPhrase{false};
	std::atomic<bool> m_collectingSamples{false};
	std::atomic<bool> m_testingPhrase{false};
	std::atomic<bool> m_interactiveCancel{false};
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::vector<std::vector<float>> m_pendingQueue;

	whisper_context *m_ctx = nullptr;
	QString m_modelLoaded;

	VoiceModelDownloader *m_downloader = nullptr;

	/* Capture format / resampler */
	int m_sampleRate = 16000;
	int m_channels = 1;
	double m_phase = 0.0;
	float m_lastSample = 0.0f;
	bool m_loopback = false;

	/* VAD state */
	std::vector<float> m_residual;
	std::vector<float> m_history;
	std::vector<float> m_segment;
	double m_noiseFloor = 0.0015;
	int m_silenceMs = 0;
	bool m_inSpeech = false;

	/* Trigger de-duplication */
	qint64 m_lastTriggerMs = 0;
	QString m_lastTriggeredPhrase;

	VoiceConfig m_config;
	QString m_status;
	QString m_transcript;
	std::atomic<bool> m_listening{false};
};

class VoiceModelDownloader : public QObject {
	Q_OBJECT

public:
	explicit VoiceModelDownloader(QObject *parent = nullptr);
	~VoiceModelDownloader() override;

	static QString TargetPath(const QString &model);

	void Start(const QString &model);
	void Cancel();

	bool Downloading() const { return m_thread.joinable(); }
	bool Cancelled() const { return m_cancel.load(); }

signals:
	void Progress(int percent);
	void Finished(bool ok, const QString &error);

private:
	void Run(const QString &model);

	std::atomic<bool> m_cancel{false};
	std::thread m_thread;
};

} // namespace akvarium
