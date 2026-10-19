/******************************************************************************
    AkvariumOBS: headless test for the replay buffer clip sound.

    Exercises the production code paths without the frontend: folder discovery,
    config round-trip, and MCI playback of a generated WAV plus a real MP3 and
    a file whose path is non-ASCII.

    Build: cmake --build build_x64 --config RelWithDebInfo --target akvarium-replay-sound-test
    Run:   akvarium-replay-sound-test <empty scratch dir> [reference.mp3]

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <utility/AkvariumReplaySound.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <util/config-file.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

/* Stand-in for the frontend helper: resolves <root>/<name>. */
static QString g_configRoot;

int GetAppConfigPath(char *path, size_t size, const char *name)
{
	const QByteArray root = g_configRoot.toUtf8();
	if (!name || !*name)
		return snprintf(path, size, "%s", root.constData());
	return snprintf(path, size, "%s/%s", root.constData(), name);
}

static bool WriteSineWav(const QString &path, double seconds, double frequency)
{
	const int rate = 44100;
	const int frames = int(rate * seconds);

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;

	auto write32 = [&file](uint32_t v) { file.write(reinterpret_cast<const char *>(&v), 4); };
	auto write16 = [&file](uint16_t v) { file.write(reinterpret_cast<const char *>(&v), 2); };

	const uint32_t dataBytes = uint32_t(frames) * 2;
	file.write("RIFF", 4);
	write32(36 + dataBytes);
	file.write("WAVEfmt ", 8);
	write32(16);
	write16(1);    /* PCM */
	write16(1);    /* mono */
	write32(rate); /* sample rate */
	write32(rate * 2);
	write16(2);
	write16(16);
	file.write("data", 4);
	write32(dataBytes);

	for (int i = 0; i < frames; i++) {
		const double t = double(i) / rate;
		/* Fade in/out so the clip is not a click. */
		const double envelope = std::min(1.0, std::min(t, seconds - t) * 20.0);
		const int16_t sample = int16_t(std::sin(2.0 * 3.14159265358979 * frequency * t) * envelope * 12000);
		file.write(reinterpret_cast<const char *>(&sample), 2);
	}

	file.close();
	return true;
}

static bool CopyFile(const QString &from, const QString &to)
{
	return QFile::copy(from, to);
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	if (argc < 2) {
		fprintf(stderr, "usage: %s <scratch dir> [reference.mp3]\n", argv[0]);
		return 2;
	}

	g_configRoot = QString::fromLocal8Bit(argv[1]);
	QDir().mkpath(g_configRoot);

	printf("config root ......... %s\n", g_configRoot.toUtf8().constData());
	printf("sounds folder ....... %s\n", akvarium::ReplaySoundDir().toUtf8().constData());

	if (!akvarium::EnsureReplaySoundDir()) {
		fprintf(stderr, "FAIL: could not create the sounds folder\n");
		return 1;
	}
	printf("folder created ...... %s\n", QDir(akvarium::ReplaySoundDir()).exists() ? "yes" : "NO");

	/* A plain file, a nested pack and a Cyrillic path must all be listed. */
	const QString dir = akvarium::ReplaySoundDir();
	const QString base = QDir(dir).filePath(QStringLiteral("sine.wav"));
	const QString nestedDir = QDir(dir).filePath(QString::fromUtf8("пак/бипы"));
	QDir().mkpath(nestedDir);
	const QString nested = QDir(nestedDir).filePath(QString::fromUtf8("клип 1.wav"));

	if (!WriteSineWav(base, 0.6, 440.0) || !WriteSineWav(nested, 0.6, 880.0)) {
		fprintf(stderr, "FAIL: could not write the test WAV files\n");
		return 1;
	}
	QFile unsupported(QDir(dir).filePath(QStringLiteral("readme.txt")));
	if (unsupported.open(QIODevice::WriteOnly))
		unsupported.write("not a sound");
	unsupported.close();

	if (argc >= 3)
		CopyFile(QString::fromLocal8Bit(argv[2]), QDir(dir).filePath(QStringLiteral("reference.mp3")));

	const QStringList files = akvarium::ReplaySoundFiles();
	printf("discovered files .... %d\n", int(files.size()));
	for (const QString &file : files)
		printf("  - %s\n", file.toUtf8().constData());

	bool ok = true;

	const QString nestedRel = QDir(dir).relativeFilePath(nested);

	if (files.contains(QStringLiteral("readme.txt"))) {
		fprintf(stderr, "FAIL: unsupported extension was listed\n");
		ok = false;
	}
	if (!files.contains(QStringLiteral("sine.wav")) || !files.contains(nestedRel)) {
		fprintf(stderr, "FAIL: expected files are missing from the list (%s)\n", nestedRel.toUtf8().constData());
		ok = false;
	}

	/* Playback: plain WAV, non-ASCII nested WAV, and MP3 when supplied. */
	auto tryPlay = [&ok](const QString &file) {
		QString error;
		const bool played = akvarium::PlayReplaySound(file, &error);
		printf("play %-28s %s%s\n", file.toUtf8().constData(), played ? "ok" : "FAILED",
		       played ? "" : (" (" + error + ")").toUtf8().constData());
		if (!played)
			ok = false;
		QThread::msleep(800);
		akvarium::StopReplaySound();
	};

	tryPlay(QStringLiteral("sine.wav"));
	tryPlay(QString::fromUtf8("пак/бипы/клип 1.wav"));
	if (argc >= 3)
		tryPlay(QStringLiteral("reference.mp3"));

	QString error;
	if (akvarium::PlayReplaySound(QStringLiteral("missing.wav"), &error)) {
		printf("missing file ........ unexpectedly played\n");
		ok = false;
	} else {
		printf("missing file ........ rejected (%s)\n", error.toUtf8().constData());
	}

	/* Config round-trip through a real config file. */
	config_t *conf = nullptr;
	if (config_open(&conf, "user.ini", CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS) {
		fprintf(stderr, "FAIL: could not open the test config\n");
		return 1;
	}

	akvarium::ReplaySoundConfig saved;
	saved.enabled = true;
	saved.sound = QString::fromUtf8("пак/бипы/клип 1.wav");
	akvarium::SaveReplaySoundConfig(conf, saved);

	const akvarium::ReplaySoundConfig loaded = akvarium::LoadReplaySoundConfig(conf);
	printf("config round-trip ... %s (%s, sound=%s)\n",
	       (loaded.enabled == saved.enabled && loaded.sound == saved.sound) ? "ok" : "FAILED",
	       loaded.enabled ? "enabled" : "disabled", loaded.sound.toUtf8().constData());
	if (loaded.enabled != saved.enabled || loaded.sound != saved.sound)
		ok = false;

	config_close(conf);

	printf("\n%s\n", ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
	return ok ? 0 : 1;
}
