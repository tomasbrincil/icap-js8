# Photos

Photographs of the author's IC-705 and the preview of the manual (J, custom firmware 20261119v14 J) on 7.078 MHz (40 m), taken by Tomas Brincil OK1BT. They are part of the documentation and are covered by `LICENSE` / `NOTICE.md` like the rest of it. The captions are in the README under "Photos".

| File | Date | Shows |
|---|---|---|
| `js8-sent.jpg` | 24 Nov 2026 | build 2858 (the build before release 0.2.1) on the radio: a one-part reply on the tap credit, `1900 TX 1500 1/1 2M1DHG SNR -1`, then `SENT: 2M1DHG SNR -1` |
| `js8-hb-replies.jpg` | 24 Nov 2026 | the DECODE screen of release 0.2.0: an HB typed on the radio's keyboard, sent on the tap credit, and six stations answering it |
| `js8-keyboard.jpg` | 24 Nov 2026 | the radio's own keyboard opened by the app, prefilled with a station's call and a reply typed after it |
| `js8-two-frames.jpg` | 24 Nov 2026 | a two-frame reply sent in consecutive slots under one red-frame arming (a build shortly before 0.2.0) |
| `js8-decode.jpg` | 23 Nov 2026 | release 0.1.0, receive only: the DECODE screen with four rows |
| `js8-app-window.jpg` | 23 Nov 2026 | release 0.1.0: the app window |
| `manual-preview.jpg`, `manual-preview-dark.jpg` | 25 Nov 2026 | not photos: nine pages of the operator's manual 0.2.1 on a light and a dark background (made with `docs/manual/nahled.py` of the development tree); the screen pictures in it are renderings of the IC-705 user interface (`NOTICE.md`, section 4) |

The call signs that can be read in the photos are those of stations heard on the air in public transmissions; nothing else about them is known or stated here.

## Rules for a photo in this folder

- JPEG only, named `docs/img/<lower-case-name>.jpg`, at most 400 kB.
- **No metadata.** The file must hold the picture and nothing else: a plain JFIF header, no Exif (camera, time, position), no XMP, ICC or IPTC block, no comment segment, nothing after the end of the image. The export check walks the segments of every photo byte by byte and refuses the file otherwise; every photo in this folder passed it.
- Before adding a photo, look at it for anything that should not be public: position, serial numbers, a screen of another device, other people's call signs in a context they did not choose.
