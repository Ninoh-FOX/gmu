/*
 * GMU Music Player - Decoder PlayGSF (GBA Audio)
 * Archivo puente fusionado con la lógica original de linuxmain.cpp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

extern "C" {
    #include "../../gmudecoder.h"
    #include "../../trackinfo.h"
    #include "../../util.h"
    #include "../../debug.h"
    
    // Inyectamos nuestro wrapper de tipos ANTES de cargar las cabeceras GSF
    #include "windows.h" 
    
    // Cabeceras de tu motor GSF original
    #include "VBA/psftag.h"
    #include "gsf.h"
    
    // Funciones del motor que NO están expuestas en los .h
    int LengthFromString(const char * timestring);
}

/* --- VARIABLES GLOBALES REQUERIDAS POR EL CORE VBA --- */
extern "C" {
    int defvolume=1000, relvolume=1000;
    int TrackLength=0, FadeLength=0;
    int IgnoreTrackLength=0, DefaultLength=150000;
    int playforever=0, fileoutput=0;
    int TrailingSilence=1000;
    int DetectSilence=1, silencedetected=0, silencelength=5;
    int noinfo=0;
    
    int cpupercent=0, sndSamplesPerSec=44100, sndNumChannels=2;
    int sndBitsPerSample=16;
    int bass_boost_enabled = 0; // Activar el Bass Boost por defecto
    int deflen=120, deffade=10;
    
    extern unsigned short soundFinalWave[2304];
    extern int soundBufferLen;
}

double decode_pos_ms = 0;
int seek_needed = -1;
static int g_playing = 0;

/* --- FILTRO LOW-SHELF (BASS BOOST) --- */
static float b0_ls, b1_ls, b2_ls, a1_ls, a2_ls;
static float x1L=0, x2L=0, y1L=0, y2L=0;
static float x1R=0, x2R=0, y1R=0, y2R=0;

static void lowshelf_init(float fs, float f0, float gainDB) {
    float A  = powf(10.0f, gainDB / 40.0f);
    float w0 = 2.0f * M_PI * f0 / fs;
    float alpha = sinf(w0) / 2.0f * sqrtf( (A + 1/A) * (1.0f/0.707f - 1.0f) + 2.0f );
    float k = cosf(w0);
    float a0f = (A+1) + (A-1)*k + 2.0f*sqrtf(A)*alpha;

    b0_ls = (A*((A+1) - (A-1)*k + 2.0f*sqrtf(A)*alpha)) / a0f;
    b1_ls = (2*A*((A-1) - (A+1)*k)) / a0f;
    b2_ls = (A*((A+1) - (A-1)*k - 2.0f*sqrtf(A)*alpha)) / a0f;
    a1_ls = (-2*((A-1) + (A+1)*k)) / a0f;
    a2_ls = ((A+1) + (A-1)*k - 2.0f*sqrtf(A)*alpha) / a0f;
}

static void lowshelf_process(short *samples, int count) {
    for (int i = 0; i < count; i += 2) {
        float inL = samples[i], inR = samples[i+1];
        float outL = b0_ls*inL + b1_ls*x1L + b2_ls*x2L - a1_ls*y1L - a2_ls*y2L;
        float outR = b0_ls*inR + b1_ls*x1R + b2_ls*x2R - a1_ls*y1R - a2_ls*y2R;
        x2L = x1L; x1L = inL; y2L = y1L; y1L = outL;
        x2R = x1R; x1R = inR; y2R = y1R; y1R = outR;
        
        if (outL > 32767.0f) { outL = 32767.0f; }
        if (outL < -32768.0f) { outL = -32768.0f; }
        if (outR > 32767.0f) { outR = 32767.0f; }
        if (outR < -32768.0f) { outR = -32768.0f; }
        
        samples[i] = (short)outL; 
        samples[i+1] = (short)outR;
    }
}

extern "C" void end_of_track() { g_playing = 0; }

