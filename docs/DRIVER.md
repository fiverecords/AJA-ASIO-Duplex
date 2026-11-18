# AJA Duplex ASIO - the driver

The ASIO driver, on the audio engine that phase 0 measures (phase 1 of
the roadmap: 0.1.x, then 1.0.0, the first version published, and 1.0.1).

Sizes and latencies here are in samples, as hosts and the settings dialog
say: one sample of every channel, 48 to a millisecond at the card's 48
kHz. ASIO calls that a frame, and so do the log and the console tools
(`duplex-asiotest`, `duplex-probe`): where this file quotes them, it says
frames.

**State in 1.0.1.** Up to 0.1.22 the driver was built with Visual Studio
2022 and run on the KONA X with the Xpand, in Resolume and in vMix
(2026-11-10 and 2026-11-11): at the defaults it played and recorded for up
to ten minutes at a time, AJA Input holding the card beside it, without a
late output or a DMA error; its round trip was measured (720 samples, 208
of them the card's and its converters'); and the next program took the
card over after one ended by force, with no restart. The engine under it
ran an hour clean at N = 128 with the driver's guard on each path, AES and
analog (phase 0's exit test, 2026-11-11). 1.0.0 adds two things to that:
the latencies it reports include the card's own share as measured there,
with nothing to set, and a takeover leaves the audio system as the program
that was ended had found it. It was built and run on the card the same
evening: two minutes in Resolume without a late output, round trips of 718
to 727 samples with 720 reported, and audio system 2 as it was before
after each of two forced closes. 1.0.1 changes the settings only, not the
audio: a program that opens them for hosts with no settings button of
their own, vMix among them ("Settings", below), and samples with their
milliseconds where the dialog said frames. 1.0.0 has since worked for
several hours in vMix. Not seen on the card yet: hours in Resolume with
AJA Input capturing, and the SDI and HDMI inputs and outputs.
[CARD_SESSION.md](CARD_SESSION.md) is the list of what was run, to run it
again.

## Installing

`AJA-Duplex-ASIO-<version>-win64.zip`, from the repository's releases or
from `build.cmd` (which leaves it in `dist\`; on the PC that built it, the
folder of that name beside the ZIP has the same files), has the driver,
`AJADuplexASIO.dll`, its settings program, `AJADuplexASIOSettings.exe`,
and two scripts:

1. Unzip it anywhere you can write to, for example `C:\AJA-Duplex-ASIO`:
   `duplex-probe` writes its reports beside it (under Program Files that
   would need administrator rights).
2. Close every host and right-click `install-driver.cmd` there, **Run as
   administrator**. It copies the driver to
   `C:\Program Files\AJA Duplex ASIO` and registers that copy, and puts
   the settings program beside it and in every user's Start menu, as
   **AJA Duplex ASIO Settings** (since 1.0.1). (The copy of the script in
   the source tree, `tools\install`, has no DLL beside it: it says so and
   stops.)

Hosts then list one driver, **AJA Duplex ASIO**: the KONA X with the
Xpand. They load it from Program Files, so the folder of the ZIP can be
moved, replaced by a newer one or deleted.

**Updating.** Hosts go on loading the installed copy until
`install-driver.cmd` runs again: a new build, or a new ZIP unpacked, changes
nothing for them by itself. Close every host (Windows does not let a DLL
in use be replaced; the script says so when one has it open) and run the
new version's `install-driver.cmd`. `duplex-asiotest --installed` says in
one line which DLL hosts load and whether it is the build beside it;
`build.cmd` ends with that line and `card-session` starts with it:

```
installed   C:\Program Files\AJA Duplex ASIO\AJADuplexASIO.dll (this build)
installed   C:\Program Files\AJA Duplex ASIO\AJADuplexASIO.dll, ANOTHER BUILD than the DLL beside this program (written 2026-11-11 10:52): hosts load that one until install-driver.cmd runs again
installed   no: registered at C:\dev\AJA-Duplex-ASIO\dist\AJA-Duplex-ASIO-0.1.20-win64\AJADuplexASIO.dll, and that file is not there: hosts list the driver and cannot open it; run install-driver.cmd (as administrator)
```

The last is what versions up to 0.1.21 left when the folder they had been
installed from was deleted or made again: they registered the DLL where it
was. Resolume then showed the driver greyed out, with no ASIO settings
button, and vMix left it out of its lists (2026-11-11). Installing this
version puts it right.

**A host shows the driver greyed out, or does not list it:** it could not
open it. Either the driver is not installed as hosts need it
(`duplex-asiotest --installed`), or it refused to open: the card's audio
system is in use by another host with the driver open, or by
`duplex-probe` in the middle of a measurement, and the driver's log has
the reason, in a line that starts `init refused` or `init failed` (see
"The log"). `duplex-asiotest --card --registered` opens the installed
driver the way a host does and prints that reason.

Up to 0.1.19 they listed a second one, the same driver on a simulated
card, for trying it in a host without the card: "AJA Duplex Simulator",
and up to 0.1.16 "AJA Duplex ASIO (simulated)". With that one registered
vMix left "AJA Duplex ASIO" out of its audio inputs and offered it only
as an output (2026-11-10); with its key deleted it listed it. Since
0.1.20, with every test left needing the card, only the card's driver is
registered, and `install-driver.cmd` takes away what those versions
registered for the other.

`uninstall-driver.cmd` (also as administrator, with every host closed)
takes away the registration, the copy in Program Files with the settings
program, and its Start menu entry, whichever folder it is run from, even
with no DLL left anywhere; the settings and the log stay.

## Trying it without the card

`duplex-asiotest`, in the same folder, is a small host. Without options it
loads the DLL beside it (no installing needed) on a simulated card that
the DLL keeps for this (AES out 1-8 looped to AES in 1-8 and XLR out L/R
to XLR in L/R; nothing registers it, so no host lists it),
asks it what hosts ask, runs it for 5 seconds and checks what comes back:
one callback per block with the halves alternating, sample positions N
apart, time stamps following them, and the round trip of a click on
output 1, against the latencies the driver reports. It ends with `PASSED`
or `FAILED`.

```
duplex-asiotest
duplex-asiotest --seconds 60 --frames 64
duplex-asiotest --card --registered     (the card, through COM, as hosts do: after installing)
duplex-asiotest --installed             (runs nothing: which DLL hosts load, and whether it is this build)
```

`build.cmd` runs it after every build, which is how a build is tried
whole, as a host would, on a PC without the card.

## On the card

Pick "AJA Duplex ASIO" in the host. On open the driver takes, by default,
the KONA X's audio system 2 (the last one) and refuses it if something else
is playing or recording on it, with a message saying so. A second program
(or a second instance in the same one) opening the driver on the same card
and audio system is refused too, and told which program has it.

**After a host closed by force** (0.1.21). A host ended from the Task
Manager, or one that crashed, while it played cannot put anything back,
and AJA's driver stops nothing when a program ends: the audio system runs
on, and the card stays in OEM task mode with its AES outputs on that audio
system (KONA X, 2026-11-11). Up to 0.1.20 the driver then refused the
audio system as in use, with nobody using it, until the PC was restarted,
which clears it all.

Now the next program to open it takes it over. From its first change to
the card until everything is back, the driver (and `duplex-probe`) keeps a
mark of which program has the audio system in hand: its process and when
it started, its name, and what it found: the task mode and, since 1.0.0,
the audio system's own settings (its channel count, rate, buffer size,
loopback, capture enable, delays and source select register, erase mode
included). The marks are the values of a volatile registry key,
`HKCU\Software\AJA Duplex ASIO\Running`: they outlive the program and are
gone with a restart of the PC, as is what they are about. On open, an
audio system that runs with the mark of a process that is gone, and runs
as the driver runs it (the playback erased behind its head), is stopped
and taken; its own settings are put back as that program had found them,
which is what it would have done when it closed; the task mode it had
found is put back if the card is still in OEM mode and no program holds
it; and the log says so:

```
card: taken over: Arena.exe (process 2488) left audio system 2 running and is gone (closed by force, or it crashed); stopped; the audio system's own settings back as it had found them; the task mode back to standard, as it had found it
```

An audio system that runs without such a mark is another program's and is
refused, as it always was. Logging off forgets the marks as a restart
does, without clearing the card: after that, a restart is still the way.
`card-session`'s step 10 is the test: a program ended by force, and the
next one starting. On the KONA X (2026-11-11, 0.1.22) both did as said,
the loop after a killed `duplex-probe` and the driver after a killed
`duplex-asiotest`, and the card needed no restart. Those takeovers
stopped the audio system and put the task mode back, and no more: against
a normal release, two bits of the audio system stayed on with nothing
running, capture enable and the erase mode, which AJA's services do not
write. That is what 1.0.0's mark carries the settings for; step 10 now
compares the audio system before the forced close and after the
takeover, and with 1.0.0 on the card (the same evening) it was the same
both times, after the loop and after the driver. (Bits left on by a
takeover of 0.1.21 or 0.1.22 are what the next program finds, and so
what it puts back: a restart of the PC clears them.)

What the card shares among its audio systems is not in the mark: the AES
outputs, the monitor, the Xpand's select and level. With the card back in
standard task mode AJA's services return them to their own settings
within a second (a report taken as the program that took over closed
still had the AES outputs and the select as the ended one had set them;
the next report had them back). Where the task mode the ended program had
found was
another program's OEM mode (AJA Input holding the card), the mode stays,
and so do those outputs on the driver's audio system until a program sets
them or the card is back in standard mode.

**What it changes while open, and puts back when the host lets it go:**

- the audio system's input becomes the Xpand's analog input (or AES, an
  SDI input's or the HDMI input's embedded audio: see the settings); with
  the Xpand's inputs, so does the Xpand's analog/AES select, which every
  audio system shares: another program's AES input stops while the driver
  records analog (with an embedded input the select is left as it was);
- AES out 1-8 play this audio system's channels 1-8, and the monitor its
  channels 1-2. The Xpand's analog output plays AES out 1-2 (Q4, phase
  0: 2026-11-10), so it plays outputs 1-2.

If the settings name an analog level, the Xpand's analog level is set to it
too, and the SDI and HDMI outputs they name play the driver's audio
system (see the settings).

The select, the level, the AES outputs, the monitor and the SDI and HDMI
outputs' audio belong to the whole card, and whoever sets them last wins. So when the host lets the driver go, each of
them goes back to what it was only if it still holds the driver's setting:
if another program set it meanwhile, it stays as that program left it.

**Other programs.** At each start the driver reads all of that back from
the card and writes it to its log in one line, with the card's task mode
(whether AJA's services set the card up from the AJA Control Panel,
"standard", or leave it to the programs, "OEM"). Then, once a second while
it plays, it reads it again. When something changed it, the log says what
and to what, once ("another program changed AES out 1-4 to audio system 1,
channels 1-4; the driver's is audio system 2, channels 1-4; left as it
is"), and says again when it is back; the settings dialog shows how many
changes this run and what is not the driver's now. The analog level is
watched too: as one of the driver's parts if the settings name a level, and
only reported ("Xpand analog level is now +18 dBu (it was +24 dBu)") if it
is left as the card has it. The SDI and HDMI outputs are watched only when
the driver plays to them: video programs change them, and the others are
none of its business. With **Keep my routing** on (the settings) the
driver puts its own back at once, and logs that. It is off by default: if AJA's audio driver or another program takes
the AES outputs, it may need them more, and two programs that both keep
their routing would take turns once a second. **OEM task mode while
playing** (the settings) is on by default since 0.1.17: in standard task
mode AJA's services put AES out 1-8 and the monitor back on the audio mixer
a second after the driver set them (Q9, phase 0: 2026-11-10). With it on,
the card goes to OEM mode at each start, so the services stop applying the
AJA Control Panel's settings, it is put back to OEM whenever someone
changes it, and it returns to what it was when the driver lets the card go,
unless another program has reserved the card by then (AJA Input does), so
that program is not left to the services. When the driver lets the card
go, the log says what it left as it was, and why. Turned off, the task
mode is only reported. `duplex-probe watch` does the same watch without a
host (docs/PROBE.md).

**When another program takes the card out of OEM mode.** AJA Input sets
OEM mode when its first instance opens the card and, when its last one
closes it, puts back the mode it found: standard, if nothing had the card
in OEM mode before it. From then on AJA's services have the card and move
AES out 1-8 and the monitor to their mixer. Since 0.1.18 the driver's look
that finds the task mode changed puts back, with the mode, everything of its own
that is not as it set it, at that look and at the next, whatever Keep my
routing says (up to 0.1.17 it took back the mode alone, and its outputs
stayed on the mixer, silent, until the host started it again). The log
has a line for each part ("routing: another program changed AES out 1-4
to audio system 4, channels 1-4; ...; put back"). Until that look, up to
a second, the outputs play the mixer.

The same at a start that takes the card out of standard mode itself (no
Resolume holding it in OEM mode: vMix, `duplex-asiotest`). The services
may be in the middle of a pass just then, and write the outputs once more
after the driver did. Since 0.1.18 the driver sets the task mode first and
waits 100 ms before it sets anything else, and its first two looks after
such a start put back whatever is not as it set it, with the same lines in
the log.

And when the driver lets the card go: the OEM mode it found at the start,
if a program held the card then, was that program's. If that program is
gone by then and nobody holds the card, the driver puts standard mode,
not that OEM mode, which nobody would be left to undo ("release: the task
mode put to standard, not back to OEM: AJIN, process ..."). A program
that ended without letting the card go holds it no longer (0.1.21: the
card still names it, and up to 0.1.20 that kept the task mode as it was).
The other way
round is not the driver's to settle: when it starts first, a program that
sets OEM mode itself finds the driver's OEM mode, takes it for the card's
own and puts it back when it closes, so if the host lets the driver go
before that program, the card stays in OEM mode after both are gone
(`duplex-probe info` says which mode the card is in; AJA's driver starts
in standard mode, so restarting the PC should put it back). AJA Input
0.43 does that; with its `Close` changed (after 0.43) it does not. vMix
does not either: it holds the card while it runs (the driver's log says
"release: the task mode left as it is: vMix, process ..., holds the
card") and leaves it in standard mode when it closes (KONA X,
2026-11-11).

**For other programs on the card: the task mode mark** (0.1.19). The
driver does not reserve the card (AcquireStreamForApplication is for the
programs that run its video), so the card does not say that the driver is
there. A named mutex does:

```
Local\AJADuplexASIO-card<index>-oem
```

It exists from the host's first start of the driver with OEM task mode on
until the driver has let the card go, <index> being the card's index in
the SDK's device scan (the driver's `Card` setting), and it goes with the
host if the host dies. A program that changes the same card's task mode
asks with `OpenMutexW(SYNCHRONIZE, FALSE, name)`: while the mutex is
there, the task mode is the driver's to put back, so that program leaves
it alone; and an OEM mode it found with the mutex there was the driver's,
so it puts standard mode, not that OEM mode, once the driver has gone.
AJA Input does both after 0.43 (its src/TaskModeHandover.h), which
settles the two orders between them: neither leaves the card in OEM mode,
and the driver's outputs are not handed to AJA's services when AJA
Input's last clip closes. The name is part of that agreement and stays
as it is. `duplex-probe` holds the card in OEM mode while it measures and
sets no mark.

**vMix** opens one ASIO driver once, for its outputs and its inputs alike:
to have the card both ways, add it as an audio input ("AJA Duplex ASIO" in
Add Input, Audio Input) and pick the same driver in Settings, Audio
Outputs (vMix matches the two by name). It picks its own buffer size,
1024 samples (21.3 ms), whatever the driver offers: in every run on the
card (2026-11-10 and 2026-11-11) it asked for 1024, and ran up to 4.4
minutes at a time without an event. It has no ASIO settings button: the
driver's settings open from the Start menu, **AJA Duplex ASIO Settings**
("Settings", below), and vMix takes changed ones when it starts again,
which is when it opens the driver.

**Channels.** Inputs: "Analog L", "Analog R" (analog, the default), "AES
1" to "AES 8" (AES), "SDI 1 ch 1" to "SDI 1 ch 16" (an SDI input's embedded
audio; SDI 2 alike), or "HDMI ch 1" to "HDMI ch 8" (the HDMI input's).
Outputs: "AES 1 + Analog L", "AES 2 + Analog R", "AES 3" to "AES 8". All
32-bit integer (ASIOSTInt32LSB), the card's own format: nothing is
converted.

**SDI and HDMI outputs**, off by default (their video may be another
program's). An SDI output the settings name embeds all 16 channels of the
driver's audio system in whatever video it carries: the driver never sets
the video, so the output plays the audio only while something feeds it
video (whether it does with nothing playing is Q17). With one on, outputs
1-8 are called "AES 1 + Analog L + SDI" and so on, and outputs 9-16, "SDI
9" to "SDI 16", exist, carried by SDI only. The HDMI output plays outputs
1-8 ("AES 3 + HDMI"). On the KONA X each SDI connector is an input or an
output, as the AJA Control Panel (or a video program) set it: the driver
reads which and, when it does not suit the settings, says so in the log
and in the dialog's box ("SDI 2 is set as an input, so the driver's audio
does not go out on it: set it as an output in the AJA Control Panel"). One
connector cannot be the driver's input and one of its outputs. To try the
SDI path, cable SDI out 2 to SDI in 1, set SDI 2 as an output and SDI 1 as
an input, pick input SDI 1 and output SDI 2: outputs 1-16 come back on
inputs 1-16.

**Buffer sizes** 64 to 2048 samples, powers of two; 128 (2.7 ms) is
offered first. Only an offer: a host picks the size (vMix, 1024).
Changed in the settings, the new size is offered at once, even by the
driver the host still has open, and the host is asked to reset: what runs
keeps its size until the host makes the buffers again.
**Sample rate** 48 kHz only (the KONA X does no 96 kHz, the SDK has no
44.1 kHz). **Clock** the card's reference, shown by name ("Card reference: Free
Run", say) and never changed (video depends on it); with an embedded input,
the video input's ("Video input: SDI 1"), as the SDK sets it. If that
signal goes and the card's audio clock stops with it, the host gets
silence until it comes back, and then a resync. Without the Xpand
connected the driver does not open: its inputs and outputs are all on it.

**Latencies** as reported to the host: input N plus the card's share on
the way in, output S plus its share on the way out, with S = N + G (G the
guard, 256 by default; N/2 + G in aggressive mode). The engine keeps the
output exactly S after each block came in, also after a late block (it
drops what came too late instead of moving the output, D1), so what is
reported holds for the whole run. The card's shares are the driver's own
figures for the KONA X with the Xpand, by input, all on the way out
(hosts use the sum): 208 samples (4.3 ms) with the analog input,
measured from XLR out to XLR in, and 130 (2.7 ms) with AES, where the
card's AES inputs resample.
With an SDI or HDMI input they are 0: those paths have not been measured.
`InputLatency` and `OutputLatency` in the settings replace them (below).
At the defaults the driver reports 128 samples in (2.7 ms) and 592 out
(12.3 ms), 720 in all (15.0 ms); the round trip of a start on the KONA X
comes out at 718 to 727 (six starts, 2026-11-11). It moves by a few
samples from one start to the next and holds within a start: the card's
two heads step 8 samples at a time, and how they stand to each other when
they are started is not the same every time.

**Buffer and guard for a host that sometimes takes long.** A block's
output must be written S samples after the block came in: whatever the
host's callback takes beyond that is a late output (the log says so, and
how long the host took). The guard adds to S without touching the input,
which stays N: so for a host whose callback now and then takes several
milliseconds, a larger guard is better than a larger buffer. Resolume on
the simulated driver took up to 8 ms now and then: N = 128 with G = 256
gives each block 8 ms (S = 384) with the input at 128, against 6 ms (S =
288) and an input of 256 with N = 256 and G = 32. So set, it ran 4 h 48
min without a dropout (its longest callback 7.3 ms), and so the driver is
set by default since 0.1.13. The price is the round trip: N + S = 512
samples, 10.7 ms before the card's own share. A host whose callback stays
well under a millisecond can take G down to what phase 0's loop found
for the card (32 gives 6 ms); a heavier show than the test, up to 320.
A late block loses what the card passed and G samples after it: with G at
least N, as by default, the whole block, so a late output is a block of
silence.

**The audio thread** runs in MMCSS "Pro Audio" at critical priority and
asks Windows 11 not to slow it down when the host seems to be in the
background (power throttling, EcoQoS); the log's "thread:" line at each
start says what it got. Between blocks it sleeps until shortly before the
next one is due, by the card's write head as just read. A model of the
card's clock, fitted to earlier readings, says it better when it has
readings to go by, which on Windows it seldom has (the timer wakes about
0.5 ms late, so the thread hardly ever polls): since 0.1.18 it is believed
only while it agrees with the reading, and dropped when it does not. Until
then a model gone stale could hold every block back a little more each
second, for good.

**Measuring the round trip** on the card: loop XLR out L to XLR in L and
run `duplex-asiotest --card`. It plays a -20 dBFS click on output 1 once a
second (keep monitors off that output) and says how many samples it takes
to come back ("frames", in its words) against the latencies reported (with
an embedded input there is no such loop, and it says so). On the KONA X it
is 718 to 727 samples at the defaults, around the 720 the driver reports
since 1.0.0: a few samples either way belong to that start, not to the
card's share, and there is nothing to set. Should it come out further off
(more than 16 samples, two steps of the heads), on another card or through
other converters, add the difference to `OutputLatency` (it is the sum
with `InputLatency` that hosts use) and run it again: the target is within
1 ms (48 samples). `card-session`'s step 7 does that. Phase 0's `loop
--path analog` measures the same thing in its "card, converters and cable"
line. On the card every click must come back within two samples of the
others (converters spread a click over a few samples, and which of them
crosses the threshold first may differ by one); on the simulated card, in
exactly the same samples. Ctrl+C, or closing its window, ends the run
early: the driver is stopped and let go as after a whole run, so the card
is put back.

## Settings

The host's "ASIO settings" button (or "Control panel", depending on the
host) opens the driver's settings, in front of the host and on the
taskbar; only one at a time (asking again brings it to the front): input,
the SDI and HDMI outputs that
play too, the Xpand's analog level, the buffer size offered first, the
guard, aggressive mode, Keep my routing, OEM task mode, the card's
latencies, card and audio system, and a box with what the driver is doing
now (with what other programs did to its routing this run, the analog
level, the task mode, and what is wrong with the SDI connectors, if
anything) and where its log is; at the bottom, the driver's version and
date and a button that opens the project's page. The output latency box
follows the input while it holds one of the card's own figures (208 with
analog, 130 with AES): one typed in stays. On OK the latencies apply at
once (the host is told and asks for them again), and so do Keep my routing
and the analog level (set after the next block, without stopping); any
other change makes the driver ask the host to reset, and the host opens it
again with the new settings. A host that has
not started the driver yet takes them the next time it opens it. The
buffer sizes, the guard and the latencies are in samples, with what they
are in milliseconds beside them.

**For a host with no settings button** (vMix has none): **AJA Duplex ASIO
Settings**, in the Start menu (since 1.0.1; `AJADuplexASIOSettings.exe`,
beside the driver in Program Files), opens the same dialog without a
host. It asks the installed driver for it the way a host does, through
COM, and does not open the card, so it can be open beside a host that has
the driver. Its "Now" box says so, and which program has the driver open,
if one has ("Open now in vMix64.exe (process 8768)"). What it saves is
for the next time a program opens the driver: one that has it open now
takes it once it is closed and opened again, which for vMix is when it
starts again. A second start brings the first one's dialog to the front.

The settings are the same for every program: whichever dialog saved them
last, a host's or this one, is what the next host to open the driver
takes.

They are kept in `HKEY_CURRENT_USER\Software\AJA Duplex ASIO`, as DWORD
values, which can also be set by hand. A missing or out-of-range value
keeps its default. `duplex-asiotest`'s simulated card reads them too,
and reports its own latencies.

| Value | Default | Meaning |
| --- | --- | --- |
| `Input` | 0 | 0 analog (XLR L/R), 1 AES 1-8, 2 an SDI input's embedded audio (16 channels), 3 the HDMI input's (8) |
| `SdiInput` | 1 | for `Input` 2: which SDI input (the KONA X has 1 and 2) |
| `BufferSize` | 128 | the size offered to hosts first, in samples: 64, 128, ..., 2048 (a host may pick another: vMix always takes 1024) |
| `Guard` | 256 | G, in samples (up to 1024): what the output keeps ahead of the card beyond one block (see "Buffer and guard" above) |
| `Aggressive` | 0 | 1: S = N/2 + G instead of N + G, half a block less latency for hosts that use under half a block |
| `InputLatency` | 0 | samples the card and converters add on the way in |
| `OutputLatency` | by `Input`: 208 analog, 130 AES, 0 SDI or HDMI | the same on the way out. While no value is saved it is the card's own figure for the input; a saved one stands whatever the input (the dialog saves what its box shows) |
| `KeepRouting` | 0 | 1: put the driver's routing back when another program changes it (checked once a second) |
| `OemTaskMode` | 1 | 1: the card in OEM task mode while the driver plays (see "Other programs"); 0: as AJA's services have it. Up to 0.1.16 the value was `OemTasks`, off by default: an `OemTasks` saved then is not read |
| `SdiOutputs` | 0 | bits: the SDI outputs that play the driver's outputs too (1 SDI out 1, 2 SDI out 2, 3 both); with any, outputs 9-16 exist |
| `HdmiOutput` | 0 | 1: the HDMI output plays outputs 1-8 too |
| `AnalogLevel` | 0 | the Xpand's analog level, what 0 dBFS is in dBu: 24, 18, 15 or 12; 0 leaves it as the card has it. It is the whole card's: other programs using the Xpand's analog I/O hear it too |
| `Card` | 0 | the AJA device, by index (`duplex-probe list`) |
| `AudioSystem` | 0 | 1-based; 0 is the card's last (2 on the KONA X) |

For example, from a command prompt (with no host that has the driver
open, or one that is opened again afterwards):

```
reg add "HKCU\Software\AJA Duplex ASIO" /v Input /t REG_DWORD /d 1 /f
reg query "HKCU\Software\AJA Duplex ASIO"
reg delete "HKCU\Software\AJA Duplex ASIO" /v Input /f
```

The second shows what is saved; the third takes one value away, so that
its default applies again.

## The log

The driver writes what it does to `%LOCALAPPDATA%\AJA Duplex ASIO\`:
`driver.log` (`simulated.log` when `duplex-asiotest` runs it on the
simulated card; in the Explorer's address bar, `%LOCALAPPDATA%\AJA Duplex
ASIO` opens the folder).
Past 2 MB a new file starts and the previous one becomes `.old`. It holds:
which program loaded the driver; the settings and the card's description at
init, or why init failed ("init failed: ...", or "init refused: ..." when
another program has the driver open on that audio system: the host then
shows the driver greyed out, or does not list it); what is wrong with the SDI connectors, if
anything ("sdi: ..."); the buffers and what the host takes; each start,
with the latencies reported, the SDI and HDMI outputs if any play, and the
routing as read back from the card, and, when one failed, the check of
the DMA buffers ("buffers: ...", below);
the late outputs, overruns, stalls and output moves (the first ten of each
kind in a run, then one in a thousand), each late output with where the
time went (how late it was written, what a block has from complete to
written, how late the driver found the block, its DMA in, and the host's
callback: when the host's time is most of it, the cure is a larger buffer
or guard, not the driver); the DMA errors, each with which DMA (input or
output, the whole block or the one at the ring's end, its size and ring
offset), what the system said of it and how the second tries went ("DMA
error, recovered" when one saved it, so nothing was lost); what other programs changed in
the routing, and what was put back (the first 20 lines in a run, then one
in a hundred); and at each stop the run's numbers: blocks, events, the
smallest margin the output kept ahead of the card, the longest DMAs and
host callback, the card's clock against the PC's, how far the playback
head was from where the input's timing put it ("the output within -8 to
+8 frames of the input's timing": the heads' steps, while the card's input
and output run together), the routing's
changes if there were any, and each buffer a spare replaced, with what it
did when tried again. **When something goes wrong, that file is what to
look at, and what a report needs.**

## DMAs

Each ring of the card's audio system has 4 MB and wraps at 4,177,920 bytes
(65,280 samples of each of 16 channels, 1.36 s): the 16 KB past the wrap
are never played or filled. The driver moves one block each way per cycle,
each DMA from the start of a buffer of its own, none of them locked (on
the KONA X a locked buffer was no faster).

- A block across the ring's end goes in one DMA from where it starts,
  running on past the end into those 16 KB, and what wraps goes again at
  the ring's start. (Up to 0.1.15 the block went in two pieces. On the
  KONA X the first, 2560 bytes ending at the ring's end, failed every lap,
  Windows saying error 31. Where it ended was taken for the cause; what
  phase 0's `cost` has shown since is that a DMA of less than a page,
  4096 bytes, fails from some buffers wherever in the ring it goes, and
  that one of 4096 bytes or more goes through from the same buffers. With
  the block whole, the driver's DMAs at its default buffer and guard are
  of a page or more, what wraps included (some 88 samples), all but what
  is left of a block that came late.)
- A DMA that fails is tried again at once from the same buffer (a passing
  fault goes through then) and, if that fails too, from one of four spare
  buffers (if the buffer was the trouble, that goes through): either way
  nothing is lost. A spare that saves a DMA takes the failed buffer's place
  for good, and at stop the DMA is tried once more from the failed buffer,
  with the card stopped: the log says whether the buffer is bad. After
  three blocks in a row whose second tries all failed, the driver stops
  trying again (each try costs a DMA's time, about 0.1 ms).
- Before that, at each start, every buffer and spare takes a 512-byte DMA
  each way at the middle of the rings, which start has just zeroed. On the
  KONA X some buffers fail every DMA of less than 4096 bytes, in both
  directions, and pass those of 4096 and 8192 (phase 0's cost: one buffer
  on 2026-11-10; on 2026-11-11 four of the first five of 128 made in a
  row, and one more). Which buffers, and why, is not known; what wraps at the
  ring's end, what is left of a late block, and every block with a buffer
  of 32 are DMAs that small. One that fails is swapped for a
  spare that passes, and kept aside, not freed, so that no allocation
  hands it back; the log says so only then ("buffers: 1 of 10 buffers
  failed a 512-byte DMA before the run ...: 1 replaced by a spare that
  passed; 3 spares left").

## What the driver tells the host

Through the host's asioMessage callback, when the host takes the message:

- a late output (the host or the PC took too long): `kAsioOverload`. The
  samples that came too late are lost; the latency stays the same;
- an input overrun (more than half a ring behind, the host hung for over
  0.7 s): `kAsioOverload` and `kAsioResyncRequest`; the sample position
  jumps by what was skipped;
- the card's clock stopped and came back (a lost embedded source, say),
  its output was restarted behind the driver's back, or its input fell
  behind its output and every block came out late (below): the output is
  placed anew, and `kAsioResyncRequest`;
- too many card errors in a row, after which the driver stops:
  `kAsioResetRequest`.

## When the card's input and output fall out of step

The two heads of an audio system run on one clock, and the driver places
its output by the input's timing. When they do not run together, the
output is where the driver was not told. Far apart (more than 1024
samples: the output stopped and started by another program), the output is
placed anew at once. Less than that, it was left where it was up to
0.1.20, and if that was behind the card's playback head, every block from
then on came out late and was dropped: on the KONA X (2026-11-11), while
the AJA Control Panel changed the input's source in standard task mode,
the capture side lost some two seconds of audio in the 22 its source was
not the card's own, and 39848 of 44161 blocks were late. Since 0.1.21, a
quarter of a second of blocks late by more than their own time accounts
for is the output out of step, and it is placed anew ("output moved" in
the log).

With an analog or AES input and OEM task mode, which are the defaults,
nothing does this. An SDI or HDMI input that loses its signal may: its
clock is the video input's.

## Not there yet

- An offset between the two heads that makes no block late is left as it
  is: the round trip is then off the reported one by up to 1024 samples
  until the next start. The log's "the output within ... frames of the
  input's timing" shows it. Placing the output anew for any offset that
  stays is to come with the tests of the embedded inputs, which say what
  their clocks do.
- The card's share of the latencies with an SDI or HDMI input, or
  through the SDI and HDMI outputs, is not measured: the driver reports
  0 for it (Q17).
- A takeover after a forced close puts back the audio system's own
  settings and the task mode, not what the card shares: with the card
  left in another program's OEM task mode, the AES outputs and the
  monitor stay on the driver's audio system until a program sets them or
  the card is back in standard mode.
- An installer: `install-driver.cmd` copies the driver to Program Files
  and registers it, but nothing lists it among Windows' installed apps,
  and the DLL carries no version resource (the driver's settings dialog
  and the log's `init` line say the version; `duplex-asiotest --installed`,
  whether hosts load this build).
- The card's clock in the log reads +0.0 ppm on the KONA X: the engine's
  model dates the head only from readings close together, and with
  Windows waking the engine about 0.5 ms late it almost never has them
  (phase 0's cost). The card runs at about +7 ppm (the probe's loop
  measures it over the run); an earlier or adaptive wake-up comes with
  phase 2's tuning.
- Whether an SDI or HDMI output plays the audio when no program feeds it
  video, and how late it comes out through embedding: Q17. Until it is
  measured, an SDI loop's round trip says (with `InputLatency`).
- The round trip moves from one start to the next by up to a step of
  each of the card's heads. On the KONA X, whose heads step 8 samples,
  six starts came out at 718 to 727 samples with 720 reported; the
  simulated output moves in 16-sample steps. The stop line's "the output
  within ... frames of the input's timing" shows where a start fell.
  Placing the output against a step, so that every start comes out the
  same, is not done.
