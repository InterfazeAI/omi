/* Pick a gauge filter by measuring it, not by guessing.
 *
 * Replays the real reporting path on the host against a draining cell with realistic ADC jitter,
 * and sweeps filter configurations. Mirrors battery_states[] and the integer arithmetic in
 * battery_get_percentage(), including its truncating cast.
 *
 * Two metrics, and both matter:
 *   reversals  direction changes in the reported series. A monotonically draining cell should
 *              produce none; each one is a percentage a user watched jump and come back.
 *   lag        worst gap between the reported percentage and the truth. A filter can reach zero
 *              reversals by simply refusing to move, so reversals alone would pick a broken gauge.
 *
 * Stage 1 is the guard's rolling mean of 8 raw reads, which already exists and is fed every 10 s.
 * Stage 2 is an optional second filter over those means, which is where the extra smoothing has to
 * come from: stage 1 cannot be lengthened without also delaying the low-battery shutdown.
 *
 * Build: cc -o out/gauge_smoothing_sim gauge_smoothing_sim.c && ./out/gauge_smoothing_sim
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define GUARD_WINDOW 8
#define MAX_STAGE2 64
#define JITTER_MV 40  /* measured: 54 mV spread across 12 samples on a stationary cell */
#define SAMPLE_S 10   /* BATT_SAMPLE_TICKS: the guard decides every 10 s */
#define DRAIN_MV_H 19 /* measured plateau/knee rate */

static const struct {
    uint16_t mv;
    uint8_t pct;
} STATES[] = {
    {4150, 100}, {4050, 92}, {3950, 84}, {3850, 73}, {3750, 64},
    {3650, 48},  {3550, 24}, {3500, 15}, {3450, 6},  {3400, 0},
};
#define NSTATES ((int) (sizeof(STATES) / sizeof(STATES[0])))

/* battery_get_percentage(), arithmetic for arithmetic. */
static uint8_t pct_of(uint16_t mv)
{
    if (mv >= STATES[0].mv) {
        return STATES[0].pct;
    }
    if (mv <= STATES[NSTATES - 1].mv) {
        return STATES[NSTATES - 1].pct;
    }
    for (int i = 0; i < NSTATES - 1; i++) {
        if (mv <= STATES[i].mv && mv > STATES[i + 1].mv) {
            float vr = (float) (STATES[i].mv - STATES[i + 1].mv);
            float pr = (float) (STATES[i].pct - STATES[i + 1].pct);
            float pos = (float) (STATES[i].mv - mv) / vr;
            return (uint8_t) (STATES[i].pct - (uint8_t) (pos * pr));
        }
    }
    return 0;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int) *(const uint16_t *) a - (int) *(const uint16_t *) b;
}

/* How the filtered percentage becomes the published one. */
enum hold {
    HOLD_DEADBAND, /* jump to it once it differs by `deadband` -- moves in deadband-sized steps */
    HOLD_SLEW,     /* same trigger, but step one point at a time toward it */
    HOLD_MONOTONE, /* never rise while discharging: monotone by construction */
};

struct cfg {
    const char *name;
    int stage2;   /* second-stage window, in guard means; 0 = none */
    int median;   /* 1 = median, 0 = mean */
    int deadband; /* percentage points required to move at all */
    enum hold hold;
};

struct result {
    int reversals;
    int max_lag;
    uint8_t last;
};

static struct result run(struct cfg c, uint16_t start_mv, uint16_t end_mv)
{
    uint16_t g[GUARD_WINDOW] = {0}, s2[MAX_STAGE2] = {0};
    uint8_t gnext = 0, gfill = 0;
    int s2next = 0, s2fill = 0;
    int reported = -1, prev_dir = 0, prev = -1;
    struct result r = {0, 0, 0};

    srand(12345); /* deterministic across configs, so they see identical noise */
    long total_s = (long) (start_mv - end_mv) * 3600 / DRAIN_MV_H;

