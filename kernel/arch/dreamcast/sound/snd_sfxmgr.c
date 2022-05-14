/* KallistiOS ##version##

   snd_sfxmgr.c
   Copyright (C) 2000, 2001, 2002, 2003, 2004 Dan Potter
   Copyright (C) 2022 Ozzy Ouzo :)

   Sound effects management system; this thing loads and plays sound effects
   during game operation.
*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <sys/queue.h>
#include <kos/fs.h>
#include <arch/irq.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>

#include "arm/aica_cmd_iface.h"

struct snd_effect;
LIST_HEAD(selist, snd_effect);

typedef struct snd_effect {
    uint32  locl, locr;
    uint32  len;
    uint16  rate;
    union {
        struct {
            uint16	    used : 1;
            uint16		stereo : 1;
            uint16		fmt : 2;
        }infos;
        uint16			informations;
    };

    LIST_ENTRY(snd_effect)  list;
} snd_effect_t;

struct selist snd_effects;

/* The next channel we'll use to play sound effects. */
static int sfx_nextchan = 0;

/* Our channel-in-use mask. */
static uint64 sfx_inuse = 0;

/* Unload all loaded samples and free their SPU RAM */
void snd_sfx_unload_all() {
    snd_effect_t* t, * n;

    t = LIST_FIRST(&snd_effects);

    while (t) {
        n = LIST_NEXT(t, list);

        snd_mem_free(t->locl);

        if (t->infos.stereo)
            snd_mem_free(t->locr);

        free(t);

        t = n;
    }

    LIST_INIT(&snd_effects);
}

/* Unload a single sample */
void snd_sfx_unload(sfxhnd_t idx) {
    snd_effect_t* t = (snd_effect_t*)idx;

    if (idx == SFXHND_INVALID) {
        dbglog(DBG_WARNING, "snd_sfx: can't unload an invalid SFXHND\n");
        return;
    }

    snd_mem_free(t->locl);

    if (t->infos.stereo)
        snd_mem_free(t->locr);

    LIST_REMOVE(t, list);
    free(t);
}

/* WAV header:
    0x08    -- "WAVE"
    0x14    -- 1 for PCM, 20 for ADPCM
    0x16    -- short num channels (1/2)
    0x18    -- long  HZ
    0x22    -- short 8 or 16 (bits)
    0x28    -- long  data length
    0x2c    -- data start

    Ozzy: *not* supporting 8bit stereo sorry..

 */

 /* Load a sound effect from a WAV file and return a handle to it */
