/* DECPLAY: the zos OGG / WAV player (ZRAM2 module, started by MUSIC.ZM or chained from MP3PLAY.ZM with
 * a playlist in the shared block). Screen, keys and playlist handling are MP3PLAY's (included below);
 * the backend behind mp3.h is dec.c / deccold.c: the stock OGGDEC.DSP (Ogg Vorbis) and PCMDEC.DSP
 * (WAV: IMA ADPCM 4 bit, linear PCM 16 bit; 1-2 channels, 8-48 kHz). Title/artist: Vorbis comments
 * (OGG), the file name (WAV). MP3 tracks of the playlist chain to MP3PLAY. */
#include "../music/shared.h"
#include "dec.h"
#define PLAYER_ZM    DECPLAY_ZM
/* (FOOTER_IN_MAIN / FMT_IN_UI: where the shared functions go; MP3PLAY's defaults fit here too) */
#define PLAYER_OTHER MP3PLAY_ZM
#define PLAYER_NOT_MINE(idx) (!((idx) & TRK_KIND))     /* an MP3 */
#define PLAYER_NODSP "NO OGGDEC/PCMDEC.DSP"
#include "../mp3play/main.c"
