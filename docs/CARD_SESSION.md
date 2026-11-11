# The session with the card

Everything that was run on the PC with the KONA X to get the driver where
it is, in order, so that it can be run again: on another PC, with another
card, or after a change. Phase 0's measurements with `duplex-probe`
first, then the driver. About three hours, two of them waiting on the
one-hour runs, then a host for as long as it can run.
[PROBE.md](PROBE.md) and [DRIVER.md](DRIVER.md) have the detail of each
step; this is the list to follow.

`card-session.cmd`, beside the programs in the ZIP, walks through all of
it: before each step it says what to cable ("CONNECT NOW") and what to set
up, runs it when Enter is pressed (S skips, Q stops), runs the long tests
at the driver's own guard, and at the end zips the reports and the
driver's log. It runs `install-driver.cmd` (step 6; Windows asks for
administrator rights) and says, when it starts, after step 6 and before
the steps with a host, which DLL hosts load and whether it is this
folder's build: hosts load the copy that step 6 puts in Program Files,
not the one beside the programs. It checks the driver's round trip
against the latencies it reports (7), looks at the card after vMix
(6-vmix) and after Resolume (8) and says in which task mode each left it,
asks for what to try with AJA Input (8), and does the forced close by
itself (10): a program ended by force, the next one taking the card over,
and the card compared before and after. What you answer at each step goes
to `reports\notes.txt`. `card-session 5c` goes on from a step;
`card-session -Simulate` tries it out on the simulated card.

**Where it stands (2026-11-11).** Five sessions on one KONA X with the
Xpand, the last with 1.0.0; [ROADMAP.md](ROADMAP.md), "Found on the
card", has each one's results.

- Steps 1 to 5: all run. Both sweeps clean from N = 64 up with any guard;
  phase 0's exit tests passed on both paths, an hour at N = 128 with the
  driver's guard, AES twice and analog once, with no late output, no DMA
  error and no glitch. With AJA Input capturing video, an audio DMA
  waited up to 2.8 ms (Q3): the driver's guard held that with 200 frames
  to spare, and a guard of 8 did not.
- Steps 6 and 7: vMix lists the driver as an input and plays and records
  through it; the round trip is 720 frames at the defaults, 208 of them
  the card's and its converters', which 1.0.0 reports by itself. From
  one start to the next it came out at 718 to 727.
- Step 8: Resolume with the driver and AJA Input, ten minutes without a
  late output or a DMA error. The hours are still to come, and so is the
  try with AJA Input's sources (Q19).
- Step 9, the SDI loop: not run.
- Step 10: both takeovers worked with 0.1.22, with no restart, and left
  two bits of audio system 2 on (capture enable and the erase mode).
  With 1.0.0, which puts the audio system back as the ended program had
  found it, audio system 2 was after each takeover as before the forced
  close.

## What to have

- The KONA X with the Xpand connected, AJA's software installed, the card
  seen in the AJA Control Panel. Resolume with AJA Input, for steps 1c, 5c
  and 8.
