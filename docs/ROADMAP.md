# Roadmap

Where AJA Duplex ASIO stands and what comes next. "The design", here and
in the code's comments, is the project's design document, written before
the code (in Spanish, and not in this repository): this file tracks it
against the code, and [DRIVER.md](DRIVER.md) says what came of it. For
what each version changed see [CHANGELOG.md](../CHANGELOG.md).

## Phases

- **Phase 0 - measure (on the card since 2026-11-10; its exit test
  passed on 2026-11-11).** A console tool, `duplex-probe`, answers the
  design's open questions on the KONA X and fixes the two numbers the
  driver is built around: the smallest buffer N that works and the output
  guard G. **Exit test:** AES out looped to AES in, N = 128, one hour
  without a lost or repeated sample; and, since the analog I/O is what
  the driver is for first, the same hour with XLR out L/R looped to XLR
  in L/R without a glitch (both set on 2026-11-08). If phase 0 does not
  get there, the design is revised. Both hours ran clean on the
  KONA X on 2026-11-11, at N = 128 with the driver's guard of 256 ("Found
  on the card", the fourth session).
- **Phases 1 to 4 - the driver**, in steps, each ending with a test on the
  KONA X before the next one starts, as laid out in the design document.
  Phase 4 ends with an installer (until then, `install-driver.cmd`, which
  since 0.1.22 copies the driver to Program Files and registers it
  there). The design has phase 1 wait for phase 0's exit test; with no
  card at hand, phase 1 was started on 2026-11-08 on the simulated card.
  Its code was written and tested against the simulation; the sessions
  with the card ran phase 0's tests first, then phase 1's.

Up to 0.1.22 the versions followed the phases: 0.0.x is phase 0, 0.1.x
phase 1. The first version published is 1.0.0: phase 1's driver as it ran
on the card. What the design keeps for the phases after it comes as 1.x.

## The first published version (1.0.0, 2026-11-11)

0.1.20 took the simulated driver out of the hosts' lists, to leave the
one that would stay if it did well on the card. It did (the third and
fourth sessions, "Found on the card"), and 1.0.0 closes what the fourth
one left open. It was built with Visual Studio and run on the card the
same evening (the fifth session):

