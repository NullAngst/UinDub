// SPDX-License-Identifier: GPL-3.0-or-later
#include "App.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace ud {

QString uiConfigDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.config";
    return base + "/uindub";
}

const QList<KeyAction>& keyActions() {
    static const QList<KeyAction> a = {
        {"play", QObject::tr("Play / pause"), QKeySequence(Qt::Key_Space)},
        {"prev_frame", QObject::tr("Back one frame"), QKeySequence(Qt::Key_Left)},
        {"next_frame", QObject::tr("Forward one frame"), QKeySequence(Qt::Key_Right)},
        {"prev_field", QObject::tr("Back one field"), QKeySequence(Qt::Key_Comma)},
        {"next_field", QObject::tr("Forward one field"), QKeySequence(Qt::Key_Period)},
        {"shuttle_back", QObject::tr("Shuttle back (J)"), QKeySequence(Qt::Key_J)},
        {"shuttle_stop", QObject::tr("Shuttle stop (K)"), QKeySequence(Qt::Key_K)},
        {"shuttle_fwd", QObject::tr("Shuttle forward (L)"), QKeySequence(Qt::Key_L)},
        {"mark_in", QObject::tr("Set in point"), QKeySequence(Qt::Key_I)},
        {"mark_out", QObject::tr("Set out point"), QKeySequence(Qt::Key_O)},
        {"mark_cut", QObject::tr("Mark a cut span from the playhead"), QKeySequence(Qt::Key_C)},
        {"analyze", QObject::tr("Analyze"), QKeySequence(Qt::Key_F5)},
        {"render", QObject::tr("Render"), QKeySequence(Qt::Key_F6)},
        {"stop", QObject::tr("Stop"), QKeySequence(Qt::Key_Escape)},
    };
    return a;
}

AppSettings& AppSettings::instance() {
    static AppSettings s;
    return s;
}

void AppSettings::load() {
    QSettings s(uiConfigDir() + "/uindub.ini", QSettings::IniFormat);
    theme = s.value("ui/theme").toString();
    recent = s.value("ui/recent").toStringList();
    lastOpenFolder = s.value("ui/last_open_folder", QDir::homePath()).toString();
    lastOutputFolder = s.value("ui/last_output_folder").toString();
    geometry = s.value("ui/geometry").toByteArray();
    state = s.value("ui/state").toByteArray();
    verifyBeforeRender = s.value("render/verify_source", false).toBool();
    keys.clear();
    s.beginGroup("keys");
    for (const KeyAction& a : keyActions()) {
        const QString v = s.value(a.id).toString();
        keys[a.id] = v.isEmpty() ? a.fallback : QKeySequence(v);
    }
    s.endGroup();
}

void AppSettings::save() const {
    QDir().mkpath(uiConfigDir());
    QSettings s(uiConfigDir() + "/uindub.ini", QSettings::IniFormat);
    s.setValue("ui/theme", theme);
    s.setValue("ui/recent", recent);
    s.setValue("ui/last_open_folder", lastOpenFolder);
    s.setValue("ui/last_output_folder", lastOutputFolder);
    s.setValue("ui/geometry", geometry);
    s.setValue("ui/state", state);
    s.setValue("render/verify_source", verifyBeforeRender);
    s.beginGroup("keys");
    for (auto it = keys.begin(); it != keys.end(); ++it) s.setValue(it.key(), it.value().toString());
    s.endGroup();
}

QKeySequence AppSettings::key(const QString& id) const {
    auto it = keys.find(id);
    if (it != keys.end()) return it.value();
    for (const KeyAction& a : keyActions())
        if (a.id == id) return a.fallback;
    return {};
}

void AppSettings::addRecent(const QString& folder) {
    recent.removeAll(folder);
    recent.prepend(folder);
    while (recent.size() > 12) recent.removeLast();
}

}  // namespace ud
