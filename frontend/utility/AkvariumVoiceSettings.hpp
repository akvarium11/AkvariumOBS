/******************************************************************************
    AkvariumOBS: voice command settings widget (Replay Buffer settings page).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "AkvariumVoice.hpp"

#include <QGroupBox>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSlider;
class QWidget;

namespace akvarium {

class VoiceCommandSettings : public QGroupBox {
	Q_OBJECT

public:
	explicit VoiceCommandSettings(QWidget *parent = nullptr);

	void Load();
	void Save();

	bool HasChanges() const { return m_dirty; }

signals:
	void Changed();

private:
	void MarkChanged();
	void SetLoading(bool loading);
	void RefreshMics();
	void UpdateStatus();
	void DownloadModel();
	void AddPhraseManual();
	void StartTrainingPhrase();
	void StartTestingPhrase();
	void CancelCurrentOperation();
	void RebuildPhraseButtons();
	void RemovePhrase(const QString &phrase);

	QCheckBox *m_enabled = nullptr;
	QComboBox *m_mic = nullptr;
	QComboBox *m_language = nullptr;
	QSlider *m_sensitivity = nullptr;
	QLabel *m_sensitivityLabel = nullptr;
	QCheckBox *m_onlyReplayBuffer = nullptr;

	/* Enrolled phrases list and button pills */
	QStringList m_phraseList;
	QScrollArea *m_phrasesScroll = nullptr;
	QWidget *m_phrasesContainer = nullptr;
	QGridLayout *m_phrasesGrid = nullptr;

	QPushButton *m_addBtn = nullptr;
	QPushButton *m_trainBtn = nullptr;
	QPushButton *m_testBtn = nullptr;
	QPushButton *m_cancelBtn = nullptr;
	QLabel *m_recordFeedback = nullptr;

	QPushButton *m_downloadBtn = nullptr;
	QProgressBar *m_progress = nullptr;

	QLabel *m_status = nullptr;
	QLabel *m_transcript = nullptr;

	bool m_dirty = false;
	bool m_loading = false;
};

} // namespace akvarium
