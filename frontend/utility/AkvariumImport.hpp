/******************************************************************************
    AkvariumOBS: first-run import of an existing OBS Studio installation.

    On the very first start we look for a regular OBS Studio config directory
    (%APPDATA%/obs-studio) and offer to copy the user's profiles and scene
    collections, so the fork starts with familiar scenes instead of an empty
    canvas. Runs once; a marker file prevents the question on later starts.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <filesystem>
#include <string>

namespace akvarium {

struct ImportResult {
	bool asked = false;
	bool imported = false;
	std::string profileName;
	std::string sceneCollectionName;
};

/* Detects an existing OBS Studio install and offers to copy its profiles and
   scene collections into `ourConfigDir`. Returns what was decided. */
ImportResult OfferImportFromOBS(const std::filesystem::path &ourConfigDir);

} // namespace akvarium
