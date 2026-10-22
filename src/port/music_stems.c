/*
 * music_stems.c - renders one of the game's music tracks as separate WAVs,
 * one per sequence channel, through the game's own synthesizer: the
 * libultra sequence player, the instrument bank from the ROM and the audio
 * microcode (src/audio/asp.c). For remixing (the CIA banner jingle).
 *
 * Put the track number (enum MUSIC_TRACKS in bondconstants.h: Facility is
 * 7) in sdmc:/3ds/ge007/stems.txt, and optionally a length in seconds in
 * stems_seconds.txt (default 180). At boot, before the audio thread
 * starts, this writes to sdmc:/3ds/ge007/stems/:
 *   track07_mix.wav            every channel, as the game plays it
 *   track07_ch03_inst12.wav    channel 3 alone (its instrument: bank index 12)
 * Each stem keeps its own share of the reverb, so the stems add up to the
 * mix. Then the game boots as usual.
 *
 * Muting: voices on channels in g_PortSeqMute play at volume 0 (__vsVol in
 * seqplayer.c), so every pass allocates and steals voices exactly as the
 * full mix does and the stems add up to it.
 */
#include <ultra64.h>
#include "audio/synthInternals.h"

extern int pal_debug_read_int(const char *path, int fallback);
extern void pal_log(const char *fmt, ...);

/* src/pal/pal_wav_3ds.c */
extern void stems_mkdir(const char *dir);
extern void *stems_wav_open(const char *dir, const char *name);
extern void stems_wav_write(void *f, const s16 *pairs, u32 count);
extern void stems_wav_close(void *f, u32 rate, u32 pairs);

extern void amPortRenderFrame(s16 *out, s32 samples);
extern void musicTrack1Play(s32 track);
extern void musicTrack1Stop(void);
extern ALCSPlayer *g_musicXTrack1SeqPlayer;
extern ALBank *g_musicInstrumentBufferPtr;

u16 g_PortSeqMute;      /* channels whose notes don't sound */
u16 g_PortSeqNotes;     /* channels that started a note */
s32 g_PortSeqPlayAt;    /* synth sample the sequence started on (csplayer.c) */

#define STEM_DIR "sdmc:/3ds/ge007/stems"
#define MAX_FRAME 1024

static s16 s_buf[MAX_FRAME * 2];

/* Appends str, or n as two digits ("xx" if negative), at s; returns the end. */
static char *put(char *s, const char *str)
{
    while (*str)
        *s++ = *str++;
    *s = 0;
    return s;
}

static char *put2(char *s, int n)
{
    *s++ = n < 0 ? 'x' : (char)('0' + n / 10 % 10);
    *s++ = n < 0 ? 'x' : (char)('0' + n % 10);
    *s = 0;
    return s;
}

/* Puts the synthesizer back as it was when the first pass started, so every
 * pass is the same render with different channels muted and the stems add
 * up to the mix:
 *   - the sample clock and each player's next-event time at 0. Event times
 *     are rounded down to 16 samples on the absolute clock, so passes that
 *     start at different times would place some notes 16 samples apart;
 *   - the reverb (the aux bus effect, CUSTOM_FX_PARAMS_N in audi.c) as
 *     alFxNew left it: an empty delay line and the chorus LFO at its start.
 * Called with every voice off (stop_and_drain). */
static void reset_synth(void)
{
    ALSynth *drvr = &alGlobals->drvr;
    drvr->curSamples = 0;
    drvr->paramSamples = 0;
    for (ALPlayer *cl = drvr->head; cl; cl = cl->next)
        cl->samplesLeft = 0;
    for (int b = 0; b < drvr->maxAuxBusses; b++) {
        ALFx *r = &drvr->auxBus[b].fx[0];
        if (!r->base)
            continue;
        for (u32 k = 0; k < r->length; k++)
            r->base[k] = 0;
        r->input = r->base;
        for (int i = 0; i < r->section_count; i++) {
            ALDelay *d = &r->delay[i];
            if (d->rs) {
                d->rsval = 1.0f;
                d->rsdelta = 0;
                d->rs->delta = 0.0f;
                d->rs->first = 1;
            }
            if (d->lp)
                d->lp->first = 1;
        }
    }
}

