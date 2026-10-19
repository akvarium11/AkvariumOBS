/******************************************************************************
    AkvariumOBS: per-scene audio mixer state implementation.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumSceneAudio.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/threading.h>

#include <QVector>

#include <vector>

namespace akvarium {

static SceneAudioState *instance = nullptr;

static void collect_scene_audio(obs_scene_t *scene, std::vector<OBSSource> &out)
{
	if (!scene)
		return;

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *list = static_cast<std::vector<OBSSource> *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);

			if (!source)
				return true;

			const char *id = obs_source_get_id(source);
			if (id && (strcmp(id, "group") == 0 || strcmp(id, "scene") == 0)) {
				obs_scene_t *child = obs_scene_from_source(source);
				if (child)
					collect_scene_audio(child, *list);
			}

			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0)
				list->emplace_back(OBSGetStrongRef(source));

			return true;
		},
		&out);

	/* obs_scene_enum_items returns items in reverse order; order is irrelevant here. */
}

SceneAudioState::SceneAudioState(OBSBasic *main_) : main(main_)
{
	saveTimer.setSingleShot(true);
	saveTimer.setInterval(250);
	connect(&saveTimer, &QTimer::timeout, this, [this]() { SaveCurrentNow(); });
}

void SceneAudioState::Init(OBSBasic *main)
{
	if (instance)
		return;

	instance = new SceneAudioState(main);
	obs_frontend_add_event_callback(frontend_event_cb, instance);
}

SceneAudioState *SceneAudioState::Instance()
{
	return instance;
}

void SceneAudioState::frontend_event_cb(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<SceneAudioState *>(data);
	if (self)
		self->FrontendEvent(event);
}

void SceneAudioState::audio_signal_cb(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	auto *self = static_cast<SceneAudioState *>(data);
	if (self)
		self->OnAudioSignal(cd);
}

void SceneAudioState::OnAudioSignal(calldata_t *cd)
{
	if (applying)
		return;

	/* Keep the audio callback cheap: only arm the debounce timer. */
	UNUSED_PARAMETER(cd);
	ScheduleSave();
}

void SceneAudioState::ScheduleSave()
{
	if (applying)
		return;

	if (!saveTimer.isActive())
		saveTimer.start();
}

void SceneAudioState::SaveCurrentNow()
{
	obs_source_t *current = obs_frontend_get_current_scene();
	if (current) {
		SaveSceneState(current);
		obs_source_release(current);
	}
}

void SceneAudioState::FrontendEvent(obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED: {
		obs_source_t *current = obs_frontend_get_current_scene();
		lastScene = OBSGetStrongRef(current);
		if (current)
			obs_source_release(current);
		break;
	}

	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		obs_source_t *current = obs_frontend_get_current_scene();

		if (!current)
			break;

		/* Freeze the outgoing scene's mixer state before applying the new one. */
		if (lastScene && lastScene.Get() != current)
			SaveSceneState(lastScene.Get(), false);

		ApplySceneState(current);

		std::vector<OBSSource> sources;
		collect_scene_audio(obs_scene_from_source(current), sources);
		for (OBSSource &source : sources)
			ConnectSource(source.Get());

		lastScene = OBSGetStrongRef(current);
		obs_source_release(current);
		break;
	}

	case OBS_FRONTEND_EVENT_EXIT:
		SaveCurrentNow();
		break;

	default:
		break;
	}
}

void SceneAudioState::ConnectSource(obs_source_t *source)
{
	if (!source)
		return;

	const char *uuid = obs_source_get_uuid(source);
	if (!uuid)
		return;

	QString key = QString::fromUtf8(uuid);
	if (connected.contains(key))
		return;

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	if (!sh)
		return;

	signal_handler_connect(sh, "mute", audio_signal_cb, this);
	signal_handler_connect(sh, "volume", audio_signal_cb, this);
	connected.insert(key);
}

bool SceneAudioState::SceneEnabled(obs_source_t *sceneSource)
{
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return true;

	bool enabled = true;
	obs_data_t *state = obs_scene_get_audio_state(scene);
	if (state) {
		if (obs_data_has_user_value(state, "enabled"))
			enabled = obs_data_get_bool(state, "enabled");
		obs_data_release(state);
	}
	return enabled;
}

void SceneAudioState::SetSceneEnabled(obs_source_t *sceneSource, bool enabled)
{
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return;

	obs_data_t *state = obs_scene_get_audio_state(scene);
	if (!state)
		return;

	obs_data_set_bool(state, "enabled", enabled);
	obs_data_release(state);

	obs_source_t *current = obs_frontend_get_current_scene();
	if (enabled && current && current == sceneSource)
		SaveSceneState(sceneSource, true);
	if (current)
		obs_source_release(current);
}

void SceneAudioState::SaveSceneState(obs_source_t *sceneSource, bool force)
{
	if (applying || !sceneSource)
		return;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return;

	if (!force && !SceneEnabled(sceneSource))
		return;

	std::vector<OBSSource> sources;
	collect_scene_audio(scene, sources);
	if (sources.empty())
		return;

	obs_data_t *state = obs_scene_get_audio_state(scene);
	if (!state)
		return;

	obs_data_t *items = obs_data_create();

	for (OBSSource &source : sources) {
		const char *uuid = obs_source_get_uuid(source.Get());
		if (!uuid)
			continue;

		obs_data_t *entry = obs_data_create();
		obs_data_set_bool(entry, "mute", obs_source_muted(source.Get()));
		obs_data_set_double(entry, "volume", obs_source_get_volume(source.Get()));
		obs_data_set_obj(items, uuid, entry);
		obs_data_release(entry);
	}

	obs_data_set_obj(state, "items", items);
	obs_data_set_bool(state, "enabled", SceneEnabled(sceneSource));

	obs_scene_set_audio_state(scene, state);
	obs_data_release(items);
	obs_data_release(state);
}

void SceneAudioState::ApplySceneState(obs_source_t *sceneSource)
{
	if (!sceneSource)
		return;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return;

	if (!SceneEnabled(sceneSource))
		return;

	obs_data_t *state = obs_scene_get_audio_state(scene);
	if (!state)
		return;

	obs_data_t *items = obs_data_get_obj(state, "items");
	if (items) {
		applying = true;

		for (obs_data_item_t *entry = obs_data_first(items); entry; obs_data_item_next(&entry)) {
			const char *uuid = obs_data_item_get_name(entry);
			obs_data_t *item = obs_data_item_get_obj(entry);
			if (!uuid || !item)
				continue;

			obs_source_t *source = obs_get_source_by_uuid(uuid);
			if (source) {
				obs_source_set_muted(source, obs_data_get_bool(item, "mute"));
				obs_source_set_volume(source, (float)obs_data_get_double(item, "volume"));
				obs_source_release(source);
			}

			obs_data_release(item);
		}

		applying = false;
		obs_data_release(items);
	}

	obs_data_release(state);
}

} // namespace akvarium
