/******************************************************************************
    AkvariumOBS: clip sound for the replay buffer (implementation).

    Playback goes through MCI so .wav and .mp3 clips play on the default
    output device without pulling in an extra audio library: the same audio
    the rest of Windows produces, so OBS desktop capture picks it up too.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumReplaySound.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>

namespace akvarium {

const char *kReplaySoundSection = "AkvariumReplaySound";

static const char *kSoundExtensions[] = {"wav", "mp3", "wma", "m4a"};
static const char *kSoundAlias = "akvarium_clipsound";

/* ------------------------------------------------------------------------- */
/* Folder and file discovery                                                 */
/* ------------------------------------------------------------------------- */

QString ReplaySoundDir()
{
	char path[512];
	if (GetAppConfigPath(path, sizeof(path), "AkvariumOBS/sounds") > 0)
		return QString::fromUtf8(path);

	return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
		.filePath(QStringLiteral("sounds"));
}

bool EnsureReplaySoundDir()
{
	const QString dir = ReplaySoundDir();
	if (dir.isEmpty())
		return false;
	if (QDir(dir).exists())
		return true;

	return QDir().mkpath(dir);
}

QString ReplaySoundPath(const QString &file)
{
	if (QFileInfo(file).isAbsolute())
		return file;

	return QDir(ReplaySoundDir()).filePath(file);
}

