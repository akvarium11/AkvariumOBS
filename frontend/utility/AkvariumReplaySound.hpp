/******************************************************************************
    AkvariumOBS: clip sound for the replay buffer.

    The user drops audio files into a folder under the user configuration
    directory and picks one in Settings > Output > Replay Buffer. The selected
    file is played on the default output device every time the replay buffer
    starts writing a clip (hotkey, menu, tray or a voice command).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QGroupBox>
#include <QString>
#include <QStringList>

#include <util/config-file.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

/* Defined in frontend/OBSApp.cpp; declared here so this module does not pull in
   the whole main window just to resolve the configuration directory. */
int GetAppConfigPath(char *path, size_t size, const char *name);

namespace akvarium {

struct ReplaySoundConfig {
	bool enabled = false;
	/* Path relative to the sounds folder; subfolders are allowed. */
	QString sound;
	/* 0..100, applied to playback. */
	int volume = 100;
};

/* Name of the settings section in the user config. */
extern const char *kReplaySoundSection;

/* %APPDATA%\obs-studio\AkvariumOBS\sounds, or its portable equivalent. */
QString ReplaySoundDir();
bool EnsureReplaySoundDir();
QString ReplaySoundPath(const QString &file);

/* Supported files (wav/mp3/wma/m4a), relative to the sounds folder, sorted. */
QStringList ReplaySoundFiles();

ReplaySoundConfig LoadReplaySoundConfig(config_t *conf);
void SaveReplaySoundConfig(config_t *conf, const ReplaySoundConfig &cfg);

/* Starts playback on the default output device; does not block. */
bool PlayReplaySound(const QString &file, int volume = 100, QString *error = nullptr);
void StopReplaySound();

/* Plays the configured sound if the feature is enabled. GUI thread only. */
void PlayReplaySoundForSave(config_t *conf);

class ReplaySoundSettings : public QGroupBox {
	Q_OBJECT

public:
	explicit ReplaySoundSettings(QWidget *parent = nullptr);

	void Load(config_t *conf);
	void Save(config_t *conf);

	bool HasChanges() const { return m_dirty; }

signals:
	void Changed();

private:
	void MarkChanged();
	void Refresh(const QString &select = QString());
	void Preview();
	void OpenFolder();
	void UpdateEnabled();

	QCheckBox *m_enabled = nullptr;
	QComboBox *m_sound = nullptr;
	QSlider *m_volume = nullptr;
	QLabel *m_volumeLabel = nullptr;
	QPushButton *m_preview = nullptr;
	QPushButton *m_refresh = nullptr;
	QPushButton *m_open = nullptr;
	QLabel *m_hint = nullptr;
	QLabel *m_status = nullptr;

	bool m_dirty = false;
	bool m_loading = false;
};

} // namespace akvarium