sfxhnd_t snd_sfx_load(const char* fn) {
    file_t  fd;
    uint32  len, hz , avgBytesPerSec, blockAlign,loopStart,loopEnd;
    uint16* tmp, stereo, bitsize, fmt;
    snd_effect_t* t;
    int ownmem;

    fd = fs_open(fn, O_RDONLY);

    if (fd <= FILEHND_INVALID) {
        dbglog(DBG_WARNING, "snd_sfx: can't open sfx %s\n", fn);
        return SFXHND_INVALID;
    }

    /* Check file magic */
    hz = 0;
    fs_seek(fd, 0x08, SEEK_SET);
    fs_read(fd, &hz, 4);

    if (strncmp((char*)&hz, "WAVE", 4)) {
        dbglog(DBG_WARNING, "snd_sfx: file is not RIFF WAVE\n");
        fs_close(fd);
        return SFXHND_INVALID;
    }

    /* Read WAV header info */
    fs_seek(fd, 0x14, SEEK_SET);
    fs_read(fd, &fmt, 2);
    fs_read(fd, &stereo, 2);
    fs_read(fd, &hz, 4);
    fs_read(fd, &avgBytesPerSec, 4);
    fs_read(fd, &blockAlign, 4);
    fs_seek(fd, 0x22, SEEK_SET);
    fs_read(fd, &bitsize, 2);

    /* Read WAV data */
    fs_seek(fd, 0x28, SEEK_SET);
    fs_read(fd, &len, 4);

    /*
    dbglog(DBG_DEBUG, "WAVE file is %s, %luHZ, %d bits/sample, %lu bytes total,"
        " format %d\n", stereo == 1 ? "mono" : "stereo", hz, bitsize, len, fmt);
    */

    /* Try to mmap it and if that works, no need to copy it again */
    ownmem = 0;
    tmp = (uint16*)fs_mmap(fd);

    if (!tmp) {
        tmp = malloc(len);
        fs_read(fd, tmp, len);
        ownmem = 1;
    }
    else {
        tmp = (uint16*)(((uint8*)tmp) + fs_tell(fd));
    }

    fs_close(fd);

    t = malloc(sizeof(snd_effect_t));
    memset(t, 0, sizeof(snd_effect_t));

    /* Common characteristics not impacted by stream type */
    t->rate = hz;
    t->infos.stereo = stereo - 1;

    if (stereo == 1) {
        /* Mono PCM/ADPCM */
        t->len = len;
        if (blockAlign == 2 || fmt == 20) { /* 16-bit samples */
            t->len >>= 1;
        }

        t->rate = hz;
        t->locl = snd_mem_malloc(len);

        if (t->locl)
            spu_memload(t->locl, tmp, len);

        t->locr = 0;

        if (fmt == 20) {
            t->infos.fmt = AICA_SM_ADPCM;
            t->len *= 4;    /* 4-bit packed samples */
        }
        else {
            if (blockAlign == 2) {
                t->infos.fmt = AICA_SM_16BIT;
            }
            else {
                t->infos.fmt = AICA_SM_8BIT;
            }
        }
    }
    else if (stereo == 2 && fmt == 1) {

        if (blockAlign < 2) {
            free(t);
            if (ownmem == 1) {
                free(tmp);
            }
            dbglog(DBG_WARNING, "snd_sfx: %s 8bit stereo is not supported\n", fn);
            return SFXHND_INVALID;
        }

        /* Stereo PCM */
        uint32 i;
        uint16* sepbuf;

        sepbuf = malloc(len / 2);

        for (i = 0; i < len / 2; i += 2) {
            sepbuf[i / 2] = tmp[i + 1];
        }

        for (i = 0; i < len / 2; i += 2) {
            tmp[i / 2] = tmp[i];
        }

        t->len = len / 4; /* Two stereo, 16-bit samples */
        t->rate = hz;
        t->locl = snd_mem_malloc(len / 2);
        t->locr = snd_mem_malloc(len / 2);

        if (t->locl)
            spu_memload(t->locl, tmp, len / 2);

        if (t->locr)
            spu_memload(t->locr, sepbuf, len / 2);

        t->infos.fmt = AICA_SM_16BIT;

        free(sepbuf);
    }
    else if (stereo == 2 && fmt == 20) {
        /* Stereo ADPCM */

        /* We have to be careful here, because the second sample might not
           start on a nice even dword boundary. We take the easy way out
           and just malloc a second buffer. */
        uint8* buf2 = malloc(len / 2);
        memcpy(buf2, ((uint8*)tmp) + len / 2, len / 2);

        t->len = len;   /* Two stereo, 4-bit samples */
        t->rate = hz;
        t->locl = snd_mem_malloc(len / 2);
        t->locr = snd_mem_malloc(len / 2);

        if (t->locl)
            spu_memload(t->locl, tmp, len / 2);

        if (t->locr)
            spu_memload(t->locr, buf2, len / 2);

        t->infos.fmt = AICA_SM_ADPCM;

        free(buf2);
    }
    else {
        free(t);
        t = SFXHND_INVALID;
    }

    if (ownmem)
        free(tmp);

    if (t != SFXHND_INVALID)
        LIST_INSERT_HEAD(&snd_effects, t, list);

    return (sfxhnd_t)t;
}


