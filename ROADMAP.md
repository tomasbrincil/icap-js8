# Roadmap

No dates and no promises. This is a hobby project of one person. The order below is rough.

## Done in 0.2.0

- Transmit: heartbeat, CQ, directed messages and free text, written on the radio's own keyboard (see the README). The rule stays: nothing goes on the air without the operator's own action on the radio - ENT on a text seen whole, the tap credit of the loader or the operator's arming.
- The dictionary in the card root instead of the Apps folder.

## Next for transmit

- Try on the radio what so far ran only in the simulator: CQ by the green frame, the HB beacon of `txhb=` over a longer time, the cancel and stop paths under real conditions.
- The keyboard on more firmware versions. The direct calls are tied to 705J 1.40 / SQ6EMM 20261119v14 J. A text-entry service of the loader would be the clean way; see our request in [sq6emm/icap-sdk#4](https://github.com/sq6emm/icap-sdk/issues/4).
- A short ramp at the end of an aborted frame (no key click).
- Heartbeats in JS8Call's 500-1000 Hz heartbeat sub-band.

## Experimental, in the code but not released

These exist in the development tree and partly in this source, run in the simulator, and are **not documented or supported** in this release:

- **Terminal link**: a small external keyboard and display (an M5 Cardputer) over the radio's network connection, which can show what the radio shows and hand a text to the radio, to be sent only by ENT on the radio's keyboard. Not in this repository; the released apps are built without it.
- **Views BAND / INBOX / HEARD** of the app window (chosen from the terminal), **ADIF log records**, **text presets** (`tx1=`..`tx4=`) and **keyer memories** read from a file written by another app (`MEM.APP`). The code is in `src/js8feat.c` and `shell/js8app_kbd.h`; whether and how they become operable from the radio alone is open.

## Generic ICAP host simulator - if there is interest

The simulator in `tests/js8sim.c` is tied to the JS8 app. The plan is to split its generic core (the fake ICAP service table, the virtual clock, the SD-card directory with the loader's file rules, audio from a file, the transmit and tap models) into a stand-alone directory with a hello-world example, so that any ICAP app can be run on a PC. Related: [sq6emm/icap-sdk#1](https://github.com/sq6emm/icap-sdk/issues/1), opened by ok1cdj. If you would use it, say so in an issue.

## Head-to-head comparison with JS8Call

Run JS8Call and this receiver on the same audio and compare what each decodes. Not done yet. Until then nobody should assume this receiver hears what JS8Call hears.

## Smaller items

- Submodes other than Normal.
- A CI job that runs `make fetch && make test` on a clean machine.
- Measurements on other IC-705 versions, if owners report them. (The IC-9700 is not supported by this build; it is packed for the IC-705 only.)
- PSKReporter spots through the firmware's `@SPOT`.
