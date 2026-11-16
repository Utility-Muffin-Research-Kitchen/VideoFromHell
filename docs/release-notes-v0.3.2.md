# Video From Hell 0.3.2

Video From Hell is an optional video player for Leaf on the Miniloong Pocket 1.
Install or update it through Pak Rat.

## Changes since 0.3.1

- Open on LoongOS 1.4. Video From Hell 0.3.1 couldn't start on that firmware,
  because LoongOS 1.4 replaced the copy of FFmpeg it used. Video From Hell now
  carries its own, so it plays the same on LoongOS 1.3 and 1.4, AV1 videos
  included.
- Show every frame. Videos with sound skipped about every other frame, for every
  format. They now play at their full frame rate.
- Keep 1080p AV1 and other videos without hardware decoding in step with the
  sound. They now decode on all four cores, where they used to fall up to a
  second behind.
- Resume exactly where you paused.

The update keeps your videos, watch history and queue. Video From Hell remains
optional and is not included in Leaf SD release ZIPs.
