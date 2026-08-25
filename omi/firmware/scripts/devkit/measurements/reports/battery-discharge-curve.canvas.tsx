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
  Pill,
  Row,
  Stack,
  Stat,
  Table,
  Text,
  useHostTheme,
} from "cursor/canvas";

// ---------------------------------------------------------------------------
// Measured data. Omi DevKit 2 on a single charge, recording continuously to SD
// with BLE advertising and no USB. 1,333 samples stitched from three
// battery.py --watch runs (drain_overnight.log, drain_to_empty.log,
// drain_to_empty2.log), 2026-08-23 00:35 -> 2026-08-24 13:23.
// Voltages are a 15-sample rolling median; single samples jitter about +-40 mV.
// ---------------------------------------------------------------------------

const HOURS = [
  0.0, 1.6, 3.2, 4.79, 6.39, 7.98, 10.92, 12.52, 14.11, 15.71, 17.3, 18.63,
  18.96, 19.29, 19.62, 19.95, 20.28, 20.61, 20.94, 21.27, 21.6, 21.93, 22.26,
  22.59, 22.92, 23.25, 23.58, 24.01, 24.34, 24.66, 24.99, 25.32, 25.65, 25.98,
  26.31, 26.64, 26.96, 27.29, 27.62, 27.95, 28.27, 28.6, 28.93, 29.26, 29.59,
  29.92, 30.25, 30.58, 30.91, 31.24, 31.57, 31.89, 32.22, 32.55, 32.95, 33.69,
  34.02, 34.35, 34.68, 34.9, 35.07, 35.24, 35.42, 35.59, 35.76, 35.93, 36.1,
  36.27, 36.44, 36.61, 36.78, 36.8,
];

const MV = [
  3997, 3964, 3926, 3884, 3857, 3819, 3751, 3736, 3700, 3677, 3662, 3635, 3638,
  3629, 3629, 3632, 3621, 3612, 3624, 3624, 3629, 3609, 3612, 3597, 3594, 3612,
  3600, 3597, 3597, 3600, 3597, 3591, 3582, 3585, 3582, 3588, 3579, 3579, 3588,
  3576, 3582, 3567, 3552, 3558, 3555, 3561, 3552, 3561, 3544, 3544, 3538, 3523,
  3529, 3523, 3523, 3505, 3511, 3499, 3493, 3493, 3475, 3478, 3464, 3470, 3478,
  3470, 3458, 3467, 3464, 3452, 3461, 3461,
];

// What the shipped gauge reported at each of those samples.
const FW_PCT = [
  95, 90, 86, 75, 76, 63, 56, 51, 42, 42, 42, 34, 31, 30, 31, 33, 29, 31, 30,
  32, 33, 28, 24, 26, 27, 33, 30, 29, 32, 26, 30, 27, 30, 28, 28, 26, 28, 25,
  27, 23, 27, 28, 22, 25, 28, 23, 24, 26, 22, 24, 19, 22, 20, 21, 19, 18, 19,
  15, 14, 11, 13, 17, 12, 15, 14, 14, 13, 14, 10, 12, 17, 12,
];

const MONITORED_H = 36.8; // logger start -> last sample
const TOTAL_MID_H = 40.05; // + midpoint of the 6.5 h death window
const LAST_MV = 3455;
const TAIL_RATE = 18.9; // mV/h measured over the final 2.7 h

// Runtime remaining, as a fraction of the mid-estimate total. This is the only
// honest "percent" available: the load is constant, so charge burned is
// proportional to elapsed time.
const MEASURED_PCT = HOURS.map((h) =>
  Number(Math.max(0, ((TOTAL_MID_H - h) / TOTAL_MID_H) * 100).toFixed(1)),
);

type Row = {
  mv: number;
  fw: number;
  measured: string;
  note: string;
  state: "above" | "measured" | "edge" | "unreachable";
};

