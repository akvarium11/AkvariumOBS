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
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>

namespace akvarium {

static QString SensitivityText(int value)
{
	switch (value) {
	case 1:
		return QStringLiteral("1 - Low (loud room / close voice)");
	case 2:
		return QStringLiteral("2 - Medium-Low");
	case 4:
		return QStringLiteral("4 - Medium-High");
	case 5:
		return QStringLiteral("5 - High (quiet voice)");
	case 3:
	default:
		return QStringLiteral("3 - Normal (recommended)");
	}
}

VoiceCommandSettings::VoiceCommandSettings(QWidget *parent)
	: QGroupBox(QStringLiteral("Voice Commands (Replay Buffer)"), parent)
{
	setCheckable(false);

	auto *layout = new QVBoxLayout(this);

	m_enabled = new QCheckBox(QStringLiteral("Enable voice commands for Replay Buffer"), this);
	layout->addWidget(m_enabled);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	m_mic = new QComboBox(this);
	form->addRow(QStringLiteral("Microphone:"), m_mic);

	m_language = new QComboBox(this);
	m_language->addItem(QStringLiteral("Auto (detect from phrases)"), QStringLiteral("auto"));
	m_language->addItem(QStringLiteral("Russian (Русский)"), QStringLiteral("ru"));
	m_language->addItem(QStringLiteral("English"), QStringLiteral("en"));
	form->addRow(QStringLiteral("Language:"), m_language);

	auto *sensLayout = new QHBoxLayout();
	m_sensitivity = new QSlider(Qt::Horizontal, this);
	m_sensitivity->setRange(1, 5);
	m_sensitivity->setValue(3);
	m_sensitivity->setTickPosition(QSlider::TicksBelow);
	m_sensitivity->setTickInterval(1);

	m_sensitivityLabel = new QLabel(SensitivityText(3), this);
	sensLayout->addWidget(m_sensitivity, 1);
	sensLayout->addWidget(m_sensitivityLabel, 2);
	form->addRow(QStringLiteral("Sensitivity:"), sensLayout);

	/* Enrolled phrases tag button container */
	m_phrasesScroll = new QScrollArea(this);
	m_phrasesScroll->setWidgetResizable(true);
	m_phrasesScroll->setFixedHeight(95);
	m_phrasesScroll->setStyleSheet(
		QStringLiteral("QScrollArea { border: 1px solid #333d47; background: #182026; border-radius: 4px; }"));

	m_phrasesContainer = new QWidget();
	m_phrasesContainer->setStyleSheet(QStringLiteral("background: transparent;"));
	m_phrasesGrid = new QGridLayout(m_phrasesContainer);
	m_phrasesGrid->setContentsMargins(6, 6, 6, 6);
	m_phrasesGrid->setSpacing(6);
	m_phrasesScroll->setWidget(m_phrasesContainer);

	form->addRow(QStringLiteral("Enrolled phrases:"), m_phrasesScroll);

	auto *btnRow = new QHBoxLayout();
	m_trainBtn = new QPushButton(QStringLiteral("➕ Train new phrase (5 samples)"), this);
	m_testBtn = new QPushButton(QStringLiteral("🎙️ Test recognition"), this);
	m_cancelBtn = new QPushButton(QStringLiteral("⏹ Cancel"), this);
	m_cancelBtn->setVisible(false);

	btnRow->addWidget(m_trainBtn);
	btnRow->addWidget(m_testBtn);
	btnRow->addWidget(m_cancelBtn);
	form->addRow(QString(), btnRow);

	m_recordFeedback = new QLabel(this);
	m_recordFeedback->setWordWrap(true);
	m_recordFeedback->setStyleSheet(QStringLiteral("font-weight: bold; color: #3daee9;"));
	form->addRow(QString(), m_recordFeedback);

	auto *modelRow = new QHBoxLayout();
	m_downloadBtn = new QPushButton(QStringLiteral("Download / verify model"), this);
	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	m_progress->setVisible(false);
	modelRow->addWidget(m_downloadBtn);
	modelRow->addWidget(m_progress);
	form->addRow(QString(), modelRow);

	m_onlyReplayBuffer =
		new QCheckBox(QStringLiteral("Listen only while Replay Buffer is running"), this);
	form->addRow(QString(), m_onlyReplayBuffer);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	form->addRow(QStringLiteral("Status:"), m_status);

	m_transcript = new QLabel(this);
	m_transcript->setWordWrap(true);
	form->addRow(QStringLiteral("Last heard:"), m_transcript);

	layout->addLayout(form);

	connect(m_enabled, &QCheckBox::toggled, this, [this]() { MarkChanged(); });
	connect(m_onlyReplayBuffer, &QCheckBox::toggled, this, [this]() { MarkChanged(); });
	connect(m_mic, &QComboBox::currentIndexChanged, this, [this]() { MarkChanged(); });
	connect(m_language, &QComboBox::currentIndexChanged, this, [this]() { MarkChanged(); });

	connect(m_sensitivity, &QSlider::valueChanged, this, [this](int val) {
		m_sensitivityLabel->setText(SensitivityText(val));
		MarkChanged();
	});

	connect(m_downloadBtn, &QPushButton::clicked, this, [this]() { DownloadModel(); });
	connect(m_trainBtn, &QPushButton::clicked, this, [this]() { StartTrainingPhrase(); });
	connect(m_testBtn, &QPushButton::clicked, this, [this]() { StartTestingPhrase(); });
	connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { CancelCurrentOperation(); });

