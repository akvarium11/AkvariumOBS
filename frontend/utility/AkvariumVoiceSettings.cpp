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
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace akvarium {

VoiceCommandSettings::VoiceCommandSettings(QWidget *parent)
	: QGroupBox(QStringLiteral("Голосовые команды (Replay Buffer)"), parent)
{
	setCheckable(false);

	auto *layout = new QVBoxLayout(this);

	m_enabled = new QCheckBox(QStringLiteral("Включить голосовые команды"), this);
	layout->addWidget(m_enabled);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	m_phrases = new QPlainTextEdit(this);
	m_phrases->setPlaceholderText(QStringLiteral("Одна фраза на строку, на английском или русском.\n"
						     "Например:\nсделай реплей\nsave replay"));
	m_phrases->setFixedHeight(90);
	form->addRow(QStringLiteral("Фразы:"), m_phrases);

	m_model = new QComboBox(this);
	m_model->addItem(QStringLiteral("tiny — самый лёгкий (~75 МБ)"), QStringLiteral("tiny"));
	m_model->addItem(QStringLiteral("base — баланс (~142 МБ)"), QStringLiteral("base"));
	m_model->addItem(QStringLiteral("small — точнее, тяжелее (~466 МБ)"), QStringLiteral("small"));
	form->addRow(QStringLiteral("Модель распознавания:"), m_model);

	auto *modelRow = new QHBoxLayout();
	m_download = new QPushButton(QStringLiteral("Скачать / проверить модель"), this);
	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	m_progress->setVisible(false);
	modelRow->addWidget(m_download);
	modelRow->addWidget(m_progress);
	form->addRow(QString(), modelRow);

	m_mic = new QComboBox(this);
	form->addRow(QStringLiteral("Микрофон:"), m_mic);

	m_threads = new QSpinBox(this);
	m_threads->setRange(1, 8);
	m_threads->setToolTip(QStringLiteral("Сколько потоков CPU отдавать распознаванию. 2 — хороший баланс."));
	form->addRow(QStringLiteral("Потоки CPU:"), m_threads);

	m_sensitivity = new QSlider(Qt::Horizontal, this);
	m_sensitivity->setRange(50, 250);
	m_sensitivity->setValue(100);
	m_sensitivity->setToolTip(QStringLiteral("Порог срабатывания VAD. Выше — меньше ложных запусков, "
						 "ниже — лучше ловит тихую речь."));
	form->addRow(QStringLiteral("Чувствительность:"), m_sensitivity);

	m_onlyReplayBuffer = new QCheckBox(QStringLiteral("Слушать только когда включён Replay Buffer"), this);
	form->addRow(QString(), m_onlyReplayBuffer);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	form->addRow(QStringLiteral("Статус:"), m_status);

	m_transcript = new QLabel(this);
	m_transcript->setWordWrap(true);
	form->addRow(QStringLiteral("Последнее услышанное:"), m_transcript);

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
		m_status->setText(ok ? QStringLiteral("Модель готова") : error);
	});
	connect(m_model, &QComboBox::currentIndexChanged, this, [this]() {
		m_dirty = true;
		UpdateStatus();
	});
	connect(m_enabled, &QCheckBox::toggled, this, [this]() { m_dirty = true; });
	connect(m_onlyReplayBuffer, &QCheckBox::toggled, this, [this]() { m_dirty = true; });
	connect(m_threads, &QSpinBox::valueChanged, this, [this]() { m_dirty = true; });
	connect(m_sensitivity, &QSlider::valueChanged, this, [this]() { m_dirty = true; });
	connect(m_phrases, &QPlainTextEdit::textChanged, this, [this]() { m_dirty = true; });
	connect(m_mic, &QComboBox::currentIndexChanged, this, [this]() { m_dirty = true; });

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
	m_mic->addItem(QStringLiteral("Авто — активный микрофон текущей сцены"), QString());

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *combo = static_cast<QComboBox *>(param);
			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;

			const char *id = obs_source_get_id(source);
			if (!id || !strstr(id, "input"))
				return true;

			const char *name = obs_source_get_name(source);
			if (name)
				combo->addItem(QString::fromUtf8(name), QString::fromUtf8(name));
			return true;
		},
		m_mic);

	m_mic->blockSignals(false);
}

void VoiceCommandSettings::Load()
{
	VoiceConfig cfg = VoiceEngine::LoadConfig();

	m_enabled->setChecked(cfg.enabled);
	m_phrases->setPlainText(cfg.phrases.join(QChar('\n')));
	m_model->setCurrentIndex(std::max(0, m_model->findData(cfg.model)));
	m_threads->setValue(cfg.threads);
	m_sensitivity->setValue((int)(cfg.sensitivity * 100.0));
	m_onlyReplayBuffer->setChecked(cfg.onlyWithReplayBuffer);

	RefreshMics();
	const int micIndex = m_mic->findData(cfg.micSource);
	m_mic->setCurrentIndex(micIndex >= 0 ? micIndex : 0);

	m_dirty = false;
	UpdateStatus();
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
	cfg.threads = m_threads->value();
	cfg.sensitivity = m_sensitivity->value() / 100.0;
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
		text = QStringLiteral("Модель ggml-%1.bin не найдена — нажмите «Скачать».").arg(model);
	} else if (auto *engine = VoiceEngine::Instance()) {
		text = engine->StatusText();
		if (text.isEmpty())
			text = QStringLiteral("Готово");
		if (engine->Listening())
			text += QStringLiteral("  (распознавание активно)");
	} else {
		text = QStringLiteral("Готово");
	}

	m_status->setText(text);
	if (auto *engine = VoiceEngine::Instance())
		m_transcript->setText(engine->LastTranscript());
}

void VoiceCommandSettings::DownloadModel()
{
	const QString model = m_model->currentData().toString();
	if (VoiceEngine::ModelReady(model)) {
		m_status->setText(QStringLiteral("Модель уже скачана"));
		return;
	}

	m_download->setEnabled(false);
	m_progress->setValue(0);
	m_progress->setVisible(true);
	m_status->setText(QStringLiteral("Загрузка модели ggml-%1.bin...").arg(model));
	m_downloader->Start(model);
}

} // namespace akvarium