const ROWS: Row[] = [
  { mv: 4074, fw: 100, measured: "—", note: "above logged range", state: "above" },
  { mv: 4029, fw: 95, measured: "—", note: "above logged range", state: "above" },
  { mv: 3983, fw: 90, measured: "98%", note: "97.9–98.2%", state: "measured" },
  { mv: 3938, fw: 85, measured: "94%", note: "93.6–94.6%", state: "measured" },
  { mv: 3893, fw: 80, measured: "89%", note: "88.6–90.3%", state: "measured" },
  { mv: 3847, fw: 70, measured: "83%", note: "81.3–84.1%", state: "measured" },
  { mv: 3802, fw: 60, measured: "76%", note: "74.0–77.9%", state: "measured" },
  { mv: 3756, fw: 50, measured: "75%", note: "72.4–76.5%", state: "measured" },
  { mv: 3665, fw: 40, measured: "57%", note: "53.2–60.2%", state: "measured" },
  { mv: 3619, fw: 30, measured: "49%", note: "44.8–53.1%", state: "measured" },
  { mv: 3528, fw: 20, measured: "21%", note: "13.9–26.9%", state: "measured" },
  { mv: 3437, fw: 10, measured: "~0%", note: "0.9 h past last sample", state: "edge" },
  { mv: 3346, fw: 5, measured: "dead", note: "5.8 h — at the window edge", state: "edge" },
  { mv: 3255, fw: 2, measured: "dead", note: "needs 10.6 h > 6.5 h", state: "unreachable" },
  { mv: 3164, fw: 1, measured: "dead", note: "needs 15.4 h > 6.5 h", state: "unreachable" },
  { mv: 3000, fw: 0, measured: "dead", note: "needs 24.0 h > 6.5 h", state: "unreachable" },
];

const REGIONS = [
  { label: "Upper", range: "4020 → 3810 mV", rate: "−22.8", span: "8.0 h", n: 96 },
  { label: "Plateau", range: "3800 → 3500 mV", rate: "−9.1", span: "23.9 h", n: 934 },
  { label: "Knee", range: "3490 → 3455 mV", rate: "−18.9", span: "2.7 h", n: 270 },
];

function Header() {
  const theme = useHostTheme();
  return (
    <Stack gap={10}>
      <H1>Where 1% actually is</H1>
      <Text style={{ color: theme.text.secondary, maxWidth: 760 }}>
        Omi DevKit 2, one charge, recording continuously to SD with BLE
        advertising and no USB. 1,333 samples over 36.8 h, then a 6.5 h gap in
        which the cell died. Because the load is constant, runtime remaining is
        a valid proxy for state of charge — which lets the measured data be
        checked against the gauge the firmware ships.
      </Text>
    </Stack>
  );
}

function Verdict() {
  return (
    <Callout
      tone="warning"
      title="The firmware's bottom four table entries are voltages this board cannot reach"
    >
      <Text>
        Falling from the last measured sample (3,455 mV) to the firmware's 1%
        point (3,164 mV) would take <strong>15.4 hours</strong> at the measured
        knee rate. The cell only had a <strong>6.5 hour</strong> window left
        before it was found flat. The same holds for 2% (10.6 h) and 0%
        (24.0 h). In practice the gauge reads roughly 10% and then the board
        simply dies — it never counts down through 5, 2, 1.
      </Text>
    </Callout>
  );
}

function KeyNumbers() {
  return (
    <Row gap={28} wrap>
      <Stat value="≥ 3,346 mV" label="Lower bound for 1%" tone="warning" />
      <Stat value="3,330–3,390 mV" label="Estimated true cutoff" />
      <Stat value="~40 h" label="Runtime on one charge" tone="success" />
      <Stat value="3,164 mV" label="Firmware's 1% (unreachable)" tone="danger" />
    </Row>
  );
}

