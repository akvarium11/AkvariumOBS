/******************************************************************************
    AkvariumOBS: application-bound scenes implementation.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumAppScenes.hpp"

#include <widgets/OBSBasic.hpp>
#include <OBSApp.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>

namespace akvarium {

static AppSceneEngine *engine = nullptr;

static QString NormalizeExe(const QString &exe)
{
	QString name = exe.toLower().trimmed();
	name.replace('\\', '/');
	const int slash = name.lastIndexOf('/');
	if (slash >= 0)
		name = name.mid(slash + 1);
	return name;
}

static QString BaseNameLower(const QString &exe)
{
	QString name = NormalizeExe(exe);
	if (name.endsWith(".exe"))
		name.chop(4);
	return name;
}

static bool ExeMatches(const QString &rule, const QString &exe)
{
	const QString r = NormalizeExe(rule);
	const QString e = NormalizeExe(exe);
	if (r.isEmpty() || e.isEmpty())
		return false;
	return r == e || BaseNameLower(r) == BaseNameLower(e);
}

QString AppSceneEngine::ConditionLabel(int condition)
{
	switch (static_cast<AppCondition>(condition)) {
	case AppCondition::Foreground:
		return QStringLiteral("In foreground");
	case AppCondition::Running:
		return QStringLiteral("Running (foreground or background)");
	case AppCondition::RunningNotForeground:
		return QStringLiteral("Running but not in foreground");
	case AppCondition::NotRunning:
		return QStringLiteral("Not running (closed)");
	}
	return QStringLiteral("?");
}

AppSceneEngine::AppSceneEngine(OBSBasic *main) : m_main(main)
{
	m_timer.setInterval(700);
	connect(&m_timer, &QTimer::timeout, this, [this]() { Tick(); });
}

void AppSceneEngine::Init(OBSBasic *main)
{
	if (engine)
		return;

	engine = new AppSceneEngine(main);
	engine->Load();
	engine->m_timer.start();

	auto *dock = new AppSceneDock(main);
	main->AddDockWidget(dock, Qt::RightDockWidgetArea);
}

AppSceneEngine *AppSceneEngine::Instance()
{
	return engine;
}

void AppSceneEngine::Load()
{
	m_rules.clear();

	config_t *cfg = App()->GetUserConfig();
	const char *json = config_get_string(cfg, "AkvariumAppScenes", "rules");
	if (!json || !*json)
		return;

	obs_data_t *root = obs_data_create_from_json(json);
	if (!root)
		return;

	obs_data_array_t *arr = obs_data_get_array(root, "rules");
	if (arr) {
		const size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			AppSceneRule rule;
			rule.exe = QString::fromUtf8(obs_data_get_string(item, "exe"));
			rule.condition = (int)obs_data_get_int(item, "condition");
			rule.sceneUuid = QString::fromUtf8(obs_data_get_string(item, "scene_uuid"));
			rule.sceneName = QString::fromUtf8(obs_data_get_string(item, "scene_name"));
			rule.enabled = obs_data_get_bool(item, "enabled");
			if (!rule.exe.isEmpty())
				m_rules.push_back(rule);
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	obs_data_release(root);
}

void AppSceneEngine::Save()
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();

	for (const AppSceneRule &rule : m_rules) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "exe", rule.exe.toUtf8().constData());
		obs_data_set_int(item, "condition", rule.condition);
		obs_data_set_string(item, "scene_uuid", rule.sceneUuid.toUtf8().constData());
		obs_data_set_string(item, "scene_name", rule.sceneName.toUtf8().constData());
		obs_data_set_bool(item, "enabled", rule.enabled);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}

	obs_data_set_array(root, "rules", arr);
	obs_data_array_release(arr);

	const char *json = obs_data_get_json(root);
	config_set_string(App()->GetUserConfig(), "AkvariumAppScenes", "rules", json ? json : "{}");
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	obs_data_release(root);

	m_lastRule = -2;
	emit RulesChanged();
}

void AppSceneEngine::SetRules(const QVector<AppSceneRule> &rules)
{
	m_rules = rules;
	Save();
}

QString AppSceneEngine::ForegroundExe() const
{
	HWND hwnd = GetForegroundWindow();
	if (!hwnd)
		return QString();

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid)
		return QString();

	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return QString();

	wchar_t path[MAX_PATH * 2];
	DWORD size = (DWORD)(sizeof(path) / sizeof(path[0]));
	QString result;
	if (QueryFullProcessImageNameW(proc, 0, path, &size))
		result = NormalizeExe(QString::fromWCharArray(path, (int)size));

	CloseHandle(proc);
	return result;
}

QSet<QString> AppSceneEngine::RunningExes() const
{
	QSet<QString> names;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return names;

	PROCESSENTRY32W entry;
	entry.dwSize = sizeof(entry);

	if (Process32FirstW(snapshot, &entry)) {
		do {
			names.insert(QString::fromWCharArray(entry.szExeFile).toLower());
		} while (Process32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	return names;
}

void AppSceneEngine::Tick()
{
	if (m_rules.isEmpty()) {
		if (m_lastRule != -1) {
			m_lastRule = -1;
			m_status.clear();
			emit StatusChanged(m_status);
		}
		return;
	}

	const QString foreground = ForegroundExe();
	const QSet<QString> running = RunningExes();

	int matched = -1;
	for (int i = 0; i < m_rules.size(); i++) {
		const AppSceneRule &rule = m_rules.at(i);
		if (!rule.enabled)
			continue;

		const bool isRunning = running.contains(BaseNameLower(rule.exe) + ".exe") ||
				       running.contains(BaseNameLower(rule.exe));
		const bool isForeground = ExeMatches(rule.exe, foreground);

		bool ok = false;
		switch (static_cast<AppCondition>(rule.condition)) {
		case AppCondition::Foreground:
			ok = isForeground;
			break;
		case AppCondition::Running:
			ok = isRunning;
			break;
		case AppCondition::RunningNotForeground:
			ok = isRunning && !isForeground;
			break;
		case AppCondition::NotRunning:
			/* "Not running" must only depend on the process list: the rule's
			   exe can never be the foreground window if it is not running. */
			ok = !isRunning;
			break;
		}

		if (ok) {
			matched = i;
			break;
		}
	}

	if (matched != m_lastRule) {
		blog(LOG_INFO, "[AkvariumAppScenes] rule %d matched (prev %d), foreground '%s'", matched,
		     m_lastRule, foreground.toUtf8().constData());
		m_lastRule = matched;
		if (matched >= 0)
			ApplyRule(matched);

		QString status;
		if (matched >= 0) {
			status = QStringLiteral("%1 -> %2  (foreground: %3)")
					 .arg(m_rules.at(matched).exe, m_rules.at(matched).sceneName,
					      foreground.isEmpty() ? QStringLiteral("-") : foreground);
		} else {
			status = QStringLiteral("No active rule  (foreground: %1)")
					 .arg(foreground.isEmpty() ? QStringLiteral("-") : foreground);
		}
		if (m_status != status) {
			m_status = status;
			emit StatusChanged(m_status);
		}
	}
}