- `AJA-Duplex-ASIO-<version>-win64.zip`, from the repository's releases or
  from `build.cmd` (`dist\`).
- Cables: four 75 ohm BNC cables for AES (out 1-2 to in 1-2, ..., out 7-8
  to in 7-8; one on 1-2 is the least), the Xpand's DB-15 to XLR breakout
  and two XLR cables (out L to in L, out R to in R). For step 9, if at
  hand: an SDI cable from SDI 2 to SDI 1.
- Monitors off or down on the Xpand's analog output: the tests play tones
  at -20 dBFS, which is +4 dBu.

## Set up

1. Unzip, and put its folder at `C:\AJA-Duplex-ASIO` (somewhere you can
   write to without administrator rights: the reports go into a folder
   beside the programs, which under Program Files would fail). On the PC
   that built it, `dist\AJA-Duplex-ASIO-<version>-win64` is that folder
   already, and a new build keeps the `reports` and session ZIPs in it
   (up to 0.1.21 a build made the folder anew, reports and all).
2. Open a command prompt in it: type `cmd` in Explorer's address bar.
3. `duplex-probe list` must show the KONA X, index 0.

Every `duplex-probe` command below writes its report to `reports` and
puts back what it changed on the card, Ctrl+C included, and since 0.1.18
when its window is closed too. Ended from the Task Manager it puts nothing
back: see "If something fails". None is for during a show.

## Part 1: phase 0 (PROBE.md)

**1. `info`, three runs, no cables.** Read-only, half a second each.

```
duplex-probe info --label idle --out reports
```

a. as above, with nothing using the card;
b. `--label aja-drivers`, with a host recording through AJA's own ASIO
   driver and Windows sound playing through the AJA WDM device;
c. `--label plugin`, with Resolume and AJA Input capturing the formats of
   a show (and `--label plugin-lowlatency` with Low Latency Mode, if shows
   use it); Resolume's audio device anything but AJA Duplex ASIO, which
   would hold audio system 2.

**2. `heads` and `cost`, no cables, Resolume closed.** 40 seconds.

```
duplex-probe heads --label idle --out reports
duplex-probe cost --label idle --out reports
```

**3. The AES loop: the four BNC cables.** The sweep takes 3 minutes; its
verdict prints the command for the one-hour test, which is phase 0's exit
test (`PASSED` at the end). The hour runs at the driver's own guard, 256,
whatever smaller one the sweep found clean: runs of ten seconds say which
guards fail, not which one holds for an hour (on 2026-11-11 the sweep was
clean from G = 8 with as little as a frame to spare in some runs). Since
0.1.16 the loops hold the card in OEM
task mode while they run (the first sweep, in standard mode, had AJA's
services put the AES outputs back on the mixer). Since 0.1.17 the AES
loop checks tones, one per channel, at -40 dBFS: the KONA X's AES inputs
resample, and in the second session no word came back as sent. The XLR
outputs play AES 1-2's tones too (-16 dBu at +24 dBu): monitors down.
Since 0.1.18 a loop that takes the card out of standard mode waits 100 ms
before it sets anything else, for a pass of AJA's services that was under
way to end. A run that says "never heard" with the cables in place is
worth a line all the same: the services would have written the outputs
later than that (Q19).

```
duplex-probe loop --channels 1-8 --label sweep --out reports
duplex-probe loop --frames 128 --guard 256 --seconds 3600 --channels 1-8 --label exit --out reports
```

**4a. The analog loop: the XLR cables** (the BNC ones can stay). The same
two runs through the Xpand's converters. In the second session every run
said one slip: the delay's drift (about 2 us in 10 s), which 0.1.17
reports as such ("delay drifted ... us").

```
duplex-probe loop --path analog --label analog-sweep --out reports
duplex-probe loop --path analog --frames 128 --guard 256 --seconds 3600 --label analog-exit --out reports
```

**4b. `routing`, BNC and XLR cables.** 8 seconds, a tone on every output.

```
duplex-probe routing --scan-select --label routing --out reports
```

**5a. `watch`, no cables.** The card in standard task mode in the AJA
Control Panel, then the panel and every program that uses the card
closed; five minutes untouched (Q9: do AJA's services rewrite the audio?).

```
duplex-probe watch --seconds 300 --label services --out reports
```

**5b. `watch` again**, changing the audio settings in the AJA Control
Panel while it runs (the AES outputs' source, the analog/AES input, the
task mode).

```
duplex-probe watch --seconds 120 --label panel --out reports
```

**5c. The AES loop with video (Q3).** BNC cables; Resolume and AJA Input
capturing as in a show, as in 1c, every format and source a show uses at
once (not on AJA Duplex ASIO); ten minutes at the driver's guard. The
report's "max DMA in ..., out ..." says how long audio waited for video,
and "min output margin" what was left of the guard. On 2026-11-11, one
1080i50 capture: 2.83 ms in and 1.07 ms out (0.3 to 0.5 ms without
video), which failed four blocks at G = 8 and would leave over 200 frames
at G = 256. More or larger captures may hold a DMA longer: run it with
what a show has.

```
duplex-probe loop --frames 128 --guard 256 --seconds 600 --channels 1-8 --label plugin --out reports
```

## Part 2: the driver (DRIVER.md)

**6. Install.** With every host closed, right-click `install-driver.cmd`,
Run as administrator (`card-session` runs it, and Windows asks). It
copies the driver to `C:\Program Files\AJA Duplex ASIO` and registers
that copy: hosts load it from there, so this folder can be moved,
replaced or deleted afterwards, and a new build is what hosts run only
once this step has run again. Hosts then list "AJA Duplex ASIO" and
nothing else of this driver's: since 0.1.20 the simulated driver of
earlier versions is not registered, and installing takes away what they
registered for it. `card-session` says what is registered afterwards and
which DLL hosts load; by hand:

```
duplex-asiotest --installed
```

`installed   C:\Program Files\AJA Duplex ASIO\AJADuplexASIO.dll (this
build)` is the answer wanted. OEM task mode is on by default since 0.1.17
(Q9).

**vMix (`card-session`'s step 6-vmix).** "AJA Duplex ASIO" must be there
in Add Input, Audio Input, and the same driver in Settings, Audio
Outputs. With the simulated driver registered beside it vMix left it out
of the inputs, and listed it once that one's key was deleted (the second
session); now it is the only one. With vMix closed again:

```
duplex-probe info --label after-vmix --out reports
```

vMix holds the card while it runs and leaves it in standard task mode
when it closes (2026-11-11; the driver's log says "release: the task mode
left as it is: vMix, process ..., holds the card"). If the report says
OEM task mode all the same, see "If something fails".

**7. The round trip, XLR out L to XLR in L** (the other cables can stay),
Resolume closed: the driver takes one program at a time. It plays a click
on output 1 once a second.

```
duplex-asiotest --card --seconds 10 > reports\asiotest-1.txt
type reports\asiotest-1.txt
```

Its "reported latencies within 1 ms" line says how many frames apart the
round trip and the latencies reported are. Since 1.0.0 the driver
reports the card's and converters' share by itself (208 frames with the
analog input), so on a KONA X with the Xpand it should say `PASSED`
with nothing set (up to 0.1.22 that share was 0 until this step had put
it in `OutputLatency`, on each PC). A few frames apart is how it comes
out: on 2026-11-11 six starts gave 718 to 727 frames, with 720 reported,
because the round trip moves from one start to the next with the offset
between the card's two heads, which step 8 frames at a time. That is
nothing to set: a run 5 frames long says nothing about the next one. If
it is further off than 16 frames, add the difference to what the driver
has (`OutputLatency`, 208 with none saved) and run it again;
`card-session` offers to, only then:

```
reg add "HKCU\Software\AJA Duplex ASIO" /v OutputLatency /t REG_DWORD /d <frames> /f
duplex-asiotest --card --seconds 10 > reports\asiotest-2.txt
type reports\asiotest-2.txt
```

**8. Resolume.** "AJA Duplex ASIO" as its audio device, with the defaults
(buffer 128, guard 256: what ran 4 h 48 min without a dropout on the
simulated card). Something on XLR in L/R shows in its input; a clip
comes out of AES 1-2 and the analog output. Then a show's set-up, AJA
Input capturing, for as long as it can run (the target is 8 hours without
a dropout); the driver's settings (ASIO settings in Resolume), opened at
any time, say how many late outputs there have been so far. To try while
it runs (Q19): take every AJA Input source out of the composition and put
one back, and change deck. If AJA Input's last clip closes the card and
the task mode it found was standard, AJA Input 0.43 puts standard back:
AJA's services move the outputs to their mixer, and the driver takes the
mode and its outputs back at its next look, within a second (since
0.1.18; before, the sound stayed off). Whether that happens, and whether
the gap is heard, the log says ("routing: another program changed task
mode ...") and you can tell. An AJA Input with the change to its Close
(after 0.43) leaves the mode alone while the driver plays: no gap, and
nothing in the log. With Resolume closed again:

```
duplex-probe info --label after-resolume --out reports
```

Its `task mode` line says how the two were let go: OEM with nothing
running is the card left in the driver's OEM mode ("If something fails").

**9. If the SDI cable is at hand (Q17).** In the AJA Control Panel SDI 1
as an input and SDI 2 as an output; in the driver's settings, input SDI
1 and outputs also to SDI 2: outputs 1-16 should come back on inputs
1-16 in Resolume, with no video played on SDI 2 (if they do not, that is
the answer to Q17).

**10. Last: a forced close (Q18).** A program ended without a chance to
put the card back, which is what a host that crashes does, and the next
one to open the card. On 2026-11-11 (0.1.20) it left audio system 2
running, the card in OEM task mode and the AES outputs on audio system 2,
the next loop was refused, and a restart of the PC cleared it all. Since
0.1.21 the next program takes the card over (DRIVER.md, "After a host
closed by force"), and since 1.0.0 it puts the audio system's own
settings back as the ended program had found them: with 0.1.22, capture
enable and the erase mode stayed on. `card-session` does it all by
itself, with `duplex-probe` and then with the driver in `duplex-asiotest`
(XLR out L to XLR in L, as in step 7), compares audio system 2 before the
forced close and after each takeover, and says at the end whether the
card was left clean; only if it was not is the PC restarted, and
`card-session 10-restart` then looks at the card once more and makes the
ZIP. If audio system 2 has capture enable and the erase mode on before
it starts, with nothing running, a takeover of 0.1.21 or 0.1.22 left them
so: the step says it, and only after a restart of the PC, which clears
them, does it show whether 1.0.0 puts them back. By hand: look at the
card, start the loop, and after ten seconds end `duplex-probe.exe` from
the Task Manager (Details, End task):

```
duplex-probe info --label before-kill --out reports
duplex-probe loop --frames 128 --guard 64 --seconds 60 --label killed --out reports
```

Then these three, in this order. The loop's first lines should say "taken
over: duplex-probe.exe (process ...) left audio system 2 running and is
gone (...); stopped; the audio system's own settings back as it had found
them; the task mode back to standard, as it had found it", and the last
report standard task mode with audio system 2 not running and its
`[audio system 2]` lines, from `capture enable` to `output delay`, as in
`info-before-kill` (the AES outputs and the Xpand's select may still
read as the ended loop set them in a report taken at once: AJA's
services put those back, within a second):

```
duplex-probe info --label after-kill --out reports
duplex-probe loop --frames 128 --guard 64 --seconds 5 --label takeover --out reports
duplex-probe info --label after-takeover --out reports
```

The same with the driver: end `duplex-asiotest.exe` from the Task Manager
a few seconds into the first of these, then run the second ("start:
ASE_OK", and `driver.log` says "card: taken over: duplex-asiotest.exe
..."):

```
duplex-asiotest --card --seconds 60
duplex-asiotest --card --seconds 10 > reports\asiotest-takeover.txt
duplex-probe info --label after-driver-takeover --out reports
```

If a loop or the driver says "audio system 2 is in use" instead, the
takeover did not work: restart the PC, run
`duplex-probe info --label after-restart --out reports`, and keep it all
for a report.

## What to keep

The session's record, to compare with the next one or to attach to an
issue on the repository
(https://github.com/fiverecords/AJA-ASIO-Duplex/issues):

- With `card-session`, the `session-<date>.zip` it leaves beside the
  programs. By hand:
- The `reports` folder, zipped (right-click, Send to, Compressed folder):
  every report, `.txt` and `.csv`, and the two asiotest files.
- `driver.log`, and `driver.log.old` if there is one, from
  `%LOCALAPPDATA%\AJA Duplex ASIO` (type that in Explorer's address bar).
- A line on anything seen or heard that the files would not say.

The reports and the log name the card's serial number, the programs that
loaded the driver and where they are on the PC: look through them before
posting them anywhere.

## If something fails

- `list` shows nothing: AJA's driver does not see the card; the AJA
  Control Panel must see it first.
- "audio system 2 is in use": another program has it running (Resolume
  on AJA Duplex ASIO, or AJA's ASIO or WDM driver, Q10). Close it, or take
  audio system 1:
  `--audio-system 1` for `duplex-probe`, `AudioSystem` 1 in the driver's
  settings.
- "audio system 2 is in use" with no program using it: one that was closed
  by force (the Task Manager, a host that crashed) while it played. AJA's
  driver stops nothing when a program ends, so the audio system runs on,
  and the card stays in OEM task mode with the outputs on audio system 2.
  Since 0.1.21 the driver and `duplex-probe` take over what one of them
  left ("taken over" in the log or the report) and this message should not
  come for that. If it does: restart the PC, which clears it, and keep the
  log (after the user has logged off, or when the program ended by force
  was not the driver or the probe, it is still the way).
- Resolume lists "AJA Duplex ASIO" greyed out, with no ASIO settings
  button, or vMix does not list it at all: the host could not open the
  driver. `duplex-asiotest --installed` says whether it is installed as
  hosts need it: "that file is not there" is a driver of 0.1.21 or earlier
  whose folder was deleted or made again (2026-11-11), and step 6 puts it
  right. If it is installed, something has the card's audio system: a
  second host with the driver open, or `duplex-probe` in the middle of a
  loop. The last lines of `driver.log` say which ("init refused: AJA
  Duplex ASIO is already open in ..."); close that program and open the
  host's audio settings again.
- vMix lists "AJA Duplex ASIO" as an audio output but not as an audio
  input, as it did with 0.1.16: step 6 says what is registered. If it
  names anything but "AJA Duplex ASIO", an earlier version's simulated
  driver is still there (run `install-driver.cmd` of this version again,
  as administrator); if it is alone and vMix still leaves it out, that
  is new: worth a report.
- The sound stops in Resolume and the driver's dialog says "changes by
  other programs" and "not the driver's now": something took its outputs
  and the driver did not take them back (it does when the task mode went
  with them, step 8; otherwise only with Keep my routing on). The log has
  what was changed, and by whom it could tell.
- The AJA Control Panel does nothing, or `duplex-probe info` says the card
  is in OEM task mode with nothing running: a program that sets OEM mode
  itself (AJA Input 0.43) found the driver's when it opened the
  card, and put that back when it closed, after the host had let the
  driver go (DRIVER.md, "When another program takes the card out of OEM
  mode"; an AJA Input with the change to its Close puts standard instead,
  and vMix leaves standard). Restart the PC; which program it was with is
  worth a report.
- A loop says "never heard ... check that cable", or "crossed cables":
  the cabling (and with the cables right, see step 3).
- A long loop fails on "late output" with a small guard: that guard was
  too small for an hour, not the card (step 3). At 256 it is worth a
  report.
- "DMA error" in `driver.log`: the line says which DMA failed, what the
  system said and how the second tries went ("DMA error, recovered": a
  second try saved it, nothing was lost); "DMA errors" in a loop's report,
  or "FAILED" in `cost`'s `[DMA checks, in short]`: worth a report, the
  file as it is.
  (`cost`'s "128 fresh buffers ... FAILED", with buffers that fail DMAs of
  less than 4096 bytes, is known since 2026-11-11: the driver finds such
  buffers at each start and swaps them.)
  (0.1.13 to 0.1.15 had one every 1.36 s on the card, a click each time;
  none since 0.1.16, which sends the block at the ring's end whole.)
- The driver does not open in a host: `driver.log` says why ("init
  failed: ...").
- Anything else: the report or the log, as it is, is what an issue needs.
