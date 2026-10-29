// SPDX-License-Identifier: GPL-3.0-or-later
// uindub --self-test: the CI hook. Builds a 10 second synthetic capture,
// damages it with known offsets, analyzes it, then renders it twice (bwdif
// to x265, QTGMC Draft to x264) and checks the counts. Exit code 0 on pass.
#pragma once

namespace ud {

int runSelfTest(bool verbose);

}  // namespace ud