void AppSceneEngine::ApplyRule(int index)
{
	if (index < 0 || index >= m_rules.size())
		return;

	const AppSceneRule &rule = m_rules.at(index);

	obs_source_t *scene = nullptr;
	if (!rule.sceneUuid.isEmpty())
		scene = obs_get_source_by_uuid(rule.sceneUuid.toUtf8().constData());

	if (!scene && !rule.sceneName.isEmpty())
		scene = obs_get_source_by_name(rule.sceneName.toUtf8().constData());

	if (!scene) {
		blog(LOG_WARNING, "[AkvariumAppScenes] scene '%s' not found for rule '%s'",
		     rule.sceneName.toUtf8().constData(), rule.exe.toUtf8().constData());
		return;
	}

	blog(LOG_INFO, "[AkvariumAppScenes] switching to scene '%s' for app '%s'",
	     obs_source_get_name(scene), rule.exe.toUtf8().constData());
	obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
}

/* ------------------------------------------------------------------------- */
/* Rule dialog                                                               */
/* ------------------------------------------------------------------------- */

AppSceneRuleDialog::AppSceneRuleDialog(QWidget *parent, const AppSceneRule &rule) : QDialog(parent)
{
	setWindowTitle(rule.exe.isEmpty() ? QStringLiteral("New rule") : QStringLiteral("Rule"));

	auto *form = new QFormLayout(this);

	m_exe = new QComboBox(this);
	m_exe->setEditable(true);
	m_exe->setInsertPolicy(QComboBox::NoInsert);
	PopulateProcesses();
	m_exe->setCurrentText(rule.exe);
	form->addRow(QStringLiteral("Application (exe):"), m_exe);

	m_condition = new QComboBox(this);
	for (int i = 0; i <= 3; i++)
		m_condition->addItem(AppSceneEngine::ConditionLabel(i), i);
	m_condition->setCurrentIndex(std::clamp(rule.condition, 0, 3));
	form->addRow(QStringLiteral("Condition:"), m_condition);

	m_scene = new QComboBox(this);
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *scene = scenes.sources.array[i];
		const char *name = obs_source_get_name(scene);
		const char *uuid = obs_source_get_uuid(scene);
		m_scene->addItem(QString::fromUtf8(name), QString::fromUtf8(uuid ? uuid : ""));
		if (rule.sceneUuid == QString::fromUtf8(uuid ? uuid : ""))
			m_scene->setCurrentIndex(m_scene->count() - 1);
		else if (rule.sceneName == QString::fromUtf8(name))
			m_scene->setCurrentIndex(m_scene->count() - 1);
	}
	obs_frontend_source_list_free(&scenes);
	form->addRow(QStringLiteral("Scene:"), m_scene);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	form->addRow(buttons);
}

