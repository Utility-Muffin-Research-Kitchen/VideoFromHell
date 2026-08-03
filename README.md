# Video From Hell

![Platform: Miniloong Pocket 1](https://img.shields.io/badge/platform-Miniloong%20Pocket%201-7FB069?labelColor=0F160E)
![License: MIT](https://img.shields.io/github/license/Utility-Muffin-Research-Kitchen/VideoFromHell?color=7FB069&labelColor=0F160E)

A local video player for [Leaf](https://github.com/Utility-Muffin-Research-Kitchen/Leaf), the custom firmware for the Miniloong Pocket 1. It is a native [Catastrophe](https://github.com/Utility-Muffin-Research-Kitchen/Catastrophe) app, packaged as a Leaf `.pak` and named after Frank Zappa's home-video release.

> **Internal testing only.** Video From Hell is not published to Pak Rat and has no public release archive yet. The release workflow is present for later human-initiated releases, but no version tag will be created by this project.

## What it does

Browse one merged local library spanning both SD cards and Leaf's recorded-gameplay folder, then play films with a console-style on-screen display. The three top-level views are Continue Watching, Recently Added, and Folders. The player resumes meaningful progress automatically, supports a durable queue, honours anamorphic pixel aspect ratios, supports fit/fill/stretch framing, and keeps the MLP1 backlight awake during active playback.

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
| Up / Down | move selection | move pinned OSD focus |
| Left / Right | jump by letter | seek −/+ 10 seconds, or move/scrub pinned OSD focus |
| A | open folder / play video | play / pause, or activate the pinned OSD control |
| B | up one folder | close submenu, close pinned OSD, or return to browser (position saved) |
| X | open item actions (play, queue, info, history) | play / pause |
| Y | — | open/pin or hide the on-screen display |
| L1 / R1 | switch top-level views | previous / next (queue-aware when the current video is queued) |
| L2 / R2 | — | hold to seek −/+ |
| SELECT | rescan library | toggle external subtitles |
| Stick click | — | cycle Fit / Fill / Stretch |
| MENU | quit | quit |

Transport actions reveal the title, elapsed/total time, scrub bar, active output, and playback status for three seconds; Y keeps the interactive controls visible. The pinned surface includes previous/rewind/play/forward/next, Queue, Subtitles, Aspect, More, and Video Information. Queue offers direct play, removal, reordering, and confirmed clear. At EOF VFH asks whether to play the next queued video, replay, or return to the library; it never advances silently. Chapters appear under More when the container supplies them. Resume is automatic; positions at or below 30 seconds are ignored and progress at 90% is marked watched.

## Video location

Put videos under `Videos/` at the root of either SD card. On current Leaf builds the ordered `$VIDEO_PATHS` list identifies both roots and VFH merges them into one library. Equal folders coalesce; duplicate filenames retain an SD label. A missing card stays visible only where necessary—for example, its Continue Watching and queue entries are marked unavailable instead of being silently removed. Leaf's primary-card `$RECORDINGS_PATH` is also exposed as the `Recorded Gameplay` virtual folder; finalized MP4s are preferred over source MKVs and in-progress/scratch artifacts are hidden until a manual rescan.

For an older launcher payload or a direct pak launch, the player falls back from `$VIDEO_PATHS` to `$VIDEO_PATH`, then `$SDCARD_PATH/Videos`, then `./Videos`. `RECORDINGS_PATH` similarly falls back to `$SDCARD_PATH/Recordings`, then `./Recordings`. It opens cleanly with an empty browser if none of those paths exists. Posters prefer an exact-stem local `.jpg`, `.jpeg`, or `.png` sidecar, then a `poster.*` image in a single-video folder, then `folder.*` for folder presentation, embedded artwork, and finally a generated frame near 10% of the film (with a dark-frame fallback near 25%). Generated thumbnails and finite failure markers are stored under `$USERDATA_PATH/VideoFromHell/thumbs-v2/`. Queue, resume, watched, duration, and last-played state share the atomic `$USERDATA_PATH/VideoFromHell/playback-v2.json` store; a prior `resume.json` is imported on first use.

## Subtitles

External SubRip sidecars are supported. Place either of these beside a film (matching is case-insensitive):

```text
Film.mkv
Film.srt
Film.en.srt
```

When a sidecar is found it starts enabled; SELECT toggles it. The renderer supports UTF-8 text and uses an eight-direction outline so captions remain legible over bright video. Embedded subtitles, ASS/SSA styling, and audio-track selection are not in this first version. Playback speed is intentionally not exposed: the target image has no `libavfilter`, so VFH cannot offer a measured pitch-preserving path without compromising A/V sync.

## Build and verify

```sh
make -C VideoFromHell test
make -C VideoFromHell package-smoke
```

`make test` runs metadata validation plus source resolution, merged-library, artwork, OSD, resume/playback-store, queue, live-status IPC, and SRT parser suites. `make package-smoke` builds the aarch64 package in the shared toolchain container, creates a deterministic `build/mlp1/VideoFromHell.pak.zip`, and verifies its contents, modes, manifest, and checksum.

Useful narrower targets:

```sh
make -C VideoFromHell mlp1
make -C VideoFromHell probe-mlp1
make -C VideoFromHell player-smoke-mlp1
make -C VideoFromHell media-smoke-mlp1
```

`vfh-probe`, `vfh-player-smoke`, and `vfh-media-smoke` are target-only acceptance tools, intentionally outside the pak. The media smoke tool exercises VFH's own metadata and lazy-poster path, rather than an external `ffmpeg` command. Regenerate the FFmpeg and MPP link stubs after changing their symbol surface with `./scripts/build-ffmpeg-link-stubs.sh`.

## Credits

FFmpeg is dynamically linked to the LGPL components already shipped by the device. Rockchip MPP is dynamically linked to the device vendor runtime. Catastrophe and Leaf are UMRK projects; see [LICENSE](LICENSE) for this app's MIT license.
