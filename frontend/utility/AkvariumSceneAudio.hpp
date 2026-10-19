/******************************************************************************
    AkvariumOBS: per-scene audio mixer state.

    Stores mute/volume of every audio source that belongs to a scene inside that
    scene, so switching scenes restores the mixer as the user configured it for
    that scene. State is persisted in the scene JSON by libobs
    (obs_scene_get_audio_state / obs_scene_set_audio_state).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

class OBSBasic;

namespace akvarium {

class SceneAudioState : public QObject {
	Q_OBJECT

public:
	static void Init(OBSBasic *main);
	static SceneAudioState *Instance();

	/* Per-scene opt-out, stored together with the scene. */
	static bool SceneEnabled(obs_source_t *sceneSource);
	static void SetSceneEnabled(obs_source_t *sceneSource, bool enabled);

	/* Snapshot the live mixer state of the scene's audio sources. */
	static void SaveSceneState(obs_source_t *sceneSource, bool force = false);

	/* Restore the stored mixer state of the scene's audio sources. */
	static void ApplySceneState(obs_source_t *sceneSource);

	/* Force-save the currently active scene (used by the scene menu). */
	static void SaveCurrentScene();

private:
	explicit SceneAudioState(OBSBasic *main);

	void FrontendEvent(obs_frontend_event event);
	void ConnectSource(obs_source_t *source);
	void ScheduleSave();
	void SaveCurrentNow();
	void OnAudioSignal(calldata_t *cd);

	static void frontend_event_cb(enum obs_frontend_event event, void *data);
	static void audio_signal_cb(void *data, calldata_t *cd);

	OBSBasic *main = nullptr;
	OBSSource lastScene;
	QTimer saveTimer;
	bool applying = false;
	QSet<QString> connected;
};

} // namespace akvarium