void AppSceneRuleDialog::PopulateProcesses()
{
	m_exe->addItem(QString());

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return;

	QStringList names;
	PROCESSENTRY32W entry;
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			names << QString::fromWCharArray(entry.szExeFile).toLower();
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);

	names.removeDuplicates();
	names.sort();
	for (const QString &name : names)
		m_exe->addItem(name);
}

AppSceneRule AppSceneRuleDialog::Rule() const
{
	AppSceneRule rule;
	rule.exe = m_exe->currentText().trimmed().toLower();
	rule.condition = m_condition->currentData().toInt();
	rule.sceneUuid = m_scene->currentData().toString();
	rule.sceneName = m_scene->currentText();
	rule.enabled = true;
	return rule;
}

/* ------------------------------------------------------------------------- */
/* Dock                                                                      */
/* ------------------------------------------------------------------------- */

AppSceneDock::AppSceneDock(QWidget *parent) : QDockWidget(QStringLiteral("Scene Binding to Applications"), parent)
{
	setObjectName(QStringLiteral("akvariumAppScenesDock"));

	auto *body = new QWidget(this);
	auto *layout = new QVBoxLayout(body);

	m_table = new QTableWidget(body);
	m_table->setColumnCount(3);
	m_table->setHorizontalHeaderLabels({QStringLiteral("Application"), QStringLiteral("Condition"),
					    QStringLiteral("Scene")});
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	layout->addWidget(m_table);

	auto *buttons = new QHBoxLayout();
	auto *add = new QPushButton(QStringLiteral("Add"), body);
	auto *edit = new QPushButton(QStringLiteral("Edit"), body);
	auto *remove = new QPushButton(QStringLiteral("Remove"), body);
	auto *up = new QPushButton(QStringLiteral("Up"), body);
	auto *down = new QPushButton(QStringLiteral("Down"), body);
	buttons->addWidget(add);
	buttons->addWidget(edit);
	buttons->addWidget(remove);
	buttons->addWidget(up);
	buttons->addWidget(down);
	buttons->addStretch(1);
	layout->addLayout(buttons);

	m_status = new QLabel(QStringLiteral("No active rule"), body);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	setWidget(body);

	connect(add, &QPushButton::clicked, this, [this]() { AddRule(); });
	connect(edit, &QPushButton::clicked, this, [this]() { EditRule(); });
	connect(remove, &QPushButton::clicked, this, [this]() { RemoveRule(); });
	connect(up, &QPushButton::clicked, this, [this]() { MoveRule(-1); });
	connect(down, &QPushButton::clicked, this, [this]() { MoveRule(1); });

	connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
		if (!item || item->column() != 0)
			return;
		AppSceneEngine *engine = AppSceneEngine::Instance();
		if (!engine)
			return;

		QVector<AppSceneRule> rules = engine->rules();
		const int row = item->row();
		if (row < 0 || row >= rules.size())
			return;

		const bool enabled = item->checkState() == Qt::Checked;
		if (rules[row].enabled == enabled)
			return;

		rules[row].enabled = enabled;
		engine->SetRules(rules);
	});

	if (auto *engine = AppSceneEngine::Instance()) {
		connect(engine, &AppSceneEngine::RulesChanged, this, &AppSceneDock::Refresh);
		connect(engine, &AppSceneEngine::StatusChanged, this, [this](const QString &status) {
			m_status->setText(status.isEmpty() ? QStringLiteral("No active rule") : status);
		});
	}

	Refresh();
}

