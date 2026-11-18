# Changelog

All notable changes to AJA Duplex ASIO are documented here. The format is
loosely based on [Keep a Changelog](https://keepachangelog.com/). 1.0.0 is
the first version published. The ones before it were numbered by the
phases of [docs/ROADMAP.md](docs/ROADMAP.md): 0.0.x is phase 0 (the
measurement tool) and 0.1.x phase 1 (the driver).

## 1.0.1 - 2026-11-18

The settings, for hosts with no settings button of their own, and in
samples, as hosts and people say, where they said frames. Nothing in the
audio changes: it plays as 1.0.0 does.

### Added
- **AJA Duplex ASIO Settings** (`AJADuplexASIOSettings.exe`): the driver's
  settings without a host, for those with no ASIO settings button (vMix
  has none). It asks the installed driver for its own dialog the way a
  host does, through COM, without opening the card, so it can be open
  while a host has the driver. Its "Now" box says so, and which program
  has the driver open, if one has ("Open now in vMix64.exe (process
  8768)"): that one takes what is saved once it is closed and opened
  again, which for vMix is when it starts. After OK a message says the
  same. A second start brings the first one's dialog to the front.
  `install-driver.cmd` copies it beside the driver in Program Files and
  adds it to every user's Start menu; `uninstall-driver.cmd` takes both
  away, the entry also by name should the program be gone. The ZIP has
  it beside the DLL. Until now the settings of a host with no button
  were set with `reg add` or from another host.

### Changed
- The settings dialog speaks of samples, with what they are in
  milliseconds: the buffer sizes ("128 samples (2.7 ms)"), the guard,
  whose text follows what is typed in its box ("samples (5.3 ms) kept
  ahead beyond a block"), the card's latencies and their note, and the
  "Now" box ("latencies reported: in 128 samples (2.7 ms), out 592
  samples (12.3 ms)"). README.md and DRIVER.md say samples too. The log,
  `duplex-asiotest` and `duplex-probe` keep frames, ASIO's word for the
  same thing.
- The buffer size offered first says that a host may pick another: vMix
  always asks for 1024 samples (21.3 ms), whatever the driver offers, as
  every run on the card showed. DRIVER.md says so.
- The log's `panel:` line says when the driver was never opened (the
  settings program), instead of "on another thread".

Unit tests: 304 cases (323 under Wine). Built with GCC, Clang, MinGW and
Visual Studio; the settings program and both install scripts ran under
Wine (the dialog, its "Now" box and the Start menu entry).

## 1.0.0 - 2026-11-11

The first version published (https://github.com/fiverecords/AJA-ASIO-Duplex),
and so 1.0.0: phase 1's driver as it ran on the card, with the two things
its last session left open closed. Up to 0.1.22 the versions were
numbered by the roadmap's phases.

The fourth session with the card, with 0.1.22 the same day, passed phase
0's exit test on both paths (an hour at N = 128 with the driver's guard,
AES twice and analog once); step 5c and ten minutes of Resolume with AJA
Input came out clean; and the takeover of 0.1.21 worked on the card, with
`duplex-probe` and with the driver (docs/ROADMAP.md, "Found on the
card").

### Changed
- The latencies reported include the card's own share with nothing set.
  `OutputLatency`, while no value is saved, is the driver's figure for
  the KONA X with the Xpand, by input: 208 frames with the analog input
  (the driver's round trip on the card: 720 frames at the defaults), 130
  with AES (the two AES hours: 129.9 and 133.7 frames beyond the
  engine's own), and 0 with an SDI or HDMI input, which have not been
  measured. At the defaults the driver reports 128 frames in and 592
  out. A saved value stands, whatever the input: a PC where
  `card-session`'s step 7 wrote 208 reports what it did. In the settings
  dialog the output latency box follows the input while it holds one of
  those figures, and keeps one typed in. Up to 0.1.22 the share was 0
  until step 7 had measured the round trip on each PC.
- A takeover after a host closed by force puts the audio system's own
  settings back as that host had found them. The mark that says which
  program has an audio system in hand now carries them (channel count,
  rate, buffer size, loopback, capture enable, delays and the source
  select register, which holds the erase mode), and the next program to
  open the audio system writes them back once it has stopped it, as the
  release that never happened would have: with 0.1.22 on the card,
  capture enable and the erase mode stayed on. They are written only
  while the audio system is still as the driver sets it (erase mode on).
  The log's `card:` line says so ("...; stopped; the audio system's own
  settings back as it had found them; the task mode back to standard, as
  it had found it"). A mark of 0.1.21 or 0.1.22 is still read, and leaves
  the settings as they are. What the card shares (the AES outputs, the
  monitor, the Xpand's select and level) is AJA's services' to put back
  in standard task mode, as before.
- `card-session`: step 7 starts from what the driver reports with nothing
  saved (208 frames with the analog input), and leaves `OutputLatency`
  alone when the round trip is within 16 frames of it, two steps of the
  card's heads: from one start to the next the round trip moves by a few
  frames (718 to 727 in six starts on the KONA X, with 720 reported), and
  a run that comes out 5 frames long says nothing about the next. Only
  further off does it offer to set `OutputLatency`, as it used to at any
  difference. Its result goes to `notes.txt`. Step 10 looks at the card
  before the forced close and
  says after each takeover whether audio system 2's own settings are as
  they were, and which are not; and it says when capture enable and the
  erase mode are on before it starts, left by a takeover of an earlier
  version: only after a restart of the PC, which clears them, does the
  step show that this version puts them back.
- The log's start line names the card's share ("latencies reported: in
  128, out 592 frames (the card's and its converters' share: 0 in, 208
  out)").

### Added
- The settings dialog says which version it is, when it is from and whose
  ("AJA Duplex ASIO 1.0.0, 2026-11-11, by fiverecords"), with a button
  that opens the project's page, github.com/fiverecords/AJA-ASIO-Duplex.
  The address, the date and the name come from CMakeLists.txt.
- README.md is the repository's front page: what the driver does, where
  it stands, installing, building, reporting a problem. CARD_SESSION.md
  is the list of what was run on the card, for running it again: the
  reports and the log are what an issue needs.

Unit tests: 303 cases (319 under Wine). Built with Visual Studio 2022 and
run on the KONA X the same evening (docs/ROADMAP.md, the fifth session):
two minutes in Resolume and a start in vMix without a late output; the
driver reporting 720 frames at the defaults, with round trips of 718 to
727; and both takeovers of step 10, after `duplex-probe` and after the
driver ended by force, leaving audio system 2 as it was before, with no
restart. That session is where step 7's offer at any difference was seen
to chase the spread between starts, and why it now has a tolerance; that
last change has been checked for syntax only.

## 0.1.22 - 2026-11-11

After the third session Resolume listed the driver greyed out, with no
ASIO settings button, and vMix did not list it at all. The cause: with
the DLL gone from the folder it had been installed from, the driver
goes. `install-driver.cmd` registered the DLL where it was, so
hosts were left with a name in their list and no file to load; installing
again put it right. The driver's own code is as in 0.1.21: what is run on
the card with that version counts for this one.

### Changed
- `install-driver.cmd` copies the driver to
  `Program Files\AJA Duplex ASIO` (with the licence files) and registers
  that copy. Hosts load it from there: the folder of the ZIP can be
  moved, replaced by a newer one or deleted, and a registration left by an
  earlier version where its folder was is moved to the copy. Run it again
  to update the driver, with every host closed: Windows does not let a
  DLL in use be replaced, and the script says so when a host has it open
  (unless what is installed is that very file already, which leaves
  nothing to replace).
- `uninstall-driver.cmd` takes away the registration and the copy in
  Program Files, from whichever folder it is run, and the registration
  also when no DLL is left anywhere to ask (it deletes the driver's two
  registry keys by name). With a host still open it unregisters, leaves
  the file and says so.
- `build.cmd` no longer makes the folder `dist\AJA-Duplex-ASIO-<version>-win64`
  anew: it stages the ZIP's files apart and copies them over that
  folder, so what a session with the card left in it (`reports\`, its
  ZIPs) stays. Up to 0.1.21 a rebuild took those with it when the session
  had been run from that folder.

### Added
- `duplex-asiotest --installed`: one line on what hosts find of the
  driver on this PC, and an exit code (0: hosts load this build). It
  reads the driver's key in the hosts' list and the DLL its class is
  registered with, and compares that file with the DLL beside the
  program: `(this build)`; `ANOTHER BUILD than the DLL beside this
  program (written <date>): hosts load that one until install-driver.cmd
  runs again`; `no: registered at <path>, and that file is not there:
  hosts list the driver and cannot open it`, which is what had happened;
  or `no: hosts do not list AJA Duplex ASIO`. With a copy installed there
  are two DLLs on a PC that builds, and a new build is not what hosts run
  until it is installed: `build.cmd` ends with that line, and
  `card-session` says it when it starts, after step 6 and before the
  steps in which a host loads the driver (6-vmix and 8), and keeps it in
  `notes.txt`.
- DRIVER.md and CARD_SESSION.md say what a driver greyed out in a host,
  or missing from its list, means: not installed as hosts need it, or the
  card's audio system in use by another program (the log's `init refused`
  or `init failed` line says which).

Unit tests: 301 cases (315 under Wine). The two install scripts ran end
to end under Wine's cmd with the MinGW build of the DLL: installing, the
folder of the ZIP gone and the driver still created through COM,
updating, a host holding the installed DLL (the same build goes through;
another is refused and what was installed stays; uninstalling unregisters
and leaves the file), the installed file deleted by hand, a registration
left in place by an earlier version, and uninstalling with no DLL left.
Wine's cmd is not Microsoft's: the scripts have not run on Windows yet,
and the changes to `build.ps1` and `card-session.ps1` have not run
anywhere (checked for syntax only).

## 0.1.21 - 2026-11-11

The third session with the card, with 0.1.20: everything but the one-hour
loops, Resolume for hours and the SDI loop. Both sweeps clean, the driver
in vMix both ways, its round trip measured (720 frames, 208 of them the
card's), the forced close as expected and cleared by a restart
(docs/ROADMAP.md, "Found on the card"). This is what those results asked
for.

### Added
- After a host closed by force, the next program takes the card over. A
  program ended while it played (the Task Manager, a crash) left audio
  system 2 running, the card in OEM task mode and the AES outputs on that
  audio system, and the driver refused it as in use until the PC was
  restarted (Q18, seen on 2026-11-11). Now the driver and `duplex-probe`
  keep a mark of which program has an audio system in hand, from their
  first change to the card until all is back: its process and when it
  started, its name, and the task mode it found, as a value of a volatile
  registry key (`HKCU\Software\AJA Duplex ASIO\Running`), which outlives
  the program and is gone with a restart. On open, an audio system that
  runs with the mark of a process that is gone, and runs as the driver
  runs it (the playback erased behind its head), is stopped and taken;
  the task mode that program found is put back if the card is still in
  OEM mode and no program holds it; and the log's `card:` lines, or the
  report's `device`, say so ("taken over: Arena.exe (process ...) left
  audio system 2 running and is gone"). One that runs without such a
  mark is another program's and is refused as before. Not tried on the
  card yet: `card-session`'s step 10 is its test.
- The engine places its output anew when every block has been late for a
  quarter of a second by more than its own time accounts for. On the
  card, while the AJA Control Panel changed the input's source, the
  capture side lost frames, the playback head went on, and 39848 of
  44161 blocks came late, with nothing to bring the output back: the two
  heads were out of step by less than the 1024 frames at which the engine
  acted. On the simulated card, an input held back 400 frames had every
  block late for good; now 93, and the round trip is what it was. With
  an analog or AES input and OEM task mode nothing does this; an SDI or
  HDMI input that loses its signal may.
- The log's stop line and the loop's report say how far the playback
  head was from where the input's timing put it ("the output within -8
  to +8 frames of the input's timing"), which is where an offset that
  makes no block late shows; `watch`'s report counts the output moves.

### Changed
- The long runs take the driver's own guard, 256. `card-session` gave the
  one-hour loops and step 5c the smallest guard that came out clean in
  the sweep, which on 2026-11-11 was G = 8: step 5c, with AJA Input
  capturing, then failed four blocks in ten minutes on an audio DMA that
  waited 2.8 ms for video, which the driver's guard holds with over 200
  frames to spare. Runs of ten seconds say which guards fail, not which
  one holds for an hour (some clean ones had a frame to spare). The
  loop's verdict prints the hour at `--guard 256`, and the script asks
  with that as its default.
- `card-session`'s step 10 ends a program by force twice, `duplex-probe`
  in a loop and the driver in `duplex-asiotest`, has the next one take
  the card over each time, and says whether the card was left clean; the
  PC is restarted, and `10-restart` run, only if it was not.
- At release, a program that reserved the card and ended without letting
  it go holds it no longer: the card still names it, and up to 0.1.20
  that kept the task mode as it was ("the task mode left as it is: ...
  holds the card") with nobody there.

### Fixed
- DRIVER.md and ROADMAP.md said the DMA that failed every lap in 0.1.13
  to 0.1.15 failed because it ended at the ring's end. `cost` passes such
  a DMA; what it finds, in two sessions, is that a DMA of less than 4096
  bytes fails from some buffers and one of a page or more goes through,
  and the piece that failed was 2560 bytes. The cure of 0.1.16 stands
  (the block whole), for that reason.

Unit tests: 294 cases (308 under Wine).

## 0.1.20 - 2026-11-11

No test is left that does not need the card, so the simulated driver
goes and only the real one stays; if this version does well on the card,
it may be the one that stays.

### Removed
- The second driver in the hosts' lists, "AJA Duplex Simulator" (up to
  0.1.16 "AJA Duplex ASIO (simulated)"), which ran the driver on a
  simulated card so that it could be tried in a host without the KONA X.
  `install-driver.cmd` now registers "AJA Duplex ASIO" alone, and takes
  away what versions up to 0.1.19 registered for the other: its class,
  and its key under `HKLM\SOFTWARE\ASIO` under either name, if that key
  still names this driver's class. `uninstall-driver.cmd` takes those
  away too. That also puts an end to Q20: vMix left the card's driver
  out of its audio inputs while the simulated one was registered beside
  it, and listed it with that key deleted, which is how the registry is
  now; whether it was the names or two drivers in one DLL is no longer
  asked.

### Fixed
- `install-driver.cmd` went out with LF line endings from 0.1.17 to
  0.1.19: an edit made on Linux dropped its CRLF (`.gitattributes` asks
  for it, but the file had not gone through git). cmd.exe runs such a file,
  but it can miss a `goto` label in one, and the script's two labels are
  its error messages ("AJADuplexASIO.dll is not in ...", and regsvr32's
  exit code): installing worked, saying why it had not might not have.
  Both install scripts end their lines in CRLF again, and the checks
  before a version goes out now look at that.

### Changed
- The simulated card stays inside the DLL, behind a class that nothing
  registers and no host can list: `duplex-asiotest` asks the DLL for it
  itself, which is how `build.cmd` tries every build whole, as a host
  would, on a PC without the card, and how `card-session -Simulate`
  tries the script. `duplex-asiotest --registered` goes with `--card`
  only, and says so.
- `card-session`: step 6 says that installing takes the simulated driver
  away, lists what is registered afterwards (and says so if it is
  anything but "AJA Duplex ASIO"), and keeps it in `reports\notes.txt`;
  step 6-vmix no longer has the `reg delete` way round.
- DRIVER.md, CARD_SESSION.md, README and the README packed with the
  programs: one driver.
- ROADMAP.md, "Before a first release": what would still stand between a
  version that does well on the card and a release (the session's exit
  tests, the hardware latencies as the driver's own figures, a host
  ended by force, what DRIVER.md lists as not there yet, an installer and
  the version number, publishing).
- Unit tests: 285 cases (297 under Wine), as in 0.1.19. The registration
  was tried under Wine with `regsvr32` on the MinGW build, with what
  0.1.19 and 0.1.16 registered put there first.

## 0.1.19 - 2026-11-11

After 0.1.18: the change to AJA Input's Close that the review had left
as AJA Input's to make (Q19), and every test of the session in
`card-session`. The first needed the driver to say that it is there.

### Added
- The task mode mark: while the driver answers for a card's task mode
  (from the host's first start with OEM task mode on until the driver has
  let the card go), the named mutex `Local\AJADuplexASIO-card<index>-oem`
  exists, <index> being the card's index in the SDK's scan. The driver
  does not reserve the card, so until now another program that changes
  the card's task mode could not tell that it was there. AJA Input, with
  the change to its Close made alongside this version (in its own
  repository, after 0.43), asks for the mutex: while it is there it
  leaves the task mode alone at its last Close, instead of putting
  standard back under the driver (a second on AJA's mixer); and an OEM
  mode it found with the mutex there is the driver's, so it puts
  standard, not that OEM mode, once the driver has gone, instead of
  leaving the card in OEM mode with nothing running. DRIVER.md, "the task
  mode mark". The simulated driver sets none.
- `card-session`: every test of the session is a step. 6 runs
  `install-driver.cmd` as administrator (Windows asks) and lists the
  drivers registered; 6-vmix is the vMix check (Q20), with an `info`
  once vMix is closed; 8 asks for what to try with AJA Input while
  Resolume plays (Q19) and ends with an `info` once Resolume is closed;
  10 is the forced close (Q18), done by the script: it starts a loop,
  ends `duplex-probe` by force ten seconds later, runs an `info` and a
  loop, and stops for the restart; 10-restart, after it, runs the last
  `info`. After each of those `info` runs it says in which task mode the
  card was left and which audio systems still run, and keeps that in
  `reports\notes.txt` with the answers given at the steps.
- `DriverOptions::taskModeHeld` (what the core tells its owner, which on
  Windows keeps the mutex) and `TaskModeMark` (win/InstanceLock.h).
- Unit tests: 285 cases (297 under Wine).

### Changed
- CARD_SESSION.md follows the script: vMix has its own heading, step 8
  ends with `info --label after-resolume`, and step 10 is the last one,
  with `--guard 64` so that the loop to end is a single run.

## 0.1.18 - 2026-11-10

A review of the driver's code with fresh eyes before the third session
with the card, against the SDK's sources, AJA's Linux driver, AJA Input's
and the second session's logs.
Four faults, fixed; what a forced close leaves on the card, made harder to
come by; the rest is in ROADMAP.md, "Found by reading", with the three
questions it leaves for the card (Q18-Q20).

### Fixed
- The engine believed its clock model whatever it said, and a model gone
  stale could hold every block back, more each second, for good. The
  model says when the next block is due; it is fed only by two looks at
  the card's head within 100 us of each other, which on Windows hardly
  happens (the timer wakes about 0.5 ms late), so a fit made early in a
  run is not corrected. When the card's clock had since moved away from
  it, the engine slept past each block by what the model was off, with no
  polling left to notice: on the simulated card with the KONA X's costs
  (a DMA 110 us, heads in steps of 8 frames), a model 500 ppm off had
  every block late after 20 s, 4037 late outputs in 40 s. Now the reading
  just taken says when the block is due at the latest, and the model is
  used only while it agrees with it: not later by more than two polls,
  nor earlier by more than 2 ms. One that disagrees is dropped (the stop
  line's card clock then reads +0.0 ppm), and the next readings start
  another. Where the model was right nothing changes: the simulated runs
  give the same numbers as before.
- With OEM task mode on, the outputs stayed on AJA's mixer after another
  program had the card in standard mode for a moment. AJA Input puts back
  the mode it found when its last instance closes (standard, if nothing
  had the card in OEM mode before it); AJA's services then move AES out
  1-8 and the monitor to the mixer within a second (Q9). The driver took
  OEM mode back at its next look and left its outputs there, silent until
  the host started it again, unless Keep my routing was on. The look that
  finds the task mode changed now puts back everything of the driver's
  that is not as it set it, and so does the next (for what the services
  wrote while the mode was going back), with or without Keep my routing.
  A change with the task mode untouched is another program's own, as
  before: reported, and put back only with Keep my routing.
- A start that took the card out of standard task mode could leave the
  outputs on AJA's mixer, with nothing to say so. The task mode and the
  routing were written in the same instant: AJA's services, in the middle
  of a pass just then, could write AES out 1-8 and the monitor once more
  after the driver had, and what the driver read back 10 ms later it took
  as its own. Not seen on the card so far: with Resolume the card is in
  OEM mode before the driver starts, and the three starts from standard
  mode in the second session (two loops and `routing`) went well. But
  vMix, `duplex-asiotest` and every `duplex-probe loop` start from it.
  Now the task mode is set first, and when that takes the card out of
  standard mode nothing else is written for 100 ms, until such a pass is
  over (`duplex-probe` too: it is in the card's set-up); and the driver's
  first two looks after such a start put back whatever is not as it set
  it, with or without Keep my routing, as after a task mode found changed.
- Letting the card go, the driver put back the task mode it had found
  even when that was the OEM mode of a program that held the card then
  (AJA Input, say) and holds it no longer: the card stayed in OEM
  mode with nothing running and AJA's services off. It puts standard mode
  then, and the log says so ("release: the task mode put to standard, not
  back to OEM: AJIN, process ..., which held the card at the start, holds
  it no longer").

### Changed
- `duplex-probe`: closing the console window (or logging off) stops the
  command as Ctrl+C does, with the five seconds Windows allows to put the
  card back. Until now only Ctrl+C did; a closed window left the audio
  system running and the card in OEM task mode, and the next run found
  the audio system in use.
- `duplex-asiotest`: Ctrl+C, or closing its window, ends the run early
  with the driver stopped and let go (it used to end the program in the
  middle, the card left as it was). With `--card` the round trip holds if
  the clicks come back within two frames of each other: converters spread
  a click over a few frames, and which of them crosses the threshold first
  may differ by one. On the simulated card they must match exactly, as
  before.
- The "audio system N is in use" message says what to do when no program
  is using it: one that was closed by force left it running, and a restart
  of the PC stops it.
- CARD_SESSION.md: step 10, what a forced close leaves on the card (Q18);
  in step 8, what to try for Q19; under "If something fails", the audio
  system in use with nobody using it, the card left in OEM mode, and vMix
  leaving the card's driver out of its inputs again (Q20). DRIVER.md:
  "When another program takes the card out of OEM mode".

### Added
- The simulated card: another program holding it when it is opened
  (`FakeCardModel::heldAtStart`).
- Unit tests: 284 cases (294 under Wine).

## 0.1.17 - 2026-11-10

The second session's reports, with 0.1.16: no DMA error in Resolume, in
the 32 loop runs or in vMix, so the click is gone; Q4, Q5, Q9, Q12 and Q14
answered. Both loops failed every run for reasons of their own: the
KONA X's AES inputs resample, so no word came back as sent, and the analog
loop called the delay's drift (about 2 us in 10 s) a slip. And vMix
offered the card's driver as an output but never as an input. What the
card said is in ROADMAP.md, "Found on the card".

### Changed
- OEM task mode while playing is on by default: in standard task mode
  AJA's services put the AES outputs and the monitor back on the audio
  mixer within a second (Q9). The value is `OemTaskMode` now, default 1:
  an `OemTasks` saved by an earlier version (off by default then) is not
  read.
- The simulated driver is "AJA Duplex Simulator". As "AJA Duplex ASIO
  (simulated)", whose name begins with the card's, vMix left "AJA Duplex
  ASIO" out of its audio inputs (with that key deleted, vMix listed it).
  Registering the DLL takes the old key away, if it is still ours. The
  card's driver keeps its name.
- `duplex-probe loop` checks AES with tones, as the analog loop does: 997
  Hz on AES 1 up to 4507 Hz on AES 8, at -40 dBFS (the XLR outputs play
  AES 1-2's too, Q4), with the round trip from a sweep. The word-by-word
  check stays, as `--path aes-words`, for inputs that do not resample;
  when its words come back changed, its report says so in one line
  instead of counting garbage. Tone runs need 3 seconds or more. The
  AES tone path's CSV has a line per run with the worst of its channels.
- The tone check (ToneLoop.h, which was AnalogLoop.h) holds each second
  against the last still one moved on by the drift so far; a second with
  a glitch in it is not measured; after a glitch, a move of more than a
  fortieth of a frame is a slip, without one more than a quarter, and
  never less than eight times the median wander, so a jittery delay is no
  slip either. Each channel's report line says how far the delay drifted
  and how much it wandered, and the analog CSV has `drift_l_us`,
  `drift_r_us`, `wander_l_us` and `wander_r_us`. A slip's size is said
  only when the delay was steady enough to tell it (0.1.16 printed sizes
  like 16363 frames). The noise is the median second's, the worst one
  apart (the first second through the Xpand was noisy while its input
  settled).
- `duplex-probe routing`: an AES input that hears its own output's tone,
  loudest, without being bit-exact is its own output resampled (`A~`),
  so Q5 is answered with AES 3-8 cabled; and Q14, which the tones alone
  cannot answer when the AES inputs resample (AES in 1-2 and the analog
  input both hear outputs 1-2), is told by the analog path's gain.
- `duplex-asiotest` loads the DLL with its own folder searched for what it
  needs, as COM does; when it cannot, it says the path it tried, whether
  the file is there, and what errors 126 and 193 mean.

### Added
- At each start the engine sends every buffer and spare through a 512-byte
  DMA each way at the middle of the rings (in 0.1.16's `cost` one buffer
  failed every DMA of 512 to 2624 bytes and passed 8 KB ones). One that
  fails is swapped for a spare that passes and kept aside, not freed; the
  driver's log says "buffers: ..." only then, and so does the loop report.
- `duplex-probe cost` shows its two check buffers with the smallest size
  and the largest; its 128 fresh buffers take a write and a read each of
  512 bytes and of 8 KB; a buffer that fails says which sizes, 64 bytes to
  8 KB, go through it, and whether it lies where a buffer was locked
  earlier in the run.
- The simulated card: AES inputs that resample (a model option: a
  fraction of a frame more delay, interpolated, with noise), and a bad
  buffer that fails only DMAs up to a size.
- Unit tests: 275 cases (285 under Wine).

### Fixed
- `duplex-probe`'s signed numbers: a value that rounds to zero reads
  +0.00, not "+-0.00" (a delay that never moved) or "-0.00".

## 0.1.16 - 2026-11-10

The first reports from the card (phase 0, steps 1 to 3) and 0.1.15 in
Resolume. The DMA error once a lap was back, and 0.1.15's log named it:
the piece of the output block that ended right at the ring's end, 2560
bytes from a plain buffer, Windows saying error 31 (ERROR_GEN_FAILURE).
0.1.14's 71 s without one were luck of where the blocks fell. The AES
sweep failed in every run, mostly without an engine event: the card was
in standard task mode, and AJA's services had the AES outputs back on
the audio mixer. What the card said is in ROADMAP.md, "Found on the card".

### Changed
- A block across the ring's end goes in one DMA from where it starts,
  running on past the end into the 16 KB the card never plays or fills
  (each ring has 4 MB and wraps at 4,177,920 bytes), and what wraps goes
  again at the ring's start from a buffer of its own: no DMA of a piece
  stops at the ring's end any more. Input blocks across the end (N = 512
  and above, or after an overrun) the same way, what comes from past the
  end thrown away. Every DMA still starts at the start of a buffer. On a
  card without room past the end, the two pieces as before. To confirm on
  the card: the click should be gone.
- A DMA that fails is tried again at once from the same buffer (a passing
  fault goes through then) and, if that fails too, from one of four spare
  buffers (a bad buffer's DMA goes through then): either way nothing is
  lost, and the log says "DMA error, recovered" with how each try went. A
  spare that saves a DMA takes the failed buffer's place for good; at
  stop the DMA is tried once more from the failed buffer, with the card
  stopped, and the log says whether the buffer is bad. After three blocks
  in a row whose second tries all failed, no more second tries (each costs
  a DMA's time, about 0.1 ms). The stop line counts "DMA errors" (audio
  lost) and "recovered".
- The engine locks no buffer (DMABufferLock): on the card a locked buffer
  was no faster, 110 us a DMA either way.
- `duplex-probe loop` and `routing` hold the card in OEM task mode while
  they run, as Resolume's AJA plugin does, and put the mode back after;
  `--tasks keep` leaves it as it is. The report says which.

### Added
- `duplex-probe cost` checks which DMAs go through at all, with the audio
  system stopped and then running: writes and reads of 512, 2560 and 8192
  bytes ending at the ring's end, running 64 bytes past it, across it, in
  the room past it, at the room's end and at the ring's start, and for
  comparison one frame before the end and mid-ring; 128 fresh buffers with
  a write and a read each; and 32 plain buffers made where a locked one
  had just been unlocked. `[DMA checks, in short]` on the console says what
  failed; the file has the tables.
- The loop report says, for each run, where the blocks met the ring's end,
  each DMA error's DMA and second tries, and any buffer a spare replaced;
  the CSV has a `dma_recovered` column.
- `duplex-probe` will not open an audio system the driver has open (in
  Resolume, say), and says so: "AJA Duplex ASIO is open in <program> on
  this card and audio system: close it first". It takes the driver's own
  lock, so the driver refuses it the same way while the probe runs.
- The simulated card has the room past the ring's end (or not, as a model
  option), a bad host buffer as a fault, and a record of every DMA for the
  tests.
- Unit tests: 258 cases.

## 0.1.15 - 2026-11-10

0.1.14 on the card: 71 s in Resolume, no DMA error, no click. Now the
session with the card can run as one guided script.

### Added
- tools/session/card-session.cmd and card-session.ps1, beside the programs
  in the ZIP: the session of CARD_SESSION.md step by step. Before each
  step it says what to cable ("CONNECT NOW" for what the steps before it
  have not asked for) and what to set up, and runs it on Enter (S skips,
  Q stops). The exit tests and the loop with video start from the G the
  sweep's report gives at N = 128 (Enter keeps it, or type another). Step
  7 measures the driver's round trip with `duplex-asiotest --card`, offers
  to set OutputLatency from it and measures again. Steps 8 and 9 keep a
  line of what was seen in reports\notes.txt, and the last step zips the
  reports and the driver's log into session-<date>.zip. `card-session
  5c` goes on from a step (double-clicked, it asks where to start);
  `card-session -Simulate` runs what can run on the simulated card, with
  short runs, to try it out.

## 0.1.14 - 2026-11-10

The first session with the card. In Resolume the driver opened the KONA X
(AJA's driver 18.0.0.29, firmware revision 8) and ran 17 minutes at the
defaults with no late output, overrun or stall, its DMAs at most 0.9 ms
and Resolume's callback at most 1 ms; but with one DMA error every 1.36 s,
753 in all.

### Fixed
- One DMA error a lap of the ring (65,280 frames, 1.36 s): the output
  block that the ring's end splits in two, its DMAs each of part of a
  locked buffer, which AJA's driver finds by the address it was locked
  at. Each was a gap of up to a block on the outputs. A DMA to or from a
  locked buffer is now always the whole block, from its start; part of a
  block (the two pieces of one split at the ring's end, or what is left of
  a late one) goes through plain buffers of its own, one a piece, as AJA's
  demos move audio. At N = 512 and above the ring's end splits input
  blocks too, which would have failed the same way; and every `duplex-probe
  loop` longer than 1.36 s would have failed on the card. The simulated
  card now fails a DMA of part of a locked buffer, as the card did, and
  the tests cover the output's split, the input's (N = 512) and a late
  block's rest.

### Added
- The log's "DMA error" line says which DMA: input or output, the whole
  block or which piece of it, its size and ring offset, from its locked
  buffer or through a plain one, and what the system said of it
  (GetLastError on Windows; 0 when the SDK refused it before asking the
  card's driver).
- Unit tests: 244 cases; 254 on Windows.

## 0.1.13 - 2026-11-09

4 h 48 min in Resolume on the simulated driver at N = 128 and G = 256:
no late output, overrun or stall; the longest host callback 7.3 ms, inside
the 8 ms each block had. Those are now the defaults, and the ZIP carries
the list for the first session with the card.

### Changed
- `Guard` is 256 frames by default (it was 32): with the buffer's 128, a
  block has 8 ms from complete to written (S = 384), against Resolume's
  callbacks of up to 8 ms, and the input latency stays 128 frames. The
  round trip goes from 288 to 512 frames plus the card's share (6.0 to
  10.7 ms); a host with short callbacks can take the guard down (32 gives
  6 ms). A Guard already saved in the registry is kept. A late block
  loses what the card passed and G frames after it, as before: with G at
  least N, as now, the whole block.
- The ZIP's README.txt and DRIVER.md suggest `C:\AJA-Duplex-ASIO` for the
  folder: `duplex-probe` writes its reports beside it, which under Program
  Files would need administrator rights.

### Added
- docs/CARD_SESSION.md, also in the ZIP: the session with the card in
  one list, from the cables to what to keep of it: phase 0's commands in
  order, the loop with AJA Input capturing (Q3), the driver's round trip
  with `duplex-asiotest --card` and the latency it gives, Resolume at the
  defaults, the SDI loop (Q17), and what to do if something fails.
- The driver's log names the versions on its "card:" line: AJA's driver,
  the card's firmware revision and the SDK.
- docs/ROADMAP.md: D3, the defaults chosen for Resolume.

### Fixed
- With the card's audio system in use by another program, the driver
  said to pick another "with --audio-system", `duplex-probe`'s option; it
  now says "in the driver's settings (Audio system)".

## 0.1.12 - 2026-11-09

2 h 39 min in Resolume on the simulated driver at 256 frames: 2 late
outputs. One was Resolume's callback (6.38 ms against the 6 ms a block
had); in the other the driver found the block 3.62 ms after it was
complete: Windows had not run the audio thread in time.

### Added
- The audio thread asks Windows 11 not to throttle it (EcoQoS, which runs
  the threads of a program it takes to be in the background slower and on
  efficiency cores), and the log says at each start what the thread got
  ("thread: MMCSS Pro Audio, critical priority; power throttling off").
- DRIVER.md: buffer or guard for a host that sometimes takes long (a
  larger guard gives each block more time without a longer input
  latency).

## 0.1.11 - 2026-11-09

Half an hour in Resolume on the simulated driver at 128 frames: 6 late
outputs, and the longest host callback of the run was 7.9 ms (18 ms in an
earlier run) against the 3.3 ms a block has. The driver's own parts stayed
small (DMA under 0.5 ms).

### Added
- Each late output in the log says where the time went: "late output at
  92.744 s (...): 4.75 ms late; a block has 3.33 ms from complete to
  written, this one was found after 0.10 ms, its DMA in took 0.02 ms and
  the host 7.90 ms". Found late is the driver (or the PC waking it); the
  host is the host's callback, which a larger buffer or guard absorbs.
- `AudioEngine::lastLateOutput()`.
- Unit tests: 239 cases; 249 on Windows.

## 0.1.10 - 2026-11-09

### Fixed
- A buffer size chosen in the settings reached Resolume a change late
  (choose 128, it took the 64 chosen before). The driver asks the host to
  reset, as a buffer change needs, but kept offering the old size until it
  was opened again, and Resolume asks the driver it still has open for the
  sizes before opening it again: its log showed init with 128, then the
  buffers made with 64. The new size is now offered at once ("buffer size
  offered first: 256 frames from now on" in the log); what runs keeps its
  size until the host makes the buffers again.
- Unit tests: 239 cases; 249 on Windows.

## 0.1.9 - 2026-11-09

First tries in a host (Resolume, the simulated driver): it loads and runs.

### Fixed
- The settings dialog, opened from the host's ASIO settings, came up
  behind Resolume with no taskbar button, so it looked as if nothing had
  opened, and asking again opened a second one. Now only one is open at a
  time (asking again brings it to the front), it comes to the front when
  it opens and has a taskbar button, and it goes over the window the user
  clicked in when that window belongs to the thread that asks; otherwise
  over the window init was given, if that thread's, or on its own. Never
  over another thread's window: that ties the two threads together and
  could hang the host. The log says which, and whether the host asked from
  the thread that called init ("panel: ..."). The message boxes after
  saving come to the front too.
- `install-driver.cmd` and `uninstall-driver.cmd` run from the source tree
  (`tools\install`, with no DLL beside them) only said that registering
  failed. They now say that the DLL is not there and where to run them
  from: the folder where the ZIP that `build.cmd` makes is unpacked. When
  regsvr32 fails they give its exit code, and a folder whose path has a
  parenthesis in it ("Program Files (x86)") no longer breaks them.
- README and DRIVER.md say where to run the installer from, and how to
  update the driver (close the hosts, unzip over, run it again).
- Unit tests: 238 cases; 248 on Windows (the dialog: one at a time, on the
  taskbar, never over another thread's window).

## 0.1.8 - 2026-11-09

The first build with MSVC (0.1.7, Visual Studio 2022 Build Tools, MSVC
19.44) passed everything: 245 unit tests and the driver test on the
simulated card. Two things it showed are fixed here.

### Fixed
- MSVC's warning C4324 on the log queue, whose two counters are padded to
  cache lines of their own on purpose: it is turned off for that class.
- `build-log.txt` and the build window turned every accented letter into
  two wrong characters on a Windows that is not in English: MSBuild prints
  in UTF-8 there, and build.ps1 read it in the console's old code page. It
  now reads programs' output as UTF-8, and puts the console's code page
  back when it ends.

## 0.1.7 - 2026-11-09

The SDI and HDMI outputs the design lists, off by default.

### Added
- The SDI outputs can play the driver's outputs too (a setting,
  `SdiOutputs`, a bit per output, and boxes in the dialog; none by
  default, since their video may be another program's): each one embeds
  all 16 channels of the driver's audio system (SetSDIOutputAudioSystem,
  SetSDIOutputAudioEnabled) into whatever video it carries, which the
  driver never sets. With one on, the driver has 16 outputs: 1-8 say
  "+ SDI" ("AES 1 + Analog L + SDI"), and 9-16, "SDI 9" to "SDI 16", only
  exist then.
- The HDMI output can play outputs 1-8 too (`HdmiOutput`, a box in the
  dialog, off by default; SetHDMIOutAudioSource8Channel): their names say
  "+ HDMI".
- Both are part of the driver's routing while they are on: read back once
  a second, reported when another program changes them, put back with Keep
  my routing, and put back as they were found when the driver lets the
  card go, unless another program set them since. Outputs the driver does
  not play to are not read at all. Changing either asks the host for a
  reset. An output the card does not have is refused at init.
- The KONA X's SDI connectors go both ways: SDI n is an input or an
  output, as the AJA Control Panel set it. The driver reads which (it
  never sets it) and says so in the log and in the dialog's box when the
  settings cannot work that way ("SDI 2 is set as an input, so the
  driver's audio does not go out on it: set it as an output in the AJA
  Control Panel"; the same for an SDI input set as an output, and for one
  connector chosen as both).
- The simulated card: the SDI and HDMI outputs' audio, which way each SDI
  connector goes (none has a signal the wrong way), and an SDI loop cable
  that brings an SDI output's 16 channels back on an SDI input. The
  driver's simulated card is cabled as the settings want it.
- `CardAudio::sdiSends()`; `CardRouting` has the SDI outputs (up to 8) and
  the HDMI output; `AudioConfig` has `sdiOutputs` and `hdmiOutput`.
- Unit tests: 238 cases (names and count of the outputs, the loop through
  SDI with every channel in its place, another program taking an output,
  restore, refusals, the connectors' way, the simulated card's cabling);
  245 on Windows.

## 0.1.6 - 2026-11-08

OEM task mode, as the design has it, as an option until Q9 is answered.

### Added
- OEM task mode while playing (a setting, `OemTasks`, and a box in the
  dialog; off by default): at each start the card goes to OEM task mode,
  so AJA's services stop applying the AJA Control Panel's settings over
  the driver's; the log says what it was. It is reasserted if anyone
  changes it, whether Keep my routing is on or not, and when the driver
  lets the card go it goes back to what it was, unless another program
  holds the card by then (it reserved it, as AJA Input does: the plugin is
  not left to the services). Turning it on or off asks the host for a
  reset. Whether it is needed at all is what `duplex-probe watch` finds
  out (Q9).
- When the driver lets the card go, the log says what it left as it was
  and why ("release: AES out 5-8 left as another program set them", "the
  task mode left as it is: AJIN, process 4242, holds the card").
- `CardAudio::writeRouting` can set the task mode, `AudioConfig.taskMode`
  sets it at configure, and `restoreNote()` says what restore left;
  `RoutingWatch` has parts it always puts back. The simulated card can be
  held by another program.
- Unit tests: 227 cases; 233 on Windows.

## 0.1.5 - 2026-11-08

The embedded inputs the design lists for the settings.

### Added
- Two more inputs: an SDI input's embedded audio (16 channels, "SDI 1 ch
  1" to "SDI 1 ch 16"; `Input` 2 and `SdiInput` 1 or 2 on the KONA X) and
  the HDMI input's (8 channels, "HDMI ch 1" to "HDMI ch 8"; `Input` 3), in
  the dialog's input list too. Their audio clock is the video input's, as
  the SDK sets it, and the clock source says so ("Video input: SDI 1"). An
  input the card does not have is refused at init, saying how many it has.
  With an embedded input the Xpand's analog/AES select is left as it was
  (its inputs are not the driver's then): only reported if it changes,
  never kept. If the audio clock stops with the video signal (Q7), the
  engine's stall path already gives the host silence and then a resync.
- The simulated card has the KONA X's video connectors (2 SDI in, 1 HDMI
  in, 2 SDI out, 1 HDMI out; `CardAudio::capabilities()`), and with a
  signal its embedded inputs carry a tone per channel (100 Hz times the
  channel number, -30 dBFS); `videoSignal` takes it away.
- `duplex-asiotest` says why it cannot measure the round trip when input 1
  is embedded, instead of failing.
- Unit tests: 221 cases (the inputs, names, clock, refusals, silence
  without a signal, a stopped clock, the select left alone); 227 on
  Windows.

## 0.1.4 - 2026-11-08

The Xpand's analog level in the settings, as the design has it.

### Added
- The analog level (a setting, `AnalogLevel`, and a list in the dialog):
  what 0 dBFS is on the Xpand's analog I/O, +24, +18, +15 or +12 dBu, or
  "As the card has it", the default, since it is one setting for the whole
  card and other programs use the Xpand too. A level chosen is set at each
  start and is part of the driver's routing from then on: read back once a
  second, reported when another program changes it, put back with Keep my
  routing, and put back as it was found when the driver lets the card go
  (unless another program set it since). Left as the card has it, it is
  only reported, as the task mode is ("Xpand analog level is now +18 dBu
  (it was +24 dBu)"). Chosen in the dialog while the host plays, it applies
  at once: the engine's thread sets it after the next block, with no reset.
  The log's start line, the dialog's box and `duplex-probe watch` show it;
  for Q9, `watch` counts a change of it like any other.
- `CardAudio`: the level is a part of `CardRouting` and `AudioConfig` has
  `analogLevelDbu`; on the real card the select and the level come in one
  read of the Xpand's register. `RoutingWatch` knows which parts are the
  driver's (the others are only reported), and `change()` and `release()`
  let the driver take a part or let it go while it runs.

### Changed
- The dialog is a little taller: the level under the input, and more room
  for what the driver is doing.
- Unit tests: 215 cases (the level set, kept, released, reported, put back,
  refused when it is not one of the Xpand's; set at once while running and
  at the next start when stopped); 220 on Windows.

## 0.1.3 - 2026-11-08

Living with other programs: the design's coexistence rules.

### Added
- The routing watch. At each start the driver reads back what it set up
  on the card and logs it in one line: its audio system's input source,
  the Xpand's analog/AES input select, where AES out 1-4 and 5-8 and the
  monitor take their audio from, and the card's task mode. Then, once a
  second while it runs, from the engine's thread after a block's output
  has gone out (a few register reads, no allocation), it reads them again.
  A change another program made (the AJA Control Panel, AJA's services or
  audio driver, any program on the SDK) goes to the log as it is found,
  once ("another program changed AES out 1-4 to audio system 1, channels
  1-4; the driver's is audio system 2, channels 1-4; left as it is"), and
  so does its going back. For the select it says what it means ("the
  analog inputs are off"). The settings dialog shows the run's count, what
  is not the driver's now and the task mode; the stop line of the log too.
  A fight with a program that keeps setting its own is logged in full for
  the first 20 lines, then one in a hundred.
- Keep my routing (a setting, `KeepRouting`, and a box in the dialog): what
  another program changed is put back at the next check, and read again to
  see that it took. Off by default, since the other program may need the
  outputs more; on or off applies at once, with no reset. The task mode is
  only reported: setting OEM mode while the driver plays, as the design
  has it, waits for Q9.
- `duplex-probe watch`: the audio system set up and running as the
  driver's, in silence, with the same watch, every change printed as it
  comes and listed in a report. Left alone for five minutes in standard
  task mode it answers Q9 (whether AJA's services rewrite the audio by
  themselves); run while someone changes things in the AJA Control Panel,
  it shows what the driver will report. `--input analog|aes`, `--keep`,
  `--seconds` (60 by default). With `--simulate`, another program is
  played: it takes AES out 1-4 at 40% of the run and sets the select to AES
  at 70%.
- `CardAudio::readRouting` and `writeRouting` (the routing as plain
  numbers, `CardRouting`), on the real card and the simulated one; the
  simulated card now keeps each AES group's and the monitor's source, and
  its input hears silence with a source other than the Xpand's.

### Changed
- When the driver lets the card go, the settings the whole card shares (the
  select, each AES group, the monitor) go back to what they were only where
  the card still holds the driver's: what another program set meanwhile
  stays. Before, the driver put its saved values back over them.
- Unit tests: 206 cases (the watch on the simulated card, the driver's log
  lines, Keep my routing on and off, a fight over the routing, the task
  mode, restore leaving another program's routing, watch's command line
  and report); 211 on Windows.

## 0.1.2 - 2026-11-08

The settings without the registry editor.

### Added
- A settings dialog, the host's "ASIO settings" button: input (analog or
  AES), the buffer size offered first, the guard, aggressive mode, the
  card's latencies in and out, the card and the audio system, and a box
  with what the driver is doing (open or not and why, the latencies it
  reports, this run's blocks and events, where the log is), with a button
  that opens the log's folder. OK checks every field and keeps the dialog
  open, saying which one is wrong. It is built in code (no resource file)
  and replaces the message box of 0.1.0. For the simulated driver the
  latency boxes are off: the simulated card reports its own.
- What a change does, as the design says: the latencies alone apply at once
  and the host is told (kAsioLatenciesChanged); anything else (input,
  buffer, guard, aggressive mode, card, audio system) asks the host to
  reset (kAsioResetRequest), after which the driver opens with the new
  settings. A host that cannot be told (before it has created buffers)
  gets them the next time it opens the driver, and the dialog says so.
  Both go to the log.
- Unit tests: 185 cases (the core applying changed settings, the
  comparison of settings); 190 on Windows, where the dialog itself is
  opened, filled in and read back (run under Wine with a virtual display
  here).

## 0.1.1 - 2026-11-08

What the driver does, written down, for the first runs on the card.

### Added
- A log for each driver: `%LOCALAPPDATA%\AJA Duplex ASIO\driver.log` (and
  `simulated.log` for the simulated one), 2 MB and then a new file, the
  previous one kept as `.old`. It has the program that loaded the driver,
  the settings, the card's description, the buffers and what the host
  takes, each start with the latencies reported, the engine's events
  (late outputs with the frames dropped so far, overruns, stalls, output
  moves: the first ten of each kind in a run, then one in a thousand), and
  at each stop the run's numbers (blocks, events, smallest output margin,
  longest DMAs and host callback, the card's clock). The engine's thread
  never waits on it: lines go through a lock-free queue to a thread of
  their own, and a full queue counts lines as lost instead (the log says
  how many). The control panel shows where the log is, and
  duplex-asiotest prints it.
- init refuses a KONA X without its Xpand (the breakout board answers in
  its status register), saying so: every input and output is on it.
- The clock source is named after the card's reference, as the AJA SDK
  names it ("Card reference: Free Run", "Card reference: Reference In",
  ...), read at init.
- Unit tests: 183 cases (10 new: the log queue under four producers, the
  writer, the file that starts over, the driver's log lines and their
  limit, the missing Xpand); 185 on Windows.

## 0.1.0 - 2026-11-08

Phase 1 starts: the ASIO driver, on the engine phase 0 measures. Written
and tested without the card, as decided on 2026-11-08: it runs on the
simulated card, in real time, loaded the way hosts load it.

### Added
- `AJADuplexASIO.dll`, the driver: a COM object implementing IASIO (hosts
  create it with the CLSID as the IID, which QueryInterface takes), a
  class factory, and `regsvr32` registration under
  `HKLM\SOFTWARE\Classes\CLSID` (InprocServer32, Apartment) and
  `HKLM\SOFTWARE\ASIO`. It registers two drivers: "AJA Duplex ASIO" for
  the card, and "AJA Duplex ASIO (simulated)", the same driver on the
  simulated card with its AES and XLR outputs looped to its inputs, so it
  can be tried in a host without the card.
- `DriverCore` (src/asio): everything IASIO does, without COM, so it is
  unit-tested anywhere. Inputs: the Xpand's analog L/R by default (D2) or
  AES 1-8; outputs AES 1-8, with the monitor on 1-2 so the analog output
  plays them whichever of the two it follows (Q4), and named so ("AES 1 +
  Analog L"). Buffers 64 to 2048 frames in powers of two, 128 offered
  first; 48 kHz only; one clock source, the card's reference, never
  changed. Latencies N plus the card's share in, S plus its share out.
  createBuffers checks sizes and channels and asks the host what it takes
  (time info; overload, resync, reset messages); the engine runs on a
  thread of its own at MMCSS "Pro Audio" priority and calls
  bufferSwitchTimeInfo (or bufferSwitch) once per block, with the sample
  position and a time stamp in timeGetTime's base, as the ASIO
  specification asks on Windows; outputReady from inside the callback
  sends the block at once. Late output: kAsioOverload (the latency holds,
  D1); input overrun: overload and kAsioResyncRequest; input resumed or
  output moved: resync; the engine giving up: kAsioResetRequest.
  future() says yes to kAsioCanTimeInfo and kAsioCanReportOverload.
- Settings in `HKCU\Software\AJA Duplex ASIO` (DriverSettings): input,
  preferred buffer, guard, aggressive mode, the card's latencies in and
  out, card and audio system. Missing or out-of-range values keep their
  defaults.
- One program at a time per card and audio system: a named mutex, and the
  next one is told which program has it.
- A first control panel: a message box with the settings in use and how
  the run is going.
- `duplex-asiotest`, a small ASIO host: loads the DLL (or creates it
  through COM with `--registered`), asks what hosts ask, runs it and checks
  the callbacks, the sample positions and time stamps, and the round trip
  of a click on output 1 against the latencies the driver reports (on the
  card, with output 1 looped to input 1). Without options it runs the
  simulated card.
- `install-driver.cmd` and `uninstall-driver.cmd` (run as administrator),
  and docs/DRIVER.md.
- The ASIO SDK 2.3.4's three headers and licence under third_party/asiosdk,
  used under the SDK's GPLv3 option (THIRD_PARTY_NOTICES.md).
- The simulated card can run on a real clock (its costs then are the real
  ones), which the simulated driver uses.
- Unit tests: 173 cases (22 new: the driver core, its settings), 175 on
  Windows (the instance lock).

### Changed
- The engine places the output from when the first block became complete,
  not when it was found: a late wake-up before the first block no longer
  adds to the latency, so O - R is S + N plus the card's own offset between
  its heads on every start, and the latency reported holds. (Under Wine
  the simulated driver's round trip had come out a millisecond long.) An
  output placed anew after it went out of step is placed the same way.
- `build.cmd` also builds the driver and duplex-asiotest, runs the driver
  on the simulated card after the unit tests, and packs
  `dist\AJA-Duplex-ASIO-<version>-win64.zip` with the driver, the test
  host, the probe, the install scripts and the docs (it was
  duplex-probe-<version>-win64.zip). CI keeps that ZIP.

### Checked off Windows only
- GCC and Clang (strict warnings) on Linux; MinGW (strict warnings) for
  every file, the SDK's Windows headers included; under Wine: the unit
  tests, the DLL built with MinGW (simulated card only: MinGW does not
  build libajantv2) registered with regsvr32, created through COM, and
  duplex-asiotest passing on it. MSVC has not compiled the driver yet.

## 0.0.7 - 2026-11-08

The decisions of 2026-11-08, and the engine changed to match D1 before
the driver is built on it.

### Decided
- D1: a late output keeps the latency. D2: analog is the driver's default
  input. Phase 0's exit test: one clean hour at N = 128 on each path, AES
  and analog. Phase 1 starts now on the simulated card, without waiting
  for the hardware session (docs/ROADMAP.md).

### Changed
- A late output keeps the latency (D1). When the card has already read
  past where a block goes, the engine drops what it passed and G frames
  after it (written nearer the head, a block may land behind it and play
  a lap later) and sends the rest on time; O stays on its timeline.
  Before, as the design had it, O moved S ahead of the head again, which
  left the real output latency longer than the reported one until the
  next start: in the simulated loop the round trip grew with each late
  output. Now a late block costs frames, never latency, and the analog
  loop shows a glitch without a slip.
- The output is placed by the playback head as it stands when the first
  block is found complete, not once the host has filled it, so the host's
  first callback adds nothing to the latency: the output latency is S, as
  reported.
- An input overrun moves the output as far as the input, so the round trip
  stays what it was; before, the output stayed behind and every block after
  came out late.
- Hangs longer than the capture ring (65280 frames, 1.36 s) are counted in
  full: after a long gap between two readings the write head is unwrapped
  by the time gone by. Before, a 2 s hang read as 30720 frames behind, no
  overrun at all, and the sample position lost a whole ring.
- The playback head is read against where the input's timing says it must
  be, so a block held up for more than half a ring still counts as late
  instead of being written a lap off.
- The clock model takes a head change only from a reading made shortly
  after the previous one (not merely after a poll's sleep), so a long host
  callback can no longer date one badly.
- A failed read of the playback head drops the block and moves O on,
  instead of leaving O a block behind.

### Added
- `EngineEvent::OutputMoved`: when the playback head is more than 1024
  frames from where the input's timing puts it, as when the card's output
  is stopped and started behind the engine's back, the output is placed
  anew (the latency changes there). With the latency kept, such a break
  would otherwise leave every block late, or far early, for good.
- Engine stats `droppedOutputFrames` and `outputMoves`. The loop report
  shows "late outputs (... frames dropped)" and output moves, and both
  CSVs have `dropped_frames` and `output_moves` columns.
- Unit tests: 151 cases (3 new: a slow first callback, a hang longer than
  the ring, an output restarted behind the engine's back); the late-output,
  overrun and loop tests now check that the round trip stays the same.

## 0.0.6 - 2026-11-08

Step 4b of phase 0, the routing questions, still without hardware.

### Added
- `duplex-probe routing`: a tone on each of the 16 outputs (25 Hz times a
  prime, 425 to 3175 Hz, -20 dBFS), with AES 1-8 and XLR L/R looped. The
  tones fit the 65280-frame playback ring and 1920-frame windows in whole
  periods, so the card loops the ring by itself (erase mode off) and an
  exact least-squares fit gives each tone's level on each input; the noise
  left tells an input through the converters from a bit-exact digital one.
  Three set-ups answer Q4 (the analog output follows AES out 1-2 or the
  monitor: the monitor is put on 15-16 to tell) and Q5 (with the analog
  input on, AES in 3-8 keep AES or go quiet); `--scan-select` writes the
  Xpand's select with every value 0-15 and records what each input hears
  (Q14), and says whether only bit 0 counts, any non-zero value selects
  analog, or some value gives AES 1-2 and analog at once. The report
  starts with the answers and what they mean for the driver's channels.
- `CardAudio`: the monitor's pair (`monitorPair`) and the Xpand's select,
  raw (`readXpandSelect`, `writeXpandSelect`; put back by `restore()`).
- The simulated card: the monitor's pair, the select's 16 values and three
  models of what they do (bit 0, any non-zero, one bit per AES pair).
- Unit tests: 148 cases (10 new), the routing answers against each model.

### Fixed
- The test framework printed a string literal through a null check that
  GCC warns about.

## 0.0.5 - 2026-11-08

An easier build, with no hardware still at hand.

### Changed
- `build.cmd` does everything from one double-click: finds Visual Studio
  (2019, 2022 or 2026, or only the Build Tools) and prefers the CMake that
  comes with it, fetches the AJA SDK when it is missing (with git, or
  without it as a ZIP from GitHub; `-Setup` is no longer needed), builds
  Release, runs the unit tests and packs
  `dist\duplex-probe-<version>-win64.zip` with the probe, PROBE.md, the
  licences and a short README for the PC with the KONA X. Everything it
  prints also goes to `build-log.txt`, and a failing step says so with its
  exit code. `-InstallTools` installs the Visual Studio 2022 Build Tools
  with the C++ workload through winget; `-NoPackage` skips the ZIP. Started
  with a double-click, it keeps its window open at the end.
- CI builds with `build.cmd` itself, on Windows PowerShell 5.1 as on a
  developer's PC, and keeps the probe's ZIP and the build log as
  artifacts of each run.
- For MSVC, which has not compiled the project yet: `<cstdlib>` where
  `std::llabs` is used, and only literal printf formats.

## 0.0.4 - 2026-11-08

The analog path first: the Xpand's analog I/O is what the driver is for
first, so the loop test runs through its converters too. Written and
tested on the simulated card, like the AES loop.

### Added
- `duplex-probe loop --path analog`: the same engine with XLR out L/R
  cabled to XLR in L/R. Converters change every sample, so instead of a
  counter each output plays a tone (997 Hz on L, 1499 Hz on R, -20 dBFS,
  +4 dBu with the Xpand at +24 dBu) after a 21 ms sweep, and each input is
  checked three ways: a per-sample test that is zero for a pure tone and
  jumps at any discontinuity (glitches, with their times); a fit every
  second, a whole number of periods, for the tone's level, phase and the
  noise and distortion left (AES17 dBFS); and the phase itself, which must
  not move in a synchronous loop. A slip gives its exact size in frames
  (lost or late) from the phase alone, because both tones are prime to
  48000. After the run, the sweep's correlation gives the round trip with
  a fraction of a frame and the polarity. The report adds the analog level
  setting and "card, converters and cable" (the converters' delay, Q12, is
  that minus the AES loop's "card and cable"); the verdict prints the
  analog exit test. Crossed L/R, a missing XLR cable and a tone that never
  comes back are named.
- `CardAudio`: monitor routing (`routeMonitor`: the monitor plays this
  audio system's 1-2; saved and put back like the AES map) and the Xpand's
  analog level (`readAnalogLevel`). The analog loop routes AES out 1-2 and
  the monitor, so it works whichever of them the analog output follows
  (Q4).
- The simulated card's analog path: the Xpand's select, a converter delay
  (40 frames), gain, noise, DC offset and polarity, the analog output
  following AES 1-2 or the monitor (Q4) and the analog input replacing
  AES 1-2 or all eight (Q5).
- Unit tests: 138 cases (19 new), among them the verifier against
  made-up converters (lost, repeated and whole-period slips, a dropout, a
  glitch while it calibrates, a level change, crossed or missing tones)
  and analog runs through the simulated Xpand.

### Changed
- docs/PROBE.md loops all four AES pairs (`--channels 1-8`) with ordinary
  75 ohm BNC cables, since the Xpand has four AES BNC in and four out.
- The phase 0 exit test, as proposed: the AES hour and an analog hour,
  both at N = 128.

## 0.0.3 - 2026-11-08

Step 3 of phase 0: the audio engine the driver will use, and the loop
test that decides whether the design holds. Written and tested on the
simulated card; it waits for the hardware session like steps 1 and 2.

### Added
- The audio engine (`AudioEngine`), the design's block cycle on top of
  `CardAudio`, with no ASIO and no thread of its own, so the probe, the
  tests and later the driver can all drive it: the start sequence (stop,
  zero the playback ring, erase mode, output then input), the wait for a
  block (sleep until 300 us before it is due by the clock model, then poll
  every 50 us), DMA in and out split at the wrap, de-interleave and
  interleave, the output written S = N + G ahead of the playback head (or
  N/2 + G in aggressive mode), `outputReady`, the block's sample position
  and time for `getSamplePosition`, and the design's recovery from a late
  output, an input more than half a ring behind and a stalled input.
- Ring arithmetic with the three ways of reading LastIn (conservative
  `W & ~3`, last byte, next byte), sample conversion, and a least-squares
  model of the card's clock against the PC's.
- `duplex-probe loop`: the engine with AES out cabled to AES in. Each
  looped output carries a frame counter; each looped input must bring
  every frame back once, in order, from the same channel. It counts
  frames lost, repeated, silent, from the wrong channel and garbage, and
  measures the round trip in frames, the card's and cable's share of it,
  the smallest output margin, DMA and client times and the card's clock
  against the PC's. Without `--frames` and `--guard` it sweeps N
  256/128/64/32 against G 64/32/16/8; the verdict gives the smallest clean
  G for each N and the command for the exit test (N = 128, one hour).
  Crossed or missing cables, words that come back changed and routing
  changed from outside are named in the report. Runs of two minutes or
  more print a line a minute, from a thread of their own so the engine's
  never waits on the console.
- The test signal is a 16-bit word in the top of the 24 bits AES carries
  (peak -48 dBFS), on the looped outputs only, so anything else that
  follows them (the Xpand's analog out may follow AES 1-2, Q4) hears a
  quiet buzz instead of full-scale steps.
- AES output routing in `CardAudio`: AES out 1-8 from channels 1-8 of the
  audio system in use (after power-up they follow audio system 1). Only
  that part of the shared register is touched, and only put back if it
  was changed. `loop` also sets the input to AES with the Xpand's select
  following it.
- The simulated card: AES outputs that follow another audio system until
  routed, a stalled audio clock, slow DMAs; and a simulated PC whose
  wake-ups are late now and then (`VirtualClock` sleep lateness).
- Unit tests: 119 cases (43 new), among them the engine on the simulated
  card (steady state, constant round trip, every sample back, late
  output, overrun, stall, `outputReady`) and whole `loop` runs.

### Changed
- `--seconds` goes up to 86400 for `loop`; `heads` keeps 600, since it
  keeps every sample in memory.

## 0.0.2 - 2026-11-08

Step 2 of phase 0, written and tested without the card: the simulated
card stands in for it until the hardware session.

### Added
- `duplex-probe heads`: on one audio system, with no cables. Step sizes,
  value modulo the frame size and rate of LastIn and LastOut; whether
  LastIn points at the last byte written or the next one (capture ring
  read around the head during the first lap, against a sentinel); whether
  the 48 kHz counter runs on the heads' clock; whether erase mode zeroes
  what was played; eAudio, eAudioInWrap and eAudioOutWrap counts and
  intervals; whether capture reaches memory without capture enable. The
  playback ring holds an inaudible marker (-138 dBFS), not the sentinel.
- `duplex-probe cost`: register read; DMA read and write of 4, 8 and
  16 KB, plain and page-locked; wake-up lateness of 0.5, 1 and 2 ms
  sleeps on the high-resolution waitable timer under MMCSS "Pro Audio";
  a first estimate of the output guard G.
- `--simulate` for both: the simulated card instead of the real one
  (the timer part of `cost` still measures the PC).
- `CardAudio`, the interface between the engine or the probe and one
  audio system of a card, and its libajantv2 implementation: refuses an
  audio system in use, saves its settings (the source select register
  raw, and the Xpand's analog/AES select) and puts them back at the end
  and on Ctrl+C.
- `FakeCardAudio` and `VirtualClock`: a simulated KONA X audio system
  with both rings, heads on a 48 kHz clock with adjustable ppm, input
  steps, output prefetch, erase mode, capture enable, wrap events,
  modelled register and DMA costs, and a loop cable from outputs to
  inputs with a delay.
- Platform layer: MMCSS "Pro Audio" scope, high-resolution waitable
  timer, Ctrl+C handling, Windows version (POSIX fallbacks for
  development builds).
- Unit tests: 76 cases, including `heads` and `cost` run end to end
  against each variant of the simulated card.

### Changed
- Every report starts with the same header, now with the system version.

## 0.0.1 - 2026-11-08

First step of phase 0: the project skeleton and a read-only look at the
card.

### Added
- Project skeleton: CMake with libajantv2 (`ntv2_18_0_0`) as a sibling
  folder and the static MSVC runtime; `build.ps1` / `build.cmd`; a Windows
  CI job that builds Release and runs the unit tests.
- `duplex-probe list`: the AJA devices the driver sees.
- `duplex-probe info`: read-only snapshot of the card's audio state. For
  each audio system: running state, capture enable, format, input source,
  embedded audio clock, erase mode, delays, buffer offsets and wrap
  address, and whether each head is moving. Also the reference and the
  rate of the 48 kHz audio timer, the task mode and who has the card
  reserved, the sources of the AES, SDI, HDMI and monitor outputs, the
  Xpand's analog/AES select field and level, the audio mixer, the running
  AutoCirculate channels with where their frames end relative to the audio
  buffers, and the SDK's SDRAM map with its conflicts. Prints a summary
  and writes the full report, with the capabilities and the decoded audio
  registers, to `info-<label>-<date>-<time>.txt`.
- Unit tests (in-tree framework, no card needed): report rendering, text
  helpers, command line, report file naming and writing.