/* --- BÚFER CIRCULAR --- */
#define GSF_BUFFER_SIZE (1024 * 512 * 2)
#define GSF_PREBUFFER_BYTES 4096
static char gsf_buffer[GSF_BUFFER_SIZE];
static int buf_read_pos = 0, buf_write_pos = 0, buf_filled_bytes = 0;
static pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  buf_cond  = PTHREAD_COND_INITIALIZER;
static volatile int emu_running = 0;
static pthread_t emu_thread;
static char current_rom_path[512];

/* --- GESTIÓN DE METADATOS Y FORMATO DE PLAYLIST --- */
typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char tracknr[16];
    char date[16];
    char custom_list_name[512]; // Aquí guardaremos la frase entera formateada
} GsfTags;

static GsfTags tags_metaonly; 
static GsfTags tags_current;  

static void safe_copy_tag(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void gsf_read_tags(const char *filename, GsfTags *tags) {
    tags->title[0] = '\0';
    tags->artist[0] = '\0';
    tags->album[0] = '\0';
    tags->tracknr[0] = '\0';
    tags->date[0] = '\0';
    tags->custom_list_name[0] = '\0';

    char *tag_buffer = (char*)malloc(50001); 
    if (tag_buffer) {
        if (psftag_readfromfile(tag_buffer, filename) >= 0) {
            char tmp[256];
            if (!psftag_getvar(tag_buffer, "title", tmp, sizeof(tmp)-1)) 
                safe_copy_tag(tags->title, sizeof(tags->title), tmp);
            if (!psftag_getvar(tag_buffer, "artist", tmp, sizeof(tmp)-1)) 
                safe_copy_tag(tags->artist, sizeof(tags->artist), tmp);
            if (!psftag_getvar(tag_buffer, "game", tmp, sizeof(tmp)-1)) 
                safe_copy_tag(tags->album, sizeof(tags->album), tmp);
            if (!psftag_getvar(tag_buffer, "track", tmp, sizeof(tmp)-1)) 
                safe_copy_tag(tags->tracknr, sizeof(tags->tracknr), tmp);
            if (!psftag_getvar(tag_buffer, "year", tmp, sizeof(tmp)-1)) 
                safe_copy_tag(tags->date, sizeof(tags->date), tmp);
        }
        free(tag_buffer); 
    }
    
    // --- CONSTRUIR EL TEXTO PARA LA PLAYLIST: "Álbum, Título, Artista, Fecha" ---
    char temp[512] = {0};
    
    if (tags->album[0])  { strcat(temp, tags->album); }
    if (tags->title[0])  { if(temp[0]) strcat(temp, ", "); strcat(temp, tags->title); }
    if (tags->artist[0]) { if(temp[0]) strcat(temp, ", "); strcat(temp, tags->artist); }
    if (tags->date[0])   { if(temp[0]) strcat(temp, ", "); strcat(temp, tags->date); }
    
    // Si la pista no tuviera tags (muy raro en GSF), ponemos un texto por defecto
    if (temp[0] == '\0') {
        strcpy(temp, "Pista de GBA (Sin Tags)");
    }
    
    safe_copy_tag(tags->custom_list_name, sizeof(tags->custom_list_name), temp);
}

/* --- INTERCEPTAR AUDIO --- */
extern "C" void writeSound(void) {
    if (!emu_running) return;

    int ret = soundBufferLen;
    static short tempBuffer[2304];
    memcpy(tempBuffer, soundFinalWave, ret);

    int time_to_end_ms = TrackLength - FadeLength;
    if (time_to_end_ms < 0) time_to_end_ms = 0;
    if (time_to_end_ms <= FadeLength) {
        float factor = (float)time_to_end_ms / (float)FadeLength;
        if (factor < 0.0f) factor = 0.0f;
        int samplesCount = ret / sizeof(short);
        for (int i = 0; i < samplesCount; i++) tempBuffer[i] = (short)(tempBuffer[i] * factor);
    }

    if (bass_boost_enabled) { lowshelf_process(tempBuffer, ret / sizeof(short)); }

    int bytes_to_write = ret;
    char *data = (char*)tempBuffer;

    pthread_mutex_lock(&buf_mutex);
    while (emu_running && (GSF_BUFFER_SIZE - buf_filled_bytes) < bytes_to_write) {
        pthread_cond_wait(&buf_cond, &buf_mutex);
    }
    if (!emu_running) { pthread_mutex_unlock(&buf_mutex); return; }

    {
        int first_chunk = GSF_BUFFER_SIZE - buf_write_pos;
        if (first_chunk > bytes_to_write) first_chunk = bytes_to_write;
        memcpy(gsf_buffer + buf_write_pos, data, first_chunk);
        if (bytes_to_write > first_chunk) {
            memcpy(gsf_buffer, data + first_chunk, bytes_to_write - first_chunk);
        }
        buf_write_pos = (buf_write_pos + bytes_to_write) % GSF_BUFFER_SIZE;
        buf_filled_bytes += bytes_to_write;
    }
    pthread_cond_signal(&buf_cond);
    pthread_mutex_unlock(&buf_mutex);

    decode_pos_ms += (ret / (2 * sndNumChannels)) * 1000.0 / sndSamplesPerSec;
}

/* --- HILO DEL EMULADOR --- */
static void* gsf_emulation_thread(void *arg) {
    decode_pos_ms = 0;
    if (!GSFRun(current_rom_path)) { emu_running = 0; return NULL; }
    
    g_playing = 1;
    lowshelf_init((float)sndSamplesPerSec, 250.0f, 5.0f);

    while (g_playing && emu_running) {
        EmulationLoop();
    }
    emu_running = 0;
    return NULL;
}

/* --- API GMU DECODER --- */
static const char *get_name(void) { return "PlayGSF Decoder (ARM32)"; }
static const char *get_file_extensions(void) { return ".gsf;.minigsf"; }

static int open_file(const char *filename) {
    strncpy(current_rom_path, filename, sizeof(current_rom_path)-1);
    current_rom_path[sizeof(current_rom_path)-1] = '\0';
    
    gsf_read_tags(filename, &tags_current);
    
    char fade_str[256], length_str[256];
    char *tag_buffer = (char*)malloc(50001); 
    
    if (tag_buffer) {
        if (psftag_readfromfile(tag_buffer, filename) >= 0) {
            if (!psftag_getvar(tag_buffer, "fade", fade_str, sizeof(fade_str)-1)) {
                FadeLength = LengthFromString(fade_str);
            } else {
                FadeLength = 10000;
            }
            if (!psftag_raw_getvar(tag_buffer, "length", length_str, sizeof(length_str)-1)) {
                TrackLength = LengthFromString(length_str) + FadeLength;
            } else {
                TrackLength = DefaultLength;
            }
        } else {
            FadeLength = 10000;
            TrackLength = DefaultLength;
        }
        free(tag_buffer);
    } else {
        FadeLength = 10000;
        TrackLength = DefaultLength;
    }

    pthread_mutex_lock(&buf_mutex);
    buf_read_pos = 0; buf_write_pos = 0; buf_filled_bytes = 0;
    emu_running = 1;
    pthread_mutex_unlock(&buf_mutex);

    if (pthread_create(&emu_thread, NULL, gsf_emulation_thread, NULL) != 0) { emu_running = 0; return 0; }
    return 1;
}

static int close_file(void) {
    if (emu_running) {
        pthread_mutex_lock(&buf_mutex);
        emu_running = 0; g_playing = 0;
        pthread_cond_signal(&buf_cond);
        pthread_mutex_unlock(&buf_mutex);
        pthread_join(emu_thread, NULL);
    }
    return 0;
}

static int get_decoder_buffer_size(void) { return 4096 * 2 * sizeof(short); }

static int decode_data(char *target, size_t max_size) {
    if (!emu_running && buf_filled_bytes == 0) return 0;

    pthread_mutex_lock(&buf_mutex);
    while (emu_running && buf_filled_bytes < GSF_PREBUFFER_BYTES) {
        pthread_cond_wait(&buf_cond, &buf_mutex);
    }

    int bytes_to_read = (buf_filled_bytes < (int)max_size) ? buf_filled_bytes : (int)max_size;

    if (bytes_to_read > 0) {
        int first_chunk = GSF_BUFFER_SIZE - buf_read_pos;
        if (first_chunk > bytes_to_read) first_chunk = bytes_to_read;
        memcpy(target, gsf_buffer + buf_read_pos, first_chunk);
        if (bytes_to_read > first_chunk) {
            memcpy(target + first_chunk, gsf_buffer, bytes_to_read - first_chunk);
        }
        buf_read_pos = (buf_read_pos + bytes_to_read) % GSF_BUFFER_SIZE;
        buf_filled_bytes -= bytes_to_read;
    }
    pthread_cond_signal(&buf_cond);
    pthread_mutex_unlock(&buf_mutex);

    return bytes_to_read;
}

static int seek(int seconds) { return 0; }
static int get_length(void) { return TrackLength / 1000; }
static int get_samplerate(void) { return sndSamplesPerSec; }
static int get_channels(void) { return sndNumChannels; }

static const char *get_meta_data(GmuMetaDataType type, int for_current_file) {
    GsfTags *tags = for_current_file ? &tags_current : &tags_metaonly;
    
    if (!for_current_file) {
        switch (type) {
            case GMU_META_TITLE:   return tags->custom_list_name; 
            case GMU_META_ARTIST:  return ""; 
            case GMU_META_ALBUM:   return ""; 
            default: return ""; 
        }
    } else {
        switch (type) {
            case GMU_META_TITLE:   return tags->title;
            case GMU_META_ARTIST:  return tags->artist;
            case GMU_META_ALBUM:   return tags->album;
            case GMU_META_TRACKNR: return tags->tracknr;
            case GMU_META_DATE:    return tags->date;
            default: return ""; 
        }
    }
}

static int meta_data_load(const char *filename) {
    gsf_read_tags(filename, &tags_metaonly);
    return 1;
}

static int get_bitrate(void) { 
    return sndSamplesPerSec * sndNumChannels * sndBitsPerSample; 
}
static const char *get_file_type(void) { 
    return "Game Boy Advance Audio (GSF)"; 
}

static int get_meta_data_int(GmuMetaDataType type, int for_current_file) { return 0; }
static int meta_data_close(void) { return 1; }
static GmuCharset meta_data_get_charset(void) { return M_CHARSET_UTF_8; }
static void set_reader_handle(Reader *r) { (void)r; }
static int next_subtrack(void) { return 0; }
static int prev_subtrack(void) { return 0; }

static GmuDecoder gd = {
    "gsf_decoder",           // identifier
    NULL,                    // init_decoder
    NULL,                    // close_decoder
    get_name,                // get_name
    NULL,                    // get_info
    get_file_extensions,     // get_file_extensions
    NULL,                    // get_mime_types
    open_file,               // open_file
    close_file,              // close_file
    decode_data,             // decode_data
    seek,                    // seek
    get_bitrate,             // get_current_bitrate (Añadido)
    get_meta_data,           // get_meta_data
    get_meta_data_int,       // get_meta_data_int
    get_samplerate,          // get_samplerate
    get_channels,            // get_channels
    get_length,              // get_length
    get_bitrate,             // get_bitrate (Añadido)
    get_file_type,           // get_file_type (Añadido)
    get_decoder_buffer_size, // get_decoder_buffer_size
    meta_data_load,          // meta_data_load
    meta_data_close,         // meta_data_close
    meta_data_get_charset,   // meta_data_get_charset
    NULL,                    // data_check_magic_bytes
    set_reader_handle,       // set_reader_handle
    next_subtrack,           // next_subtrack
    prev_subtrack,           // prev_subtrack
    NULL                     // handle
};

extern "C" GmuDecoder *GMU_REGISTER_DECODER(void) { return &gd; }