sfxhnd_t snd_sfx_loadEx(const char* fn, SFXMGR_READER* reader)
{
    int fp;
    uint32  len, hz, avgBytesPerSec, blockAlign;
    uint16* tmp, stereo, bitsize, fmt;
    snd_effect_t* t;
    int ownmem;

    if (reader == NULL) {
        dbglog(DBG_WARNING, "snd_sfx_loadEx: invalid NULL parameter (reader)\n");
        return SFXHND_INVALID;
    }

    fp = reader->Open(fn);

    if (fp == NULL) {
        dbglog(DBG_WARNING, "snd_sfx: can't open sfx %s\n", fn);
        return SFXHND_INVALID;
    }

    /* Check file magic */
    hz = 0;

    reader->Seek(0x08, SEEK_SET);
    reader->Read(&hz,4);

    if (strncmp((char*)&hz, "WAVE", 4)) {
        dbglog(DBG_WARNING, "snd_sfx: file is not RIFF WAVE\n");
        reader->Close();
        return SFXHND_INVALID;
    }

    /* Read WAV header info */
    reader->Seek(0x14, SEEK_SET);
    reader->Read(&fmt, 2);
    reader->Read(&stereo, 2);
    reader->Read(&hz, 4);
    reader->Read(&avgBytesPerSec, 4);
    reader->Read(&blockAlign, 4);
    
    reader->Seek(0x22, SEEK_SET);
    reader->Read(&bitsize, 2);

    /* Read WAV data */
    reader->Seek(0x28, SEEK_SET);
    reader->Read(&len, 4);

    /*
    dbglog(DBG_DEBUG, "WAVE file is %s, %luHZ, %d bits/sample, %lu bytes total,"
        " format %d\n", stereo == 1 ? "mono" : "stereo", hz, bitsize, len, fmt);
    */

    /* Try to mmap it and if that works, no need to copy it again */
    ownmem = 0;

    #if 1 /* not using mmap intentionaly */
    tmp = malloc(len);
    if (tmp == NULL) {
        dbglog(DBG_WARNING, "snd_sfx: allocation error(0) for sfx %s\n", fn);
        return SFXHND_INVALID;
    }
    reader->Read(tmp, len);
    ownmem = 1;

    #else
    tmp = (uint16*)fs_mmap(fd);

    if (!tmp) {
        tmp = malloc(len);
        fs_read(fd, tmp, len);
        ownmem = 1;
    }
    else {
        tmp = (uint16*)(((uint8*)tmp) + fs_tell(fd));
    }
    #endif
    

    reader->Close();

    t = malloc(sizeof(snd_effect_t));
    if (t == NULL) {
        free(tmp);
        dbglog(DBG_WARNING, "snd_sfx: allocation error(1) for sfx %s\n", fn);
        return SFXHND_INVALID;
    }

    memset(t, 0, sizeof(snd_effect_t));

    /* Common characteristics not impacted by stream type */
    t->rate = hz;
    t->infos.stereo = stereo - 1;

    if (stereo == 1) {
        /* Mono PCM/ADPCM */
        if (blockAlign == 2 || fmt == 20) { /* 16-bit samples */
            t->len >>= 1;
        }

        t->locl = snd_mem_malloc(len);

        if (t->locl)
            spu_memload(t->locl, tmp, len);

        t->locr = 0;

        if (fmt == 20) {
            t->infos.fmt = AICA_SM_ADPCM;
            t->len *= 4;    /* 4-bit packed samples */
        }
        else {
            if (blockAlign == 2) {
                t->infos.fmt = AICA_SM_16BIT;
            }
            else {
                t->infos.fmt = AICA_SM_8BIT;
            }
        }
    }
    else if (stereo == 2 && fmt == 1) {
        /* Stereo PCM */
        uint32 i;
        uint16* sepbuf;

        if (blockAlign < 2) {
            free(t);
            if (ownmem == 1) {
                free(tmp);
            }
            dbglog(DBG_WARNING, "snd_sfx: %s 8bit stereo is not supported\n", fn);
            return SFXHND_INVALID;
        }


        sepbuf = malloc(len / 2);
        if (sepbuf == NULL) {
            free(t);
            free(tmp);
            dbglog(DBG_WARNING, "snd_sfx: allocation error(2) for sfx %s\n", fn);
            return SFXHND_INVALID;
        }

        for (i = 0; i < len / 2; i += 2) {
            sepbuf[i / 2] = tmp[i + 1];
        }

        for (i = 0; i < len / 2; i += 2) {
            tmp[i / 2] = tmp[i];
        }

        t->len = len / 4; /* Two stereo, 16-bit samples */
        t->locl = snd_mem_malloc(len / 2);
        t->locr = snd_mem_malloc(len / 2);

        if (t->locl)
            spu_memload(t->locl, tmp, len / 2);

        if (t->locr)
            spu_memload(t->locr, sepbuf, len / 2);

        t->infos.fmt = AICA_SM_16BIT;

        free(sepbuf);
    }
    else if (stereo == 2 && fmt == 20) {
        /* Stereo ADPCM */

        /* We have to be careful here, because the second sample might not
           start on a nice even dword boundary. We take the easy way out
           and just malloc a second buffer. */
        uint8* buf2 = malloc(len / 2);
        if (buf2 == NULL) {
            free(t);
            free(tmp);
            dbglog(DBG_WARNING, "snd_sfx: allocation error(3) for sfx %s\n", fn);
            return SFXHND_INVALID;
        }

        memcpy(buf2, ((uint8*)tmp) + len / 2, len / 2);

        t->len = len;   /* Two stereo, 4-bit samples */
        t->locl = snd_mem_malloc(len / 2);
        t->locr = snd_mem_malloc(len / 2);

        if (t->locl)
            spu_memload(t->locl, tmp, len / 2);

        if (t->locr)
            spu_memload(t->locr, buf2, len / 2);

        t->infos.fmt = AICA_SM_ADPCM;

        free(buf2);
    }
    else {
        free(t);
        t = SFXHND_INVALID;
    }

    if (ownmem)
        free(tmp);

    if (t != SFXHND_INVALID)
        LIST_INSERT_HEAD(&snd_effects, t, list);

    return (sfxhnd_t)t;
}


