/* DirectSound 8 on SDL3 audio (dsound.c). */
#pragma once

/* Registers the shims (before the images are mapped). */
void dsound_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void dsound_setup(void);

/* What the game has done with DirectSound, for the log: plain counters, updated where it happens. */
typedef struct DsStats
{
    unsigned made, failed, live, peak_live, dups;
    unsigned plays, peak_playing, lock_failed, notifies, notify_refused;
    unsigned qi_missed, qi_last; /* QueryInterface asked for something we lack: the IID's first dword */
    unsigned fail_tag, fail_ch, fail_bits, fail_rate, fail_flags; /* the last buffer refused */
} DsStats;
void dsound_stats(DsStats* out);
/* From the main thread (user32's pump, user32_pump_hook): a [dsound] line every 10 s when something changed. On the Switch
 * always, elsewhere with FFXI_AUDIO_STATS=1. */
void dsound_report(void);
