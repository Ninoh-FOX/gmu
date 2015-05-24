/* 
 * Gmu Music Player
 *
 * Copyright (c) 2006-2015 Johannes Heimansberg (wejp.k.vu)
 *
 * File: log.c  Created: 091218
 *
 * Description: Gmu log bot client (for logging played tracks)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of
 * the License. See the file COPYING in the Gmu's main directory
 * for details.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../gmufrontend.h"
#include "../core.h"
#include "../fileplayer.h"
#include "../debug.h"

static int       logging_enabled = 0;
static TrackInfo previous;
static int       minimum_playtime_seconds = 0, minimum_playtime_percent = 0;
static FILE     *lf = NULL;

static const char *get_name(void)
{
	return "Gmu Log Bot v0.1";
}

static void shut_down(void)
{
	if (lf) {
		wdprintf(V_DEBUG, "logbot", "Closing file.\n");
		fclose(lf);
		sync();
	}
}

static int init(void)
{
	ConfigFile *cf = gmu_core_get_config();

	gmu_core_config_acquire_lock();
	cfg_add_key(cf, "Log.Enable", "no");
	cfg_key_add_presets(cf, "Log.Enable", "yes", "no", NULL);
	cfg_add_key(cf, "Log.MinimumPlaytimeSec", "10");
	cfg_key_add_presets(cf, "Log.MinimumPlaytimeSec", "5", "10", "30", "60", "90", NULL);
	cfg_add_key(cf, "Log.MinimumPlaytimePercent", "10");
	cfg_key_add_presets(cf, "Log.MinimumPlaytimePercent", "1", "10", "25", "50", "75", NULL);
	cfg_add_key(cf, "Log.File", "gmutracks.log");
	if (cfg_get_boolean_value(cf, "Log.Enable")) {
		const char *logfile;

		wdprintf(V_INFO, "logbot", "Initializing logger.\n");
		logging_enabled = 1;
		logfile = cfg_get_key_value(cf, "Log.File");
		if (!logfile) logfile = "gmutracks.log";
		if (!(lf = fopen(logfile, "a"))) logging_enabled = 0;
		if (logging_enabled) wdprintf(V_INFO, "logbot", "Logging to %s\n", logfile);

		minimum_playtime_seconds = cfg_get_int_value(cf, "Log.MinimumPlaytimeSec");
		if (minimum_playtime_seconds <= 0) minimum_playtime_seconds = 30;
		minimum_playtime_percent = cfg_get_int_value(cf, "Log.MinimumPlaytimePercent");
		if (minimum_playtime_percent <= 0) minimum_playtime_percent = 50;
	} else {
		wdprintf(V_INFO, "logbot", "Logging has been disabled.\n");
	}
	gmu_core_config_release_lock();
	trackinfo_clear(&previous);
	return logging_enabled;
}

static void save_previous_trackinfo()
{
	time_t     ttime;
	struct tm *t;
	char      *time_str = NULL;
	int        i = 0;

	time(&ttime);
	t = localtime(&ttime);
	time_str = asctime(t);
	i = strlen(time_str);
	time_str[i-1] = '\0'; /* Strip the '\n' at the end of the string */

	/* Save trackinfo of previous track to logfile... */
	if (trackinfo_get_channels(&previous) && lf) {
		fprintf(lf, "%s;\"%s\";\"%s\";\"%s\";%d:%02d\n", time_str,
		        trackinfo_get_artist(&previous), trackinfo_get_title(&previous),
		        trackinfo_get_album(&previous), trackinfo_get_length_minutes(&previous),
		        trackinfo_get_length_seconds(&previous));
		fflush(lf);
	}
}

static void update_trackinfo(TrackInfo *ti)
{
	if (ti && trackinfo_acquire_lock(ti)) {
		trackinfo_copy(&previous, ti);
		trackinfo_release_lock(ti);
	} else {
		trackinfo_clear(&previous);
	}
}

static int event_callback(GmuEvent event, int param)
{
	if (logging_enabled) {
		static int pt = 0, mtp = 0;

		switch (event) {
			case GMU_QUIT:
				break;
			case GMU_TRACKINFO_CHANGE:
				/* Check if there is track info data in the buffer that needs to
				 * be written to the log (minimum playtime requirement check)... */
				if (pt >= mtp &&
				    (pt >= minimum_playtime_seconds ||
				     minimum_playtime_seconds > gmu_core_get_length_current_track())) {
					/* Get track info for the current track */
					save_previous_trackinfo();
				}
				update_trackinfo(gmu_core_get_current_trackinfo_ref());
				break;
			case GMU_PLAYBACK_STATE_CHANGE:
				pt = file_player_playback_get_time() / 1000;
				mtp = (gmu_core_get_length_current_track() * minimum_playtime_percent) / 100;
				if (gmu_core_get_status() == STOPPED && pt >= mtp &&
				    (pt >= minimum_playtime_seconds ||
				     minimum_playtime_seconds > gmu_core_get_length_current_track())) {
					save_previous_trackinfo();
					trackinfo_clear(&previous);
				}
				break;
			default:
				break;
		}
	}
	return 0;
}

static GmuFrontend gf = {
	"logbot",
	get_name,
	init,
	shut_down,
	NULL,
	event_callback,
	NULL
};

GmuFrontend *gmu_register_frontend(void)
{
	return &gf;
}
