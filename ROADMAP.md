# Roadmap

No dates and no promises. This is a hobby project of one person. The order below is rough.

## Transmit - in review, not published yet

Heartbeat, CQ and directed messages. A transmit engine exists and runs in the PC simulator. It is under a safety review and is **not part of this repository**; this release cannot transmit at all.

The rule it is being built to, and that will not change: the app transmits only when **the owner has armed the beacon on the radio AND a human has requested exactly that message**. Publication is planned for after the review is closed; no date.

## Text entry with the radio's own keyboard

The IC-705 has an on-screen keyboard (for example in the RTTY memory editor). Apps cannot use it today. See our request in [sq6emm/icap-sdk#4](https://github.com/sq6emm/icap-sdk/issues/4) (text entry is the first wish there). Free text from the radio itself depends on it.

## Terminal link

A way to read decoded messages, and later to type messages, on a small external device or a computer over the radio's network connection. Exists as an experiment in the simulator; not published.

## Generic ICAP host simulator - if there is interest

The simulator in `tests/js8sim.c` is tied to the JS8 app. The plan is to split its generic core (the fake ICAP service table, the virtual clock, the SD-card directory with the loader's file rules, audio from a file) out of the JS8 test harness into a stand-alone directory with a hello-world example, so that any ICAP app can be run on a PC. Related: [sq6emm/icap-sdk#1](https://github.com/sq6emm/icap-sdk/issues/1), opened by ok1cdj. If you would use it, say so in an issue; that decides whether it gets done. For now the simulator ships as it is, see [`docs/SIMULATOR.md`](docs/SIMULATOR.md).

## Head-to-head comparison with JS8Call

Run JS8Call and this receiver on the same audio and compare what each decodes. Not done yet. Until then nobody should assume this receiver hears what JS8Call hears.

## Moving the dictionary out of the Apps folder

The 257 dictionary files sit next to the apps today, and many files in that folder slow down the radio's app rescan. They should live somewhere else on the card, or in fewer files, as far as the loader's file service allows.

## Smaller items

- Submodes other than Normal.
- A CI job that runs `make fetch && make test` on a clean machine.
- Measurements on other IC-705 versions, if owners report them. (The IC-9700 is not supported by this build; it is packed for the IC-705 only.)
- A check of the built binary, not only of the sources, that no transmit or network service is called.
- Notes on the app window for an incomplete dictionary and for `JS8.CFG` entries that were ignored.
