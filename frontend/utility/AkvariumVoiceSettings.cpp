/******************************************************************************
    AkvariumOBS: voice command settings widget implementation.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumVoiceSettings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>

namespace akvarium {

VoiceCommandSettings::VoiceCommandSettings(QWidget *parent)
	: QGroupBox(QStringLiteral("Voice Commands (Replay Buffer)"), parent)
{
	setCheckable(false);

	auto *layout = new QVBoxLayout(this);

	m_enabled = new QCheckBox(QStringLiteral("Enable voice commands"), this);
	layout->addWidget(m_enabled);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	m_phrases = new QPlainTextEdit(this);
	m_phrases->setPlaceholderText(QStringLiteral("One phrase per line, English or Russian.\n"
						     "For example:\nsave replay\nсделай реплей"));
	m_phrases->setFixedHeight(90);
	form->addRow(QStringLiteral("Phrases:"), m_phrases);

	m_model = new QComboBox(this);
	m_model->addItem(QStringLiteral("Fast - tiny, lowest latency (~75 MB)"), QStringLiteral("tiny"));
	m_model->addItem(QStringLiteral("Balanced - base, recommended (~142 MB)"), QStringLiteral("base"));
	m_model->addItem(QStringLiteral("Accurate - small, slower (~466 MB)"), QStringLiteral("small"));
	form->addRow(QStringLiteral("Recognition model:"), m_model);

	auto *modelRow = new QHBoxLayout();
	m_download = new QPushButton(QStringLiteral("Download / verify model"), this);
	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	m_progress->setVisible(false);
	modelRow->addWidget(m_download);
	modelRow->addWidget(m_progress);
	form->addRow(QString(), modelRow);

	m_mic = new QComboBox(this);
	form->addRow(QStringLiteral("Microphone:"), m_mic);

	m_onlyReplayBuffer = new QCheckBox(QStringLiteral("Listen only while the Replay Buffer is running"), this);
	form->addRow(QString(), m_onlyReplayBuffer);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	form->addRow(QStringLiteral("Status:"), m_status);

	m_transcript = new QLabel(this);
	m_transcript->setWordWrap(true);
	form->addRow(QStringLiteral("Last heard:"), m_transcript);

	layout->addLayout(form);

	m_downloader = new VoiceModelDownloader(this);

	connect(m_download, &QPushButton::clicked, this, [this]() { DownloadModel(); });
	connect(m_downloader, &VoiceModelDownloader::Progress, this, [this](int percent) {
		m_progress->setVisible(true);
		m_progress->setValue(percent);
	});
	connect(m_downloader, &VoiceModelDownloader::Finished, this, [this](bool ok, const QString &error) {
		m_progress->setVisible(false);
		m_download->setEnabled(true);
		m_status->setText(ok ? QStringLiteral("Model ready") : error);
	});
	connect(m_model, &QComboBox::currentIndexChanged, this, [this]() {
		MarkChanged();
		UpdateStatus();
	});
	connect(m_enabled, &QCheckBox::toggled, this, [this]() { MarkChanged(); });
	connect(m_onlyReplayBuffer, &QCheckBox::toggled, this, [this]() { MarkChanged(); });
	connect(m_phrases, &QPlainTextEdit::textChanged, this, [this]() { MarkChanged(); });
	connect(m_mic, &QComboBox::currentIndexChanged, this, [this]() { MarkChanged(); });

	if (auto *engine = VoiceEngine::Instance()) {
		connect(engine, &VoiceEngine::StatusChanged, this, [this]() { UpdateStatus(); }, Qt::QueuedConnection);
		connect(engine, &VoiceEngine::TranscriptChanged, this,
			[this](const QString &text) { m_transcript->setText(text); }, Qt::QueuedConnection);
	}
}

void VoiceCommandSettings::RefreshMics()
{
	m_mic->blockSignals(true);
	m_mic->clear();
	m_mic->addItem(QStringLiteral("Auto - active microphone of the current scene"), QString());

	/* OBS audio sources (they know their device id). */
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *combo = static_cast<QComboBox *>(param);
			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;

			const char *id = obs_source_get_id(source);
			if (!id || (!strstr(id, "input") && !strstr(id, "output_capture")))
				return true;

			const char *name = obs_source_get_name(source);
			if (name)
				combo->addItem(QStringLiteral("OBS: %1").arg(QString::fromUtf8(name)),
					       QStringLiteral("source:%1").arg(QString::fromUtf8(name)));
			return true;
		},
		m_mic);

	/* Windows endpoints, so a device does not have to be an OBS source. */
	HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	if (SUCCEEDED(comResult)) {
		IMMDeviceEnumerator *enumerator = nullptr;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
					       __uuidof(IMMDeviceEnumerator), (void **)&enumerator))) {
			for (EDataFlow flow : {eCapture, eRender}) {
				IMMDeviceCollection *collection = nullptr;
				if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) ||
				    !collection)
					continue;

				UINT count = 0;
				collection->GetCount(&count);

				for (UINT i = 0; i < count; i++) {
					IMMDevice *device = nullptr;
					if (FAILED(collection->Item(i, &device)) || !device)
						continue;

					LPWSTR id = nullptr;
					if (SUCCEEDED(device->GetId(&id)) && id) {
						IPropertyStore *props = nullptr;
						QString label;
						if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
							PROPVARIANT value;
							PropVariantInit(&value);
							if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
							    value.vt == VT_LPWSTR)
								label = QString::fromWCharArray(value.pwszVal);
							PropVariantClear(&value);
							props->Release();
						}

						const QString prefix = flow == eRender ? QStringLiteral("loopback:")
										       : QStringLiteral("wasapi:");
						m_mic->addItem(QStringLiteral("%1: %2")
								       .arg(flow == eRender ? QStringLiteral("Output")
											    : QStringLiteral("Input"),
									    label.isEmpty() ? QString::fromWCharArray(id) : label),
							       prefix + QString::fromWCharArray(id));

						CoTaskMemFree(id);
					}

					device->Release();
				}

				collection->Release();
			}

			enumerator->Release();
		}

		CoUninitialize();
	}

	m_mic->blockSignals(false);
}