    for (long t = 0; t <= total_s; t += SAMPLE_S) {
        uint16_t truth = (uint16_t) (start_mv - (long) DRAIN_MV_H * t / 3600);
        int noise = (rand() % (2 * JITTER_MV + 1)) - JITTER_MV;

        g[gnext] = (uint16_t) (truth + noise);
        gnext = (uint8_t) ((gnext + 1) % GUARD_WINDOW);
        if (gfill < GUARD_WINDOW) {
            gfill++;
            continue;
        }
        uint32_t sum = 0;
        for (int i = 0; i < GUARD_WINDOW; i++) {
            sum += g[i];
        }
        uint16_t basis = (uint16_t) (sum / GUARD_WINDOW);

        if (c.stage2 > 0) {
            s2[s2next] = basis;
            s2next = (s2next + 1) % c.stage2;
            if (s2fill < c.stage2) {
                s2fill++;
            }
            if (c.median) {
                uint16_t tmp[MAX_STAGE2];
                for (int i = 0; i < s2fill; i++) {
                    tmp[i] = s2[i];
                }
                qsort(tmp, s2fill, sizeof(tmp[0]), cmp_u16);
                basis = tmp[s2fill / 2];
            } else {
                uint32_t s = 0;
                for (int i = 0; i < s2fill; i++) {
                    s += s2[i];
                }
                basis = (uint16_t) (s / s2fill);
            }
        }

        int pct = pct_of(basis);
        if (reported < 0) {
            reported = pct;
        } else if (c.hold == HOLD_MONOTONE) {
            if (pct < reported) {
                reported = pct;
            }
        } else if (abs(pct - reported) >= c.deadband) {
            if (c.hold == HOLD_SLEW) {
                reported += (pct > reported) ? 1 : -1;
            } else {
                reported = pct;
            }
        }

        int lag = abs(reported - (int) pct_of(truth));
        if (lag > r.max_lag) {
            r.max_lag = lag;
        }
        if (prev >= 0 && reported != prev) {
            int dir = reported > prev ? 1 : -1;
            if (prev_dir != 0 && dir != prev_dir) {
                r.reversals++;
            }
            prev_dir = dir;
        }
        prev = reported;
        r.last = (uint8_t) reported;
    }
    return r;
}

int main(void)
{
    const uint16_t start = 3700, end = 3500; /* steepest stretch of the curve, 0.24 %/mV */
    struct cfg cfgs[] = {
        {"single read (shipped)", -1, 0, 0, HOLD_DEADBAND}, /* handled specially below */
        {"guard mean only", 0, 0, 0, HOLD_DEADBAND},
        {"+ deadband 3", 0, 0, 3, HOLD_DEADBAND},
        {"median of 16 + dead 2", 16, 1, 2, HOLD_DEADBAND},
        {"median of 32 + dead 3", 32, 1, 3, HOLD_DEADBAND},
        {"median of 64 + dead 3", 64, 1, 3, HOLD_DEADBAND},
        {"median of 16 + slew", 16, 1, 2, HOLD_SLEW},
        {"median of 32 + slew", 32, 1, 2, HOLD_SLEW},
        {"median of 64 + slew", 64, 1, 2, HOLD_SLEW},
        {"median of 16 + monotone", 16, 1, 0, HOLD_MONOTONE},
        {"median of 32 + monotone", 32, 1, 0, HOLD_MONOTONE},
        {"median of 64 + monotone", 64, 1, 0, HOLD_MONOTONE},
    };

    printf("cell drains %u -> %u mV at %d mV/h, sampled every %d s, ADC jitter +/-%d mV\n",
           start, end, DRAIN_MV_H, SAMPLE_S, JITTER_MV);
    printf("stage 1 is the guard's mean of %d; stage 2 smooths those means\n\n", GUARD_WINDOW);
    printf("  %-26s %10s %8s %8s\n", "config", "reversals", "max lag", "final");

    for (int i = 0; i < (int) (sizeof(cfgs) / sizeof(cfgs[0])); i++) {
        struct cfg c = cfgs[i];
        struct result r;
        if (c.stage2 < 0) {
            /* No stage 1 at all: one raw read straight to a percentage, as it shipped. */
            srand(12345);
            long total_s = (long) (start - end) * 3600 / DRAIN_MV_H;
            int prev = -1, dir = 0;
            r.reversals = 0;
            r.max_lag = 0;
            for (long t = 0; t <= total_s; t += SAMPLE_S) {
                uint16_t truth = (uint16_t) (start - (long) DRAIN_MV_H * t / 3600);
                int noise = (rand() % (2 * JITTER_MV + 1)) - JITTER_MV;
                int pct = pct_of((uint16_t) (truth + noise));
                int lag = abs(pct - (int) pct_of(truth));
                if (lag > r.max_lag) {
                    r.max_lag = lag;
                }
                if (prev >= 0 && pct != prev) {
                    int d = pct > prev ? 1 : -1;
                    if (dir != 0 && d != dir) {
                        r.reversals++;
                    }
                    dir = d;
                }
                prev = pct;
                r.last = (uint8_t) pct;
            }
        } else {
            r = run(c, start, end);
        }
        printf("  %-26s %10d %8d %7u%%\n", c.name, r.reversals, r.max_lag, r.last);
    }

    printf("\n  Truth falls %u%% -> %u%% over this stretch.\n", pct_of(start), pct_of(end));
    printf("  Reversals must reach ~0 without max lag growing past a few points -- a filter that\n"
           "  simply refuses to move scores zero reversals and is a broken gauge.\n");
    return 0;
}
