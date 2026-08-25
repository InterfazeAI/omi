import {
  Callout,
  Card,
  CardBody,
  CardHeader,
  Code,
  Divider,
  Grid,
  H1,
  H2,
  H3,
  LineChart,
  Row,
  Stack,
  Stat,
  Table,
  Text,
  useHostTheme,
} from "cursor/canvas";

// ---------------------------------------------------------------------------
// Every number here is read from the firmware as it stands, not from memory.
//
//   thresholds + flow  omi/firmware/devkit/src/main.c
//                      BATT_WARN_MV 3500, BATT_CRITICAL_MV 3420,
//                      BATT_BOOT_MIN_MV 3470, BATT_SAMPLE_TICKS 20 (10 s),
//                      BATT_WINDOW 8, BATT_CRITICAL_STRIKES 3, BATT_BOOT_SAMPLES 8
//   gauge table        omi/firmware/devkit/src/lib/battery/battery.c battery_states[]
//   gestures           omi/firmware/devkit/src/button.c TAP_THRESHOLD 300,
//                      MULTI_TAP_WINDOW 600, RESET_WARN_TIME 2000, RESET_HOLD_TIME 5000
//
// Percentages are not interpolated by hand: they are the output of the same
// integer arithmetic battery_get_percentage() performs, including its
// truncating (uint8_t) cast, replayed over battery_states[].
//
// Runtime-remaining hours come from the 1,333-sample discharge in
// DEBUGGING.md trap 14, rebased so 0 h sits at the observed 3,400 mV cutoff
// and full charge (4,150 mV) sits at 45.7 h.
// ---------------------------------------------------------------------------

const MV = [
  4150, 4100, 4050, 4000, 3950, 3900, 3850, 3800, 3750, 3700, 3650, 3600, 3550,
  3500, 3470, 3450, 3420, 3400,
];

/** battery_get_percentage() output at each voltage above. */
const FW_PCT = [
  100, 96, 92, 88, 84, 79, 73, 69, 64, 56, 48, 36, 24, 15, 10, 6, 3, 0,
];

/** Hours of recording left, from the measured discharge. Approximate. */
const HOURS_LEFT = [
  45.7, 43.5, 41.3, 39.6, 38.0, 36.1, 33.3, 30.3, 29.4, 25.5, 21.8, 17.3, 11.1,
  6.3, 4.2, 3.0, 1.2, 0,
];

/** The same hours expressed as a share of a full charge — what the table was fitted to. */
const MEASURED_PCT = HOURS_LEFT.map((h) => Math.round((h / 45.7) * 100));

const EVENT: Record<number, string> = {
  4150: "full charge, off the charger",
  3500: "warning — yellow blink starts",
  3470: "boot gate refuses a cold start",
  3420: "graceful shutdown fires",
  3400: "regulator dropout — board stops",
};

const ROW_TONE: Record<number, "warning" | "danger"> = {
  3500: "warning",
  3470: "warning",
  3420: "danger",
  3400: "danger",
};

