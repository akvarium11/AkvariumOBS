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
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

namespace akvarium {

class VoiceCommandSettings : public QGroupBox {
	Q_OBJECT

public:
	explicit VoiceCommandSettings(QWidget *parent = nullptr);

	void Load();
	void Save();

	bool HasChanges() const { return m_dirty; }

private:
	void RefreshMics();
	void UpdateStatus();
	void DownloadModel();

	QCheckBox *m_enabled = nullptr;
	QPlainTextEdit *m_phrases = nullptr;
	QComboBox *m_model = nullptr;
	QComboBox *m_mic = nullptr;
	QSpinBox *m_threads = nullptr;
	QSlider *m_sensitivity = nullptr;
	QCheckBox *m_onlyReplayBuffer = nullptr;
	QLabel *m_status = nullptr;
	QLabel *m_transcript = nullptr;
	QPushButton *m_download = nullptr;
	QProgressBar *m_progress = nullptr;

	VoiceModelDownloader *m_downloader = nullptr;
	bool m_dirty = false;
};

} // namespace akvarium