int AppSceneDock::SelectedRow() const
{
	auto items = m_table->selectedItems();
	if (items.isEmpty())
		return -1;
	return items.first()->row();
}

void AppSceneDock::Refresh()
{
	AppSceneEngine *engine = AppSceneEngine::Instance();
	if (!engine)
		return;

	const QVector<AppSceneRule> rules = engine->rules();
	m_table->blockSignals(true);
	m_table->setRowCount(rules.size());

	for (int i = 0; i < rules.size(); i++) {
		const AppSceneRule &rule = rules.at(i);
		auto *exeItem = new QTableWidgetItem(rule.exe);
		auto *condItem = new QTableWidgetItem(AppSceneEngine::ConditionLabel(rule.condition));
		auto *sceneItem = new QTableWidgetItem(rule.sceneName);

		Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
		exeItem->setFlags(flags);
		exeItem->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);

		m_table->setItem(i, 0, exeItem);
		m_table->setItem(i, 1, condItem);
		m_table->setItem(i, 2, sceneItem);
	}
}

void AppSceneDock::AddRule()
{
	AppSceneEngine *engine = AppSceneEngine::Instance();
	if (!engine)
		return;

	AppSceneRuleDialog dialog(this, AppSceneRule());
	if (dialog.exec() != QDialog::Accepted)
		return;

	AppSceneRule rule = dialog.Rule();
	if (rule.exe.isEmpty())
		return;

	QVector<AppSceneRule> rules = engine->rules();
	rules.push_back(rule);
	engine->SetRules(rules);
}

void AppSceneDock::EditRule()
{
	AppSceneEngine *engine = AppSceneEngine::Instance();
	const int row = SelectedRow();
	if (!engine || row < 0)
		return;

	QVector<AppSceneRule> rules = engine->rules();
	if (row >= rules.size())
		return;

	AppSceneRuleDialog dialog(this, rules.at(row));
	if (dialog.exec() != QDialog::Accepted)
		return;

	AppSceneRule rule = dialog.Rule();
	rule.enabled = rules.at(row).enabled;
	if (!rule.exe.isEmpty())
		rules[row] = rule;
	engine->SetRules(rules);
}

void AppSceneDock::RemoveRule()
{
	AppSceneEngine *engine = AppSceneEngine::Instance();
	const int row = SelectedRow();
	if (!engine || row < 0)
		return;

	QVector<AppSceneRule> rules = engine->rules();
	if (row >= rules.size())
		return;

	rules.removeAt(row);
	engine->SetRules(rules);
}

void AppSceneDock::MoveRule(int delta)
{
	AppSceneEngine *engine = AppSceneEngine::Instance();
	const int row = SelectedRow();
	if (!engine || row < 0)
		return;

	QVector<AppSceneRule> rules = engine->rules();
	const int target = row + delta;
	if (target < 0 || target >= rules.size())
		return;

	rules.swapItemsAt(row, target);
	engine->SetRules(rules);
	m_table->selectRow(target);
}

void AppSceneDock::OnSelectionChanged() {}

} // namespace akvarium
