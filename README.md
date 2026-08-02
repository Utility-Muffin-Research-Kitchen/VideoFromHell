# Video From Hell

![Platform: Miniloong Pocket 1](https://img.shields.io/badge/platform-Miniloong%20Pocket%201-7FB069?labelColor=0F160E)
![License: MIT](https://img.shields.io/github/license/Utility-Muffin-Research-Kitchen/VideoFromHell?color=7FB069&labelColor=0F160E)

A local video player for [Leaf](https://github.com/Utility-Muffin-Research-Kitchen/Leaf), the custom firmware for the Miniloong Pocket 1. It is a native [Catastrophe](https://github.com/Utility-Muffin-Research-Kitchen/Catastrophe) app, packaged as a Leaf `.pak` and named after Frank Zappa's home-video release.

> **Internal testing only.** Video From Hell is not published to Pak Rat and has no public release archive yet. The release workflow is present for later human-initiated releases, but no version tag will be created by this project.

## What it does

Browse video folders on either SD card, select a film, and play it with a console-style on-screen display. The player remembers a cleanly stopped film, honours anamorphic pixel aspect ratios, supports fit/fill/stretch framing, and keeps the MLP1 backlight awake during active playback.

H.264 and HEVC video use the MLP1's Rockchip MPP hardware decoder. MP4 packets are converted to Annex-B for MPP, decoded to NV12, and uploaded directly to an SDL NV12 texture. Other formats retain the FFmpeg software decoder path when the device can open them. Audio is decoded through FFmpeg and output over ALSA, following Leaf's current sound route.

The pak contains neither FFmpeg nor Rockchip MPP. It links against the exact vendor FFmpeg 4.4 and MPP SONAMEs already present on the device; the compact libraries in `third_party/*/stub` are link-time metadata only.

## Install

### Internal testing

With `VideoFromHell`, `Leaf`, `Catastrophe`, `Jawaka`, and `mlp1-toolchain` as sibling checkouts, build and explicitly stage the pak:

```sh
make -C VideoFromHell package-smoke
make -C Leaf stage-app APP=VideoFromHell DEVICE=mlp1
```

This is an explicit developer stage only. Video From Hell is intentionally excluded from Leaf's default payload, managed-app list, and end-user release ZIPs.

To test Pak Rat metadata without publishing the app, generate a local feed:

```sh
python3 Leaf/scripts/pakrat-local-feed.py --app-dir VideoFromHell
```

### When public releases begin

The eventual user-facing route will be **Menu → Actions → Pak Rat**. A release will appear in the Apps tab after download and install. Until a human deliberately creates a matching `v0.*` tag, Pak Rat has nothing to install.

## Controls

| Button | Browser | Playing |
| --- | --- | --- |
| Up / Down | move selection | — |
| Left / Right | jump by letter | seek −/+ 10 seconds |
| A | open folder / play video | play / pause |
| B | up one folder / quit source chooser | return to browser (position saved) |
| X | — | play / pause |
| Y | — | pin / hide the on-screen display |
| L1 / R1 | — | previous / next video in this folder |
| L2 / R2 | — | hold to seek −/+ |
| SELECT | — | toggle external subtitles |
| Stick click | — | cycle Fit / Fill / Stretch |
| MENU | quit | quit |

Transport actions reveal the title, elapsed/total time, scrub bar, and aspect mode for three seconds; Y keeps it visible. If a remembered position is meaningful, opening a film offers **Resume** or **Start over**.

## Video location

Put videos under `Videos/` at the root of either SD card. On current Leaf builds the ordered `$VIDEO_PATHS` list identifies both roots, so the source chooser labels them `SD1`, `SD2`, and so on. A missing secondary card is skipped without hiding the primary card.

For an older launcher payload or a direct pak launch, the player falls back from `$VIDEO_PATHS` to `$VIDEO_PATH`, then `$SDCARD_PATH/Videos`, then `./Videos`. It opens cleanly with an empty browser if none of those paths exists. The app stores poster thumbnails under `$USERDATA_PATH/VideoFromHell/thumbs/` and resume positions in `$USERDATA_PATH/VideoFromHell/resume.json`.

## Subtitles

External SubRip sidecars are supported. Place either of these beside a film (matching is case-insensitive):

```text
Film.mkv
Film.srt
Film.en.srt
```

When a sidecar is found it starts enabled; SELECT toggles it. The renderer supports UTF-8 text and uses an eight-direction outline so captions remain legible over bright video. Embedded subtitles, ASS/SSA styling, and track selection are not in this first version.

## Build and verify

```sh
make -C VideoFromHell test
make -C VideoFromHell package-smoke
```

`make test` runs metadata validation plus source-resolution, resume-store, and SRT parser suites. `make package-smoke` builds the aarch64 package in the shared toolchain container, creates a deterministic `build/mlp1/VideoFromHell.pak.zip`, and verifies its contents, modes, manifest, and checksum.

Useful narrower targets:

```sh
make -C VideoFromHell mlp1
make -C VideoFromHell probe-mlp1
make -C VideoFromHell player-smoke-mlp1
```

`vfh-probe` and `vfh-player-smoke` are hardware-decode acceptance tools, intentionally outside the pak. Regenerate the FFmpeg and MPP link stubs after changing their symbol surface with `./scripts/build-ffmpeg-link-stubs.sh`.

## Credits

FFmpeg is dynamically linked to the LGPL components already shipped by the device. Rockchip MPP is dynamically linked to the device vendor runtime. Catastrophe and Leaf are UMRK projects; see [LICENSE](LICENSE) for this app's MIT license.
