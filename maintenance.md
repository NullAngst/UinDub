# Maintenance

A check to run every couple of months, so UinDub keeps building and keeps doing what the README says. It takes about an hour, most of it waiting on CI.

Nothing in the build is pinned. Every run of `packaging/build-appimage.sh` fetches the newest release of Qt, FFmpeg, x264, x265, SVT-AV1, dav1d, Opus, Python, VapourSynth and the QTGMC plugins. So most maintenance is catching the day one of them changes something under us.

## Prerequisites

- A GitHub account with push access to NullAngst/UinDub.
- A Linux machine with the AppImage-sized disk space free (about 30 GB for a local build, less for just testing the release).
- A short real PingDub capture to test with. I keep a 2-minute clip from a tape with captions on it, since captions, cuts and audio together are where the muxing problems show up.

## 1. Did CI go green?

1. Open the Actions tab on GitHub and look at the newest run of "Build AppImage".
2. If nothing has run in a while, start one by hand: Actions > Build AppImage > Run workflow, since a green build from three months ago says nothing about today's upstream releases.
3. A red run usually means one upstream release changed. The log names the step: the `step "..."` lines in `build-appimage.sh` print what is being built and which tag.

Common ones:
- **FFmpeg renamed or removed an API.** UinDub compiles against whatever FFmpeg is newest. Look for deprecated calls first: `avcodec_get_supported_config` replaced `pix_fmts`, interlace flags moved into `AVFrame.flags` (see `src/core/Av.h`). Fix the code, not the version.
- **havsfunc.** QTGMC comes from havsfunc r33 and is effectively frozen there. If a VapourSynth release breaks r33's keyword names again, the patch in `build-appimage.sh` is where to add the rename.
- **Ubuntu 22.04 container.** The workflow builds in `ubuntu:22.04` for the glibc 2.35 floor. When GitHub or Docker drops that image, move to the next LTS and say in the README that the floor moved.

## 2. Run the tests locally

1. Get the VapourSynth headers: `pip install vapoursynth` and note where its `include` folder is.
2. Configure: `cmake -S . -B build -G Ninja -DVAPOURSYNTH_INCLUDE=/path/to/vapoursynth/include`
3. Build: `cmake --build build`
4. Test: `cd build && ctest --output-on-failure`
4.5. Without a VapourSynth stack the `qtgmc` test fails by design. `ctest -E qtgmc` runs the rest, or point `UINDUB_PYTHON` at a Python that has the plugins.

The tests worth reading when they fail:
- `test_upload`: the 1080p scale, BT.709 tags, the contact sheet, and the check that audio and video stay interleaved in the file with caption tracks on. That last one guards the caption interleaving fix from 2026.11.04; if it fails, do not ship.
- `test_detect`: idet, cropdetect and ebur128 on synthetic tape. A new FFmpeg that renames a `lavfi.*` metadata key breaks these first.
- `test_output`: every encoder and container.
- `test_project_move`: File > Save project as. Guards the 2026.11.13 fix where picking a folder inside the project copied the project into itself until the path ran out (`finish/finish/finish/...`). It is pure std::filesystem, so a libstdc++ change to `weakly_canonical` or `fs::copy` is what would break it.

## 3. Check the AppImage by hand

1. Download the newest `UinDub-x86_64.AppImage` from the Actions run or the release.
2. Run `./UinDub-x86_64.AppImage --self-test --verbose`. It should end with `self-test: passed`.
3. Open your real clip and run through it:
   - Analyze tab: tick everything that's enabled, **Analyze selected**. Each line should get a result. Check idet's field order agrees with the Source tab.
   - Timeline: zoom in with = until you see frame ticks, drag the in bracket, check the preview follows it.
   - Output: pick **YouTube 1080p: H.264**, render, and play the MP4 in mpv or VLC from start to end with the sound up. Listen for pops or dropouts. Check the contact sheet next to it shows the title and runtime centered along the top.
   - Render once more with the default x265 + FLAC in MKV and listen again.
4. Check `ffprobe -show_streams` on the 1080p file says `color_space=bt709` and `sample_aspect_ratio=1:1`.

## 4. Things that go stale on their own

- **YouTube's upload recommendations.** The presets follow https://support.google.com/youtube/answer/1722171 (H.264, AAC-LC 384 kb/s stereo, MP4). If that page changes codec or bitrate advice, update `applyUploadPreset` in `src/core/Output.cpp`, the tooltip in the Output tab, and the README's "Uploading to YouTube".
- **The -14 LUFS note** in the Analyze tab's loudness line. If YouTube changes its playback loudness, change the sentence in `src/ui/Tabs.cpp`.
- **SVT-AV1 presets.** The AV1 upload preset uses preset 5 and CRF 22. SVT-AV1 has shifted what its preset numbers mean between releases; if AV1 renders get much slower or bigger after an update, retune those two numbers.
- **GitHub Actions versions.** `actions/checkout`, `actions/upload-artifact` and `softprops/action-gh-release` print deprecation warnings in the run log well before they stop working. Bump the major version when they do.

## 5. Release

1. Add an entry at the top of `changes.md` for what changed.
2. Tag it: `git tag v<version>` then `git push origin v<version>`.
3. CI builds and attaches the AppImage and `.zsync` to the release.

Now UinDub is building against current upstream releases, the tests and the self-test pass, and a real tape renders with clean audio and a contact sheet.
