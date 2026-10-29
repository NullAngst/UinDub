// SPDX-License-Identifier: GPL-3.0-or-later
// The comb test detects TFF and BFF on synthetic motion; a GV-USB2 capture
// from before the 2026-10-28 driver build is flagged.
#include "core/Synth.h"
#include "testing.h"

#include <cstdio>

using namespace ud;

TEST_CASE("comb test finds the real order") {
    for (FieldOrder truth : {FieldOrder::TFF, FieldOrder::BFF}) {
        const std::string dir = tempDir("field_order");
        SynthScene scene;
        SynthFile f;
        f.path = dir + "/tape.mkv";
        f.frames = 40;
        f.truth = truth;
        f.tag = truth;
        f.driver = "cx231xx";
        std::string err;
        CHECK(writeSynthCapture(f, scene, DamageModel::none(), err));
        SourceInfo s;
        CHECK(probeSource(f.path, true, s, err));
        SourceReader r;
        CHECK(r.open(s, err));
        double conf = 0;
        const FieldOrder got = combTest(r, s, 0, 30, &conf);
        std::printf("  truth %s: comb says %s (confidence %.2f)\n", fieldOrderName(truth), fieldOrderName(got), conf);
        CHECK(got == truth);
        const FieldOrderCheck c = checkFieldOrder(r, s);
        CHECK(c.chosen == truth);
        CHECK(!c.gvusb2Trap);
    }
}

TEST_CASE("GV-USB2 before the fixed driver: tagged BFF, really TFF") {
    const std::string dir = tempDir("field_order_trap");
    SynthScene scene;
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = 40;
    f.truth = FieldOrder::TFF;
    f.tag = FieldOrder::BFF;
    f.driver = "gvusb2";
    f.captureStart = "2026-10-27T21:14:03";
    std::string err;
    CHECK(writeSynthCapture(f, scene, DamageModel::none(), err));
    SourceInfo s;
    CHECK(probeSource(f.path, true, s, err));
    CHECK(s.fieldTag == FieldOrder::BFF);
    SourceReader r;
    CHECK(r.open(s, err));
    const FieldOrderCheck c = checkFieldOrder(r, s);
    CHECK(c.gvusb2Trap);
    CHECK(c.chosen == FieldOrder::TFF);
    CHECK(c.comb == FieldOrder::TFF);
    CHECK(!c.notes.empty());
    for (const std::string& n : c.notes) std::printf("  note: %s\n", n.c_str());
    // After the fix date the tag is trusted.
    CHECK(!gvusb2TagIsWrong("gvusb2", "2026-10-28T09:00:00"));
    CHECK(gvusb2TagIsWrong("gvusb2", "2026-10-27T23:59:59"));
    CHECK(!gvusb2TagIsWrong("em28xx", "2025-01-01T00:00:00"));
}

TEST_CASE("disagreement between tag and comb test is reported") {
    const std::string dir = tempDir("field_order_warn");
    SynthScene scene;
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = 40;
    f.truth = FieldOrder::BFF;
    f.tag = FieldOrder::TFF;
    f.driver = "cx231xx";
    std::string err;
    CHECK(writeSynthCapture(f, scene, DamageModel::none(), err));
    SourceInfo s;
    CHECK(probeSource(f.path, true, s, err));
    SourceReader r;
    r.open(s, err);
    const FieldOrderCheck c = checkFieldOrder(r, s);
    CHECK(c.chosen == FieldOrder::TFF);  // the tag stands; the user decides
    bool warned = false;
    for (const std::string& n : c.notes) warned |= n.find("comb test") != std::string::npos;
    CHECK(warned);
}

TEST_MAIN()
