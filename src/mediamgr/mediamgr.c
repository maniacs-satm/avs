/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <re.h>
#include "avs.h"
#include "avs_mediamgr.h"
#include "avs_lockedqueue.h"
#include <pthread.h>
#include "mediamgr.h"
#include "mm_platform.h"
#include "avs_flowmgr.h"
#include <unistd.h>


typedef enum {
	MM_HEADSET_PLUGGED = 0,
	MM_HEADSET_UNPLUGGED,
	MM_BT_DEVICE_CONNECTED,
	MM_BT_DEVICE_DISCONNECTED,
	MM_SPEAKER_ENABLE_REQUEST,
	MM_SPEAKER_DISABLE_REQUEST,
	MM_CALL_START,
	MM_CALL_STOP,
	MM_VIDEO_CALL_START,
	MM_VIDEO_CALL_STOP
} mm_route_update_event;

struct mm_route_state_machine {
	bool prefer_loudspeaker;
	bool bt_device_is_connected;
	bool wired_hs_is_connected;
	enum mediamgr_auplay cur_route;
	enum mediamgr_auplay route_before_call;
};

typedef enum {
	MM_PLAYBACK_NONE = 0,
	MM_PLAYBACK_MIXING,
	MM_PLAYBACK_EXCLUSIVE
} mm_playback_mode;

typedef enum {
	MM_MARSHAL_EXIT = 0,
	MM_MARSHAL_PLAY_MEDIA,
	MM_MARSHAL_PAUSE_MEDIA,
	MM_MARSHAL_STOP_MEDIA,
	MM_MARSHAL_CALL_STATE,
	MM_MARSHAL_ENABLE_SPEAKER,
	MM_MARSHAL_HEADSET_CONNECTED,
	MM_MARSHAL_BT_DEVICE_CONNECTED,
	MM_MARSHAL_REGISTER_MEDIA,
	MM_MARSHAL_DEREGISTER_MEDIA,
	MM_MARSHAL_SET_INTENSITY,
} mm_marshal_id;

struct mm_message {
	union{
		struct {
			char media_name[128];
		} media_elem;
		struct {
			enum mediamgr_state state;
		} state_elem;
		struct {
			bool val;
		} bool_elem;
		struct {
			char media_name[128];
			void *media_object;
			bool mixing;
			bool incall;
			int intensity;
			int priority;
			bool is_call_media;
		} register_media_elem;
		struct {
			int intensity;
		} set_intensity_elem;
	};
};

struct mediamgr {
	struct mqueue *mq;
	struct dict *sounds;

	enum mediamgr_state prev_call_state;
	enum mediamgr_state call_state;
	volatile bool started;

	pthread_t thread;
	mediamgr_mcat_changed_h *mcat_changed_h;
	void *arg;

	struct mm_route_state_machine router;

	int intensity_thres;

	mediamgr_route_changed_h *route_changed_h;
	void* route_changed_arg;
};


/* prototypes */
static void *mediamgr_thread(void *arg);

const char *MMroute2Str(enum mediamgr_auplay route)
{
    switch (route) {
            
        case MEDIAMGR_AUPLAY_EARPIECE:		return "Earpiece";
        case MEDIAMGR_AUPLAY_SPEAKER:		return "Speakerphone";
        case MEDIAMGR_AUPLAY_HEADSET:		return "Headset";
        case MEDIAMGR_AUPLAY_BT:			return "Bluetooth";
        case MEDIAMGR_AUPLAY_LINEOUT:		return "LINE";
        case MEDIAMGR_AUPLAY_SPDIF:			return "SPDIF";
        default: return "Unknown";
    }
}

static void mm_destructor(void *arg)
{
	struct mediamgr *mm = arg;

	if (mm->started) {
		mqueue_push(mm->mq, MM_MARSHAL_EXIT, NULL);

		/* waits untill re_cancel() is called on mediamgr_thread */
		pthread_join(mm->thread, NULL);
	}

	dict_flush(mm->sounds);
	mem_deref(mm->sounds);
	mem_deref(mm->mq);

	mm_platform_free(mm);
}