sfxhnd_t snd_sfx_load_mem(void* pSample, uint8 format, uint16 freq, uint8 chan, uint32 len)
{
    snd_effect_t* t;
    uint16* pSampleW;

    if (pSample == NULL || !len) {
        dbglog(DBG_WARNING, "snd_sfx_load_mem: invalid NULL parameter\n");
        return SFXHND_INVALID;
    }

    pSampleW = (uint16*) pSample;

    t = malloc(sizeof(snd_effect_t));
    if (t == NULL) {
        dbglog(DBG_WARNING, "snd_sfx_load_mem: allocation error(0)\n");
        return SFXHND_INVALID;
    }
    memset(t, 0, sizeof(snd_effect_t));

    /* Common characteristics not impacted by stream type */
    t->rate = freq;
    t->infos.stereo = chan;
    t->infos.fmt = format;

    if (chan == 0) {
        /* Mono PCM/ADPCM */
        t->len = len;
        if (format != AICA_SM_8BIT) { 
            t->len >>= 1; /* 16-bit samples */
        }
        t->locl = snd_mem_malloc(len);

        if (t->locl)
            spu_memload(t->locl, pSampleW, len);

        t->locr = 0;
        
        if (format == AICA_SM_ADPCM) {
            
            t->len *= 4;    /* 4-bit packed samples */
        }
    }
    else 
    if (chan == 1 && format == AICA_SM_16BIT) {
        /* Stereo PCM */
        uint32 i;
        uint16* sepbuf;

        sepbuf = malloc(len / 2);
        if (sepbuf == NULL) {
            free(t);
            dbglog(DBG_WARNING, "snd_sfx_load_mem: allocation error(1)\n");
            return SFXHND_INVALID;
        }

        for (i = 0; i < len / 2; i += 2) {
            sepbuf[i / 2] = pSampleW[i + 1];
        }

        for (i = 0; i < len / 2; i += 2) {
            pSampleW[i / 2] = pSampleW[i];
        }

        t->len = len / 4; /* Two stereo, 16-bit samples */
        t->locl = snd_mem_malloc(len / 2);
        t->locr = snd_mem_malloc(len / 2);

        if (t->locl)
            spu_memload(t->locl, pSampleW, len / 2);

        if (t->locr)
            spu_memload(t->locr, sepbuf, len / 2);

        free(sepbuf);
    }
    else 
    if (chan == 1 && format == AICA_SM_ADPCM) {
        /* Stereo ADPCM */

        /* We have to be careful here, because the second sample might not
           start on a nice even dword boundary. We take the easy way out
           and just malloc a second buffer. */
        uint8* buf2 = malloc(len / 2);
        if (buf2 == NULL) {
            free(t);
            dbglog(DBG_WARNING, "snd_sfx_load_mem: allocation error(2)\n");
            return SFXHND_INVALID;
        }

        memcpy(buf2, ((uint8*)pSampleW) + len / 2, len / 2);

        t->len = len;   /* Two stereo, 4-bit samples */
        t->locl = snd_mem_malloc(len / 2);
        t->locr = snd_mem_malloc(len / 2);

        if (t->locl)
            spu_memload(t->locl, pSampleW, len / 2);

        if (t->locr)
            spu_memload(t->locr, buf2, len / 2);

        free(buf2);
    }
    else {
        dbglog(DBG_WARNING, "snd_sfx_load_mem: unsupported sample format\n");
        free(t);
        t = SFXHND_INVALID;
    }

    if (t != SFXHND_INVALID)
        LIST_INSERT_HEAD(&snd_effects, t, list);

    return (sfxhnd_t)t;
}


