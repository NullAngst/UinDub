// SPDX-License-Identifier: GPL-3.0-or-later
// A contact sheet of the finished file, laid out like PingDub's: a thumbnail
// every N seconds (30 by default), 320 pixels wide with the aspect corrected,
// the time under each, 6 across and 10 down per JPEG page. Each page has the
// tape's title and runtime centered along the top ("Title - 1:58:03").
// The thumbnails come from decoding the rendered file again, so the sheet
// shows exactly what was written, crop, color, scale and all.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ud {

struct ContactSheetOptions {
    std::string title;      // "" uses the file name
    double runtime = 0;     // seconds; 0 reads it from the file
    double interval = 30;   // seconds between thumbnails
    int columns = 6, rows = 10, tileWidth = 320;
};

struct ContactSheetResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> pages;
    int thumbs = 0;
};

// "<path without extension>.contact01.jpg" and so on.
std::string contactPagePath(const std::string& videoPath, int page);
// "Title - H:MM:SS"
std::string contactHeader(const std::string& title, double seconds);
// "H:MM:SS", whole seconds.
std::string runtimeString(double seconds);

// Reads `videoPath` back and writes the pages next to it. Pages left over
// from an earlier, longer render of the same name are removed.
// `progress` gets 0..1 and returns false to stop.
ContactSheetResult writeContactSheet(const std::string& videoPath, const ContactSheetOptions& o,
                                     const std::function<bool(double)>& progress = {});

}  // namespace ud