int mediamgr_alloc(struct mediamgr **mmp,
		   mediamgr_mcat_changed_h *mcat_handler, void *arg)
{
	struct mediamgr *mm;
	int err = 0;

	if (!mmp || !mcat_handler) {
		return EINVAL;
	}

	mm = mem_zalloc(sizeof(*mm), mm_destructor);
	if (!mm)
		return ENOMEM;

	err = dict_alloc(&mm->sounds);
	if (err) {
		goto out;
	}

	mm->started = false;

	err = pthread_create(&mm->thread, NULL, mediamgr_thread, mm);
	if (err != 0) {
		goto out;
	}
	for ( int cnt = 0; cnt < 10000; cnt++) {
		if (mm->started) {
			break;
		}
		usleep(1000);  /* 1ms so allow to wait 10 secs */
	}

	mm->mcat_changed_h = mcat_handler;
	mm->arg = arg;
	mm->router.cur_route = MEDIAMGR_AUPLAY_UNKNOWN;
	mm->route_changed_h = NULL;

	mm->intensity_thres = MM_INTENSITY_THRES_ALL;

 out:
	if (err) {
		mem_deref(mm);
	}
	else {
		*mmp = mm;
	}

	return err;
}


static bool stop_playing_during_call(char *key, void *val, void *arg)
{
	struct sound *snd = (struct sound *)val;

	(void)key;
	(void)arg;

	if (!snd->incall) {
		if (mm_platform_is_sound_playing(snd)) {
			mm_platform_stop_sound(snd);
		}
	}

	return false;
}


static void update_route(struct mediamgr *mm, mm_route_update_event event)
{
	int ret = 0;

	enum mediamgr_auplay cur_route = mm_platform_get_route();
	enum mediamgr_auplay wanted_route = cur_route;

	switch (event) {

	case MM_HEADSET_PLUGGED:
		/* Always switch to HS when plugged in */
		wanted_route = MEDIAMGR_AUPLAY_HEADSET;
		mm->router.wired_hs_is_connected = true;
		mm->router.prefer_loudspeaker = false;
		break;

	case MM_HEADSET_UNPLUGGED:
		if(mm->call_state == MEDIAMGR_STATE_INVIDEOCALL){
			mm->router.prefer_loudspeaker = true;
		}
		if(cur_route == MEDIAMGR_AUPLAY_SPEAKER) {
			wanted_route = MEDIAMGR_AUPLAY_SPEAKER;
		}
		else if (mm->router.bt_device_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_BT;
		}
		else if (mm->router.prefer_loudspeaker) {
			wanted_route = MEDIAMGR_AUPLAY_SPEAKER;
		}
		else {
			wanted_route = MEDIAMGR_AUPLAY_EARPIECE;
		}
		mm->router.wired_hs_is_connected = false;
		break;

	case MM_BT_DEVICE_CONNECTED:
		if (mm->call_state == MEDIAMGR_STATE_INCALL ||
			mm->call_state == MEDIAMGR_STATE_INVIDEOCALL){
            
			/* Always switch to BT when plugged in */
			wanted_route = MEDIAMGR_AUPLAY_BT;
		}
		mm->router.bt_device_is_connected = true;
		break;

	case MM_BT_DEVICE_DISCONNECTED:
		if (mm->router.wired_hs_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_HEADSET;
		}
		else if (mm->router.prefer_loudspeaker) {
			wanted_route = MEDIAMGR_AUPLAY_SPEAKER;
		}
		else {
			wanted_route = MEDIAMGR_AUPLAY_EARPIECE;
		}
		mm->router.bt_device_is_connected = false;
		break;

	case MM_SPEAKER_ENABLE_REQUEST:
		wanted_route = MEDIAMGR_AUPLAY_SPEAKER;
		mm->router.prefer_loudspeaker = true;
		break;

	case MM_SPEAKER_DISABLE_REQUEST:
		if (mm->router.wired_hs_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_HEADSET;
		}
		else if (mm->router.bt_device_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_BT;
		}
		else {
			wanted_route = MEDIAMGR_AUPLAY_EARPIECE;
		}
		mm->router.prefer_loudspeaker = false;
		break;

	case MM_CALL_START:
		mm->router.route_before_call = cur_route;
		if (mm->router.wired_hs_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_HEADSET;
		}
		else if (mm->router.bt_device_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_BT;
		}
		else if (mm->router.prefer_loudspeaker) {
			wanted_route = MEDIAMGR_AUPLAY_SPEAKER;
		}
		else {
			wanted_route = MEDIAMGR_AUPLAY_EARPIECE;
		}
		break;

	case MM_VIDEO_CALL_START:
		mm->router.route_before_call = cur_route;
		if (mm->router.wired_hs_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_HEADSET;
		}
		else if (mm->router.bt_device_is_connected) {
			wanted_route = MEDIAMGR_AUPLAY_BT;
		}
		else {
			wanted_route = MEDIAMGR_AUPLAY_SPEAKER;
		}
		mm->router.prefer_loudspeaker = true;
		mm->router.prefer_loudspeaker = false;
		break;
            
	case MM_CALL_STOP:
	case MM_VIDEO_CALL_STOP:
		mm->router.prefer_loudspeaker = false;
		wanted_route = MEDIAMGR_AUPLAY_EARPIECE;
		break;
	}

    info("mm: wanted_route = %s cur_route = %s \n",
         MMroute2Str(wanted_route), MMroute2Str(cur_route));
    
	if (wanted_route != cur_route) {

		switch (wanted_route) {

		case MEDIAMGR_AUPLAY_HEADSET:
			ret = mm_platform_enable_headset();
			break;

		case MEDIAMGR_AUPLAY_EARPIECE:
			ret = mm_platform_enable_earpiece();
			break;

		case MEDIAMGR_AUPLAY_SPEAKER:
			ret = mm_platform_enable_speaker();
			break;

		case MEDIAMGR_AUPLAY_BT:
			ret = mm_platform_enable_bt_sco();
			break;

		default:
			error("mediamgr: Unsupported device");
			break;
		}
	}

	/* Check that we got what we asked for */
	cur_route = mm_platform_get_route();
	if (wanted_route != cur_route && ret >= 0) {
		if (mm->call_state != MEDIAMGR_STATE_INCALL &&
			mm->call_state != MEDIAMGR_STATE_INVIDEOCALL) {
            
			cur_route = wanted_route;
		} else {
			error("mediamgr: Route Change didnt happen (wanted=%d, current=%d) (ret=%d)\n", wanted_route, cur_route, ret);
		}
		// SSJ waybe wait 100 ms and try again ??
		//     Android and BT dosnt change immidiatly
	}
	if (mm->route_changed_h) {
		mm->route_changed_h(cur_route, mm->route_changed_arg);
	}
}


