// SPDX-License-Identifier: GPL-3.0-or-later
// File > Save project as: a folder inside the project is refused without
// writing anything (it used to copy itself into finish/finish/finish/...),
// and a normal move still carries every file.
#include "core/Synth.h"
#include "testing.h"

#include "core/Project.h"

#include <filesystem>
#include <fstream>

using namespace ud;
namespace fs = std::filesystem;

namespace {

Project makeProject(const std::string& folder) {
    Project p;
    p.folder = folder;
    std::string err;
    CHECK(p.save(err));
    std::ofstream(fs::path(folder) / "offsets.zst") << "offsets";
    fs::create_directories(fs::path(folder) / "preview-cache");
    std::ofstream(fs::path(folder) / "preview-cache" / "f0.bin") << "cache";
    return p;
}

size_t entries(const fs::path& dir) {
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) ++n;
    return n;
}

}  // namespace

TEST_CASE("move into an empty folder inside the project is refused") {
    const fs::path root = tempDir("project_move_inside");
    const fs::path proj = root / "tbw_tff";
    Project p = makeProject(proj.string());
    fs::create_directories(proj / "finish");  // what the folder dialog's New Folder makes
    const size_t before = entries(proj);

    std::string err;
    CHECK(!moveProject(p, (proj / "finish").string(), err));
    CHECK(!err.empty());
    CHECK(p.folder == proj.string());
    CHECK(fs::is_empty(proj / "finish"));
    CHECK(entries(proj) == before);
    CHECK(fs::exists(proj / "project.json"));
}

TEST_CASE("move into a deeper new folder inside the project, or onto itself, is refused") {
    const fs::path root = tempDir("project_move_deep");
    const fs::path proj = root / "clip";
    Project p = makeProject(proj.string());
    std::string err;
    CHECK(!moveProject(p, (proj / "a" / "b").string(), err));
    CHECK(!fs::exists(proj / "a"));
    CHECK(!moveProject(p, proj.string(), err));
    CHECK(!moveProject(p, proj.string() + "/", err));
    CHECK(!moveProject(p, (proj / "." / "x" / "..").string(), err));
    CHECK(p.folder == proj.string());
}

TEST_CASE("a sibling whose name starts with the project's name is not inside it") {
    const fs::path root = tempDir("project_move_prefix");
    const fs::path proj = root / "clip";
    Project p = makeProject(proj.string());
    std::string err;
    CHECK(moveProject(p, (root / "clip-final").string(), err));
    CHECK(p.folder == (root / "clip-final").string());
}

TEST_CASE("normal move carries every file and leaves nothing behind") {
    const fs::path root = tempDir("project_move_ok");
    const fs::path proj = root / "clip";
    Project p = makeProject(proj.string());
    const fs::path dest = root / "moved" / "clip";
    std::string err;
    CHECK(moveProject(p, dest.string(), err));
    CHECK(p.folder == dest.string());
    CHECK(!fs::exists(proj));
    CHECK(fs::exists(dest / "project.json"));
    CHECK(fs::exists(dest / "offsets.zst"));
    CHECK(fs::exists(dest / "preview-cache" / "f0.bin"));
}

TEST_CASE("move into a folder that is not empty is refused") {
    const fs::path root = tempDir("project_move_full");
    const fs::path proj = root / "clip";
    Project p = makeProject(proj.string());
    fs::create_directories(root / "busy");
    std::ofstream(root / "busy" / "keep.txt") << "x";
    std::string err;
    CHECK(!moveProject(p, (root / "busy").string(), err));
    CHECK(fs::exists(proj / "project.json"));
    CHECK(fs::exists(root / "busy" / "keep.txt"));
}

TEST_MAIN()
