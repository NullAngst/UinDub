// SPDX-License-Identifier: GPL-3.0-or-later
// App-wide settings: theme, key bindings, recent projects, window layout.
// Stored in ~/.config/uindub/uindub.ini; projects keep everything else.
#pragma once

#include <QByteArray>
#include <QKeySequence>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ud {

QString uiConfigDir();

struct KeyAction {
    QString id;
    QString label;
    QKeySequence fallback;
};
const QList<KeyAction>& keyActions();

struct AppSettings {
    QString theme;                    // "" follows the desktop on first run
    QMap<QString, QKeySequence> keys;  // action id -> binding
    QStringList recent;               // project folders
    QString lastOpenFolder;
    QString lastOutputFolder;
    QByteArray geometry, state;
    bool verifyBeforeRender = false;

    void load();
    void save() const;
    QKeySequence key(const QString& id) const;
    void addRecent(const QString& folder);
    static AppSettings& instance();
};

}  // namespace ud