int snd_sfx_play_chnEx(int chn, sfxhnd_t idx, int start, int end, int looping, int loopStart, int loopEnd, int freq, int vol, int pan) {
    int saveStart=start;
    snd_effect_t* t = (snd_effect_t*)idx;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    if (t->infos.fmt == AICA_SM_ADPCM) {
        start *= 4;
        end *= 4;
        loopStart *= 4;
        loopEnd *= 4;
    }

    if (start >= 65535) start = 65534;
    if (end >= 65535) end = 65534;
    if (loopStart >= 65535) loopStart = 65534;
    if (loopEnd >= 65535) loopEnd = 65534;

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = chn;
    chan->cmd = AICA_CH_CMD_START;
    chan->base = t->locl+saveStart;
    chan->type = t->infos.fmt;
    chan->length = end;
    chan->loop = looping;
    chan->loopstart = loopStart;
    chan->loopend = loopEnd;
    chan->freq = freq;
    chan->pos = 0; //unused
    chan->vol = vol;

    if (!t->infos.stereo) {
        chan->pan = pan;
        snd_sh4_to_aica(tmp, cmd->size);
    }
    else {
        chan->pan = 0;

        snd_sh4_to_aica_stop();
        snd_sh4_to_aica(tmp, cmd->size);

        cmd->cmd_id = chn + 1;
        chan->base = t->locr;
        chan->pan = 255;
        snd_sh4_to_aica(tmp, cmd->size);
        snd_sh4_to_aica_start();
    }

    return chn;
}