export default function BatteryFlow() {
  const theme = useHostTheme();
  const caption = { color: theme.text.tertiary, fontSize: 12 };
  const muted = { color: theme.text.secondary };

  return (
    <Stack gap={28} style={{ padding: 28, maxWidth: 1100 }}>
      <Stack gap={6}>
        <H1>Battery flow, power-on to power-off</H1>
        <Text style={muted}>
          Omi DevKit 2 · every threshold, state and percentage the firmware can
          be in, in the order it passes through them
        </Text>
      </Stack>

      <Callout tone="info" title="Four voltages decide everything">
        <Text>
          A cold start is refused below <Code>3,470 mV</Code> (10%), the warning
          LED begins at <Code>3,500 mV</Code> (15%), a running board shuts
          itself down at <Code>3,420 mV</Code> (3%), and the hardware stops on
          its own near <Code>3,400 mV</Code> (0%). Charging outranks all four —
          on USB the gate is skipped and the shutdown never arms.
        </Text>
      </Callout>

      <Row gap={16} wrap>
        <Stat value="100%" label="4,150 mV · full" />
        <Stat value="15%" label="3,500 mV · warn" tone="warning" />
        <Stat value="10%" label="3,470 mV · boot gate" tone="warning" />
        <Stat value="3%" label="3,420 mV · shutdown" tone="danger" />
        <Stat value="~45.7 h" label="full charge, recording" />
        <Stat value="22 mV" label="measured load sag" />
      </Row>

      <Divider />

      {/* ---- primary artifact: the lookup table -------------------------- */}
      <Stack gap={10}>
        <H2>Voltage to percentage</H2>
        <Text style={muted}>
          What the Battery Service reports at each cell voltage, what the
          firmware does there, and roughly how much recording is left.
        </Text>
        <Table
          headers={[
            "Cell",
            "Reported",
            "Runtime left",
            "What happens at this voltage",
          ]}
          columnAlign={["right", "right", "right", "left"]}
          rowTone={MV.map((mv) => ROW_TONE[mv])}
          rows={MV.map((mv, i) => [
            `${mv.toLocaleString()} mV`,
            `${FW_PCT[i]}%`,
            HOURS_LEFT[i] === 0 ? "—" : `~${HOURS_LEFT[i].toFixed(1)} h`,
            EVENT[mv] ?? "",
          ])}
        />
        <Text style={caption}>
          Reported column is the exact output of{" "}
          <Code>battery_get_percentage()</Code> including its truncating cast ·
          runtime from the 1,333-sample discharge, ±6.5 h at the bottom
        </Text>
      </Stack>

      <Stack gap={10}>
        <H3>The gauge now agrees with the clock</H3>
        <LineChart
          categories={MV.map((mv) => `${mv.toLocaleString()} mV`)}
          series={[
            { name: "Reported by firmware", data: FW_PCT, tone: "info" },
            {
              name: "Measured runtime remaining",
              data: MEASURED_PCT,
              tone: "success",
            },
          ]}
          height={300}
          beginAtZero
          yMax={100}
          valueSuffix="%"
          referenceLines={[
            { value: 15, label: "warn", tone: "warning" },
            { value: 3, label: "shutdown", tone: "danger" },
          ]}
        />
        <Text style={caption}>
          x-axis is cell voltage, falling left to right as the cell discharges ·
          y-axis is percent · the two series track because{" "}
          <Code>battery_states[]</Code> was fitted to the measured curve; the old
          generic profile reported 50% where three quarters of the runtime
          remained
        </Text>
      </Stack>

      <Divider />

      {/* ---- the lifecycle ---------------------------------------------- */}
      <Stack gap={10}>
        <H2>The flow, in order</H2>
        <Table
          headers={["#", "Stage", "Battery decision", "Outcome"]}
          columnAlign={["right", "left", "left", "left"]}
          rows={[
            [
              "1",
              "Wake",
              "none",
              "Button press, USB attach or RESET. Waking from SYSTEMOFF is a full reset, so all averages start empty.",
            ],
            [
              "2",
              "Peripheral init",
              "none",
              "A failure is recorded and boot continues — degraded, not halted, so BLE stays reachable for diagnosis.",
            ],
            [
              "3",
              "battery_boot_gate()",
              "8 consecutive reads",
              "Charging → skip. No usable reading → continue. ≥ 3,470 mV → continue. Below → 3 yellow blinks, SYSTEMOFF, card never mounted.",
            ],
            [
              "4",
              "Card mount + transport",
              "none",
              "Ring buffer opened, BLE advertising, recording begins.",
            ],
            [
              "5",
              "Main loop, every 500 ms",
              "LED refresh",
              "set_led_state() repaints from the priority list below.",
            ],
            [
              "6",
              "battery_guard(), every 10 s",
              "8-sample rolling mean",
              "First 80 s: filling, no decision possible. Charging → strikes cleared. Mean < 3,500 mV → yellow blink. Mean < 3,420 mV → strike.",
            ],
            [
              "7",
              "Three consecutive strikes",
              "30 s below 3,420 mV",
              "turnoff_all(): flush the write batch, close the segment, then SYSTEMOFF.",
            ],
            [
              "8",
              "SYSTEMOFF",
              "none",
              "LEDs off, button armed as the only wake source, watchdog disabled, USB interrupts masked. Draw falls to ~1 µA.",
            ],
          ]}
        />
      </Stack>

      <Grid columns={2} gap={16}>
        <Card>
          <CardHeader>LED priority, highest first</CardHeader>
          <CardBody>
            <Table
              headers={["Colour", "Meaning"]}
              rows={[
                ["red ×3", "erase-and-unbond finished"],
                ["yellow, steady", "5 s hold in progress — release to cancel"],
                ["yellow, blinking", "battery below 3,500 mV"],
                ["green, blinking", "charging"],
                ["blue, steady", "recording, host connected"],
                ["red, steady", "recording, no connection"],
              ]}
            />
            <Text style={{ ...caption, marginTop: 8 }}>
              Yellow is red + green together. Red never means battery — it is
              recording — which is why the low-battery signal was moved to
              yellow.
            </Text>
          </CardBody>
        </Card>

        <Card>
          <CardHeader>Why each decision takes as long as it does</CardHeader>
          <CardBody>
            <Table
              headers={["Window", "Duration", "Reason"]}
              rows={[
                ["Boot gate", "~ms", "nothing loaded yet, so reads can be back to back"],
                ["Guard fill", "80 s", "8 samples at 10 s before any average means anything"],
                ["Strikes", "30 s", "3 consecutive means, so one excursion cannot trigger it"],
                ["Worst case", "110 s", "fill plus strikes, from a cold start"],
              ]}
            />
            <Text style={{ ...caption, marginTop: 8 }}>
              Single reads jitter ±40 mV — worth more than three hours of real
              discharge on the plateau — so nothing is decided on one sample.
            </Text>
          </CardBody>
        </Card>
      </Grid>

      <Stack gap={10}>
        <H3>Button gestures that end or reset the session</H3>
        <Table
          headers={["Gesture", "Timing", "Effect"]}
          rows={[
            ["Single tap", "< 300 ms", "notifies the host over BLE"],
            ["Double tap", "two within 600 ms", "notifies the host over BLE"],
            ["Triple tap", "three within 600 ms", "shutdown — same path as the low-battery guard"],
            ["Hold 2 s", "2,000 ms", "steady yellow warning; release now and nothing happens"],
            ["Hold 5 s", "5,000 ms", "erase the card and release the bond, then three red blinks"],
          ]}
        />
      </Stack>

      <Callout tone="warning" title="What is estimated, and what is measured">
        <Text>
          Measured: the 22 mV load sag, the discharge curve, and the 3,293–3,309
          mV rail that puts the real cutoff near 3,400 mV. Estimated: the hours
          column, from one cell at one temperature, with the bottom bounded by a
          6.5 h window in which the cell died unmonitored and everything above
          4,020 mV extrapolated. The sag was also taken mid-discharge, where
          internal resistance is lowest — expect more of it near the knee, which
          is the argument for keeping 50 mV of margin between the gate and the
          shutdown rather than trimming to the measurement.
        </Text>
      </Callout>
    </Stack>
  );
}
