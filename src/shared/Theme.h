// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
// Themes are token files (src/ui/themes/*.theme, plus any the user drops in
// ~/.config/uindub/themes/) that drive a QPalette and one stylesheet. The
// format is PingDub's, so a theme file copied over works as-is.
#pragma once

#include <QColor>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ud {

struct ThemeTokens {
    QString id;
    QString name;
    bool dark = false;
    QMap<QString, QString> tokens;
};

// Colors that keep their meaning in every theme.
namespace StatusColor {
inline const QColor Ok{0x3F, 0xB9, 0x50};
inline const QColor Warn{0xD2, 0x99, 0x22};
inline const QColor Bad{0xF8, 0x51, 0x49};
// The preview surround is always neutral gray, so tape color is never
// judged against a pink or teal frame.
inline const QColor PreviewSurround{0x5A, 0x5A, 0x5A};
}  // namespace StatusColor

class ThemeManager {
public:
    static ThemeManager& instance();
    QStringList ids() const;
    QString nameOf(const QString& id) const;
    // Applies to the whole application, live.
    void apply(const QString& id);
    QString current() const { return m_current; }
    // Light or Dark depending on the desktop's color scheme.
    QString systemDefault() const;
    QColor okColor() const;

private:
    ThemeManager();
    void load();
    QMap<QString, ThemeTokens> m_themes;
    QStringList m_order;
    QString m_current;
    QString m_qss;
};

}  // namespace ud
