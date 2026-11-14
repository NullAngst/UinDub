# Changes

2026.11.13 - Bug fix

* Fixed: File > Save project as with a folder inside the project (the dialog opened inside it, so a New Folder there was the easy pick) made `finish/finish/finish/...` folders until the path hit the 4096-byte limit. rename refused the move, the code took any rename failure to mean "different file system", and the recursive copy fallback copied into its own output. A folder inside the project, or the project itself, is now refused with a message and nothing is written
* The copy fallback now only runs when rename says the target is on another file system (EXDEV); any other rename error is reported as is
* If that copy fails partway, the half-made copy is removed; the original was never touched
* Save project as opens the folder dialog next to the project instead of inside it
* New test: test_project_move

Reasons for changes: a project folder full of nested `finish` folders, thousands deep, reported from real use.

2026.11.04 - UX overhaul / Backend overhaul

* Timeline: a bar over it with play/pause, frame step, start and end, In, Out, Go In, Go Out, Clear, Cut, Cut Sel and Uncut buttons, each running the same action as its key (tooltips name the key)
* Timeline: zoom buttons, a zoom slider, Fit and Trim views, a scrollbar to pan when zoomed, and frame ticks once a frame is 6 px wide; zoom now goes down to 10 frames on screen
* Timeline: drag the in and out brackets and either edge of a cut; the preview follows the handle, and it snaps like Shift-drag does (Alt to place freely)
* Timeline: right-click to set in or out at the pointer, start or end a cut there, zoom to the selection or to fit
* New keys: X cut selection, Delete remove cut, Alt+X clear trim, Home and End, Shift+I and Shift+O go to in and out, = and - zoom, \ fit, Z zoom to the trim
* Status bar shows the in and out times and how much the trim keeps
* In and out points can no longer cross each other: setting one past the other moves the other back to the clip's edge
* Analyze tab: the comb test, FFmpeg idet (field order and telecined film), cropdetect (black borders), the auto levels measurement, ebur128 loudness with a sample peak and clip count, the PingDub checksum verify and Edge Lock's own analysis, picked with ticks and run in a row over the trim or the whole clip, with Apply per result, Apply all, or apply as they come in
* Tools > Analyze and the F5 key are now called Analyze Edge Lock, since the Analyze tab covers more
* Output: Resize to 720p, 1080p, 1440p or 2160p with square pixels at the picture's display shape, through swscale (Lanczos, Spline or Bicubic); HD sizes convert the matrix to BT.709 and tag the file BT.709; interlaced output (Deinterlace None) is never scaled
* Output: upload presets for YouTube at 1080p and 4K, with H.264 High and AAC 384 kb/s in MP4 (YouTube's recommendation) or SVT-AV1; encoders and presets this build lacks are greyed out
* Output: contact sheet of the finished file next to it (<name>.contact01.jpg and on), laid out like PingDub's, with "Title - Runtime" centered along the top of every page; the title is the tape label, else the file name; on by default, every 30 s, both settable
* The sidecar JSON lists the contact sheet pages
* Fixed: with caption tracks on, every caption packet went into the muxer when the file opened, which left FFmpeg's interleaver flushing in arrival order; audio landed in the file ahead of the video by the encoder's whole delay (388 ms in a test with x265 medium, more with slow presets and more threads). Caption packets now go in as the picture reaches them
* Fixed: a crossfade at a cut that fell across two audio reads sent the audio reader back across the cut, which means decoding from the start of the file each time; the crossfade is now mixed once and served from memory
* Self-test also renders a 1080p MP4 and checks the contact sheet
* New tests: test_upload (scale geometry, 1080p render, contact sheet, a/v interleave with captions) and test_detect (idet, cropdetect, ebur128 on synthetic tape)
* libswscale is now linked (it was already in the AppImage's FFmpeg)

Reasons for changes: trimming took keys only and the timeline zoom was hidden behind Ctrl+wheel; analysis was spread across four tabs; YouTube gives 480p uploads its lowest bitrate and smooths them, so tapes need an upscale before upload; the contact sheet from the capture showed the master, not what was actually rendered; and audio was reported popping and dropping on everything but Copy. I could not reproduce that with clean synthetic sources on FFmpeg 6.1 or 8.1.3 (the decoded samples and the per-stream timestamps come out right for FLAC, AAC, Opus and Copy alike), but the caption interleaving bug above is real, is in every audio codec's path, and gets worse with slow presets on many cores, so it is the first suspect.
