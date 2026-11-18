# AJA Duplex ASIO

A full-duplex **ASIO driver for the AJA KONA X with the KONA Xpand** on
Windows: inputs and outputs in one driver, on the card's own clock, with a
buffer size the host can choose. Open source, built on
[libajantv2](https://github.com/aja-video/libajantv2), AJA's SDK.

https://github.com/fiverecords/AJA-ASIO-Duplex

> Not affiliated with or endorsed by AJA Video Systems.
> AJA, KONA and Xpand are trademarks of AJA Video Systems, Inc.;
> ASIO is a trademark of Steinberg Media Technologies GmbH.

## Why

With AJA's own Windows software today:

- the AJA ASIO driver only captures: it has no outputs;
- the AJA WDM audio device only plays: it has no inputs;
- the buffer size cannot be changed and the latency is high.

## What it does

- **Inputs:** the Xpand's analog L/R (the default), AES 1-8, or the audio
  embedded in an SDI input (16 channels) or in the HDMI input (8).
- **Outputs:** AES 1-8, with the Xpand's analog output on 1-2; if asked,
  the SDI outputs too (16 channels) and the HDMI output (8).
- **Buffers** of 64 to 2048 samples at 48 kHz, 128 (2.7 ms) by default.
  The latencies it reports to the host include what the card and its
  converters add (measured on the KONA X: 208 samples, 4.3 ms, through
  the analog path, about 130 through AES), so recordings line up with
  nothing to set.
- **Beside other programs on the card:** it takes one audio system (the
  KONA X's second, which AJA's own drivers leave free) and puts everything
  back when the host lets it go. It was written to run in Resolume next
  to the [AJA Input](https://github.com/fiverecords/AJA-Input-FFGL) plugin
  capturing video on the same card. It reads its routing back once a
  second and says when another program changes it; and after a host that
  crashed, the next one takes the card over without a restart of the PC.
- A settings dialog: the host's "ASIO settings" button opens it, and for
  a host that has none, such as vMix, **AJA Duplex ASIO Settings** in the
  Start menu. A log of what it does, and two console tools:
  `duplex-asiotest`, a small host that checks the driver, and
  `duplex-probe`, which measures the card.

It is a user-mode DLL that the host loads into its own process. It talks
to AJA's kernel driver through libajantv2, the way any SDK program does:
it reads the card's capture buffer and writes its playback buffer
directly, block by block, following the card's read and write heads, so
video timing plays no part. It does not replace AJA's driver and needs no
kernel code.

## Where it stands

Version 1.0.1 (2026-11-18). 1.0.0 (2026-11-11) was the first one
published; 1.0.1 adds the settings for hosts with no settings button and
speaks of samples where it said frames, and plays as 1.0.0 does. What
follows was run on one KONA X with the Xpand (AJA's driver 18.0.0.29,
firmware revision 8) on Windows 11, with 1.0.0 and the versions that led
to it:

- The audio engine under the driver ran an hour at a 128-sample buffer
  without a lost block, a DMA error or a glitch, AES out looped to AES in
  and XLR out looped to XLR in.
- The driver ran in Resolume Arena with the AJA Input plugin open on the
  same card, ten minutes without a late output, and played and recorded
  in vMix; 1.0.0 has since worked there for several hours.
- Round trip at the defaults, XLR out to XLR in: 720 samples, 15.0 ms (a
  128-sample buffer and a 256-sample output guard, which gives a host's
  callback 8 ms, and 208 samples of the card and its converters). That is
  what the driver reports; a start comes out within 7 samples of it. A
  host with short callbacks can take the guard down: 32 gives about 10.3
  ms. vMix picks its own buffer, 1024 samples (21.3 ms), whatever the
  driver offers.
- After a program playing through the card was ended by force, the next
  one took the card over with no restart of the PC, and left it as it
  was before.
- Not tried on the card yet: the SDI and HDMI inputs and outputs. They
  are written and run on a simulated card only.
- Other AJA cards: the code asks the SDK what each card has, but only the
  KONA X with the Xpand has been tested, and the driver refuses a KONA X
  without it.

[docs/ROADMAP.md](docs/ROADMAP.md) has what was measured on the card and
what is left; [CHANGELOG.md](CHANGELOG.md), what each version changed.

## Installing

1. AJA's software for the card installed, and the card seen in the AJA
   Control Panel.
2. Get `AJA-Duplex-ASIO-<version>-win64.zip` from the
   [releases](https://github.com/fiverecords/AJA-ASIO-Duplex/releases), or
   build it (below), and unzip it anywhere.
3. With every host closed, right-click `install-driver.cmd`, **Run as
   administrator**. It copies the driver to
   `C:\Program Files\AJA Duplex ASIO` and registers it there, and adds
   **AJA Duplex ASIO Settings** to the Start menu.
4. Pick **AJA Duplex ASIO** as the audio device in the host.
5. Its settings (the input, the outputs, the buffer offered, the analog
   level...) open with the host's "ASIO settings" button. In a host with
   no such button, such as vMix, open **AJA Duplex ASIO Settings** from
   the Start menu: it shows the same settings, and a host that has the
   driver open takes the new ones when it is opened again.

Run the new version's `install-driver.cmd` to update it, and
`uninstall-driver.cmd` to take it away. Settings, channels, latencies,
the log and what to do when a host shows the driver greyed out are in
[docs/DRIVER.md](docs/DRIVER.md).

## Building

Windows 10/11 x64 with Visual Studio 2022, or only its free Build Tools,
with "Desktop development with C++" (it brings the compiler and CMake).
Then double-click `build.cmd`, or run it from a command prompt. The first
time it also fetches the AJA SDK into the folder next to this one, with git
or, without git, as a ZIP from GitHub:

```
<workspace>\AJA-ASIO-Duplex
<workspace>\libajantv2        (tag ntv2_18_0_0)
```

It builds the Release version, runs the unit tests and the driver on a
simulated card, and leaves:

- `build\Release\AJADuplexASIO.dll` (the driver),
  `AJADuplexASIOSettings.exe` (its settings, for hosts with no settings
  button), `duplex-asiotest.exe`, `duplex-probe.exe` and
  `duplex-tests.exe`;
- `dist\AJA-Duplex-ASIO-<version>-win64.zip`: all but the tests, with the
  install scripts, `card-session.cmd` and the documents, for the PC with
  the KONA X, which needs nothing installed but AJA's driver;
- `dist\AJA-Duplex-ASIO-<version>-win64\`: the same files, unpacked, for
  when that PC is this one. What a session with the card left in it
  (`reports\`, its ZIPs) is kept from one build to the next.

Hosts do not load what a build leaves: they load the copy that
`install-driver.cmd` put in Program Files. The build's last lines say
whether that copy is this build.

Everything it prints also goes to `build-log.txt`: if a step fails, that
file says why. `build.cmd -InstallTools` installs the Build Tools with
winget when Visual Studio is missing, `-Clean` rebuilds from scratch and
`-Config Debug` builds a debug version. Each push also builds on GitHub,
with the same script, and the run keeps the ZIP.

Everything that is not Windows-specific (the driver's core included,
without COM) also builds on Linux for development (`libudev-dev` is needed
by the SDK):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Checking it on a card

`card-session.cmd`, in the ZIP, walks through every test that was run on
the KONA X, step by step: what to cable, what to set up, and then the
run. [docs/CARD_SESSION.md](docs/CARD_SESSION.md) is the same list to
follow by hand. The quick ones, with the driver installed:

```
duplex-asiotest --installed          which DLL hosts load, and whether it is this build
duplex-asiotest --card --registered  the driver on the card, as a host opens it: XLR out L to XLR in L
```

Without a card, `duplex-asiotest` alone runs the driver on a simulated one
inside the DLL.

## duplex-probe

```
duplex-probe list
duplex-probe info  [--device <d>] [--label <text>] [--out <folder>]
duplex-probe heads [--device <d>] [--audio-system <n>] [--seconds <s>] [--simulate] ...
duplex-probe cost  [--device <d>] [--audio-system <n>] [--simulate] ...
duplex-probe loop  [--path aes|analog] [--frames <N>] [--guard <G>] [--seconds <s>] [--tasks oem|keep] [--simulate] ...
duplex-probe routing [--scan-select] [--tasks oem|keep] [--simulate] ...
duplex-probe watch [--seconds <s>] [--input analog|aes] [--keep] [--simulate] ...
```

The tool the driver's numbers come from. `info` changes nothing on the
card, so it can run next to a show. `heads`, `cost` and `loop` use one
audio system that nothing else is using and put it back as they found it;
`loop` and `routing` hold the card in OEM task mode meanwhile, so AJA's
services leave the routing alone. `loop` runs the driver's own audio
engine with AES out cabled to AES in, and checks what comes back; with
`--path analog`, XLR out cabled to XLR in, through the Xpand's converters.
`routing` plays a tone on each output and finds where each one comes back.
`watch` runs the audio system as the driver does, in silence, and prints
every change another program makes to its routing. `--simulate` runs them
against a simulated card, with no hardware. [docs/PROBE.md](docs/PROBE.md)
has each command.

## Reporting a problem

[Issues](https://github.com/fiverecords/AJA-ASIO-Duplex/issues). The
driver's log says what it did and why it refused: `driver.log` in
`%LOCALAPPDATA%\AJA Duplex ASIO` (type that in Explorer's address bar).
The log and `duplex-probe`'s reports name the card's serial number and
the programs that loaded the driver: look through them before posting
them.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). Copyright (c) 2026 Joaquin
Villodre. The driver is built from the ASIO SDK's headers under its GPLv3
licence option, so the DLL is distributed under GPLv3. Third-party code
and its notices are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
