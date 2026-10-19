/******************************************************************************
    AkvariumOBS: per-scene audio mixer state implementation.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AkvariumSceneAudio.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <util/threading.h>

#include <QVector>

#include <algorithm>
#include <vector>
#include <utility>

namespace akvarium {

static SceneAudioState *instance = nullptr;
bool SceneAudioState::applying = false;

/* AkvariumOBS: the audio mixer lists every audio source, and global sources
   (Mic/Aux, Desktop Audio) are not scene items. A per-scene mixer state must
   therefore cover all audio sources of the collection, not just scene items. */
struct CollectCtx {
	std::vector<OBSSource> *out;
	std::vector<obs_source_t *> *seen;
};

static void add_audio_source(CollectCtx &ctx, obs_source_t *source)
{
	if (!source)
		return;

	const char *id = obs_source_get_id(source);
	if (id && (strcmp(id, "scene") == 0 || strcmp(id, "group") == 0))
		return;

	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return;

	if (std::find(ctx.seen->begin(), ctx.seen->end(), source) != ctx.seen->end())
		return;

	ctx.seen->push_back(source);
	ctx.out->emplace_back(OBSSource(source));
}

/* AkvariumOBS: the mixer shows every audio source. Global devices (Mic/Aux,
   Desktop Audio) are private sources living in mixer output channels, so they
   must be collected separately from the regular source enumeration. */
static void collect_audio_sources(std::vector<OBSSource> &out)
{
	std::vector<obs_source_t *> seen;
	CollectCtx ctx{&out, &seen};

	for (int channel = 1; channel < 7; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (source) {
			add_audio_source(ctx, source);
			obs_source_release(source);
		}
	}

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			add_audio_source(*static_cast<CollectCtx *>(param), source);
			return true;
		},
		&ctx);
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
	blog(LOG_INFO, "[AkvariumSceneAudio] initialized");
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

	/* Remember which scene the change belongs to: the user may switch scenes
	   before the debounce fires, and the state must go to the right scene. */
	obs_source_t *current = obs_frontend_get_current_scene();
	pendingScene = current;
	if (current)
		obs_source_release(current);

	if (!saveTimer.isActive())
		saveTimer.start();
}

void SceneAudioState::SaveCurrentNow()
{
	if (pendingScene)
		SaveSceneState(pendingScene.Get());
}

void SceneAudioState::FrontendEvent(obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED: {
		obs_source_t *current = obs_frontend_get_current_scene();
		lastScene = current;

		std::vector<OBSSource> sources;
		collect_audio_sources(sources);
		for (OBSSource &source : sources)
			ConnectSource(source.Get());

		if (current) {
			ApplySceneState(current);
			obs_source_release(current);
		}
		break;
	}

	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		obs_source_t *current = obs_frontend_get_current_scene();

		if (!current)
			break;

		blog(LOG_INFO, "[AkvariumSceneAudio] scene changed: '%s' -> '%s'",
		     lastScene ? obs_source_get_name(lastScene.Get()) : "(none)", obs_source_get_name(current));

		/* Freeze the outgoing scene's mixer state before applying the new one. */
		if (lastScene && lastScene.Get() != current)
			SaveSceneState(lastScene.Get(), false);

		ApplySceneState(current);

		std::vector<OBSSource> sources;
		collect_audio_sources(sources);
		for (OBSSource &source : sources)
			ConnectSource(source.Get());

		lastScene = current;
		obs_source_release(current);
		break;
	}

	case OBS_FRONTEND_EVENT_EXIT:
		/* Scene data is already torn down here: only stop our timers and drop
		   references, otherwise shutdown would touch freed sources. */
		saveTimer.stop();
		lastScene = nullptr;
		pendingScene = nullptr;
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
	collect_audio_sources(sources);
	if (sources.empty())
		return;

	obs_data_t *state = obs_scene_get_audio_state(scene);
	if (!state)
		return;

	obs_data_array_t *items = obs_data_array_create();

	for (OBSSource &source : sources) {
		const char *uuid = obs_source_get_uuid(source.Get());
		if (!uuid)
			continue;

		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "uuid", uuid);
		obs_data_set_string(entry, "name", obs_source_get_name(source.Get()));
		obs_data_set_bool(entry, "mute", obs_source_muted(source.Get()));
		obs_data_set_double(entry, "volume", obs_source_get_volume(source.Get()));
		obs_data_array_push_back(items, entry);
		obs_data_release(entry);
	}

	obs_data_set_array(state, "items", items);
	obs_data_set_bool(state, "enabled", SceneEnabled(sceneSource));

	obs_scene_set_audio_state(scene, state);
	obs_data_array_release(items);
	obs_data_release(state);

	blog(LOG_INFO, "[AkvariumSceneAudio] saved %d audio sources for scene '%s'", (int)sources.size(),
	     obs_source_get_name(sceneSource));
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

	obs_data_array_t *items = obs_data_get_array(state, "items");
	blog(LOG_INFO, "[AkvariumSceneAudio] applying state for scene '%s'", obs_source_get_name(sceneSource));

	if (items) {
		/* Resolve against live sources: global audio devices are private
		   sources and cannot be found through obs_get_source_by_uuid. */
		std::vector<OBSSource> live;
		collect_audio_sources(live);

		std::vector<std::pair<QString, obs_source_t *>> byUuid;
		std::vector<std::pair<QString, obs_source_t *>> byName;
		for (OBSSource &source : live) {
			const char *uuid = obs_source_get_uuid(source.Get());
			const char *name = obs_source_get_name(source.Get());
			if (uuid)
				byUuid.emplace_back(QString::fromUtf8(uuid), source.Get());
			if (name)
				byName.emplace_back(QString::fromUtf8(name), source.Get());
		}

		const size_t count = obs_data_array_count(items);
		blog(LOG_INFO, "[AkvariumSceneAudio]  %d stored entries, %d live sources", (int)count, (int)live.size());

		applying = true;

		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(items, i);
			if (!item)
				continue;

			const QString uuid = QString::fromUtf8(obs_data_get_string(item, "uuid"));
			const QString name = QString::fromUtf8(obs_data_get_string(item, "name"));

			obs_source_t *source = nullptr;
			for (auto &pair : byUuid) {
				if (pair.first == uuid) {
					source = pair.second;
					break;
				}
			}
			if (!source) {
				for (auto &pair : byName) {
					if (pair.first == name) {
						source = pair.second;
						break;
					}
				}
			}

			if (source) {
				const bool muted = obs_data_get_bool(item, "mute");
				const float volume = (float)obs_data_get_double(item, "volume");
				obs_source_set_muted(source, muted);
				obs_source_set_volume(source, volume);
				blog(LOG_INFO, "[AkvariumSceneAudio]  restore '%s' muted=%d vol=%.3f",
				     obs_source_get_name(source), (int)muted, volume);
			} else {
				blog(LOG_INFO, "[AkvariumSceneAudio]  skip '%s' (source gone)", name.toUtf8().constData());
			}

			obs_data_release(item);
		}

		applying = false;
		obs_data_array_release(items);
	}

	obs_data_release(state);
}

} // namespace akvarium