QStringList ReplaySoundFiles()
{
	QStringList files;

	const QString dir = ReplaySoundDir();
	if (dir.isEmpty() || !QDir(dir).exists())
		return files;

	QDir base(dir);
	QStringList filters;
	for (const char *ext : kSoundExtensions)
		filters << QStringLiteral("*.") + QString::fromLatin1(ext);

	QDirIterator it(dir, filters, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		it.next();
		files << base.relativeFilePath(it.filePath());
	}

	files.sort(Qt::CaseInsensitive);
	return files;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

ReplaySoundConfig LoadReplaySoundConfig(config_t *conf)
{
	ReplaySoundConfig cfg;
	if (!conf)
		return cfg;

	cfg.enabled = config_get_bool(conf, kReplaySoundSection, "Enabled");
	const char *sound = config_get_string(conf, kReplaySoundSection, "Sound");
	if (sound && *sound)
		cfg.sound = QString::fromUtf8(sound);

	return cfg;
}

void SaveReplaySoundConfig(config_t *conf, const ReplaySoundConfig &cfg)
{
	if (!conf)
		return;

	config_set_bool(conf, kReplaySoundSection, "Enabled", cfg.enabled);
	config_set_string(conf, kReplaySoundSection, "Sound", cfg.sound.toUtf8().constData());
	config_save_safe(conf, "tmp", nullptr);
}

/* ------------------------------------------------------------------------- */
/* Playback (MCI)                                                            */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32

static bool mciOpen = false;
static QTimer *mciCloseTimer = nullptr;

static MCIERROR MciCommand(const QString &command, QString *result = nullptr)
{
	wchar_t buffer[512] = {};
	const MCIERROR err = mciSendStringW(command.toStdWString().c_str(), buffer, 512, nullptr);

	if (result)
		*result = QString::fromWCharArray(buffer).trimmed();

	return err;
}

static QString MciErrorText(MCIERROR err)
{
	wchar_t buffer[MAXERRORLENGTH] = {};
	if (mciGetErrorStringW(err, buffer, MAXERRORLENGTH))
		return QString::fromWCharArray(buffer);

	return QStringLiteral("MCI error %1").arg(int(err));
}

static void ScheduleMciClose(int durationMs)
{
	if (!mciCloseTimer) {
		mciCloseTimer = new QTimer;
		mciCloseTimer->setSingleShot(true);
		QObject::connect(mciCloseTimer, &QTimer::timeout, mciCloseTimer, []() { StopReplaySound(); });
	}

	/* Close the handle once the file has played out, so the user can move or
	   delete clips without restarting the application. */
	mciCloseTimer->start(durationMs > 0 ? durationMs + 500 : 30000);
}

#endif

void StopReplaySound()
{
#ifdef _WIN32
	if (mciCloseTimer)
		mciCloseTimer->stop();
	if (!mciOpen)
		return;

	mciSendStringW(L"stop akvarium_clipsound", nullptr, 0, nullptr);
	mciSendStringW(L"close akvarium_clipsound", nullptr, 0, nullptr);
	mciOpen = false;
#endif
}

bool PlayReplaySound(const QString &file, QString *error)
{
	if (file.isEmpty()) {
		if (error)
			*error = QStringLiteral("No sound selected");
		return false;
	}

	const QString path = ReplaySoundPath(file);
	if (!os_file_exists(path.toUtf8().constData())) {
		if (error)
			*error = QStringLiteral("File not found: %1").arg(QDir::toNativeSeparators(path));
		return false;
	}

#ifdef _WIN32
	StopReplaySound();

	/* "waveaudio" only understands RIFF WAV; everything else is handed to the
	   DirectShow MCI device, which decodes mp3/wma/m4a. */
	const QString suffix = QFileInfo(path).suffix().toLower();
	const QString type = suffix == QStringLiteral("wav") ? QStringLiteral("waveaudio")
							    : QStringLiteral("mpegvideo");

	const QString openCmd = QStringLiteral("open \"%1\" type %2 alias %3")
					.arg(QDir::toNativeSeparators(path), type,
					     QString::fromLatin1(kSoundAlias));

	MCIERROR err = MciCommand(openCmd);
	if (err != 0) {
		if (error)
			*error = MciErrorText(err);
		return false;
	}
	mciOpen = true;

	MciCommand(QStringLiteral("set %1 time format milliseconds").arg(QString::fromLatin1(kSoundAlias)));

	QString lengthText;
	MciCommand(QStringLiteral("status %1 length").arg(QString::fromLatin1(kSoundAlias)), &lengthText);

	bool haveLength = false;
	const int lengthMs = lengthText.toInt(&haveLength);

	err = MciCommand(QStringLiteral("play %1").arg(QString::fromLatin1(kSoundAlias)));
	if (err != 0) {
		if (error)
			*error = MciErrorText(err);
		StopReplaySound();
		return false;
	}

	ScheduleMciClose(haveLength ? lengthMs : 0);
	return true;
#else
	if (error)
		*error = QStringLiteral("Clip sounds are only supported on Windows");
	return false;
#endif
}

void PlayReplaySoundForSave(config_t *conf)
{
	if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
		QMetaObject::invokeMethod(QCoreApplication::instance(), [conf]() { PlayReplaySoundForSave(conf); },
					  Qt::QueuedConnection);
		return;
	}

	const ReplaySoundConfig cfg = LoadReplaySoundConfig(conf);
	if (!cfg.enabled || cfg.sound.isEmpty())
		return;

	QString error;
	if (!PlayReplaySound(cfg.sound, &error))
		blog(LOG_WARNING, "[clipsound] could not play '%s': %s", cfg.sound.toUtf8().constData(),
		     error.toUtf8().constData());
	else
		blog(LOG_INFO, "[clipsound] playing '%s'", cfg.sound.toUtf8().constData());
}

/* ------------------------------------------------------------------------- */
/* Settings widget                                                           */
/* ------------------------------------------------------------------------- */