int snd_sfx_play_chn(int chn, sfxhnd_t idx, int vol, int pan) {
    int size;
    snd_effect_t* t = (snd_effect_t*)idx;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    size = t->len;

    if (size >= 65535) size = 65534;

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = chn;
    chan->cmd = AICA_CH_CMD_START;
    chan->base = t->locl;
    chan->type = t->infos.fmt;
    chan->length = size;
    chan->loop = 0;
    chan->loopstart = 0;
    chan->loopend = size;
    chan->freq = t->rate;
    chan->vol = vol;

    if (!t->infos.stereo) {
        chan->pan = pan;
        snd_sh4_to_aica(tmp, cmd->size);
    }
    else {
        chan->pan = 0;

        snd_sh4_to_aica_stop();
        snd_sh4_to_aica(tmp, cmd->size);

        cmd->cmd_id = chn + 1;
        chan->base = t->locr;
        chan->pan = 255;
        snd_sh4_to_aica(tmp, cmd->size);
        snd_sh4_to_aica_start();
    }

    return chn;
}

int snd_sfx_play(sfxhnd_t idx, int vol, int pan) {
    int chn, moved, old;

    /* This isn't perfect.. but it should be good enough. */
    old = irq_disable();
    chn = sfx_nextchan;
    moved = 0;

    while (sfx_inuse & (1 << chn)) {
        chn = (chn + 1) % 64;

        if (sfx_nextchan == chn)
            break;

        moved++;
    }

    irq_restore(old);

    if (moved && chn == sfx_nextchan) {
        return -1;
    }
    else {
        sfx_nextchan = (chn + 2) % 64;  // in case of stereo
        return snd_sfx_play_chn(chn, idx, vol, pan);
    }
}


void snd_sfx_update_volume(int channel, sfxhnd_t idx, int vol) {
    snd_effect_t* t = (snd_effect_t*)idx;

    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = channel;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL;
    chan->vol = vol;
    snd_sh4_to_aica(tmp, cmd->size);

    if (t->infos.stereo) {
        cmd->cmd_id = chan + 1;
        snd_sh4_to_aica(tmp, cmd->size);
    }
}

void snd_sfx_update_frequency(int channel, sfxhnd_t idx, int freq) {
    snd_effect_t* t = (snd_effect_t*)idx;

    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = channel;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ;
    chan->freq = freq;
    snd_sh4_to_aica(tmp, cmd->size);

    if (t->infos.stereo) {
        cmd->cmd_id = chan + 1;
        snd_sh4_to_aica(tmp, cmd->size);
    }
}

void snd_sfx_update_pan(int channel, sfxhnd_t idx, int pan) {
    snd_effect_t* t = (snd_effect_t*)idx;

    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = channel;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_PAN;
    chan->pan = pan;
    snd_sh4_to_aica(tmp, cmd->size);

    if (!t->infos.stereo) {
        chan->pan = pan;
        snd_sh4_to_aica(tmp, cmd->size);
    }
    else {
        chan->pan = 0;

        snd_sh4_to_aica_stop();
        snd_sh4_to_aica(tmp, cmd->size);

        cmd->cmd_id = chan + 1;
        chan->base = t->locr;
        chan->pan = 255;
        snd_sh4_to_aica(tmp, cmd->size);
        snd_sh4_to_aica_start();
    }
}



void snd_sfx_stop(int chn) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = chn;
    chan->cmd = AICA_CH_CMD_STOP;
    chan->base = 0;
    chan->type = 0;
    chan->length = 0;
    chan->loop = 0;
    chan->loopstart = 0;
    chan->loopend = 0;
    chan->freq = 44100;
    chan->vol = 0;
    chan->pan = 0;
    snd_sh4_to_aica(tmp, cmd->size);
}

void snd_sfx_stop_all() {
    int i;

    for (i = 0; i < 64; i++) {
        if (sfx_inuse & (1 << i))
            continue;

        snd_sfx_stop(i);
    }
}

int snd_sfx_chn_alloc() {
    int old, chn;

    old = irq_disable();

    for (chn = 0; chn < 64; chn++)
        if (!(sfx_inuse & (1 << chn)))
            break;

    if (chn >= 64)
        chn = -1;
    else
        sfx_inuse |= 1 << chn;

    irq_restore(old);

    return chn;
}

void snd_sfx_chn_free(int chn) {
    int old;

    old = irq_disable();
    sfx_inuse &= ~(1 << chn);
    irq_restore(old);
}
