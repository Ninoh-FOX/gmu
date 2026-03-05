/*
 * GMU Music Player - Decoder Game Music Emu (libgme)
 * Multitrack support with safe threaded manual switching and auto-advance.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gme/gme.h>

#include "../gmudecoder.h"
#include "../trackinfo.h"
#include "../util.h"
#include "../reader.h"
#include "../debug.h"

static Music_Emu *emu = NULL;
static int current_track = 0;
static int sample_rate = 44100;
static int total_tracks = 1;
static int track_ended = 0;

/* Variables de control Multitrack */
static int track_changed_flag = 0;
static int default_loop_time_ms = 150000; /* 2.5 min loop */
static volatile int pending_track_change = 0; /* Bandera para hilos */

static TrackInfo ti_metaonly;

static const char *get_name(void) { return "Game Music Emu decoder v0.3"; }
static const char *get_file_extensions(void) { return ".nsf;.nsfe;.spc;.rsn;.ay;.gym;.hes;.gbs;.kss;.vgm;.vgz;.sap"; }

static void play_current_subtrack(void)
{
	gme_start_track(emu, current_track);

	gme_info_t *info = NULL;
	if (!gme_track_info(emu, &info, current_track)) {
		int fade_time = (info->length > 0) ? info->length : default_loop_time_ms;
		gme_set_fade(emu, fade_time, 8000);
		gme_free_info(info);
	}
	track_changed_flag = 1;
}

static int open_file(const char *filename)
{
	if (emu) {
		gme_delete(emu);
		emu = NULL;
	}
	sample_rate = 44100;
	current_track = 0;
	track_ended = 0;
	track_changed_flag = 0;
	pending_track_change = 0;

	if (gme_open_file(filename, &emu, sample_rate) != NULL) {
		emu = NULL;
		return 0;
	}
	total_tracks = gme_track_count(emu);
	play_current_subtrack();
	return 1;
}

static int close_file(void)
{
	if (emu) {
		gme_delete(emu);
		emu = NULL;
	}
	return 0;
}

static int get_decoder_buffer_size(void) { return 4096 * 2 * sizeof(short); }

static int decode_data(char *target, size_t max_size)
{
	if (!emu || track_ended) return 0;

	/* 1. Comprobar si el usuario presionó un botón (Seguro para Hilos) */
	if (pending_track_change != 0) {
		current_track += pending_track_change;
		pending_track_change = 0;
		play_current_subtrack();
	} 
	/* 2. Si no, comprobar si la pista terminó sola (Auto-Avance) */
	else if (gme_track_ended(emu)) {
		if (current_track < total_tracks - 1) {
			current_track++;
			play_current_subtrack();
		} else {
			track_ended = 1;
			return 0;
		}
	}
	
	int num_samples = max_size / (2 * sizeof(short));
	void *p = target;
	gme_err_t err = gme_play(emu, num_samples * 2, (short*)p);
	if (err) {
		track_ended = 1;
		return 0;
	}
	return max_size;
}

static int seek(int seconds)
{
	if (!emu) return 0;
	return (gme_seek(emu, seconds * 1000) == NULL) ? 1 : 0;
}

static int get_length(void) { return 0; }
static int get_samplerate(void) { return sample_rate; }
static int get_channels(void) { return 2; }

static const char *get_meta_data(GmuMetaDataType type, int for_current_file)
{
	static char meta[256];
	gme_info_t *info = NULL;

	if (for_current_file) {
		if (!emu) return NULL;
		if (gme_track_info(emu, &info, current_track)) return NULL;
		switch (type) {
			case GMU_META_TITLE:
				strncpy(meta, info->song ? info->song : "", sizeof(meta) - 1);
				break;
			case GMU_META_ARTIST:
				strncpy(meta, info->author ? info->author : "", sizeof(meta) - 1);
				break;
			case GMU_META_ALBUM:
				strncpy(meta, info->game ? info->game : "", sizeof(meta) - 1);
				break;
			default:
				meta[0] = 0;
		}
		gme_free_info(info);
	} else {
		switch (type) {
			case GMU_META_TITLE:
				strncpy(meta, ti_metaonly.title, sizeof(meta) - 1);
				break;
			case GMU_META_ARTIST:
				strncpy(meta, ti_metaonly.artist, sizeof(meta) - 1);
				break;
			case GMU_META_ALBUM:
				strncpy(meta, ti_metaonly.album, sizeof(meta) - 1);
				break;
			default:
				meta[0] = 0;
		}
	}
	return meta;
}

static int get_meta_data_int(GmuMetaDataType type, int for_current_file)
{
	if (for_current_file && type == GMU_META_IS_UPDATED) {
		if (track_changed_flag) {
			track_changed_flag = 0;
			return 1;
		}
	}
	return 0;
}

static int meta_data_load(const char *filename)
{
	Music_Emu  *meta_emu = NULL;
	gme_info_t *info = NULL;

	if (gme_open_file(filename, &meta_emu, sample_rate) != NULL) return 0;
	if (!meta_emu) return 0;
	if (gme_track_info(meta_emu, &info, 0)) return 0;

	/* Magia para la Playlist: Asignamos el nombre del JUEGO a la variable TÍTULO.
	 * Así Gmu mostrará "Autor - Juego" en la lista. */
	const char *display_title = (info->game && info->game[0] != '\0') ? info->game : 
	                            (info->song ? info->song : "Unknown Game");

	strncpy(ti_metaonly.title,  display_title, sizeof(ti_metaonly.title) - 1);
	
	strncpy(ti_metaonly.artist, info->author ? info->author : "", sizeof(ti_metaonly.artist) - 1);
	strncpy(ti_metaonly.album,  info->game   ? info->game   : "", sizeof(ti_metaonly.album) - 1);
	ti_metaonly.length = (info->length > 0 ? info->length / 1000 : 0);
	
	gme_free_info(info);
	gme_delete(meta_emu);
	return 1;
}

static int meta_data_close(void) { return 1; }
static GmuCharset meta_data_get_charset(void) { return M_CHARSET_UTF_8; }
static void set_reader_handle(Reader *r) { (void)r; }

/* Funciones invocadas desde la interfaz mediante fileplayer.c */
static int next_subtrack(void) {
	if (!emu) return 0;
	if (current_track + pending_track_change < total_tracks - 1) {
		pending_track_change++;
		return 1;
	}
	return 0;
}

static int prev_subtrack(void) {
	if (!emu) return 0;
	if (current_track + pending_track_change > 0) {
		pending_track_change--;
		return 1;
	}
	return 0;
}

static GmuDecoder gd = {
	"gme_decoder",
	NULL,
	NULL,
	get_name,
	NULL,
	get_file_extensions,
	NULL,
	open_file,
	close_file,
	decode_data,
	seek,
	NULL,
	get_meta_data,
	get_meta_data_int,
	get_samplerate,
	get_channels,
	get_length,
	NULL,
	NULL,
	get_decoder_buffer_size,
	meta_data_load,
	meta_data_close,
	meta_data_get_charset,
	NULL,
	set_reader_handle,
	next_subtrack, /* Conectado */
	prev_subtrack, /* Conectado */
	NULL
};

GmuDecoder *GMU_REGISTER_DECODER(void) { return &gd; }