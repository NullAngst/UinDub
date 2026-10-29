// SPDX-License-Identifier: GPL-3.0-or-later
// UinDub: post-processing for PingDub captures. GUI only; the command line
// takes --version, --help and --self-test (the CI hook), plus an optional
// file or project folder to open.
#include "core/Av.h"
#include "core/SelfTest.h"
#include "ui/App.h"
#include "ui/MainWindow.h"
#include "shared/Theme.h"

#include <QApplication>
#include <QIcon>

#include <cstdio>
#include <cstring>

namespace {

void usage() {
    std::printf(
        "UinDub %s: Edge Lock software TBC, deinterlacing, color, crop, cuts and encoding for PingDub captures.\n\n"
        "Usage: uindub [file or project folder]\n"
        "       uindub --self-test [--verbose]   build, analyze and render a synthetic clip; exit 0 on pass\n"
        "       uindub --version\n"
        "       uindub --help\n",
        UINDUB_VERSION);
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, verbose = false;
    const char* open = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--version")) {
            std::printf("UinDub %s\n", UINDUB_VERSION);
            return 0;
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!std::strcmp(a, "--self-test")) {
            selfTest = true;
        } else if (!std::strcmp(a, "--verbose")) {
            verbose = true;
        } else if (a[0] == '-' && a[1] == '-') {
            // Qt's own options (-platform and friends) use one dash and pass through.
            std::fprintf(stderr, "uindub: unknown option %s\n", a);
            usage();
            return 2;
        } else if (a[0] != '-') {
            open = a;
        }
    }
    ud::quietFfmpegLog();
    if (selfTest) return ud::runSelfTest(verbose);

    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QApplication::setApplicationName("UinDub");
    QApplication::setOrganizationName("UinDub");
    QApplication::setApplicationVersion(UINDUB_VERSION);
    QApplication::setDesktopFileName("io.github.nullangst.UinDub");
    QApplication::setWindowIcon(QIcon(":/uindub.svg"));

    ud::AppSettings& s = ud::AppSettings::instance();
    s.load();
    ud::ThemeManager& tm = ud::ThemeManager::instance();
    if (s.theme.isEmpty() || !tm.ids().contains(s.theme)) {
        s.theme = tm.systemDefault();
        s.save();
    }
    tm.apply(s.theme);

    ud::MainWindow w;
    w.show();
    if (open) w.openPath(QString::fromLocal8Bit(open));
    return QApplication::exec();
}
