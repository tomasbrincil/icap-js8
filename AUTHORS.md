# Authors

This file says plainly who did what in *this* repository. The people and projects
whose work it is built on are in [`CREDITS.md`](CREDITS.md); licences are in
[`NOTICE.md`](NOTICE.md).

## Tomas Brincil, OK1BT

GitHub: [tomasbrincil](https://github.com/tomasbrincil)

Initiator, tester and maintainer. Had the idea, set the goals and the rules
(above all: nothing may transmit without the operator's action), made the
decisions, installed the builds on his own IC-705, ran them on the air, measured them
and reported back what the radio really did. He is the person responsible for
this project and the one to contact.

## Claude (Anthropic)

The code, the tests, the host simulator and the documentation in this repository
were written by Claude, Anthropic's AI model, working as a coding agent under
Tomas Brincil's direction in November 2026. That includes the
research of the JS8 protocol from the published JS8Call sources, the port of the
SDK's FT8 decoder to JS8, the message layer, the transmit engine and its safety
rules, the use of the radio's own keyboard (on the entry points found by BI1BIF),
and the reviews of its own work.

This is stated so that nobody mistakes the code for hand-written, peer-reviewed
radio software:

- No human has reviewed the code line by line.
- What was verified on a real radio is only what the README lists as tested on the
  radio (receive; and for 0.2.0 the keyboard, one-frame and two-frame transmissions).
  Everything else was tested in a PC simulator or not at all, and
  the README says which.
- An AI model can be confidently wrong. Where the behaviour of the radio's loader
  was not known, the code and the simulator contain assumptions; the known ones
  are listed in `docs/SIMULATOR.md`.

Claude is not a legal person, holds no copyright and takes no responsibility;
responsibility for publishing and maintaining this project lies with the
maintainer.

## Contributors

None yet. Contributions are welcome; they are accepted under the licence of the
file they change (see `NOTICE.md`).