ReplaySoundSettings::ReplaySoundSettings(QWidget *parent)
	: QGroupBox(QStringLiteral("Replay Sound (Replay Buffer)"), parent)
{
	auto *layout = new QVBoxLayout(this);

	m_enabled = new QCheckBox(QStringLiteral("Play a sound when a replay is saved"), this);
	layout->addWidget(m_enabled);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	m_sound = new QComboBox(this);
	form->addRow(QStringLiteral("Sound:"), m_sound);

	auto *buttons = new QHBoxLayout();
	m_preview = new QPushButton(QStringLiteral("Preview"), this);
	m_refresh = new QPushButton(QStringLiteral("Refresh list"), this);
	m_open = new QPushButton(QStringLiteral("Open sounds folder"), this);
	buttons->addWidget(m_preview);
	buttons->addWidget(m_refresh);
	buttons->addWidget(m_open);
	buttons->addStretch(1);
	form->addRow(QString(), buttons);

	m_hint = new QLabel(this);
	m_hint->setWordWrap(true);
	m_hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("Folder:"), m_hint);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	form->addRow(QString(), m_status);

	layout->addLayout(form);

	connect(m_enabled, &QCheckBox::toggled, this, [this]() {
		MarkChanged();
		UpdateEnabled();
	});
	connect(m_sound, &QComboBox::currentIndexChanged, this, [this]() {
		MarkChanged();
		UpdateEnabled();
	});
	connect(m_preview, &QPushButton::clicked, this, [this]() { Preview(); });
	connect(m_refresh, &QPushButton::clicked, this, [this]() { Refresh(); });
	connect(m_open, &QPushButton::clicked, this, [this]() { OpenFolder(); });

	Refresh();
}

void ReplaySoundSettings::Refresh(const QString &select)
{
	const QString wanted = !select.isEmpty() ? select : m_sound->currentData().toString();

	m_sound->blockSignals(true);
	m_sound->clear();
	m_sound->addItem(QStringLiteral("None"), QString());

	const QStringList files = ReplaySoundFiles();
	for (const QString &file : files)
		m_sound->addItem(file, file);

	int index = m_sound->findData(wanted);
	if (index < 0 && !wanted.isEmpty()) {
		m_sound->addItem(QStringLiteral("%1 (missing)").arg(wanted), wanted);
		index = m_sound->count() - 1;
	}
	m_sound->setCurrentIndex(std::max(0, index));
	m_sound->blockSignals(false);

	EnsureReplaySoundDir();
	m_hint->setText(QStringLiteral("%1\nSupported files: .wav, .mp3, .wma, .m4a")
				.arg(QDir::toNativeSeparators(ReplaySoundDir())));
	m_status->clear();

	UpdateEnabled();
}

void ReplaySoundSettings::Load(config_t *conf)
{
	m_loading = true;

	const ReplaySoundConfig cfg = LoadReplaySoundConfig(conf);
	m_enabled->setChecked(cfg.enabled);
	m_sound->clear();
	Refresh(cfg.sound);

	m_dirty = false;
	m_loading = false;
}

void ReplaySoundSettings::Save(config_t *conf)
{
	ReplaySoundConfig cfg;
	cfg.enabled = m_enabled->isChecked();
	cfg.sound = m_sound->currentData().toString();
	SaveReplaySoundConfig(conf, cfg);

	m_dirty = false;
}

void ReplaySoundSettings::MarkChanged()
{
	m_dirty = true;
	if (!m_loading)
		emit Changed();
}

void ReplaySoundSettings::UpdateEnabled()
{
	/* The list stays usable so a sound can be auditioned before the feature is
	   switched on; only the preview depends on an actual selection. */
	m_preview->setEnabled(!m_sound->currentData().toString().isEmpty());
}

void ReplaySoundSettings::Preview()
{
	const QString file = m_sound->currentData().toString();
	if (file.isEmpty()) {
		m_status->setText(QStringLiteral("Choose a sound first."));
		return;
	}

	QString error;
	if (PlayReplaySound(file, &error))
		m_status->setText(QStringLiteral("Playing %1").arg(file));
	else
		m_status->setText(error);
}

void ReplaySoundSettings::OpenFolder()
{
	EnsureReplaySoundDir();
	QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::toNativeSeparators(ReplaySoundDir())));
}

} // namespace akvarium