function DischargeCurve() {
  const theme = useHostTheme();
  return (
    <Stack gap={8}>
      <H2>Cell voltage over time on a single charge</H2>
      <Text style={{ color: theme.text.secondary }}>
        The trace stops at 3,455 mV because logging stopped there, not because
        the cell did. Note how far above the firmware's 1% line it ends — that
        gap is the whole finding.
      </Text>
      <LineChart
        categories={HOURS.map((h) => `${h.toFixed(1)} h`)}
        series={[{ name: "Cell voltage (15-sample median)", data: MV, tone: "info" }]}
        height={330}
        beginAtZero={false}
        yMin={3100}
        yMax={4050}
        valueSuffix=" mV"
        referenceLines={[
          { value: 3437, label: "fw 10%", tone: "neutral" },
          { value: 3346, label: "fw 5% · 1% floor", tone: "warning" },
          { value: 3164, label: "fw 1% — never reached", tone: "danger" },
        ]}
      />
      <Text style={{ color: theme.text.tertiary, fontSize: 12 }}>
        Source: battery.py --watch, three runs stitched · 2026-08-23 00:35 →
        2026-08-24 13:23 · x-axis is hours since logging began, y-axis is
        millivolts at the cell
      </Text>
    </Stack>
  );
}

function GaugeComparison() {
  const theme = useHostTheme();
  return (
    <Stack gap={8}>
      <H2>Shipped gauge vs. measured runtime remaining</H2>
      <Text style={{ color: theme.text.secondary }}>
        The firmware reads <em>low</em> through the whole middle of the
        discharge — it says 50% with three quarters of the runtime still left —
        then the two curves cross near 20% and the gauge runs out of scale.
      </Text>
      <LineChart
        categories={HOURS.map((h) => `${h.toFixed(1)} h`)}
        series={[
          { name: "Measured runtime remaining", data: MEASURED_PCT, tone: "success" },
          { name: "Firmware reported", data: FW_PCT, tone: "danger" },
        ]}
        height={300}
        beginAtZero
        yMax={100}
        valueSuffix="%"
      />
      <Text style={{ color: theme.text.tertiary, fontSize: 12 }}>
        Measured series assumes the 40.05 h mid-estimate total · firmware series
        is the raw value read from the battery characteristic at each sample
      </Text>
    </Stack>
  );
}

function RegionRates() {
  const theme = useHostTheme();
  return (
    <Card>
      <CardHeader>Discharge rate is not constant</CardHeader>
      <CardBody>
        <Stack gap={12}>
          <Text style={{ color: theme.text.secondary }}>
            Least-squares fits over three regions. Extrapolating the upper slope
            predicted about a day; the plateau is where the cell actually spends
            its life, which is why it ran ~40 h.
          </Text>
          <Table
            headers={["Region", "Voltage range", "Rate (mV/h)", "Duration", "Samples"]}
            columnAlign={["left", "left", "right", "right", "right"]}
            rows={REGIONS.map((r) => [
              <Text weight="medium">{r.label}</Text>,
              <Code>{r.range}</Code>,
              r.rate,
              r.span,
              String(r.n),
            ])}
          />
          <Text style={{ color: theme.text.secondary }}>
            The knee is already <strong>2.1× steeper</strong> than the plateau
            and still steepening when logging stopped — so the true cutoff sits
            at the optimistic end of the estimates below.
          </Text>
        </Stack>
      </CardBody>
    </Card>
  );
}

function BoundTable() {
  const theme = useHostTheme();
  const toneFor = (s: Row["state"]) =>
    s === "unreachable" ? "danger" : s === "edge" ? "warning" : undefined;
  return (
    <Stack gap={8}>
      <H2>Every firmware table entry, checked against the data</H2>
      <Table
        headers={[
          "Voltage",
          "Firmware says",
          "Actually remaining",
          "Range / why not",
        ]}
        columnAlign={["right", "right", "right", "left"]}
        rowTone={ROWS.map((r) => toneFor(r.state))}
        striped
        rows={ROWS.map((r) => [
          <Code>{r.mv.toLocaleString()} mV</Code>,
          `${r.fw}%`,
          <Text
            weight={r.state === "unreachable" ? "medium" : "normal"}
            style={{
              color:
                r.state === "unreachable" ? theme.text.tertiary : theme.text.primary,
            }}
          >
            {r.measured}
          </Text>,
          <Text style={{ color: theme.text.secondary }}>{r.note}</Text>,
        ])}
      />
      <Text style={{ color: theme.text.tertiary, fontSize: 12 }}>
        "Actually remaining" is the mid-estimate; the range column spans the
        0 h and 6.5 h death-window bounds. The top two rows sit above 4,020 mV,
        where logging began, so they were never observed.
      </Text>
    </Stack>
  );
}

