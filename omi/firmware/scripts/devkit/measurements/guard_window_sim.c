/* Does battery_guard()'s rolling window actually roll?
 *
 * Replicates the indexing and strike logic from main.c exactly, once with the shipped
 * `window[filled % BATT_WINDOW]` and once with the separate write cursor, and feeds both the same
 * descending cell voltage. Build: cc -o /tmp/guard_sim /tmp/guard_sim.c && /tmp/guard_sim
 */
#include <stdio.h>
#include <stdint.h>

#define BATT_WINDOW 8
#define BATT_CRITICAL_MV 3420
#define BATT_WARN_MV 3500
#define BATT_CRITICAL_STRIKES 3

/* Returns the sample number at which the board would shut down, or -1 if it never does. */
static int run(int rotate, uint16_t start_mv, uint16_t floor_mv, int step, uint16_t *worst_avg,
               int *warned_at)
{
    uint16_t window[BATT_WINDOW] = {0};
    uint8_t next = 0, filled = 0, strikes = 0;
    uint16_t mv = start_mv;

    *worst_avg = 0xFFFF;
    *warned_at = -1;

    for (int sample = 0; sample < 4000; sample++) {
        if (rotate) {
            window[next] = mv;
            next = (uint8_t) ((next + 1) % BATT_WINDOW);
        } else {
            window[filled % BATT_WINDOW] = mv; /* the shipped version */
        }

        if (filled < BATT_WINDOW) {
            filled++;
        } else {
            uint32_t sum = 0;
            for (int i = 0; i < BATT_WINDOW; i++) {
                sum += window[i];
            }
            uint16_t avg = (uint16_t) (sum / BATT_WINDOW);
            if (avg < *worst_avg) {
                *worst_avg = avg;
            }
            if (avg < BATT_WARN_MV && *warned_at < 0) {
                *warned_at = sample;
            }
            if (avg < BATT_CRITICAL_MV) {
                if (++strikes >= BATT_CRITICAL_STRIKES) {
                    return sample;
                }
            } else {
                strikes = 0;
            }
        }

        if (mv > floor_mv + step) {
            mv = (uint16_t) (mv - step);
        } else {
            mv = floor_mv; /* cell pinned at the floor, as flat as it will ever read */
        }
    }
    return -1;
}

int main(void)
{
    const uint16_t start = 4100, floor_mv = 3300;
    printf("cell ramps %u mV -> %u mV and stays there. critical %u mV, warn %u mV\n\n", start,
           floor_mv, BATT_CRITICAL_MV, BATT_WARN_MV);

    for (int rotate = 0; rotate <= 1; rotate++) {
        uint16_t worst;
        int warned, fired = run(rotate, start, floor_mv, 10, &worst, &warned);
        printf("  %-22s lowest average %u mV   warn LED %-12s shutdown %s\n",
               rotate ? "with write cursor:" : "shipped (filled %):", worst,
               warned < 0 ? "NEVER" : "fires", fired < 0 ? "NEVER FIRES" : "fires");
    }

    printf("\n  the shipped index pins 7 of 8 slots to the first samples after boot, so the mean\n"
           "  floors at (7*%u + %u)/8 = %u mV -- above both thresholds, at any cell voltage.\n",
           start, floor_mv, (7 * start + floor_mv) / 8);
    return 0;
}