/* Stops the player and runs the synthesizer until every voice is off. */
static void stop_and_drain(s32 frame)
{
    musicTrack1Stop();
    for (int i = 0; i < 600 && alCSPGetState(g_musicXTrack1SeqPlayer) != AL_STOPPED; i++)
        amPortRenderFrame(s_buf, frame);
    for (int i = 0; i < 30; i++)
        amPortRenderFrame(s_buf, frame);
}

static int render(const char *name, s32 track, u16 mute, s32 seconds, s32 rate, s32 frame)
{
    void *f = stems_wav_open(STEM_DIR, name);
    if (!f) {
        pal_log("stems: can't write %s/%s.wav", STEM_DIR, name);
        return 0;
    }
    reset_synth();
    g_PortSeqMute = mute;
    g_PortSeqPlayAt = -1;
    musicTrack1Play(track);
    /* Every file starts on the sample the sequence starts on. Where that
     * falls in a frame differs between passes; without this the stems
     * would be out of step with each other by a fraction of a frame. */
    u32 want = (u32)((s64)seconds * rate), done = 0;
    for (int i = 0; done < want && i < seconds * 70 + 600; i++) {
        s32 t0 = alGlobals->drvr.curSamples;
        amPortRenderFrame(s_buf, frame);
        if (g_PortSeqPlayAt < 0)
            continue;
        s32 skip = g_PortSeqPlayAt - t0;
        if (skip >= frame)
            continue;
        if (skip < 0)
            skip = 0;
        u32 n = (u32)(frame - skip);
        if (n > want - done)
            n = want - done;
        stems_wav_write(f, s_buf + 2 * skip, n);
        done += n;
    }
    stems_wav_close(f, rate, done);
    stop_and_drain(frame);
    g_PortSeqMute = 0;
    pal_log("stems: %s.wav", name);
    return 1;
}

static int instrument_index(int chan)
{
    ALInstrument *inst = g_musicXTrack1SeqPlayer->chanState[chan].instrument;
    ALBank *bank = g_musicInstrumentBufferPtr;
    for (int i = 0; bank && i < bank->instCount; i++)
        if (bank->instArray[i] == inst)
            return i;
    return -1;
}

void portMusicStems(void)
{
    s32 track = pal_debug_read_int("sdmc:/3ds/ge007/stems.txt", 0);
    if (track <= 0)
        return;
    s32 seconds = pal_debug_read_int("sdmc:/3ds/ge007/stems_seconds.txt", 180);
    s32 rate = alGlobals->drvr.outputRate;
    s32 frame = ((rate + 59) / 60 + 15) & ~15;   /* about a video frame */
    char name[48], *p;
    int inst[16];

    stems_mkdir(STEM_DIR);
    pal_log("stems: track %ld, %ld s at %ld Hz", (long)track, (long)seconds, (long)rate);

    /* the full mix first; it also finds the channels that play notes */
    g_PortSeqNotes = 0;
    put(put2(put(name, "track"), track), "_mix");
    if (!render(name, track, 0, seconds, rate, frame))
        return;
    u16 used = g_PortSeqNotes;
    for (int c = 0; c < 16; c++)
        inst[c] = instrument_index(c);

    /* developer token -audiocheck: the mix again through audio/dsp_synth.c,
     * every voice mixed in software as the DSP would play it, to compare
     * with the mix above sample for sample; then stop */
    {
        extern int pal_debug_has_token(const char *token);
        extern s32 g_PortDspSynth, g_PortDspSoft;
        if (pal_debug_has_token("-audiocheck")) {
            g_PortDspSynth = g_PortDspSoft = 1;
            put(put2(put(name, "track"), track), "_dspmix");
            render(name, track, 0, seconds, rate, frame);
            g_PortDspSynth = g_PortDspSoft = 0;
            pal_log("stems: done");
            return;
        }
    }

    for (int c = 0; c < 16; c++) {
        if (!(used & (1 << c)))
            continue;
        p = put2(put(name, "track"), track);
        p = put2(put(p, "_ch"), c);
        put2(put(p, "_inst"), inst[c]);
        render(name, track, (u16)~(1 << c), seconds, rate, frame);
    }
    pal_log("stems: done");
}