	if (auto *engine = VoiceEngine::Instance()) {
		connect(engine, &VoiceEngine::ModelDownloadProgress, this,
			[this](int percent) {
				m_downloadBtn->setEnabled(false);
				m_progress->setVisible(true);
				m_progress->setValue(percent);
				m_recordFeedback->setText(QStringLiteral("Downloading model... %1%").arg(percent));
			},
			Qt::QueuedConnection);

		connect(engine, &VoiceEngine::ModelDownloadFinished, this,
			[this](bool ok, const QString &msg) {
				m_progress->setVisible(false);
				m_downloadBtn->setEnabled(true);
				m_recordFeedback->setText(
					ok ? QStringLiteral("✅ Recognition model is ready")
					   : QStringLiteral("❌ Model download failed: %1").arg(msg));
				UpdateStatus();
			},
			Qt::QueuedConnection);

		connect(engine, &VoiceEngine::PhraseTestFinished, this,
			[this](bool matched, const QString &heardText, const QString &matchedPhrase, double score) {
				m_trainBtn->setEnabled(true);
				m_testBtn->setEnabled(true);
				m_cancelBtn->setVisible(false);
				m_testBtn->setText(QStringLiteral("🎙️ Test recognition"));

				if (heardText.isEmpty()) {
					m_recordFeedback->setText(QStringLiteral("⚠️ Audio not recognized."));
				} else if (matched) {
					m_recordFeedback->setText(
						QStringLiteral("✅ Match! Heard: \"%1\" (triggered \"%2\", match %3%)")
							.arg(heardText, matchedPhrase)
							.arg((int)(score * 100)));
				} else {
					m_recordFeedback->setText(
						QStringLiteral("❌ No match. Heard: \"%1\" (not enrolled)")
							.arg(heardText));
				}
			},
			Qt::QueuedConnection);

		connect(engine, &VoiceEngine::SampleProgress, this,
			[this](int current, int total, const QString &statusText) {
				m_recordFeedback->setText(
					QStringLiteral("🔴 [%1/%2] %3").arg(current).arg(total).arg(statusText));
			},
			Qt::QueuedConnection);

		connect(engine, &VoiceEngine::SampleCollected, this,
			[this](int sampleIdx, const QString &heardText) {
				if (heardText.isEmpty()) {
					m_recordFeedback->setText(
						QStringLiteral("⚠️ Sample %1: Silence / not recognized. Please repeat...")
							.arg(sampleIdx));
				} else {
					m_recordFeedback->setText(
						QStringLiteral("🎙️ Sample %1 heard: \"%2\"").arg(sampleIdx).arg(heardText));
				}
			},
			Qt::QueuedConnection);

		connect(engine, &VoiceEngine::SampleCollectionFinished, this,
			[this](const QString &phrase, const QStringList &learnedVariants) {
				m_trainBtn->setEnabled(true);
				m_testBtn->setEnabled(true);
				m_cancelBtn->setVisible(false);

				if (learnedVariants.isEmpty()) {
					m_recordFeedback->setText(
						QStringLiteral("⚠️ Training cancelled or no speech recognized."));
					return;
				}

				if (!m_phraseList.contains(phrase, Qt::CaseInsensitive)) {
					m_phraseList.append(phrase);
				}

				m_recordFeedback->setText(
					QStringLiteral("✅ Enrolled \"%1\" (5 samples)! Variations: %2")
						.arg(phrase, learnedVariants.join(QStringLiteral(", "))));

				Save();
				RebuildPhraseButtons();
			},
			Qt::QueuedConnection);

		connect(engine, &VoiceEngine::StatusChanged, this, [this]() { UpdateStatus(); }, Qt::QueuedConnection);
		connect(engine, &VoiceEngine::TranscriptChanged, this,
			[this](const QString &t) { m_transcript->setText(t); }, Qt::QueuedConnection);
	}
}

