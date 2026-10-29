# UinDub

UinDub turns a PingDub capture master (or any interlaced SD file) into a file you actually watch: it straightens wobbly lines with Edge Lock, a software TBC, then deinterlaces, fixes color and levels, crops, cuts and encodes.

It is the second half of [PingDub](https://github.com/NullAngst/PingDub). PingDub captures a lossless master and never touches it. UinDub opens that master read-only with its sidecars (chapters, captions, the capture log, checksums), keeps every setting in a project folder, and writes a new file. The master stays as it was.

## What it does

- **Edge Lock** (off by default). Measures each line's horizontal position against a reference edge, per field, and shifts it back. The reference is the left blanking border (one click), and optionally the right border and up to two edges you draw. Analyze writes the per-line offsets to the project; render applies them with Lanczos-3. It only ever shifts lines sideways, so it cannot invent picture.
- **Deinterlace.** QTGMC (bundled, Slower, double rate by default), bwdif, yadif, IVTC (fieldmatch + decimate) for film, or None.
- **Color.** Brightness, contrast, gamma, black and white point, saturation, hue, per-channel lift/gamma/gain, chroma delay and blur. All in float, then legalized. Per chapter if you want it.
- **Crop.** Free crop, snapped to what the output format needs, with a safe-crop guide from the Edge Lock shifts and PingDub's head-switching lines.
- **Trim and cut.** In and out points plus any number of cut spans. Audio is cut at the matching sample with a 5 ms crossfade; chapters and captions move with the picture.
- **Encode.** x265 (RF 15, slow, 10-bit 4:2:0) by default, or x264, SVT-AV1, FFV1. 4:2:2 for x265 and x264. FLAC audio by default, or Opus, AAC, or a straight PCM copy. MKV by default, MP4 if you need it (no FLAC or captions in MP4).
- **Themes.** Light, Dark, Verdigris, Kawaii Light and Kawaii Dark, in PingDub's theme format.

Output is `<source name>.uindub.mkv` in a folder you pick (the first render asks), with a `<source name>.uindub.json` sidecar holding every setting and the source's SHA-256, so any render can be redone exactly. Nothing is written next to the master unless you pick its folder.

## Prerequisites

- An x86-64 Linux desktop with glibc 2.35 or newer (Ubuntu 22.04, Kubuntu, Fedora 36, openSUSE Leap 15.5 or Tumbleweed, anything from 2022 on). X11 or Wayland.
- Working OpenGL. Any AMD, Intel or NVIDIA driver from the last decade is fine.
- Disk space for the output. An hour of 10-bit x265 at RF 15 is a few GB.

Nothing else. The AppImage carries Qt, FFmpeg, the encoders, Python, VapourSynth and every QTGMC plugin.

## Install

1. Download `UinDub-x86_64.AppImage` from the [latest release](https://github.com/NullAngst/UinDub/releases/latest).
2. Make it executable: `chmod +x UinDub-x86_64.AppImage`
3. Run it: `./UinDub-x86_64.AppImage`

It updates in place with AppImageUpdate or whichever AppImage manager you use, since the release carries a `.zsync` file.

## Using it

I'll walk through how I do a tape. Adapt the order if your tape is clean and doesn't need Edge Lock.

1. **File > Open** the capture's `.mkv`. If it's one segment of a split capture, UinDub offers to open all of them as one source. A project folder is made at `~/Videos/UinDub/<source name>/` and saves itself as you go.
2. **Source tab.** Check the field order line. UinDub compares the container tag, PingDub's JSON and a comb test on the first moving frames, and says which it's using. GV-USB2 captures from before PingDub's 2026-10-28 fix are tagged wrong; UinDub knows and uses the comb test.

   GET THE FIELD ORDER RIGHT. Wrong field order makes every motion judder back and forth, Edge Lock measures the wrong field, and no other setting fixes it. If the comb test and the tag disagree, step through a few fields with comma and period and look.

3. **Edge Lock tab** (skip if the picture doesn't wobble). Tick Edge Lock, park on a frame with picture reaching the left side, and click **Use left border**. If the right border is visible too, it's added. Press **F5** to analyze. The offset strip beside the preview shows the correction per line, and the overlay ticks show each measured line: green measured, amber filled in, red lost.
   - Where the border goes dark (black scenes, fades), Edge Lock eases off instead of guessing. If a scene still wobbles there, **Draw edge...** on a vertical edge in the picture (a door frame, a pole) and analyze again.
   - Right-click a span on the timeline to re-analyze it alone, mark it "don't correct", or carry a reference forward.
4. **Deinterlace tab.** QTGMC Slower at double rate is the default and what I use. Scrubbing previews with bwdif so it stays live; **View > Preview with QTGMC** shows the real thing on the paused frame. Pick IVTC for film transfers.
5. **Color tab.** Auto levels is a decent start. Watch the scopes dock (View > Scopes) and the clipped count at the bottom of the tab.
6. **Crop tab.** Drag the edges on the preview or type them. **Crop to the safe guide** trims exactly what Edge Lock shifting and head switching leave ragged.
7. **Trim and cut** on the timeline: **I** and **O** set in and out, **C** starts a cut and **C** again ends it. Shift-drag selects a span; it snaps to chapters and signal-loss marks (hold Alt to place it freely).
8. **Output tab**, then **F6**. I use x265 10-bit RF 15 in MKV with FLAC, which is the default. The first render asks where to put the file and remembers the folder. Renders go into the queue dock, so you can line up several and keep working.

Now there's a `<tape>.uindub.mkv` in your output folder, straightened, deinterlaced and cut, with its chapters and captions, and a `.uindub.json` that says exactly how it was made.

### Keys

Every one of these can be rebound in Settings > Preferences.

| Key | Does |
| --- | --- |
| Space | Play / pause |
| Left, Right | One frame back, forward |
| Comma, Period | One field back, forward |
| J, K, L | Shuttle back, stop, forward (press again to go faster) |
| I, O | In point, out point |
| C | Start a cut, press again to end it |
| F5 | Analyze (Edge Lock) |
| F6 | Render |
| Esc | Stop whatever is running |

Ctrl+F toggles field view, Ctrl+0/1/2/4 set the zoom (fit, 100, 200, 400 percent, nearest neighbor so you see real samples).

### Warble test

**Tools > Warble test** takes a clean clip and writes an FFV1 copy damaged with known per-line offsets (jitter, a crawling wobble, a top-of-frame hook, a field offset), as its own project. Analyze it and the status bar and Edge Lock tab show how far the result is from the truth, in pixels RMS. Use a real clean capture of a good tape. This is how to check Edge Lock on your own footage before a bad tape ever shows up.

## Things to know

- **QTGMC comes from havsfunc r33.** havsfunc dropped QTGMC after r33. The build takes the newest havsfunc release that still has it, so it's effectively frozen at r33 until someone brings it back. The plugins it calls (mvtools, znedi3, fmtconv, eedi3m and friends) are all built at their newest release. Two old keyword names in r33 are patched to the form current VapourSynth accepts; nothing else in it is changed.
- **Edge Lock was tuned on synthetic damage.** I don't have a warbly capture from this chain, so the defaults were tuned with the warble generator on synthetic clips, landing around 0.06 px RMS. Real tape will do worse than that. The warble test is there so you can measure it on yours, and the settings are all exposed.
- **Lanczos-3 rings on hard edges.** A shift by a fraction of a pixel on a full-swing step edge can move code values by up to about 1 on 8-bit. Real video edges are softer and come in well under that.
- **IVTC moves your in and out points inward** to the nearest whole film cycle, so a cut never splits a cadence. The `.uindub.json` lists the ranges that were actually kept.
- **The sidecar is `<name>.uindub.json`,** not `<name>.uindub.uindub.json`, even though the output is `<name>.uindub.mkv`.
- **No hardware encoders.** VAAPI on AMD has no RF-style constant quality, so an RF number would mean something else there.
- **No denoising.** QTGMC's noise processing is off. That's a different job for a different tool.
- **ffprobe reports 59.94 fps MKVs as 19001/317.** That's Matroska's 1 ms timestamps, the same as any FFmpeg-made MKV. Players go by the timestamps and play it right.

## Building from source

Everything is in one script, the same one CI runs:

```
bash packaging/build-appimage.sh
```

It needs GCC 12.3 or newer (VapourSynth builds kernels with `-march=znver4`; on Ubuntu 22.04 that means `apt install gcc-12 g++-12` and running with `CC=gcc-12 CXX=g++-12`), Ninja, nasm, patchelf, autotools, curl, Python 3 with venv, the FFTW and Python build headers, and the X11/Wayland/OpenGL development packages. The apt line in `.github/workflows/build.yml` is the full list for Ubuntu. It downloads and builds the newest release of each dependency; nothing is pinned. `USE_SYSTEM_QT=1` and `SYSTEM_CODECS=1` use your distro's Qt and encoders instead, which is quicker for a local build.

For working on the code itself, a plain CMake build runs fine against distro FFmpeg and Qt:

1. Get the VapourSynth headers (the `include` folder of a VapourSynth checkout, or `pip install vapoursynth` and use the package's `include` folder).
2. Configure: `cmake -S . -B build -G Ninja -DVAPOURSYNTH_INCLUDE=/path/to/vapoursynth/include`
3. Build: `cmake --build build`
4.5. QTGMC needs a Python with VapourSynth and the plugins installed. Point UinDub at it with `UINDUB_PYTHON=/path/to/python`, since a dev build has no bundled stack. Without it, QTGMC says it's unavailable and everything else works.
5. Test: `cd build && UINDUB_PYTHON=/path/to/python ctest`

`uindub --self-test` builds a 10 second synthetic capture, damages it, analyzes it, renders it with bwdif to x265 and QTGMC to x264, and checks every count. CI runs it on the finished AppImage. Set `UINDUB_WARBLE_CLIP=/path/to/clean.mkv` when running the tests to run the warble test on real footage.

## License

GPL-3.0-or-later. The AppImage includes x264, x265 and FFmpeg's GPL filters, so the whole bundle is GPL.