static void mediamgr_enter_call(struct mediamgr *mm, struct dict *sounds)
{
	dict_apply(sounds, stop_playing_during_call, NULL);

	mm_platform_enter_call();
}

static void mediamgr_exit_call(struct mediamgr *mm, struct dict *sounds)
{
	mm_platform_exit_call();
}


static bool check_play_mode(char *key, void *val, void *arg)
{
	struct sound* snd = (struct sound*)val;
	mm_playback_mode *mode = (mm_playback_mode*)arg;

	(void)key;

	if (mm_platform_is_sound_playing(snd)) {
		if (snd->mixing) {
			*mode = MM_PLAYBACK_MIXING;
		}
		else {
			*mode = MM_PLAYBACK_EXCLUSIVE;
			return true;
		}
	}

	return false;
}


static bool stop_play(char *key, void *val, void *arg)
{
	struct sound *snd = (struct sound *)val;

	(void)key;
	(void)arg;

	if (mm_platform_is_sound_playing(snd)) {
		mm_platform_stop_sound(snd);
	}

	return false;
}


static bool mediamgr_can_play_sound(struct mediamgr *mm,
				    struct dict *sounds, struct sound *to_play)
{
	mm_playback_mode mode = MM_PLAYBACK_NONE;

	/* Check intensity setting */
	if (to_play->intensity > mm->intensity_thres) {
		return false;
	}

	/* Some sounds are not allowed in-call */
	bool incall = (mm->call_state == MEDIAMGR_STATE_INCALL ||
					mm->call_state == MEDIAMGR_STATE_INVIDEOCALL);
	if (!to_play->incall && incall) {
		return false;
	}

	if (to_play->priority > 0) {
		return true;
	}

	dict_apply(sounds, check_play_mode, &mode);

	/* Only allow 1 exclusive or many mixings */
	switch (mode) {

	case MM_PLAYBACK_NONE:
		return true;

	case MM_PLAYBACK_EXCLUSIVE:
		return false;

	case MM_PLAYBACK_MIXING:
		return to_play->mixing;
	}
}


static int mediamgr_post_media_command(struct mediamgr *mm,
				       mm_marshal_id cmd,
				       const char* media_name)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		return -1;
	}
	strncpy(elem->media_elem.media_name, media_name,
		sizeof(elem->media_elem.media_name) - 1);

	return mqueue_push(mm->mq, cmd, elem);
}