void VoiceCommandSettings::RefreshMics()
{
	m_mic->blockSignals(true);
	m_mic->clear();

	m_mic->addItem(QStringLiteral("Auto (Active Scene Microphone)"), QString());

	if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		IMMDeviceEnumerator *enumerator = nullptr;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
					       __uuidof(IMMDeviceEnumerator), (void **)&enumerator))) {
			for (EDataFlow flow : {eCapture, eRender}) {
				IMMDeviceCollection *col = nullptr;
				if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col) {
					UINT count = 0;
					col->GetCount(&count);
					for (UINT i = 0; i < count; i++) {
						IMMDevice *dev = nullptr;
						if (FAILED(col->Item(i, &dev)) || !dev)
							continue;

						LPWSTR id = nullptr;
						dev->GetId(&id);

						QString label;
						IPropertyStore *props = nullptr;
						if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
							PROPVARIANT val;
							PropVariantInit(&val);
							if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &val)) &&
							    val.vt == VT_LPWSTR)
								label = QString::fromWCharArray(val.pwszVal);
							PropVariantClear(&val);
							props->Release();
						}

						const QString prefix = (flow == eRender) ? QStringLiteral("loopback:")
											: QStringLiteral("wasapi:");
						const QString devName = label.isEmpty() ? QString::fromWCharArray(id) : label;
						m_mic->addItem(QStringLiteral("%1: %2")
								       .arg((flow == eRender) ? QStringLiteral("Output")
											      : QStringLiteral("Input"),
									    devName),
							       prefix + QString::fromWCharArray(id));

						CoTaskMemFree(id);
						dev->Release();
					}
					col->Release();
				}
			}
			enumerator->Release();
		}
		CoUninitialize();
	}

	m_mic->blockSignals(false);
}

void VoiceCommandSettings::RebuildPhraseButtons()
{
	if (!m_phrasesGrid)
		return;

	QLayoutItem *child = nullptr;
	while ((child = m_phrasesGrid->takeAt(0)) != nullptr) {
		if (child->widget())
			child->widget()->deleteLater();
		delete child;
	}

	if (m_phraseList.isEmpty()) {
		auto *emptyLabel = new QLabel(
			QStringLiteral("No phrases enrolled yet. Click '➕ Train new phrase' below."),
			m_phrasesContainer);
		emptyLabel->setStyleSheet(QStringLiteral("color: #888888; font-style: italic; padding: 12px;"));
		emptyLabel->setAlignment(Qt::AlignCenter);
		m_phrasesGrid->addWidget(emptyLabel, 0, 0, 1, 2);
		return;
	}

	int row = 0;
	int col = 0;
	const int maxCols = 2;

	for (const QString &phrase : m_phraseList) {
		auto *btn = new QPushButton(QStringLiteral("✕   %1").arg(phrase), m_phrasesContainer);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setToolTip(QStringLiteral("Click to remove phrase \"%1\"").arg(phrase));
		btn->setStyleSheet(
			QStringLiteral("QPushButton { "
				       "  background-color: #253342; "
				       "  border: 1px solid #3daee9; "
				       "  border-radius: 4px; "
				       "  color: #ffffff; "
				       "  padding: 6px 12px; "
				       "  font-weight: bold; "
				       "  text-align: left; "
				       "} "
				       "QPushButton:hover { "
				       "  background-color: #7a2222; "
				       "  border-color: #e74c3c; "
				       "} "));

		connect(btn, &QPushButton::clicked, this, [this, phrase]() { RemovePhrase(phrase); });

		m_phrasesGrid->addWidget(btn, row, col);
		col++;
		if (col >= maxCols) {
			col = 0;
			row++;
		}
	}
}

void VoiceCommandSettings::RemovePhrase(const QString &phrase)
{
	m_phraseList.removeAll(phrase);
	Save();
	RebuildPhraseButtons();
	m_recordFeedback->setText(QStringLiteral("Removed phrase: \"%1\"").arg(phrase));
	MarkChanged();
}

