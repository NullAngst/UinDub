// SPDX-License-Identifier: GPL-3.0-or-later
// A project is a folder: project.json with every setting, offsets.zst and
// tracks.zst from Analyze, and a preview cache. The master is only ever read.
#pragma once

#include "Color.h"
#include "Damage.h"
#include "Deinterlace.h"
#include "EdgeLock.h"
#include "Edit.h"
#include "Output.h"
#include "Source.h"

#include <string>
#include <vector>

namespace ud {

class JsonValue;

struct CropSettings {
    int left = 0, right = 0, top = 0, bottom = 0;
    bool squarePixels = false;
    bool zero() const { return !left && !right && !top && !bottom && !squarePixels; }
    bool operator==(const CropSettings& o) const {
        return left == o.left && right == o.right && top == o.top && bottom == o.bottom && squarePixels == o.squarePixels;
    }
};

struct Project {
    std::string folder;
    std::string sourcePath;
    bool joinSegments = true;
    std::string tapeLabel;

    // Source tab
    FieldOrder fieldOrder = FieldOrder::Unknown;  // what render uses; Unknown: not checked yet
    bool fieldOverride = false;                   // set by hand in the Source tab

    // Edge Lock tab
    bool edgeEnabled = false;
    EdgeLockSettings edge;
    std::vector<Reference> refs;
    std::vector<EdgeOverride> overrides;
    std::string analysisKey;                 // what the stored offsets were analyzed with
    std::vector<int64_t> detectedCuts;
    std::vector<std::pair<int, int64_t>> autoEnds;

    // Deinterlace, Color, Crop, Output tabs and the timeline
    DeintSettings deint;
    ColorSettings color;
    bool colorPerChapter = false;
    ChapterColors chapterColors;
    CropSettings crop;
    EditSettings edit;
    OutputSettings out;

    // Tools > Warble test: the damage this copy carries, for scoring.
    bool hasTruth = false;
    DamageModel truth;

    std::string projectFile() const;
    std::string offsetsPath() const;
    std::string tracksPath() const;
    std::string cacheDir() const;

    JsonValue settingsJson() const;  // everything but the folder
    bool fromJson(const JsonValue& j, std::string& err);
    bool save(std::string& err) const;
    static bool load(const std::string& path, Project& out, std::string& err);

    // The key Analyze results are stamped with: references, Edge Lock
    // settings that change measurement, and field order.
    std::string currentAnalysisKey() const;
    bool analysisCurrent() const;
    // Edge Lock will run at render: on, references set, analyzed.
    bool edgeActive() const;
    FieldOrder renderFieldOrder() const { return fieldOrder == FieldOrder::BFF ? FieldOrder::BFF : FieldOrder::TFF; }
    // Color for a source frame, honoring per-chapter settings.
    const ColorSettings& colorFor(int64_t sourceFrame, const std::vector<Chapter>& chapters) const;
};

// ~/Videos/UinDub/<source name>/ (XDG videos folder when set).
std::string defaultProjectFolder(const std::string& sourcePath);
// Moves a project folder (File > Save project as).
bool moveProject(Project& p, const std::string& newFolder, std::string& err);

// Settings as JSON (also written into renders and .uindub.json).
JsonValue edgeSettingsJson(const EdgeLockSettings& s);
EdgeLockSettings edgeSettingsFromJson(const JsonValue& j);

}  // namespace ud