void mediamgr_play_media(struct mediamgr *mm, const char *media_name)
{
	if (mediamgr_post_media_command(mm, MM_MARSHAL_PLAY_MEDIA,
					media_name) != 0) {
		error("mediamgr_play_media failed \n");
	}
}


void mediamgr_pause_media(struct mediamgr *mm, const char *media_name)
{
	if (mediamgr_post_media_command(mm, MM_MARSHAL_PAUSE_MEDIA,
					media_name) != 0) {
		error("mediamgr_pause_media failed \n");
	}
}


void mediamgr_stop_media(struct mediamgr *mm, const char *media_name)
{
	if (mediamgr_post_media_command(mm, MM_MARSHAL_STOP_MEDIA,
					media_name) != 0) {
		error("mediamgr_stop_media failed \n");
	}
}


void mediamgr_set_call_state(struct mediamgr *mm, enum mediamgr_state state)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_set_call_state failed \n");
		return;
	}
	elem->state_elem.state = state;
	if (mqueue_push(mm->mq, MM_MARSHAL_CALL_STATE, elem) != 0) {
		error("mediamgr_set_call_state failed \n");
	}
}


void mediamgr_register_route_change_h(struct mediamgr *mm,
				      mediamgr_route_changed_h *handler,
				      void *arg)
{
	if (!mm) {
		error("mediamgr_register_route_change_h failed no mm \n");
		return;
	}

	mm->route_changed_h   = handler;
	mm->route_changed_arg = arg;
}


void mediamgr_enable_speaker(struct mediamgr *mm, bool enable)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_enable_speaker failed \n");
		return;
	}
	elem->bool_elem.val = enable;
	if (mqueue_push(mm->mq, MM_MARSHAL_ENABLE_SPEAKER, elem) != 0) {
		error("mediamgr_enable_speaker failed \n");
	}
}


void mediamgr_headset_connected(struct mediamgr *mm, bool connected)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_headset_connected failed \n");
		return;
	}
	elem->bool_elem.val = connected;
	if (mqueue_push(mm->mq, MM_MARSHAL_HEADSET_CONNECTED, elem) != 0) {
		error("mediamgr_headset_connected failed \n");
	}
}


void mediamgr_bt_device_connected(struct mediamgr *mm, bool connected)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_bt_device_connected failed \n");
		return;
	}
	elem->bool_elem.val = connected;
	if (mqueue_push(mm->mq, MM_MARSHAL_BT_DEVICE_CONNECTED, elem) != 0) {
		error("mediamgr_bt_device_connected failed \n");
	}
}


void mediamgr_register_media(struct mediamgr *mm,
			     const char *media_name,
			     void* media_object,
			     bool mixing,
			     bool incall,
			     int intensity,
			     int priority,
			     bool is_call_media)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_register_media failed \n");
		return;
	}
	debug("%s: \n", __FUNCTION__);
	strncpy(elem->register_media_elem.media_name, media_name, sizeof(elem->register_media_elem.media_name) - 1);
	elem->register_media_elem.media_object = media_object;
	elem->register_media_elem.mixing = mixing;
	elem->register_media_elem.incall = incall;
	elem->register_media_elem.intensity = intensity;
	elem->register_media_elem.priority = priority;
	elem->register_media_elem.is_call_media = is_call_media;
	if (mqueue_push(mm->mq, MM_MARSHAL_REGISTER_MEDIA, elem) != 0) {
		error("mediamgr_register_media failed \n");
	}
}


void mediamgr_unregister_media(struct mediamgr *mm, const char *media_name)
{
	struct mm_message *elem;
	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_unregister_media failed \n");
		return;
	}
	debug("%s: \n", __FUNCTION__);
	strncpy(elem->register_media_elem.media_name, media_name,
		sizeof(elem->register_media_elem.media_name) - 1);
	elem->register_media_elem.media_object = NULL;
	if (mqueue_push(mm->mq, MM_MARSHAL_DEREGISTER_MEDIA, elem) != 0) {
		error("mediamgr_unregister_media failed \n");
	}
}


void mediamgr_set_sound_mode(struct mediamgr *mm,
			     enum mediamgr_sound_mode mode)
{
	struct mm_message *elem;
	int intensity;

	switch (mode) {

	case MEDIAMGR_SOUND_MODE_ALL:
		intensity =  MM_INTENSITY_THRES_ALL;
		break;

	case MEDIAMGR_SOUND_MODE_SOME:
		intensity =  MM_INTENSITY_THRES_SOME;
		break;

	case MEDIAMGR_SOUND_MODE_NONE:
		intensity =  MM_INTENSITY_THRES_NONE;
		break;
	}

