# duplex-probe: running the phase 0 measurements

`duplex-probe` is the phase 0 tool (see [ROADMAP.md](ROADMAP.md)). Each
command prints a summary and leaves a report file; the files are what goes
back into the design.

It is in `AJA-Duplex-ASIO-<version>-win64.zip`, from the repository's
releases or from `build.cmd` (`dist\`): unzip it on the PC with the KONA X
and run it from there (it needs nothing installed but AJA's driver). On
the PC that built it, the folder of that name beside the ZIP has the same
files (a new build keeps the `reports` in it), and `build\Release\` works
too. Writing the reports to a folder of their own
keeps them together:

```
duplex-probe list
duplex-probe info --label idle --out reports
```

Order for a session with the card: `list`, the three `info` runs,
`heads` and `cost`, then `loop` with the AES cables, `loop --path analog`
with the XLR ones, `routing --scan-select` with both, and `watch` (no
cables needed) at the end. [CARD_SESSION.md](CARD_SESSION.md) has the
whole session as one list, the driver's part included.

The commands that use an audio system refuse it while AJA Duplex ASIO has
it open (in Resolume, say): "AJA Duplex ASIO is open in <program> on this
card and audio system: close it first". The driver refuses it the same way
while the probe has it.

## `list`

One line per AJA device: index, model, serial number and SDK device ID.
The index, the serial number or the model name can be given to
`--device` (default: index 0).

## `info` - three runs

Read-only: it reads registers and driver state, and never writes a
register, reserves the card or changes its task mode, so it can run next
to anything, a show included. It takes about half a second.

The console shows the state; the file adds the card's capabilities and
every audio register decoded by the SDK.

1. **Idle.** Nothing else using the card: Resolume closed, no host with
   the AJA ASIO driver open. The AJA Control Panel may stay open.

   ```
   duplex-probe info --label idle --out reports
   ```

2. **AJA's drivers playing.** A host capturing through the AJA ASIO
   driver (a track armed for recording is enough) and Windows sound
   playing through the AJA WDM device (any audio with it as the output
   device), both at the same time.

   ```
   duplex-probe info --label aja-drivers --out reports
   ```

   Answers which audio system each of them uses (Q10): look at
   `input running` / `output running` in the `[audio system N]` sections.

3. **The plugin capturing.** Resolume with AJA Input on the inputs and
   formats used in shows (for example 2 x 1080p, or 2160p), in its normal
   mode.

   ```
   duplex-probe info --label plugin --out reports
   ```

   Answers whether the video frames come near the audio buffers (Q11):
   `[autocirculate]` says where each channel's frames end and how far that
   is below the audio buffers, and `[sdram]` lists any conflict the SDK's
   SDRAMAuditor finds.

   If shows use the plugin's Low Latency Mode, a fourth run with it on
   (`--label plugin-lowlatency`) is worth having. In that mode the
   FrameStores are driven by hand, not by AutoCirculate, and the auditor
   only sees the frame each FrameStore is on at that moment, so the
   report covers less of the memory the plugin uses.

**What to keep:** the three (or four) `reports\info-*.txt` files.

## `heads` and `cost` - step 2, no cables

Both use one audio system: the card's last one (2 on a KONA X) unless
`--audio-system` says otherwise. They refuse it if its input or output is
running, which means another program is using it. They change its
settings for the run and put them back at the end, also after Ctrl+C and,
since 0.1.18, when the console window is closed (Windows gives a program
about five seconds then). Ended from the Task Manager, a command puts
nothing back: the audio system runs on, with the card in OEM task mode.
Since 0.1.21 the next command (or the driver) to open that audio system
takes it over, stops it and puts the task mode back (and, since 1.0.0,
the audio system's own settings as the ended command had found them),
and says so in its first lines ("taken over: duplex-probe.exe (process
...) left audio system 2 running and is gone"); before, it counted as in
use until the PC was restarted (DRIVER.md, "After a host closed by
force"). The same goes
for every command below.

Not during a show:

- `heads` sets the audio system's input to AES, and the SDK then rewrites
  the Xpand's analog/AES select, which every audio system shares. `heads`
  puts it back straight away (microseconds later), but an input in use on
  another audio system could click.
- An output that follows that audio system (AES, SDI embedder, HDMI, if
  something routed it there) plays a -138 dBFS marker for the first 1.4 s
  of `heads`, then silence.

```
duplex-probe heads --label idle --out reports
duplex-probe cost --label idle --out reports
```

`heads` takes about 25 s (`--seconds` sets the 10 s of head sampling) and
answers, in its report:

- how each head moves: step sizes, the value modulo the frame size, the
  rate against the PC's clock (Q1);
- whether LastIn points at the last byte written or at the next one, by
  reading the capture ring around it during the first lap (Q1);
- whether the 48 kHz counter runs on the heads' clock (Q16);
- whether erase mode zeroes what was played;
- whether eAudio, eAudioInWrap and eAudioOutWrap arrive, and how often (Q6);
- whether capture reaches memory without SetAudioCaptureEnable (Q13).

`cost` takes about 15 s, on a thread in MMCSS "Pro Audio" with the
high-resolution timer, as the engine will run:

- register read time, 100 000 reads (Q2);
- DMA read and write of 4, 8 and 16 KB, plain and page-locked (Q2);
- how late the timer wakes up after 0.5, 1 and 2 ms sleeps;
- a first estimate of the output guard G for N = 64, 128 and 256;
- since 0.1.16, which DMAs go through at all, with the audio system
  stopped and then running (for under a second, with the playback ring
  zeroed first): writes to the playback ring and reads from the capture
  ring of 512, 2560 and 8192 bytes ending at the ring's end, running 64
  bytes past it, across it, in the 16 KB past it that the card leaves
  alone, ending at the 4 MB of the ring's room, at the ring's start, and
  for comparison one frame before the end and mid-ring; 128 fresh buffers
  with a write and a read each of 512 bytes and of 8 KB (a bad one fails
  wherever it goes, and again when tried again); and 32 plain buffers made
  where a locked one had just been unlocked. The console shows what failed
  (`[DMA checks, in short]`); the file has the tables. 0.1.15 on the KONA
  X had the piece of the output block that ended at the ring's end fail
  every lap: this says whether it is the place, the size or the buffer.
- since 0.1.17, the buffers those checks go through are shown first with
  the smallest size and the largest (in 0.1.16 the read buffer passed 8 KB
  mid-ring and then failed every read of 512 to 2624 bytes, wherever it
  went); a buffer that fails, there or among the fresh ones, says which
  sizes go through it, 64 bytes to 8 KB, and whether it lies where a
  buffer was locked earlier in the run (a stale lock in AJA's driver is
  one explanation).

**What to keep:** `reports\heads-*` and `reports\cost-*`, the .txt
and the .csv of each. The .csv files are large; zipped they shrink a lot.

## Without the card: `--simulate`

```
duplex-probe heads --simulate
duplex-probe cost --simulate
duplex-probe loop --simulate
```

They run against the simulated card (FakeCardAudio): every card number in
the report is made up, and the header says so. It is there to see the
reports and check the tool before the hardware session.

One part is real even then: the wake-up test of `cost --simulate`
measures this PC's timer. Run on the KONA X machine, it is already the
timer half of Q2.

## `loop` - step 3, AES cable

`loop` runs the audio engine the driver will use, on one audio system,
with AES out cabled to AES in. Each looped output plays a tone of its own
(997 Hz on AES 1, 1499 Hz on AES 2, up to 4507 Hz on AES 8) after a short
sweep, and each looped input is checked for what a glitch does to a tone:
discontinuities, frames lost or repeated, the level, and the round trip.
It is the test that decides whether the design holds.

Up to 0.1.16 each looped output carried a frame counter, and each input
had to bring every frame back once, in order, word for word: on the KONA X
no word came back as sent (2026-11-10), because its AES inputs resample
(AJA: "synchronous or nonsynchronous, internal sample rate conversion").
That check stays, as `--path aes-words`, for inputs that do not resample
(another card, the simulated one); when the words come back changed, its
report says so in one line and points here.

**Cabling.** The Xpand has four AES inputs and four AES outputs on BNC,
two channels per connector (AES 1-8), and LTC in and out on connectors of
their own (not used here). Loop all four pairs, each out to the same in
(out 1-2 to in 1-2, ..., out 7-8 to in 7-8), with ordinary 75 ohm video
BNC cables, which AES3id is made for; `--channels 1-8` then checks all
eight channels. With a single cable on 1-2, leave `--channels` out (the
default is 1-2).

**What it changes, for the run** (all put back at the end, Ctrl+C
included). Not during a show:

- the card goes to OEM task mode, as Resolume's AJA plugin sets it: in
  standard task mode AJA's services may set the routing their way while
  a run goes on (in the first sweep on the KONA X, 2026-11-10, every run
  failed so, with the AES outputs back on AJA's mixer). Since 0.1.18 the
  mode goes first, and 100 ms pass before anything else is set when the
  card was in standard mode, for a pass of the services under way to
  end. `--tasks keep` leaves the card's mode as it is;
- the audio system's input becomes AES, and so does the Xpand's analog/AES
  select, which every audio system shares: an analog input in use
  elsewhere goes quiet for the run;
- AES out 1-8 play this audio system (after power-up they follow audio
  system 1); the looped outputs play their tones at -40 dBFS, and the
  others are silent. The Xpand's analog output plays AES out 1-2 (Q4), so
  997 and 1499 Hz come out of the XLR outputs too, at -16 dBu with the
  Xpand at +24 dBu: keep monitors on them down. (`--path aes-words` plays
  a quiet buzz instead, peak -48 dBFS.)

**The sweep** first, 16 runs of 10 s (about 3 minutes): N 256, 128, 64
and 32 against G 64, 32, 16 and 8. Each run prints CLEAN or FAILED. Runs
need 3 seconds at least: the tones are checked by whole seconds, after the
first quarter of a second (`--path aes-words` takes any length).

```
duplex-probe loop --channels 1-8 --label sweep --out reports
```

**The exit test of phase 0** next: N = 128 for one hour, at the driver's
own guard, 256 (the verdict prints the command). The sweep's runs last
seconds: they say which guards fail, not which one holds for an hour. On
the KONA X (2026-11-11) both sweeps were clean from G = 8, some runs with
a frame to spare, and ten minutes at G = 8 with video being captured
failed four blocks: an audio DMA waited 2.8 ms. A smaller guard, for a
host with short callbacks and no video, is worth an hour of its own:

```
duplex-probe loop --frames 128 --guard 256 --seconds 3600 --channels 1-8 --label exit --out reports
```

It prints a line a minute while it runs (`clean so far`, or how many
problems so far: Ctrl+C ends it early and still writes the report).
`PASSED` in the verdict is the exit test of phase 0 passed. Run it on
the PC as it is normally set up, with Resolume closed: the same test with
the plugin capturing is step 5.

Also worth a short run, once `heads` has said whether LastIn points at
the last byte or the next one: `--head last` or `--head next` with the
same N and G, against the default `conservative` (one frame less latency
if it holds).

**The report:**

- `[runs]`, one entry per run: CLEAN or FAILED and what went wrong; the
  round trip in frames and ms, from the sweep, to a fraction of a frame;
  N + S, the engine's share, and the rest, the card's, the cable's and the
  AES inputs' resampler's; for each channel the level that comes back,
  noise and distortion, and how much the delay drifted and wandered (see
  `loop --path analog`); the smallest margin of
  the output over the playback head; the longest DMA in and out and the
  client's time; the card's clock against the PC's; how far the playback
  head was from where the input's timing put it ("output within -8 to +8
  frames of the input's timing": the heads' steps, while input and output
  run together); the first engine
  events with their time in the run, a DMA error with which DMA and how
  the second tries went (from the same buffer, then from a spare:
  `DMA recovered` when one saved it, and the run stays clean); where the
  blocks met the ring's end ("output 40 + 88 frames": the block there in
  one DMA running on past the end, then 88 frames at the start); and any
  buffer a spare replaced, with what it did when tried again at stop.
- `late outputs (... frames dropped)`: blocks that came after the card had
  read past where they go. Since 0.0.7 the engine drops what the card
  passed, and G frames after it, and sends the rest on time: frames are
  lost (they come back as silence) but the round trip stays the same,
  which is the latency the driver will report (D1). `output moves`: the
  card's output head was not where the input said it must be (its output
  restarted by something else, say, or since 0.1.21 its input held back
  until every block came late) and the output was placed anew, where the
  round trip is the reported one again.
- `[verdict]`: for each N, the smallest G that came out clean; the
  smallest clean N; the exit test.
- Named problems: glitches, slips and level changes on a channel, with
  their time in the run (as with the analog loop), `crossed cables` (an
  input hears its pair's tone), `never heard ... check that cable`. With
  `--path aes-words`: frames lost, repeated, silent or from the wrong
  channel, `crossed cables`, `words changed` (the AES inputs resample or
  carry less than 24 bits: the words need both bit-exact),
  `silence without a late output` (something re-routed AES out or the
  input during the run: AJA services in standard task mode, or the
  Control Panel, Q9).
- The .csv has a line per run: the engine's counts, and for the tones the
  worst of the channels (glitches, slips and level changes added up, the
  lowest and highest level, the worst noise, the shortest and longest
  round trip, the drift's range and the largest wander).
- A start whose check of the DMA buffers found a bad one (the driver's
  engine checks them all, 0.1.17: DRIVER.md, "DMAs") says so in its run's
  entry ("buffers: ...").

**What to keep:** `reports\loop-*`, the .txt and the .csv of the
sweep and of the exit test.

**Without the card:** `duplex-probe loop --simulate`. The simulated PC
wakes up late now and then (one wake-up in a thousand, by 2 ms), so the
small combinations of the sweep fail the way they might on a real PC; the
numbers are made up. Its AES inputs do not resample, so `--path
aes-words` passes there too.

## `loop --path analog` - step 4a, XLR cables

The same engine through the Xpand's converters, the path that matters
most here, with the same tone check as the AES loop: each output plays a
tone of its own (997 Hz on L, 1499 Hz on R) after a short sweep, and each
input is checked for what a glitch does to a tone.

**Cabling.** XLR out L to XLR in L and out R to in R, through the Xpand's
DB-15 breakout. **Nothing else on those outputs:** the tones are -20 dBFS,
+4 dBu with the Xpand at +24 dBu, which is loud on a monitor. The AES
cables can stay.

**What it changes, for the run** (all put back at the end, Ctrl+C
included):

- the audio system's input becomes analog, and the Xpand's analog/AES
  select with it, which every audio system shares: AES in 1-2 go away for
  everyone during the run (only those, Q5: 2026-11-10);
- AES out 1-8 and the monitor play this audio system: the analog output
  plays AES out 1-2 (Q4), so it plays our L and R.

**The sweep** first, then **the analog exit test**, as with AES:

```
duplex-probe loop --path analog --label analog-sweep --out reports
duplex-probe loop --path analog --frames 128 --guard 256 --seconds 3600 --label analog-exit --out reports
```

Runs need 3 seconds at least: the checks work on whole seconds, after the
first quarter of a second.

**The report**, per run, besides what the AES loop gives:

- glitches on L and R with their time in the run: any discontinuity of
  the tone (silence, a click, frames lost or repeated);
- slips: times the tone's phase moved by frames (`frames lost`: the
  stream jumped ahead; `frames late`: it fell back), with their size when
  a clean second came before and the delay was steady enough to tell it.
  A late output does not slip the tone any more (0.0.7, D1): it is a
  glitch, a short gap, with the phase where it was; a slip means the
  timeline itself moved (a stall, an output move);
- the delay's drift and wander. On the KONA X the analog loop's delay
  drifted by about 2 us in 10 s, the same on L and R, and 0.1.16 called
  that a slip in every run. Since 0.1.17 each second is held against the
  last still one moved on by the drift so far; a second with a glitch in
  it is not measured; after a glitch a move of more than a fortieth of a
  frame is a slip, without one more than a quarter of a frame, and never
  less than eight times the median wander. `delay drifted +1.60 us,
  wander 0.01 us`: how much later the tone came back at the end than at
  the start, the slips aside, and what is left of each second's move once
  the drift is out (RMS);
- the level that comes back and the gain of the loop, the noise and
  distortion of the median second (and of the worst, AES17 dBFS: the
  first second through the Xpand was noisier while its input settled),
  and the polarity (an inverted cable is noted, not failed);
- the round trip from the sweep, to a fraction of a frame, and "card,
  converters and cable": what InputLatency plus OutputLatency must add for
  the XLRs (Q12). (Through AES the resampler adds its own, so the AES
  loop's round trip is no longer "card and cable" only.)

Named problems: `L and R crossed`, `never heard its tone` (the XLR cable,
or the analog output takes its signal from somewhere this does not route).

**Without the card:** `duplex-probe loop --path analog --simulate`, with
converters of 40 frames, -0.1 dB and -100 dBFS of noise.

## `routing` - step 4b, AES and XLR cables

Each of the 16 outputs plays a tone of its own and each input says which
it hears, so in a few seconds it answers where the Xpand's analog output
takes its signal from (Q4), what the analog input replaces (Q5) and, with
`--scan-select`, what each value of the Xpand's select does (Q14). It does
not need the engine: the tones fit the playback ring exactly, so the card
loops it by itself.

**Cabling:** the four AES pairs looped and XLR out L/R to XLR in L/R, as for
the two loops. Nothing else on the XLR outputs: every output plays a tone
at -20 dBFS.

```
duplex-probe routing --scan-select --label routing --out reports
```

About 8 seconds (2 without `--scan-select`). It changes, for those
seconds, the same shared settings as the analog loop (the Xpand's select,
AES out 1-8 and the monitor, and the task mode to OEM unless `--tasks
keep`) and, with `--scan-select`, writes the select with every value from
0 to 15, where the SDK only ever writes 0 and 1. Everything goes back
afterwards.

**The report** gives the answers first, then the three set-ups it ran
(input AES; input analog with the monitor on 15-16; input analog with the
monitor on 1-2) and the scan, input by input: `A` its own AES output, `A~`
the same resampled on the way (not bit-exact, its own output's tone the
loudest: the KONA X's AES inputs, 2026-11-10), `L` and `R` the analog
output's left and right, `oN` the tone of output N, `-` nothing. An input
through converters or a resampler is told from a bit-exact one by its
noise.

- **Q4:** with the monitor on 15-16, the analog input hears outputs 1-2
  (the analog output follows AES 1-2) or 15-16 (it follows the monitor).
- **Q5:** with the analog input on, the cabled AES inputs 3-8 still hear
  AES (only AES 1-2 is replaced) or nothing (the whole AES input is).
  Up to 0.1.16 only a bit-exact AES input counted, and on the KONA X Q5
  came out unknown with AES 3-8 cabled; `A~` counts since 0.1.17.
- **Q14:** one row per select value. "Only bit 0 counts" and "any value but
  0 selects analog" are the dull answers; a value that puts the analog on
  other inputs while AES in 1-2 stay is the one worth having, since it
  would give AES and analog at the same time.
- **For the driver:** what that means for its channel names and inputs.

**Without the card:** `duplex-probe routing --simulate --scan-select`
(analog output on AES 1-2, analog input replacing AES 1-2 only, only bit 0
of the select counting).

## `watch` - step 5, no cables

The audio system set up and running exactly as the driver's (its input,
the select following it, AES out 1-8 and the monitor routed to it), in
silence, while its routing is read back once a second, as the driver does
while it plays. Each change another program makes is printed the moment it
is found, as the driver's log would put it, and listed in the report.

**Q9: do AJA's services rewrite the audio?** Put the card in standard task
mode in the AJA Control Panel, close it and every program that uses the
card, and let it run for five minutes:

```
duplex-probe watch --seconds 300 --label services --out reports
```

Nothing changed means the services leave the audio routing alone, and the
driver needs no OEM task mode for it; any change, with nobody touching the
card, means they rewrite it. The report's answer says which.

**What the driver will report.** Then run it again and, while it runs,
open the AJA Control Panel and change its audio settings (the AES outputs'
source, the analog/AES input, the task mode), or start a program that
plays through AJA's own audio driver:

```
duplex-probe watch --seconds 120 --label panel --out reports
```

Changing the input's source there takes the audio system's input off the
card's clock for as long as it lasts: on the KONA X (2026-11-11) the
capture side lost some two seconds of frames in the 22 its source was
not the card's own, and up to 0.1.20 the engine was left with every block
late (39848 of 44161). Since 0.1.21 the report's
`engine` line counts the times the output was placed anew for it ("output
moves"), and the late outputs are those of the quarter of a second each
one takes to be told.

`--keep` puts the routing back as the driver's Keep my routing does (and
shows whether the other program fights back); `--input aes` sets up the AES
input instead of the analog one. The Xpand's analog level is left as the
card has it, as the driver does by default: a change of it is reported but
not put back, and for Q9 it counts like any other. The audio system is put back afterwards,
and of the card's shared settings only what still holds `watch`'s own.

**Without the card:** `duplex-probe watch --simulate` plays another program
that takes AES out 1-4 at 40% of the run and sets the select to AES at 70%.

## What comes next

Coexistence with video: the loops with Resolume and the AJA Input plugin
capturing (Q3, how long audio DMAs wait). Step 6 needs gear that is not
there yet (an SDI source with embedded audio, a free-running AES device).

The driver itself (phase 1) is in [DRIVER.md](DRIVER.md): `loop --path
analog`'s "card, converters and cable" line is what the driver adds to
the latencies it reports (208 frames on the KONA X, its own figure since
1.0.0; `InputLatency` and `OutputLatency` for another), and
`duplex-asiotest --card` with the XLR loop checks that.