function Reasoning() {
  const theme = useHostTheme();
  return (
    <Grid columns={2} gap={16}>
      <Card>
        <CardHeader>How the bound is derived</CardHeader>
        <CardBody>
          <Stack gap={10}>
            <Text style={{ color: theme.text.secondary }}>
              Two hard anchors make this measurable rather than a guess:
            </Text>
            <Stack gap={6}>
              <Text>
                <strong>1.</strong> The last sample was 3,455 mV at 13:23, with
                the board still running.
              </Text>
              <Text>
                <strong>2.</strong> It was found flat at 19:53 — so death
                happened inside a 6.5 h window.
              </Text>
            </Stack>
            <Divider />
            <Text style={{ color: theme.text.secondary }}>
              At the measured knee rate of 18.9 mV/h, 6.5 h buys 123 mV, landing
              at <Code>3,332 mV</Code>. If the knee steepened 2× it lands near{" "}
              <Code>3,209 mV</Code>. Either way the cell never approached the
              3,164 mV the table calls 1%.
            </Text>
          </Stack>
        </CardBody>
      </Card>

      <Card>
        <CardHeader>Why it stops around 3.35 V</CardHeader>
        <CardBody>
          <Stack gap={10}>
            <Text style={{ color: theme.text.secondary }}>
              The regulator, not the chemistry, sets the floor. VDD held
              3,293–3,309 mV across the entire 565 mV cell swing and never
              sagged — including at 3,455 mV, leaving only ~150 mV of headroom.
            </Text>
            <Text style={{ color: theme.text.secondary }}>
              Once the cell nears the 3.3 V rail plus dropout, VDD collapses and
              the SD card and radio go with it. That is the real 0%, and it sits
              roughly <strong>350 mV above</strong> where the table puts it.
            </Text>
            <Divider />
            <Row gap={8} wrap align="center">
              <Pill tone="success">battery_guard trips at 3,350 mV</Pill>
              <Text style={{ color: theme.text.secondary }}>
                — which lands right at the true cutoff.
              </Text>
            </Row>
          </Stack>
        </CardBody>
      </Card>
    </Grid>
  );
}

function Caveats() {
  const theme = useHostTheme();
  return (
    <Stack gap={8}>
      <H3>What this does not establish</H3>
      <Stack gap={6}>
        <Text style={{ color: theme.text.secondary }}>
          Logging began at 4,020 mV, not a full 4,150 mV charge, so the top of
          the curve is missing and total runtime is a slight underestimate.
        </Text>
        <Text style={{ color: theme.text.secondary }}>
          The death time is bounded to 6.5 h, not known. Every percentage above
          carries that uncertainty, which is why the range column exists.
        </Text>
        <Text style={{ color: theme.text.secondary }}>
          One cell, one discharge, at one temperature. The shape should hold;
          the absolute hours will drift with load and age.
        </Text>
      </Stack>
    </Stack>
  );
}

export default function BatteryDischargeCurve() {
  return (
    <Stack gap={28} style={{ padding: 28, maxWidth: 1080 }}>
      <Header />
      <Verdict />
      <KeyNumbers />
      <DischargeCurve />
      <Divider />
      <GaugeComparison />
      <RegionRates />
      <BoundTable />
      <Reasoning />
      <Caveats />
    </Stack>
  );
}
