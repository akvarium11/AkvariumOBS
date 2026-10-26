/******************************************************************************
    AkvariumOBS: Vosk speech recognition engine dynamic loader and C++ wrapper.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <windows.h>
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace akvarium {

typedef struct VoskModel VoskModel;
typedef struct VoskRecognizer VoskRecognizer;

class VoskApi {
public:
	typedef void (*Fn_vosk_set_log_level)(int);
	typedef VoskModel *(*Fn_vosk_model_new)(const char *);
	typedef void (*Fn_vosk_model_free)(VoskModel *);
	typedef VoskRecognizer *(*Fn_vosk_recognizer_new)(VoskModel *, float);
	typedef VoskRecognizer *(*Fn_vosk_recognizer_new_grm)(VoskModel *, float, const char *);
	typedef void (*Fn_vosk_recognizer_free)(VoskRecognizer *);
	typedef void (*Fn_vosk_recognizer_set_words)(VoskRecognizer *, int);
	typedef void (*Fn_vosk_recognizer_set_partial_words)(VoskRecognizer *, int);
	typedef int (*Fn_vosk_recognizer_accept_waveform)(VoskRecognizer *, const char *, int);
	typedef int (*Fn_vosk_recognizer_accept_waveform_s)(VoskRecognizer *, const short *, int);
	typedef int (*Fn_vosk_recognizer_accept_waveform_f)(VoskRecognizer *, const float *, int);
	typedef const char *(*Fn_vosk_recognizer_result)(VoskRecognizer *);
	typedef const char *(*Fn_vosk_recognizer_partial_result)(VoskRecognizer *);
	typedef const char *(*Fn_vosk_recognizer_final_result)(VoskRecognizer *);
	typedef void (*Fn_vosk_recognizer_reset)(VoskRecognizer *);

	Fn_vosk_set_log_level set_log_level = nullptr;
	Fn_vosk_model_new model_new = nullptr;
	Fn_vosk_model_free model_free = nullptr;
	Fn_vosk_recognizer_new recognizer_new = nullptr;
	Fn_vosk_recognizer_new_grm recognizer_new_grm = nullptr;
	Fn_vosk_recognizer_free recognizer_free = nullptr;
	Fn_vosk_recognizer_set_words recognizer_set_words = nullptr;
	Fn_vosk_recognizer_set_partial_words recognizer_set_partial_words = nullptr;
	Fn_vosk_recognizer_accept_waveform recognizer_accept_waveform = nullptr;
	Fn_vosk_recognizer_accept_waveform_s recognizer_accept_waveform_s = nullptr;
	Fn_vosk_recognizer_accept_waveform_f recognizer_accept_waveform_f = nullptr;
	Fn_vosk_recognizer_result recognizer_result = nullptr;
	Fn_vosk_recognizer_partial_result recognizer_partial_result = nullptr;
	Fn_vosk_recognizer_final_result recognizer_final_result = nullptr;
	Fn_vosk_recognizer_reset recognizer_reset = nullptr;

	static VoskApi &Instance()
	{
		static VoskApi instance;
		return instance;
	}

	bool Load(const QString &explicitPath = QString())
	{
		if (m_loaded)
			return true;

		QStringList candidates;
		if (!explicitPath.isEmpty())
			candidates << explicitPath;

		const QString appDir = QCoreApplication::applicationDirPath();
		candidates << appDir + QStringLiteral("/libvosk.dll")
		           << appDir + QStringLiteral("/../../deps/vosk/bin/libvosk.dll")
		           << appDir + QStringLiteral("/../../../deps/vosk/bin/libvosk.dll")
		           << QStringLiteral("libvosk.dll");

		for (const QString &cand : candidates) {
			const QString nativePath = QDir::toNativeSeparators(cand);
			const int lastSep = nativePath.lastIndexOf(QLatin1Char('\\'));
			if (lastSep > 0) {
				const QString dir = nativePath.left(lastSep);
				SetDllDirectoryW(reinterpret_cast<LPCWSTR>(dir.utf16()));
			}

			m_hLib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr,
			                        LOAD_WITH_ALTERED_SEARCH_PATH);
			if (m_hLib)
				break;
		}

		if (!m_hLib)
			return false;

		set_log_level = reinterpret_cast<Fn_vosk_set_log_level>(GetProcAddress(m_hLib, "vosk_set_log_level"));
		model_new = reinterpret_cast<Fn_vosk_model_new>(GetProcAddress(m_hLib, "vosk_model_new"));
		model_free = reinterpret_cast<Fn_vosk_model_free>(GetProcAddress(m_hLib, "vosk_model_free"));
		recognizer_new = reinterpret_cast<Fn_vosk_recognizer_new>(GetProcAddress(m_hLib, "vosk_recognizer_new"));
		recognizer_new_grm = reinterpret_cast<Fn_vosk_recognizer_new_grm>(GetProcAddress(m_hLib, "vosk_recognizer_new_grm"));
		recognizer_free = reinterpret_cast<Fn_vosk_recognizer_free>(GetProcAddress(m_hLib, "vosk_recognizer_free"));
		recognizer_set_words = reinterpret_cast<Fn_vosk_recognizer_set_words>(GetProcAddress(m_hLib, "vosk_recognizer_set_words"));
		recognizer_set_partial_words = reinterpret_cast<Fn_vosk_recognizer_set_partial_words>(GetProcAddress(m_hLib, "vosk_recognizer_set_partial_words"));
		recognizer_accept_waveform = reinterpret_cast<Fn_vosk_recognizer_accept_waveform>(GetProcAddress(m_hLib, "vosk_recognizer_accept_waveform"));
		recognizer_accept_waveform_s = reinterpret_cast<Fn_vosk_recognizer_accept_waveform_s>(GetProcAddress(m_hLib, "vosk_recognizer_accept_waveform_s"));
		recognizer_accept_waveform_f = reinterpret_cast<Fn_vosk_recognizer_accept_waveform_f>(GetProcAddress(m_hLib, "vosk_recognizer_accept_waveform_f"));
		recognizer_result = reinterpret_cast<Fn_vosk_recognizer_result>(GetProcAddress(m_hLib, "vosk_recognizer_result"));
		recognizer_partial_result = reinterpret_cast<Fn_vosk_recognizer_partial_result>(GetProcAddress(m_hLib, "vosk_recognizer_partial_result"));
		recognizer_final_result = reinterpret_cast<Fn_vosk_recognizer_final_result>(GetProcAddress(m_hLib, "vosk_recognizer_final_result"));
		recognizer_reset = reinterpret_cast<Fn_vosk_recognizer_reset>(GetProcAddress(m_hLib, "vosk_recognizer_reset"));

		if (set_log_level && model_new && model_free && recognizer_new && recognizer_free &&
		    recognizer_accept_waveform && recognizer_final_result) {
			m_loaded = true;
			set_log_level(-1);
			return true;
		}

		FreeLibrary(m_hLib);
		m_hLib = nullptr;
		return false;
	}

	bool IsLoaded() const { return m_loaded; }

private:
	VoskApi() = default;
	~VoskApi()
	{
		if (m_hLib) {
			FreeLibrary(m_hLib);
			m_hLib = nullptr;
		}
	}

	HMODULE m_hLib = nullptr;
	bool m_loaded = false;
};

} // namespace akvarium
