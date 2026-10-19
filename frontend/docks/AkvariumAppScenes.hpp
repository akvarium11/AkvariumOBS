/******************************************************************************
    AkvariumOBS: application-bound scenes.

    Lets the user bind scenes to applications. A rule matches on one of:
      - application is in the foreground
      - application is running (foreground or background)
      - application is running but not in the foreground
      - application is not running (covers "switch when app closes")
    The first matching rule wins. Rules are evaluated on a light 700 ms poll.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QLabel>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTableWidget>
#include <QTimer>
#include <QVector>

class OBSBasic;

namespace akvarium {

enum class AppCondition {
	Foreground = 0,
	Running = 1,
	RunningNotForeground = 2,
	NotRunning = 3,
};

struct AppSceneRule {
	QString exe;
	int condition = 0;
	QString sceneUuid;
	QString sceneName;
	bool enabled = true;
};

class AppSceneEngine : public QObject {
	Q_OBJECT

public:
	static void Init(OBSBasic *main);
	static AppSceneEngine *Instance();

	QVector<AppSceneRule> rules() const { return m_rules; }
	void SetRules(const QVector<AppSceneRule> &rules);
	void Load();
	void Save();

	QString ForegroundExe() const;
	QString LastStatus() const { return m_status; }

	static QString ConditionLabel(int condition);

signals:
	void RulesChanged();
	void StatusChanged(const QString &status);

private:
	explicit AppSceneEngine(OBSBasic *main);

	void Tick();
	void ApplyRule(int index);
	QSet<QString> RunningExes() const;

	OBSBasic *m_main = nullptr;
	QTimer m_timer;
	QVector<AppSceneRule> m_rules;
	int m_lastRule = -2;
	QString m_status;
};

class AppSceneRuleDialog : public QDialog {
	Q_OBJECT

public:
	AppSceneRuleDialog(QWidget *parent, const AppSceneRule &rule);

	AppSceneRule Rule() const;

private:
	void PopulateProcesses();

	QComboBox *m_exe = nullptr;
	QComboBox *m_condition = nullptr;
	QComboBox *m_scene = nullptr;
};

class AppSceneDock : public QDockWidget {
	Q_OBJECT

public:
	explicit AppSceneDock(QWidget *parent);

	void Refresh();

private:
	void AddRule();
	void EditRule();
	void RemoveRule();
	void MoveRule(int delta);
	void OnSelectionChanged();

	int SelectedRow() const;

	QTableWidget *m_table = nullptr;
	QLabel *m_status = nullptr;
};

} // namespace akvarium