	elem = mem_zalloc(sizeof(struct mm_message), NULL);
	if (!elem) {
		error("mediamgr_set_sound_mode failed \n");
		return;
	}
	debug("%s: set mode to %u\n", __FUNCTION__, intensity);
	elem->set_intensity_elem.intensity = intensity;
	if (mqueue_push(mm->mq, MM_MARSHAL_SET_INTENSITY, elem) != 0) {
		error("mediamgr_set_sound_mode failed \n");
	}
}


enum mediamgr_auplay mediamgr_get_route(const struct mediamgr *mm)
{
	return mm_platform_get_route();
}


static void mqueue_handler(int id, void *data, void *arg)
{
	struct mediamgr *mm = (struct mediamgr*)arg;
	struct mm_marshal_elem *marshal = (struct mm_marshal_elem*)data;
	struct sound *curr_sound;

	switch ((mm_marshal_id)id) {

	case MM_MARSHAL_EXIT:
		re_cancel();
		break;

	case MM_MARSHAL_PLAY_MEDIA: {
		const char *mname = ((struct mm_message*)marshal)->media_elem.media_name;

		curr_sound = dict_lookup(mm->sounds, mname);
		if (!curr_sound) {
			error("%s: couldn't find media %s\n",
			      __FUNCTION__, mname);
		}
		else {
			debug("%s: want to play media %s \n",
			      __FUNCTION__, mname);

			if (mediamgr_can_play_sound(mm, mm->sounds,
						    curr_sound)) {
				if (curr_sound->priority > 0) {
					debug("%s: stop other media\n",
					      __FUNCTION__);
					dict_apply(mm->sounds, stop_play, NULL);
				}
				debug("%s: play media %s \n",
				      __FUNCTION__, mname);

				if (curr_sound->is_call_media
					&& (mm->call_state != MEDIAMGR_STATE_INCALL &&
						mm->call_state != MEDIAMGR_STATE_INVIDEOCALL)) {
					mm_platform_enter_call();
					update_route(mm, MM_CALL_START);
				}
				mm_platform_play_sound(curr_sound);
			}
		}
	}
		break;

	case MM_MARSHAL_PAUSE_MEDIA: {
		const char *mname = ((struct mm_message*)marshal)->media_elem.media_name;

		curr_sound = dict_lookup(mm->sounds, mname);
		if (!curr_sound) {
			error("%s: couldn't find media %s\n",
			      __FUNCTION__, mname);
		}
		else {
			mm_platform_pause_sound(curr_sound);
		}
	}
		break;

	case MM_MARSHAL_STOP_MEDIA: {
		const char *mname = ((struct mm_message*)marshal)->media_elem.media_name;

		curr_sound = dict_lookup(mm->sounds, mname);
		if (!curr_sound) {
			error("%s: couldn't find media %s\n", __FUNCTION__, mname);
		}
		else {
			mm_platform_stop_sound(curr_sound);
			if (curr_sound->is_call_media
				&& (mm->call_state != MEDIAMGR_STATE_INCALL &&
					mm->call_state != MEDIAMGR_STATE_INVIDEOCALL)) {
				mm_platform_exit_call();
				update_route(mm, MM_CALL_STOP);
			}
		}
	}
		break;

	case MM_MARSHAL_CALL_STATE: {
		mm_route_update_event event;
		bool has_changed = false;
		bool fire_callback = false;

		switch (((struct mm_message*)marshal)->state_elem.state) {

		case MEDIAMGR_STATE_INCALL:
			mm->call_state = MEDIAMGR_STATE_INCALL;
			mediamgr_enter_call(mm, mm->sounds);
			event = MM_CALL_START;
			has_changed = true;
			fire_callback = true;
			break;

		case MEDIAMGR_STATE_INVIDEOCALL:
			mm->call_state = MEDIAMGR_STATE_INVIDEOCALL;
			mediamgr_enter_call(mm, mm->sounds);
			event = MM_VIDEO_CALL_START;
			has_changed = true;
			fire_callback = false;
			break;
                
		case MEDIAMGR_STATE_NORMAL:
			mm->call_state = MEDIAMGR_STATE_NORMAL;
			mediamgr_exit_call(mm, mm->sounds);
			event = MM_CALL_STOP;
			has_changed = true;
			fire_callback = true;
			break;

		case MEDIAMGR_STATE_HOLD:
			if(mm->call_state == MEDIAMGR_STATE_INCALL ||
				mm->call_state == MEDIAMGR_STATE_INVIDEOCALL) {
				info("%s: putting call on hold\n", __FUNCTION__);
				mm->prev_call_state = mm->call_state;
				mm->call_state = MEDIAMGR_STATE_HOLD;
				event = MM_CALL_STOP;
				has_changed = true;
				fire_callback = true;
			}
			break;

		case MEDIAMGR_STATE_RESUME:
			if(mm->call_state == MEDIAMGR_STATE_HOLD) {
				info("%s: resuming call\n", __FUNCTION__);
				event = MM_CALL_START;
				has_changed = true;
				fire_callback = true;
				mm->call_state =  mm->prev_call_state;
				mediamgr_enter_call(mm, mm->sounds);
			}
			break;

		}

		if (has_changed) {
			update_route(mm, event);
		}
		if(fire_callback){
			enum mediamgr_state new_state = ((struct mm_message*)marshal)->state_elem.state;
			debug("%s: calling mcat changed %d\n", __FUNCTION__, new_state);
			mm->mcat_changed_h(new_state, mm->arg);
		}
	}
		break;

	case MM_MARSHAL_ENABLE_SPEAKER: {
		bool enable = ((struct mm_message*)marshal)->bool_elem.val;

		mm_route_update_event event;
		if (enable) {
			event = MM_SPEAKER_ENABLE_REQUEST;
		}
		else {
			event = MM_SPEAKER_DISABLE_REQUEST;
		}
		update_route(mm, event);
	}
		break;

	case MM_MARSHAL_HEADSET_CONNECTED: {
		bool connected = ((struct mm_message*)marshal)->bool_elem.val;

		mm_route_update_event event;
		if (connected) {
			event = MM_HEADSET_PLUGGED;
		}
		else {
			event = MM_HEADSET_UNPLUGGED;
		}
		update_route(mm, event);
	}
		break;

	case MM_MARSHAL_BT_DEVICE_CONNECTED: {
		bool connected = ((struct mm_message*)marshal)->bool_elem.val;

		mm_route_update_event event;
		if (connected) {
			event = MM_BT_DEVICE_CONNECTED;
		}
		else {
			event = MM_BT_DEVICE_DISCONNECTED;
		}
		update_route(mm, event);
	}
		break;

	case MM_MARSHAL_REGISTER_MEDIA: {
		const char *mname = ((struct mm_message*)marshal)->register_media_elem.media_name;
		void *mobject = ((struct mm_message*)marshal)->register_media_elem.media_object;
		bool mixing = ((struct mm_message*)marshal)->register_media_elem.mixing;
		bool incall = ((struct mm_message*)marshal)->register_media_elem.incall;
		int intensity = ((struct mm_message*)marshal)->register_media_elem.intensity;
		int priority = ((struct mm_message*)marshal)->register_media_elem.priority;
		int is_call_media = ((struct mm_message*)marshal)->register_media_elem.is_call_media;

		mm_platform_registerMedia(mm->sounds, mname, mobject, mixing, incall, intensity, priority, is_call_media);
	}
		break;

	case MM_MARSHAL_DEREGISTER_MEDIA: {
		const char *mname = ((struct mm_message*)marshal)->register_media_elem.media_name;

		mm_platform_unregisterMedia(mm->sounds, mname);
	}
		break;

	case MM_MARSHAL_SET_INTENSITY:
		mm->intensity_thres = ((struct mm_message*)marshal)->set_intensity_elem.intensity;
		break;
	}

	mem_deref(data);
}


static void *mediamgr_thread(void *arg)
{
	struct mediamgr *mm = (struct mediamgr*)arg;
	int err;

	err = re_thread_init();
	if (err) {
		warning("mediamgr_thread: re_thread_init failed (%m)\n", err);
		goto out;
	}

	err = mqueue_alloc(&mm->mq, mqueue_handler, mm);
	if (err) {
		error("mediamgr_thread: cannot allocate mqueue (%m)\n", err);
		goto out;
	}

	debug("%s: read %d sounds\n", __FUNCTION__, dict_count(mm->sounds));

	if (mm_platform_init(mm, mm->sounds) != 0) {
		error("%s: failed to load init media manager platform\n", __FUNCTION__);
		goto out;
	}

	mm->started = true;
	re_main(NULL);
	info("%s thread exiting\n", __FUNCTION__);

	re_thread_close();

out:
	return NULL;
}

