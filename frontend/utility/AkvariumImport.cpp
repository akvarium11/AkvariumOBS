/******************************************************************************
    AkvariumOBS: first-run import of an existing OBS Studio installation.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumImport.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QString>

#include <cstdio>
#include <system_error>

namespace akvarium {

static bool CopyTree(const std::filesystem::path &from, const std::filesystem::path &to)
{
	std::error_code ec;

	if (!std::filesystem::exists(from, ec))
		return false;

	std::filesystem::create_directories(to, ec);
	std::filesystem::copy(from, to,
			      std::filesystem::copy_options::recursive |
				      std::filesystem::copy_options::overwrite_existing,
			      ec);

	blog(LOG_INFO, "[AkvariumImport] copied '%s' -> '%s'", from.u8string().c_str(), to.u8string().c_str());
	return !ec;
}

/* Reads a value from an ini file without pulling in the OBS config machinery. */
static std::string ReadIniValue(const std::filesystem::path &file, const char *section, const char *key)
{
	QFile f(QString::fromStdString(file.u8string()));
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return {};

	const QByteArray data = f.readAll();
	const QByteArray sectionHeader = QByteArray("[") + section + "]";
	const QByteArray keyPrefix = QByteArray(key) + "=";

	bool inSection = false;
	for (const QByteArray &rawLine : data.split('\n')) {
		const QByteArray line = rawLine.trimmed();
		if (line.startsWith('[') && line.endsWith(']')) {
			inSection = line == sectionHeader;
			continue;
		}
		if (inSection && line.startsWith(keyPrefix))
			return line.mid(keyPrefix.size()).trimmed().toStdString();
	}

	return {};
}

static std::filesystem::path WindowsOBSConfigDir()
{
	const char *appData = getenv("APPDATA");
	if (!appData || !*appData)
		return {};

	return std::filesystem::u8path(appData) / "obs-studio";
}

ImportResult OfferImportFromOBS(const std::filesystem::path &ourConfigDir)
{
	ImportResult result;

	const std::filesystem::path marker = ourConfigDir / ".akvarium-import-done";
	std::error_code ec;
	if (std::filesystem::exists(marker, ec))
		return result;

	const std::filesystem::path source = WindowsOBSConfigDir();
	if (source.empty() || !std::filesystem::exists(source / "basic" / "scenes", ec)) {
		std::filesystem::create_directories(ourConfigDir, ec);
		os_quick_write_utf8_file(marker.u8string().c_str(), "none", 4, false);
		return result;
	}

	/* Same directory (non-portable build): nothing to import. */
	std::error_code ecSource, ecOurs;
	const bool sameDir =
		std::filesystem::equivalent(source, ourConfigDir, ecSource) && !ecSource;
	if (sameDir) {
		os_quick_write_utf8_file(marker.u8string().c_str(), "none", 4, false);
		return result;
	}

	result.asked = true;

	QString text = QStringLiteral(
		"OBS Studio was found on this PC.\n\n"
		"Copy its profiles and scene collections into AkvariumOBS?\n\n"
		"Source: %1").arg(QString::fromStdString(source.u8string()));

	QMessageBox box(QMessageBox::Question, QStringLiteral("Import OBS settings"), text, QMessageBox::NoButton);
	QPushButton *yesButton = box.addButton(QStringLiteral("Yes, copy my OBS setup"), QMessageBox::YesRole);
	box.addButton(QStringLiteral("No, start empty"), QMessageBox::NoRole);
	box.setDefaultButton(yesButton);

	box.exec();
	const bool copy = box.clickedButton() == yesButton;

	if (copy) {
		bool ok = true;
		ok &= CopyTree(source / "basic" / "profiles", ourConfigDir / "basic" / "profiles");
		ok &= CopyTree(source / "basic" / "scenes", ourConfigDir / "basic" / "scenes");

		/* Last used profile and collection, so the copy is actually opened. */
		const std::filesystem::path sourceUserIni = source / "user.ini";
		result.profileName = ReadIniValue(sourceUserIni, "Basic", "Profile");
		result.sceneCollectionName = ReadIniValue(sourceUserIni, "Basic", "SceneCollection");

		result.imported = ok;
		blog(LOG_INFO, "[AkvariumImport] import %s (profile '%s', collection '%s')", ok ? "done" : "failed",
		     result.profileName.c_str(), result.sceneCollectionName.c_str());
	} else {
		blog(LOG_INFO, "[AkvariumImport] user declined the import");
	}

	os_quick_write_utf8_file(marker.u8string().c_str(), copy ? "imported" : "declined", 8, false);
	return result;
}

} // namespace akvarium