void VoiceCommandSettings::Load()
{
	m_loading = true;
	VoiceConfig cfg = VoiceEngine::LoadConfig();

	m_enabled->setChecked(cfg.enabled);
	m_phraseList = cfg.phrases;
	RebuildPhraseButtons();

	m_sensitivity->setValue(cfg.sensitivity);
	m_sensitivityLabel->setText(SensitivityText(cfg.sensitivity));
	m_onlyReplayBuffer->setChecked(cfg.onlyWithReplayBuffer);

	const QString lang = cfg.language.trimmed().toLower();
	int langIdx = m_language->findData(lang);
	if (langIdx < 0)
		langIdx = 0;
	m_language->setCurrentIndex(langIdx);

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
	VoiceConfig cfg = VoiceEngine::LoadConfig();
	cfg.enabled = m_enabled->isChecked();
	cfg.phrases = m_phraseList;

	/* Prune aliases whose base phrase was removed */
	auto it = cfg.aliases.begin();
	while (it != cfg.aliases.end()) {
		bool phraseExists = false;
		for (const QString &p : cfg.phrases) {
			if (p.compare(it->first, Qt::CaseInsensitive) == 0) {
				phraseExists = true;
				break;
			}
		}
		if (!phraseExists)
			it = cfg.aliases.erase(it);
		else
			++it;
	}

	cfg.micSource = m_mic->currentData().toString();
	cfg.sensitivity = m_sensitivity->value();
	cfg.onlyWithReplayBuffer = m_onlyReplayBuffer->isChecked();
	cfg.language = m_language->currentData().toString();
	if (cfg.language.isEmpty())
		cfg.language = QStringLiteral("auto");
	cfg.model = VoiceEngine::AutoModel();

	VoiceEngine::SaveConfig(cfg);

	if (!VoiceEngine::ModelReady(cfg.model))
		DownloadModel();

	if (auto *engine = VoiceEngine::Instance())
		engine->ReapplyConfig();

	m_dirty = false;
}

void VoiceCommandSettings::UpdateStatus()
{
	const QString model = VoiceEngine::AutoModel();
	const bool ready = VoiceEngine::ModelReady(model);

	QString text;
	if (!ready) {
		text = QStringLiteral("Model ggml-%1.bin is not downloaded. Click 'Download / verify model'.")
			       .arg(model);
	} else if (auto *engine = VoiceEngine::Instance()) {
		text = engine->StatusText();
		if (text.isEmpty())
			text = QStringLiteral("Ready");
		if (engine->Listening())
			text += QStringLiteral(" (listening: %1)").arg(model);
	} else {
		text = QStringLiteral("Ready");
	}

	m_status->setText(text);
	if (auto *engine = VoiceEngine::Instance())
		m_transcript->setText(engine->LastTranscript());
}

void VoiceCommandSettings::DownloadModel()
{
	auto *engine = VoiceEngine::Instance();
	if (!engine)
		return;

	const QString model = VoiceEngine::AutoModel();
	if (VoiceEngine::ModelReady(model)) {
		m_recordFeedback->setText(QStringLiteral("✅ Model %1 is verified and ready to use").arg(model));
		engine->ReapplyConfig();
		UpdateStatus();
		return;
	}

	m_downloadBtn->setEnabled(false);
	m_progress->setValue(0);
	m_progress->setVisible(true);
	m_recordFeedback->setText(QStringLiteral("Starting download of model %1...").arg(model));
	engine->DownloadMissingModels();
}

void VoiceCommandSettings::StartTrainingPhrase()
{
	auto *engine = VoiceEngine::Instance();
	if (!engine)
		return;

	if (!VoiceEngine::ModelReady(VoiceEngine::AutoModel())) {
		m_recordFeedback->setText(QStringLiteral("Download the recognition model first"));
		return;
	}

	bool ok = false;
	const QString phrase = QInputDialog::getText(
		this,
		QStringLiteral("Train Voice Phrase"),
		QStringLiteral("Enter the phrase to train (you will speak it 5 times):"),
		QLineEdit::Normal,
		QString(),
		&ok);

	if (!ok || phrase.trimmed().isEmpty())
		return;

	const QString cleanPhrase = phrase.trimmed();

	m_trainBtn->setEnabled(false);
	m_testBtn->setEnabled(false);
	m_cancelBtn->setVisible(true);
	m_recordFeedback->setText(
		QStringLiteral("🎓 Waiting for speech: Say \"%1\" into microphone...").arg(cleanPhrase));

	engine->StartSampleCollection(cleanPhrase, 5);
}

void VoiceCommandSettings::StartTestingPhrase()
{
	auto *engine = VoiceEngine::Instance();
	if (!engine)
		return;

	if (!VoiceEngine::ModelReady(VoiceEngine::AutoModel())) {
		m_recordFeedback->setText(QStringLiteral("Download the recognition model first"));
		return;
	}

	m_trainBtn->setEnabled(false);
	m_testBtn->setEnabled(false);
	m_cancelBtn->setVisible(true);
	m_recordFeedback->setText(QStringLiteral("🎙️ Testing... Speak any of your enrolled phrases into microphone"));
	engine->StartPhraseTest(5);
}

void VoiceCommandSettings::CancelCurrentOperation()
{
	if (auto *engine = VoiceEngine::Instance()) {
		engine->CancelSampleCollection();
		engine->CancelPhraseRecording();
	}
	m_trainBtn->setEnabled(true);
	m_testBtn->setEnabled(true);
	m_cancelBtn->setVisible(false);
	m_recordFeedback->setText(QStringLiteral("Operation cancelled."));
}

} // namespace akvarium