void VoiceCommandSettings::Load()
{
	m_loading = true;
	VoiceConfig cfg = VoiceEngine::LoadConfig();

	m_enabled->setChecked(cfg.enabled);
	m_phrases->setPlainText(cfg.phrases.join(QChar('\n')));
	m_model->setCurrentIndex(std::max(0, m_model->findData(cfg.model)));
	m_onlyReplayBuffer->setChecked(cfg.onlyWithReplayBuffer);

	RefreshMics();
	const int micIndex = m_mic->findData(cfg.micSource);
	m_mic->setCurrentIndex(micIndex >= 0 ? micIndex : 0);

	m_dirty = false;
	m_loading = false;
	UpdateStatus();
}

void VoiceCommandSettings::MarkChanged()
{
	m_dirty = true;
	if (!m_loading)
		emit Changed();
}

void VoiceCommandSettings::SetLoading(bool loading)
{
	m_loading = loading;
}

void VoiceCommandSettings::Save()
{
	VoiceConfig cfg;
	cfg.enabled = m_enabled->isChecked();
	cfg.phrases = m_phrases->toPlainText().split(QChar('\n'), Qt::SkipEmptyParts);
	for (QString &phrase : cfg.phrases)
		phrase = phrase.trimmed();
	cfg.phrases.removeAll(QString());
	cfg.model = m_model->currentData().toString();
	cfg.micSource = m_mic->currentData().toString();
	cfg.threads = 0; /* auto: the engine picks a thread count from the CPU */
	cfg.onlyWithReplayBuffer = m_onlyReplayBuffer->isChecked();

	if (cfg.phrases.isEmpty())
		cfg.phrases << QStringLiteral("сделай реплей");

	VoiceEngine::SaveConfig(cfg);
	if (auto *engine = VoiceEngine::Instance())
		engine->ReapplyConfig();

	m_dirty = false;
}

void VoiceCommandSettings::UpdateStatus()
{
	const QString model = m_model->currentData().toString();
	const bool ready = VoiceEngine::ModelReady(model);

	QString text;
	if (!ready) {
		text = QStringLiteral("Model ggml-%1.bin is missing - use Download.").arg(model);
	} else if (auto *engine = VoiceEngine::Instance()) {
		text = engine->StatusText();
		if (text.isEmpty())
			text = QStringLiteral("Ready");
		if (engine->Listening())
			text += QStringLiteral("  (listening)");
	} else {
		text = QStringLiteral("Ready");
	}

	m_status->setText(text);
	if (auto *engine = VoiceEngine::Instance())
		m_transcript->setText(engine->LastTranscript());
}

void VoiceCommandSettings::DownloadModel()
{
	const QString model = m_model->currentData().toString();
	if (VoiceEngine::ModelReady(model)) {
		m_status->setText(QStringLiteral("Model already downloaded"));
		return;
	}

	m_download->setEnabled(false);
	m_progress->setValue(0);
	m_progress->setVisible(true);
	m_status->setText(QStringLiteral("Downloading model ggml-%1.bin...").arg(model));
	m_downloader->Start(model);
}

} // namespace akvarium