- The hardware latencies are the driver's own figures for the KONA X with
  the Xpand, by input: 208 frames through the analog path (the driver's
  round trip; 207.2 in phase 0's analog hour) and 130 through AES with
  its resampler (the two AES hours: 129.9 and 133.7). A fresh install
  reports them with nothing set, and `InputLatency` and `OutputLatency`
  replace them. Up to 0.1.22 they were 0 until `card-session`'s step 7
  had measured the round trip and written the difference to that PC's
  registry. On the card the driver reports 720 frames at the defaults,
  and the round trip of a start comes out at 718 to 727: it moves by a
  few frames from one start to the next, with the offset between the
  card's two heads.
- A takeover after a host ended by force (Q18) puts the audio system's
  own settings back as that host had found them. 0.1.22's, seen on the
  card, stopped the audio system and put the task mode back, and left
  capture enable and the erase mode on. 1.0.0's left the audio system as
  it was before the forced close, with `duplex-probe` and with the
  driver.
- The settings dialog says which version it is, its date and whose, with
  a button to the repository.

1.0.1 (2026-11-18) adds what using it in vMix asked for: vMix has no
ASIO settings button, so the settings open from the Start menu too ("AJA
Duplex ASIO Settings"), and the dialog says samples, with milliseconds,
where it said frames. The audio is 1.0.0's.

Still open, for the versions to come:

- On the card: hours in Resolume with AJA Input capturing (ten minutes
  so far on the card, 4 h 48 min on the simulated one), which phase 1's
  exit test, as proposed below, asks for; and the try with AJA Input's
  sources (Q19).
- The embedded inputs, SDI and HDMI, and the SDI and HDMI outputs:
  untried on the card (Q7, Q17), their latencies not measured. What
  0.1.21 does when their clock leaves the card's is the least of it;
  offsets that make no block late are left (DRIVER.md, "Not there yet").
- What [DRIVER.md](DRIVER.md) lists under "Not there yet": the clock
  model that gets no readings on Windows, and what a takeover leaves of
  the card's shared settings where another program's OEM task mode stays.
- An installer, which the design puts in phase 4: one to run instead of
  two scripts, an entry among Windows' installed apps, and a version
  resource in the DLL. Its first half is done since 0.1.22:
  `install-driver.cmd` copies the driver to
  `Program Files\AJA Duplex ASIO` and registers that copy, after hosts
  were left on 2026-11-11 with a driver they listed and could not open
  ("Found on the card").

## Phase 1, step by step

The ASIO driver on the engine phase 0 measures. Its parts follow the
design's structure: `src/asio` (DriverCore: what IASIO does, without COM;
the COM class, class factory and registration in `src/asio/win`), the
settings in `src/common/DriverSettings` (HKCU), and a small host,
`tools/asiotest`. See [DRIVER.md](DRIVER.md).

| Step | What | State |
| --- | --- | --- |
| 1 | ASIO SDK 2.3.4 headers in third_party/asiosdk (GPLv3 option) | done (0.1.0) |
| 2 | DriverCore: init, channels, sizes, rate, clock, latencies, createBuffers, start/stop, sample position, outputReady, future, messages to the host | done (0.1.0), unit-tested on the simulated card, also on its own thread in real time |
| 3 | AJADuplexASIO.dll: COM object, class factory, regsvr32 registration, HKCU settings, control panel showing the settings and the run | done (0.1.0); built with MinGW and run under Wine, not yet with MSVC |
| 4 | The simulated card as a second driver, for trying the driver in a host without the card ("AJA Duplex Simulator"; "AJA Duplex ASIO (simulated)" up to 0.1.16, which made vMix leave the card's driver out of its inputs), and `duplex-asiotest` (a host that checks callbacks, positions, times and the round trip against the reported latencies) | done (0.1.0; renamed 0.1.17). The second driver is gone since 0.1.20, with every test left needing the card: the simulated card stays inside the DLL, unregistered, for `duplex-asiotest` and the build's own test |
| 5 | The driver's log; the Xpand checked at init; the card's reference named | done (0.1.1) |
| 5b | A settings dialog: latencies at once, the rest through the host's reset | done (0.1.2) |
| 5c | The routing and task mode read back at start and once a second (the design's coexistence rules): changes logged and shown, put back with Keep my routing (off by default); at release only the driver's own routing is put back | done (0.1.3); `duplex-probe watch` runs the same watch on the card without a host |
| 5d | The Xpand's analog level in the settings: as the card has it by default, or +24/+18/+15/+12 dBu, set at start, applied at once from the dialog, watched and kept like the routing | done (0.1.4) |
| 5e | The embedded inputs: an SDI input's 16 channels or the HDMI input's 8, on the video input's clock | done (0.1.5); on the card it needs an SDI source with embedded audio (step 6 of phase 0's gear) |
| 5f | OEM task mode while playing, as the design has it (set at start, reasserted, back afterwards unless another program holds the card); the log says what release left | done (0.1.6); on by default since 0.1.17 (Q9) |
| 5g | The SDI and HDMI outputs, as the design has them: off by default, 16 outputs with an SDI output on, part of the routing watch, put back afterwards; the SDI connectors' way read and reported | done (0.1.7) |
| 6 | On the card: `duplex-asiotest --card` with the XLR loop (round trip against the reported latencies, which phase 0's measurements fill in), then a session in a host | the session's list is [CARD_SESSION.md](CARD_SESSION.md) (0.1.13). First contact 2026-11-10: opened in Resolume, 17 min at the defaults with no late output, and a DMA error once a lap of the ring. 0.1.14 moved the pieces of the block split at the ring's end to plain buffers (71 s without an error, by luck of where the blocks fell); 0.1.15's log named the piece ending at the ring's end; 0.1.16 runs that block on past the end and tries failed DMAs again: no DMA error since, in Resolume and in vMix (N = 1024, 4.4 min). 0.1.17 checks every DMA buffer at start. 2026-11-11, 0.1.20: `duplex-asiotest --card` passes with `OutputLatency` 208 (round trip 720 frames at the defaults); vMix lists the driver as an input and ran it both ways; five minutes in Resolume without a late output. The same day, 0.1.22: ten minutes in Resolume with AJA Input without a late output, and 0.1.21's takeover after a forced close seen working. 1.0.0 reports the 208 frames by itself and its takeover leaves the audio system as it was found: built with Visual Studio and run on the card that evening, two minutes in Resolume without a late output. To come: Resolume for hours with AJA Input |

**Exit test of phase 1** (proposed): in a host,
N = 128, the analog loop recorded for one hour without a glitch, and a
click played and recorded through the loop lining up within 1 ms (48
frames) after the host's latency compensation, the design's latency
target.

## Phase 0, step by step

Each step is a `duplex-probe` command that leaves a report file behind.
They go from least to most invasive; `loop` comes early because it decides
whether the design holds.

| Step | Command | Needs | Answers | State |
| --- | --- | --- | --- | --- |
| 1 | `info` | nothing; three runs: idle, AJA drivers playing, plugin capturing | Q10, Q11, Q15; the baseline routing | run on the KONA X 2026-11-10 (see "Found on the card") |
| 2 | `heads`, `cost` | nothing (audio system 2 only) | Q1 (steps, last/next byte), Q2, Q6, Q13, Q16; a first G | run on the KONA X 2026-11-10; `cost` checks which DMAs go through since 0.1.16, to run again |
| 3 | `loop` | AES loop cable | Q1 (no lost/repeated samples), N min and G, the round trip | first sweep 2026-11-10 not valid (standard task mode: the AES outputs went back to AJA's mixer meanwhile); the second, in OEM task mode, had no DMA error, and late outputs only at N 32 with G 32 or less, but no word came back as sent: the KONA X's AES inputs resample. Since 0.1.17 the loop checks AES with tones (`--path aes-words` keeps the words); run again 2026-11-11: clean from N = 64 with any G, N = 32 from G = 32; the one-hour run at the driver's guard PASSED the same day, twice |
| 4a | `loop --path analog` | XLR loop cables (and AES) | Q12; the analog path's glitches, level and noise | run 2026-11-10: no glitch at N 64 and above with any G, nor at N 32 with G 64; card, converters and cable 207-218 frames (Q12); one slip a run that was the delay's drift, which 0.1.17 tells from a slip; run again 2026-11-11: clean from N = 64 with any G, N = 32 with G = 64, no false slips, card and converters 208.5 to 218.1 frames; the one-hour run at the driver's guard PASSED the same day |
| 4b | `routing` | AES and XLR loop cables | Q4, Q5, Q14 | run 2026-11-10: Q4 and Q14 answered, Q5 from the noise and the tones (0.1.17 says it) |
| 5 | `watch` (0.1.3), then coexistence | `watch`: nothing; then Resolume + AJA Input | Q9 with `watch` left alone in standard task mode; Q3 with the plugin | Q9 answered 2026-11-10; Q3 answered 2026-11-11 for one 1080i50 capture (2.8 ms); run again that day at the driver's guard, clean; to run again with all of a show's captures (CARD_SESSION.md, step 5c) |
| 6 | external gear | SDI source with embedded audio; free-running AES device; an SDI cable from SDI 2 to SDI 1 | Q7, Q8; Q17 with the driver (outputs 1-16 back on the SDI input) | when available |

`loop` is the first command that uses the real engine (AudioEngine and
CardAudio, without ASIO), so the code it brings in is the driver's own.
The analog path comes before the routing questions because it is the one
that matters most here; it does not need their answers, since it routes
both possible sources of the analog output (AES 1-2 and the monitor) to
channels 1-2.

Without the card, every command but `info` and `list` runs against the
simulated card (`--simulate`, and in the unit tests). Its model has a
parameter for each thing phase 0 is meant to find out (head steps, last
byte or next, prefetch, erase, capture enable, costs, a loop cable), so
the analysis is tested against every answer the card could give; only
the card itself can say which one is true.

## Found on the card

The first session, 2026-11-10: KONA X (AJA's driver 18.0.0.29, firmware
revision 8, SDK 18.0.0), Windows 11 (build 26200), with the Xpand.

- Q1: both heads move in steps of 512 bytes (8 frames), one every 169 us,
  and read 63 modulo 64; LastIn points at the last byte written.
- Q2: a register read takes 4 us; a DMA of 1 to 16 KB about 110 us
  whatever its size (p99.9 225 us), locked or not. Windows' high-resolution
  timer wakes 0.5 ms late (96% of wake-ups past 300 us), so the engine's
  early wake-up of 300 us rarely leaves it time to poll, and its clock
  model, fed by polls, says +0.0 ppm.
- Q6: no audio interrupts on Windows (eAudio, eAudioInWrap and
  eAudioOutWrap: none in 3 s). Polling stays.
- Q10: AJA's ASIO driver captures on audio system 1 (analog), AJA's WDM
  driver plays on audio system 3 (8 channels): audio system 2, the
  driver's default, is free.
- Q11: AutoCirculate's frames end at 24 MB; the audio buffers start at
  1872 MB, far above.
- Q15 (in part): idle, in standard task mode (AJA's services hold the
  card), the AES outputs, the monitor, the SDI and HDMI outputs all play
  audio system 4, the audio mixer: main audio system 1, aux 1 audio system
  2 (channels 1-2 muted), aux 2 audio system 3. With Resolume's plugin the
  card is in OEM task mode, held by Resolume.
- Q16: the 48 kHz counter runs on the heads' clock; the card's clock is
  about +7 ppm against the PC's.
- Erase mode works. Without SetAudioCaptureEnable the input head stands
  still (Q13: the driver calls it).
- The first AES sweep failed in every run, mostly without engine events
  (lost, repeated, wrong-channel and changed words, and inputs that never
  heard their output): the card was in standard task mode, the driver
  was not running, and AJA's services had the AES outputs back on the
  mixer. 0.1.16's loops hold OEM task mode while they run.
- One DMA error a lap in the driver and in the sweep: 0.1.15's log named
  the piece of the output block that ended right at the ring's end,
  2560 bytes, Windows error 31 (ERROR_GEN_FAILURE), from a plain buffer.
  0.1.16 runs such a block on past the end instead, tries a failed DMA
  again from the same buffer and from a spare, and `cost` checks which
  DMAs go through.

The second session, the same day, with 0.1.16:

- No DMA error: 58.6 s in Resolume, the 32 loop runs, 4.4 minutes in vMix
  at N = 1024. The click is gone.
- `cost`: every place at the ring's end went through, and so did 128
  fresh buffers and 32 made where a locked one had been, with the audio
  stopped and running. But one buffer, the read buffer of the stopped
  pass, failed every read of 512 to 2624 bytes, wherever it went, and
  passed every read of 8 KB. A stale lock in AJA's driver at that address
  (the DMA cost had just locked and unlocked buffers of 4 to 16 KB) would
  fit, but plain buffers made where a locked one had been all passed.
  0.1.17's engine checks every buffer with a small DMA at start, and
  `cost` says which sizes a bad buffer passes and where it lies.
- Q9: yes. In standard task mode AJA's services moved AES out 1-4, AES out
  5-8 and the monitor back to the mixer (audio system 4) within a second
  of the probe setting them. OEM task mode is on by default since 0.1.17.
- The KONA X's AES inputs resample (AJA's spec: "synchronous or
  nonsynchronous, internal sample rate conversion"): no word came back as
  sent, and the AES sweep's counts were those of random words (half with
  the valid bit clear, 7 in 16 of the rest from another channel). The
  routing probe's tones came back from AES with a residual of -58 to -46
  dBFS rising 6 dB an octave, a jitter of some 4.5 us, which changed from
  second to second (-86 to -50 dBFS in the select scan). The Xpand's
  analog input seems to pass the same way (its residual followed the AES
  inputs' in the scan), and the analog loop's delay drifted about 2 us in
  10 s, the same on L and R.
- Q4: the analog output plays AES out 1-2.
- Q5: with the analog input on, AES in 3-8 still heard their own outputs:
  only AES in 1-2 are replaced (0.1.16 said "unknown", wanting bit-exact
  AES; 0.1.17 counts a resampled AES input).
- Q14: only bit 0 of the select counts. The tones alone could not tell
  (resampled AES in 1-2 and the analog input both hear outputs 1-2), but
  the level did: in 1 heard out 1 at -20.4 dBFS with every odd value, the
  analog path's gain, and at -20.0 with every even one; AES in 3-8 the
  same with all sixteen.
- Q12: card, converters and cable 207-218 frames (4.3-4.5 ms) in the
  analog sweep; the steps of 8 frames account for most of the spread.
  The noise through the Xpand: -86 dBFS on L, -83 on R, the first second
  noisier while its input settled.
- The engine, in both sweeps: no event at N 64 and above with any G, nor
  at N 32 with G 64; at N 32 with G 32 or less, a few late outputs (2 to
  73 in 10 s). Through the Xpand no glitch but those.
- vMix offered "AJA Duplex ASIO" as an output but never as an input while
  "AJA Duplex ASIO (simulated)" was registered: with that key deleted it
  did. Whether it mixes up drivers whose names begin the same (0.1.17
  named the simulated one "AJA Duplex Simulator") or two drivers in one
  DLL was never found out: 0.1.20 registers the card's driver alone.

The third session, 2026-11-11, with 0.1.20 (everything but the two
one-hour loops, Resolume for hours and the SDI loop):

- Both sweeps clean at N 64 and above with any G, and at N 32 from G 32
  (AES) and G 64 (analog): the false failures of the second session are
  gone, and no DMA error in the 32 runs. Some clean runs had a frame to
  spare (N 64 with G 8; N 32 with G 64 and with G 32): ten seconds say
  which guards fail, not which one holds for an hour.
- Q3: with AJA Input capturing one 1080i50 source, ten minutes of the AES
  loop at N 128: an audio DMA in waited 2.83 ms and one out 1.07 ms (0.3
  to 0.5 ms without video; in Resolume with the driver that morning, 1.3
  ms each way in five minutes). The run was at G = 8, which
  `card-session` took from the sweep, and four blocks came late by up to
  39 frames; the driver's own guard, 256, would have kept over 200. Since
  0.1.21 the long runs take the driver's guard.
- Q12 again, with the driver: `duplex-asiotest --card` at the defaults
  heard its clicks 720 frames later, 15.0 ms, 208 more than the driver
  reported; with `OutputLatency` 208 it passes, 0 frames apart. The
  analog sweep: 208.5 to 218.1 frames.
- vMix, with one driver registered: it lists "AJA Duplex ASIO" among its
  audio inputs and ran it 74 s with 2 inputs and 2 outputs at N = 1024
  without an event. It holds the card while it runs ("release: the task
  mode left as it is: vMix, process ..., holds the card") and leaves it
  in standard task mode when it closes.
- Q18: `duplex-probe` ended by force in the middle of a loop left audio
  system 2 running, input and output, the card in OEM task mode, AES out
  1-8 on audio system 2 and the Xpand's select as the loop had set it;
  the next loop was refused. A restart of the PC cleared all of it. 0.1.21
  takes such an audio system over (DRIVER.md, "After a host closed by
  force").
- Starts from standard task mode, with the 100 ms of 0.1.18: the 32 runs
  of the two sweeps heard their tones, and both `duplex-asiotest` runs
  their clicks (the part of Q19 about AJA's services writing late).
- `watch` while the AJA Control Panel changed the input's source: the
  capture side of the audio system lost frames while its source was an
  embedded one (839 blocks of 45000 in the two minutes; 418 the day
  before), the playback head went on, and from the first time on almost
  every block was late: 39848 of 44161 (18087 of 44582 the day before,
  unnoticed then). The two heads were out of step by less than the 1024
  frames at which the engine placed the output anew, and it stayed where
  it was. 0.1.21 places it anew after a quarter of a second of blocks
  late beyond their own account.
- `cost`: four of the first five of 128 fresh buffers made in a row failed
  every DMA of 64 to 2048 bytes, write and read, and passed those of 4096
  and 8192; one more earlier in the run, where a locked buffer had been
  freed. With the second session's (512 to 2624 bytes failing, 8 KB
  passing, anywhere in the ring) the rule reads: less than a page fails
  from some buffers, a page or more goes through. That is also what
  0.1.13 to 0.1.15 met: the piece that failed every lap was 2560 bytes,
  the other, 5632, never did, and a DMA of 2560 bytes ending at the
  ring's end goes through in `cost` from a buffer that is not one of
  those. Which buffers, and why, is not known. The engine checks its
  buffers with a small DMA at each start and tries a failed one again
  from a spare; never asking for less than a page would take the
  question away, and is not done.
- The same day, after the restart that step 10 asked for: Resolume listed
  the driver greyed out, with no ASIO settings button, and vMix did not
  list it. Neither the forced close nor the restart did it: the folder
  the driver had been installed from had been deleted, and with the DLL
  gone from it the driver goes, because `install-driver.cmd` registered
  the DLL where it was, and hosts were left with a name in their list and
  no file to load. Installing 0.1.21 put it right. Since
  0.1.22 installing copies the driver to `Program Files\AJA Duplex ASIO`
  and registers that copy, and `duplex-asiotest --installed` says which
  DLL hosts load and whether it is this build (`build.cmd` ends with that
  line and `card-session` starts with it). A build no longer makes the
  folder of its ZIP anew either: up to 0.1.21 a rebuild took with it the
  reports of a session run from that folder.

Fourth session, 2026-11-11 (0.1.22; what was left, but for the hours in
Resolume and the SDI loop):

- Phase 0's exit test passed on both paths. AES out 1-8 to AES in 1-8,
  an hour at N = 128, G = 256, twice: 1,350,008 blocks each, no late
  output, no DMA error, no gap, slip or level change on any of the eight
  channels; round trip 641.9 and 645.7 frames; smallest output margin 201
  and 305 frames of 384; longest DMA 2.92 ms and 0.90 ms in, 0.65 ms out.
  XLR out L/R to XLR in L/R, the same hour, once: clean; round trip 719.2
  frames (14.98 ms), card, converters and cable 207.2; margin 217; DMAs
  0.96 ms in, 1.29 ms out. A first analog hour was stopped with Ctrl+C
  after 751.8 clean seconds (its report says FAILED for the interruption
  alone; its longest DMA in, 3.05 ms).
- The 2.92 ms and 3.05 ms DMAs of the first AES hour and the analog run
  that followed it (12:37 to 13:50) are as long as the 2.83 ms that a
  video capture cost in the third session; what was running on the PC
  then is not known (both started with the card in standard task mode,
  so AJA Input was not capturing when they did). The hour that had it
  kept 201 frames of margin: a guard of 64 would have kept 9.
- Step 5c at the driver's guard, with Resolume and AJA Input holding the
  card (OEM task mode when it started): ten minutes clean, margin 305
  frames, DMAs 1.00 ms in and 0.59 ms out. What AJA Input was capturing
  is not in the report.
- Resolume with the driver at the defaults and AJA Input holding the
  card (0.1.22): 597.3 s, 223,986 blocks, no late output, no DMA error;
  margin 273 frames; DMAs 1.6 ms in and 1.5 ms out, host callback 1.5 ms.
  Resolume then let the driver go first and the card ended in standard
  task mode. The log has no "routing: another program changed task mode"
  line: the try with AJA Input's sources (Q19) either was not made or
  changed nothing. All of the driver's runs that day, in Resolume, vMix
  and `duplex-asiotest`: 30 starts, 1663 s, no late output and no DMA
  error.
- Q18, the takeover of 0.1.21, on the card (step 10). `duplex-probe`
  ended by force ten seconds into a loop left audio system 2 running in
  OEM task mode, as before; the next loop said "taken over:
  duplex-probe.exe (process 30620) left audio system 2 running and is
  gone (closed by force, or it crashed); stopped; the task mode back to
  standard, as it had found it", and ran clean. The same with the driver
  inside `duplex-asiotest`: the next one took the card over, started,
  and measured the round trip it reports (720 frames). No restart. The
  card after each takeover, line by line against the card after a normal
  release (`info`): the same but for two bits of audio system 2, capture
  enable and the erase mode, which stay on as the killed program had set
  them ("Not fixed").
- 0.1.22's install to Program Files, `duplex-asiotest --installed` and
  the new PowerShell of `card-session` and `build.ps1` all ran on
  Windows for the first time, without an error: hosts went on loading the
  driver with the folder it was installed from deleted, and the session's
  notes say "installed driver: C:\Program Files\AJA Duplex
  ASIO\AJADuplexASIO.dll (this build)".

After it, 1.0.0: the driver reports the 208 frames of the analog path,
and 130 with the AES input, as its own figures; and a takeover puts back
the audio system's own settings, which the mark now carries as the
program found them. In the fourth session's reports the takeover's `info`
shows why two bits were all that differed: with the card back in
standard task mode AJA's services wrote the audio system's source and
loopback and the Xpand's select again within the seconds between two
reports (the killed loop had left the source on AES and the loopback
off; both read as found afterwards), and capture enable and the erase
mode are not theirs to write.

Fifth session, 2026-11-11 in the evening (1.0.0, built with Visual
Studio 2022; steps 6, 7 and 10):

- It builds, its own test on the simulated card passes, it installs
  ("installed driver: ... (this build)"), and hosts run it: Resolume,
  holding the card, 117.9 s and 44,202 blocks at the defaults without a
  late output (margin 289 frames; DMAs 1.19 ms in and 0.45 ms out, host
  callback 1.13 ms), the settings dialog opened three times meanwhile;
  vMix, outputs only at N = 1024, 17 s. The driver's seven starts that
  evening, one of them ended by force: 56,288 blocks, no late output, no
  DMA error.
- The latencies: the driver reported 128 frames in and 592 out, 720 in
  all. The round trip of three starts: 725 to 727, 718 and 727 frames.
  With the three of the earlier sessions (720, 720 to 721, 720): 718 to
  727 in six starts, each steady within two frames over its ten clicks.
  The spread is between starts, and goes with the offset between the
  card's two heads, which the stop line shows: "the output within -3 to
  +10 frames of the input's timing" in the runs of 725 to 727, "-12 to
  +3" in the one of 718. The heads step 8 frames at a time, and where
  each stands when the driver starts them is not the driver's to choose.
  Every run is within 7 frames (0.15 ms) of the 720 reported.
- Step 7 of `card-session` offered to add the first run's 5 frames to
  `OutputLatency`, as it did for any difference, and 213 was saved: the
  next run came out 7 frames short of the 725 then reported. A run's few
  frames say nothing about the next one. The step now leaves
  `OutputLatency` alone within 16 frames, two steps of the heads, and
  keeps its result in `notes.txt`.
- Q18, the takeover with the audio system's settings (step 10), from a
  card with audio system 2 as a restart leaves it. `duplex-probe` ended
  by force ten seconds into a loop; the next loop said "taken over:
  duplex-probe.exe (process 25676) left audio system 2 running and is
  gone (closed by force, or it crashed); stopped; the audio system's own
  settings back as it had found them; the task mode back to standard, as
  it had found it", and ran clean. The `info` after it has audio system
  2 as before the forced close in every line: capture enable and the
  erase mode off, the loopback and the source as found (the ended loop
  had set AES and no loopback). The same with the driver inside
  `duplex-asiotest`: taken over, started, round trip 727 frames with 725
  reported, and the card afterwards as before the forced close line by
  line, but for clocks, counters and the mixer's meters. No restart.
- What the card shares was not back yet in the first of those reports,
  taken as the loop that took over closed: AES out 1-8 still on audio
  system 2 and the Xpand's select on AES, as the ended loop had set
  them. Seventeen seconds later, in the last report, AJA's services had
  both as before. Their second is not always up when the next program
  looks.
- The new PowerShell of `card-session`'s step 10 ran on Windows without
  an error, and said both times "audio system 2's own settings: as
  before the forced close".

## Found by reading (0.1.18)

A review of the driver's code on 2026-11-10, before the third session,
against the SDK's sources, AJA's Linux driver (driver/linux in the SDK)
and AJA Input's (github.com/fiverecords/AJA-Input-FFGL, src/AJADevice.cpp).

Fixed in 0.1.18:

- The engine believed its clock model whatever it said. The model gets a
  reading only when two looks at the card's head come within 100 us of
  each other, which on Windows hardly happens (see Q2), so one fitted
  early in a run is never corrected. Once the card's clock had moved away
  from that fit (it moves with temperature) the model put every block a
  little later each second, the engine slept past it, and after a while
  every block was late, for good: on the simulated card with the KONA X's
  costs, a model 500 ppm off had every block late after 20 s. The model
  is believed now only while it agrees with the reading just taken (not
  later by more than two polls, nor earlier by more than 2 ms), and
  dropped otherwise.
- With OEM task mode on, the driver took the mode back when another
  program changed it, but not what AJA's services had done meanwhile. AJA
  Input sets OEM mode at its first Open and, at its last Close, puts back
  the mode it found: standard, if nothing had the card in OEM mode before
  it (Resolume itself reserved the card, as "ARNA", in the first session:
  with that up first AJA Input finds OEM and puts OEM back). The
  services then move the AES outputs and the monitor to the mixer within a
  second (Q9), and the driver's outputs stayed there, silent, unless Keep
  my routing was on. The look that finds the task mode changed now puts
  everything of the driver's back with it, and so does the next.
- At release the driver put back the task mode it had found, also when
  that was the OEM mode of a program that held the card then and was gone
  by now, which left the card in OEM mode with AJA's services off. It
  puts standard mode in that case.
- A start that took the card out of standard mode wrote the task mode
  and the routing in the same instant, and took as its own what it read
  back when the engine had started, 10 ms later (the second session's
  logs: createBuffers to start). AJA's services, in the middle of a pass
  when the mode changed, could write the outputs after the driver did,
  and nothing would say so. In those logs, in standard mode, the
  services had not yet written over the driver's outputs 11 ms after it
  set them, and had a second later, so how long a pass of theirs takes
  is not known. The task mode goes first now, and the rest waits 100 ms
  when that took the card out of standard mode (Ntv2CardAudio::configure,
  so `duplex-probe` has it too); and the driver's first two looks after
  such a start put back what is not as it set it. What is left: the
  routing taken as the driver's is still what the card reads back, so a
  pass of the services longer than those 100 ms that wrote within the 10
  ms after would go unnoticed.

Not fixed, to decide with the card's answers:

- A host closed by force leaves the audio system running and the card in
  OEM task mode, and the driver then refused the audio system as in use
  (AJA's driver stops nothing when a program ends: ntv2_release, in its
  Linux source, only frees DMA streams and locked pages). Q18, answered
  on 2026-11-11, and written in 0.1.21: a mark of which program has the
  audio system in hand, and the next one to open it takes over what a
  process that is gone left. The mark is a volatile registry value, not
  one of AJA's virtual registers for OEM programs, as first thought:
  those are 286 slots that AJA reserves for nobody ("collisions may occur
  with other OEM applications", ntv2virtualregisters.h), on a card that
  Resolume, vMix and AJA Input use too; the registry key is the driver's
  own, and forgotten at a restart as the card's state is. Seen on the
  card on 2026-11-11 (fourth session): it works, with the probe and with
  the driver. What that takeover did not put back: the audio system's
  capture enable and erase mode, which stayed as the program that died
  had set them (both on; a normal release puts back what it found, both
  off on this card), because the mark only carried the task mode it
  found. Since 1.0.0 the mark carries the audio system's own settings
  as the program found them (channel count, rate, buffer size, loopback,
  capture enable, delays and the source select register), and a takeover
  writes them back as that program's release would have; a mark of
  0.1.21 or 0.1.22 is still read, and leaves them as they are. Seen on
  the card the same evening (the fifth session): the audio system came
  back as it was before the forced close. What is left: the settings the
  card shares (the AES outputs,
  the monitor, the Xpand's select and level). With the card in standard
  task mode AJA's services put them back; where the mode the dead program
  found was another program's OEM, they do not, and the outputs stay
  pointed at the stopped audio system until a program sets them. A
  release puts those back only where the card still holds what the
  program set, and the mark does not say what that was: it would have to
  be written again each time the driver takes a part of the routing as
  its own, from the audio thread.
- The other half of the task mode, settled in 0.1.19 and in AJA Input
  (the change to its Close, after 0.43). When the driver started first,
  AJA Input found the driver's OEM mode and put it back at its last
  Close, so if the host let the driver go first, the card stayed in OEM
  mode with nothing running; and when AJA Input was first, its last Close
  put standard back while the driver played, a second on AJA's mixer each
  time. The driver now keeps a named mutex while it answers for the task
  mode (DRIVER.md, "the task mode mark"), and AJA Input leaves the mode
  alone while it is there, and puts standard, not an OEM mode that was the
  driver's, once the driver has gone. What the card can still say (Q19):
  when AJA Input's last clip closes the card in Resolume at all (a change
  of deck? Resolume reserves the card for itself, as "ARNA", so there the
  mode is Resolume's whatever the two do), in which order Resolume lets
  the two go, and what vMix, which sets OEM mode itself after the driver
  has, puts back when it closes.
- `duplex-probe loop` has the wait but no looks afterwards: a run that
  says "never heard" with the cables in place would be the services
  writing later than that. Q19 too.
- vMix: that the simulated driver's name made it leave the card's out
  was a guess that fitted; two drivers in one DLL fits as well, and
  0.1.17 only changed the name (Q20). Settled without the answer on
  2026-11-11: with every test left needing the card, the simulated
  driver was taken out, and 0.1.20 registers the card's alone, as the
  registry was when vMix listed it in the second session.

Smaller things, left as they are: the lock between programs is named by
the settings' audio system (0, "the last", and 2 are two names for the
same one, so `duplex-probe --audio-system 2` and a host on the default do
not see each other's lock, only the running audio system); hosts show 124
characters of an error message and some of the driver's are longer; an
exception thrown inside the driver (out of memory, say) would cross into
the host; DllMain calls DisableThreadLibraryCalls, which Microsoft advises
against with the static runtime (Windows refuses the call for a DLL with
thread-local storage, as this one has, so it should be doing nothing);
configure does not clear the audio system's pause and tone bits, which
another program could have left set; with LastIn at the last byte (Q1)
the head could be read as such, which finds each block one step of 8
frames sooner.

## Open questions

From the design document, plus the ones the SDK check of 2026-11-08 added
(Q13-Q16), the SDI and HDMI outputs (Q17, 2026-11-09) and the review of
2026-11-10 (Q18-Q20).

| # | Question | Decides |
| --- | --- | --- |
| Q1 | Does ReadAudioLastIn advance sample by sample or in chunks, and does it point at the last byte written or the next one? | Minimum buffer; how frames are counted |
| Q2 | What do a register read and a small DMA cost on Windows, with and without DMABufferLock? | Polling rate and the guard G |
| Q3 | How long does an audio DMA wait while video is being captured? With one 1080i50 capture: up to 2.83 ms in, 1.07 ms out, in ten minutes (2026-11-11, morning); 1.00 ms in a second ten minutes at the driver's guard, clean (afternoon; what was captured is not in the report). With all that a show captures, still to measure (`card-session 5c`) | Worst-case G with the plugin: 256, the default, held it with over 200 frames to spare |
| Q4 | Does the Xpand's analog output follow AES 1-2 or the monitor? Answered: AES 1-2 | Output routing and channel names |
| Q5 | Does the analog input replace the whole AES input or only AES 1-2? Answered: only AES 1-2 | Whether AES and analog can be used together |
| Q6 | Does AJA's driver deliver eAudio or eAudioInWrap on Windows, and how often? | Whether polling can go |
| Q7 | With an embedded source, do the audio clock and the output stop when the signal is lost? | Behaviour when an input drops in a show |
| Q8 | With external AES not locked to the card's reference: sample-rate conversion or slips? | Whether sync must be required |
| Q9 | In NTV2_STANDARD_TASKS, do AJA's services rewrite the audio settings? Answered: yes, within a second | Whether OEM task mode is needed |
| Q10 | Which audio system do AJA's ASIO and WDM drivers use? | Default audio system |
| Q11 | Do the plugin's frames reach the audio buffers at the top of memory (16 MB in the design; 24 MB if the mixer's third buffered audio system counts)? | Whether video can overwrite audio |
| Q12 | How much delay do the Xpand's converters add? Card, converters and cable: 207-218 frames; 208 by the driver's own round trip (2026-11-11), which is the driver's figure since 1.0.0; from one start to the next the round trip moves between 718 and 727 frames, with 720 reported | Latencies reported to the host |
| Q13 | Does capture reach memory without SetAudioCaptureEnable? (ntv2llburn calls it; the design's start() does not) | start() sequence |
| Q14 | What do the other values of the Xpand's 4-bit analog select field do? The SDK only writes 0 or 1. Answered: nothing; only bit 0 counts | Possibly AES and analog together |
| Q15 | Where does the KONA X's audio mixer sit, does any output follow it, and what uses its audio system (the SDK counts it as a third buffered one)? | Output routing; default audio system |
| Q16 | Does GetRawAudioTimer run on the card's audio clock (the heads' clock)? | What the clock model is built on |
| Q17 | Does an SDI or HDMI output carry the driver's audio when no program feeds it video, and how much later than AES does it come out? | Whether the SDI and HDMI outputs need a video source; their latency |
| Q18 | What does a program ended by force leave on the card (the audio system running, the task mode, the outputs), and does restarting the PC clear it? All of that, and yes (2026-11-11). 0.1.21's takeover was seen on the card the same day, with `duplex-probe` and with the driver: no restart (CARD_SESSION.md, step 10). It left capture enable and the erase mode on; 1.0.0 puts the audio system's own settings back, seen on the card that evening | How the driver takes over an audio system a crashed host left |
| Q19 | When does AJA Input's last clip close the card while the driver plays (a change of deck? taking its sources out?), is a gap heard, and in which order does Resolume let the two go at exit? In which task mode do Resolume and vMix leave the card when they close (`card-session`'s `info` after each)? vMix: standard (2026-11-11). Do AJA's services ever write the outputs after the driver took the card out of standard mode, the 100 ms it waits notwithstanding? Not in 34 starts on 2026-11-11 (driver.log's "routing:" and "release:" lines, "put back" within two seconds of a start; a loop run that never hears its tones) | Whether the handover between the driver and AJA Input (0.1.19's mark) covers what Resolume does; what to do about vMix; whether the wait at start is long enough |
| Q20 | With "AJA Duplex ASIO" and "AJA Duplex Simulator" both registered, does vMix list the card's driver as an audio input? Not asked any more: since 0.1.20 only the card's driver is registered, as when vMix listed it on 2026-11-10 | Whether the name was it, or two drivers in one DLL |

## Design decisions

Found while building against the simulated card: choices to make, not
something the card can answer.

| # | Question | What was seen | Decided |
| --- | --- | --- | --- |
| D1 | After a late output, keep the latency or keep the samples? The design moved O to S ahead of the playback head again and sent kAsioResyncRequest. | In the simulated `loop`, a block 2 ms late left the round trip 2 ms longer until the next start: the output really was S + 2 ms behind while getLatencies still said S, so recordings would line up 2 ms off. The analog loop showed it too: the tone came back "96 frames late". | 2026-11-08: keep the latency. Since 0.0.7 the engine drops what the card passed and G frames after it, sends the rest on time, and leaves O on its timeline; an input overrun moves O as far as R. A late block costs samples (and, in the driver, an overload message to the host), never latency. |
| D2 | Make analog the driver's default input? The design lets the panel choose and names no default. | The Xpand's analog I/O is what the driver is for first. | 2026-11-08: analog is the default input (phase 1). |
| D3 | Default buffer and guard: the smallest the card holds (phase 0's loop), or what Resolume needs? | Resolume's callback, on the driver's thread, took up to 8 ms now and then on the simulated driver: at N = 128, G = 32 (3.3 ms a block) 6 late outputs in 32 min; at N = 256, G = 32 2 in 2 h 39 min; at N = 128, G = 256 (8 ms) none in 4 h 48 min. A larger G costs output latency only. | 2026-11-09: N = 128 and G = 256 by default (0.1.13), for Resolume. The round trip is then 512 frames (10.7 ms) plus the card's share, over the 10 ms target, which still holds for hosts with short callbacks with a smaller G. |

## Out of scope (first version)

- A kernel driver or an own WDM device.
- Sample rates other than 48 kHz (the KONA X does not do 96 kHz; the SDK
  has no 44.1 kHz).
- Several host programs at once: an ASIO driver has one client.
- Video, and cards other than the KONA X (the code reads each card's
  capabilities from the SDK, but only the KONA X is tested).
