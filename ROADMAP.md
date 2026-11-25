# Roadmap

No dates and no promises. This is a hobby project of one person. The order below is rough.

## Done in 0.2.1

- CQ removed (owner decision): the green frame is not used.
- Clearer screen texts from a review with a first-time user in mind: `SENT: <text>`, `N parts: MODE > hold FT8 (red)`, decoded rows without SNR and offset. A printed operator's manual (English and Czech PDF).

## Done in 0.2.0

- Transmit: heartbeat, CQ (removed again in 0.2.1), directed messages and free text, written on the radio's own keyboard (see the README). The rule stays: nothing goes on the air without the operator's own action on the radio - ENT on a text seen whole, the tap credit of the loader or the operator's arming.
- The dictionary in the card root instead of the Apps folder.

## Next for transmit

- **A position beacon** (planned for the next release, designed with the owner on 24-25 November 2026): `#BCN` typed on the radio's keyboard switches a beacon on or off (only `#BCN` is recognised; any text starting with `#` is never sent). It goes out only while the red frame is on: a heartbeat at once and then every 10 minutes, and the GPS position as an 8-character locator (about 500 m) to APRS (`@APRSIS GRID`) with the first beacon and then only when the locator changes (no position without a valid GPS fix), on a 750 Hz tone (the heartbeat sub-band); two minutes after each beacon the app lists who answered. It replaces `txhb=`. Meant to run under the operator's control; unattended operation needs whatever your licence requires. The green frame is left to the radio, which lights it while a one-part reply is on the air.
- Try on the radio what so far ran only in the simulator: the HB beacon of `txhb=` over a longer time, the cancel and stop paths under real conditions.
- The keyboard on more firmware versions. The direct calls are tied to 705J 1.40 / SQ6EMM 20261119v14 J. A text-entry service of the loader would be the clean way; see our request in [sq6emm/icap-sdk#4](https://github.com/sq6emm/icap-sdk/issues/4).
- A short ramp at the end of an aborted part (no key click).

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
