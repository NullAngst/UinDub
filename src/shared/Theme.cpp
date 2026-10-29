// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
#include "Theme.h"

#include "ui/App.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>

namespace ud {

ThemeManager& ThemeManager::instance() {
    static ThemeManager m;
    return m;
}

ThemeManager::ThemeManager() { load(); }

static ThemeTokens readTheme(const QString& path) {
    ThemeTokens t;
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup("theme");
    for (const QString& k : s.childKeys()) t.tokens[k] = s.value(k).toString();
    t.id = QFileInfo(path).completeBaseName();
    t.name = t.tokens.value("name", t.id);
    t.dark = t.tokens.value("base") == "dark";
    return t;
}

void ThemeManager::load() {
    const QStringList builtin = {"light", "dark", "verdigris", "kawaii-light", "kawaii-dark"};
    for (const QString& id : builtin) {
        const QString p = ":/themes/" + id + ".theme";
        if (QFile::exists(p)) {
            m_themes[id] = readTheme(p);
            m_order << id;
        }
    }
    // Built-in files added to the resource folder without a code change.
    for (const QString& f : QDir(":/themes").entryList({"*.theme"})) {
        const QString id = QFileInfo(f).completeBaseName();
        if (!m_themes.contains(id)) {
            m_themes[id] = readTheme(":/themes/" + f);
            m_order << id;
        }
    }
    // User themes.
    QDir user(uiConfigDir() + "/themes");
    for (const QString& f : user.entryList({"*.theme"}, QDir::Files)) {
        ThemeTokens t = readTheme(user.filePath(f));
        if (!m_themes.contains(t.id)) m_order << t.id;
        m_themes[t.id] = t;
    }
    QFile q(":/themes/style.qss");
    if (q.open(QIODevice::ReadOnly)) m_qss = QString::fromUtf8(q.readAll());
}

QStringList ThemeManager::ids() const { return m_order; }
QString ThemeManager::nameOf(const QString& id) const { return m_themes.value(id).name; }

QString ThemeManager::systemDefault() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs == Qt::ColorScheme::Dark) return "dark";
    if (cs == Qt::ColorScheme::Light) return "light";
#endif
    const QColor w = QGuiApplication::palette().color(QPalette::Window);
    return w.lightness() < 128 ? "dark" : "light";
}

QColor ThemeManager::okColor() const {
    const auto it = m_themes.find(m_current);
    if (it == m_themes.end()) return StatusColor::Ok;
    return QColor(it->tokens.value("ok", "#3FB950"));
}

static QString mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t)
        .name();
}

void ThemeManager::apply(const QString& idIn) {
    QString id = m_themes.contains(idIn) ? idIn : systemDefault();
    if (!m_themes.contains(id)) id = m_order.isEmpty() ? QString() : m_order.first();
    if (id.isEmpty()) return;
    const ThemeTokens& t = m_themes[id];
    m_current = id;

    const QColor window(t.tokens.value("window", "#F6F6F5"));
    const QColor panel(t.tokens.value("panel", "#FFFFFF"));
    const QColor text(t.tokens.value("text", "#1D1D1F"));
    const QColor accent(t.tokens.value("accent", "#2F6FDB"));
    const QColor accent2(t.tokens.value("accent2", "#6B7280"));

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, panel);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, accent.lightness() > 150 ? QColor("#000000") : QColor("#FFFFFF"));
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::PlaceholderText, QColor(mix(text, window, 0.5)));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(mix(text, window, 0.55)));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(mix(text, window, 0.55)));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(mix(text, window, 0.55)));

    QMap<QString, QString> tok = t.tokens;
    tok["border"] = mix(panel, text, t.dark ? 0.22 : 0.16);
    tok["muted"] = mix(text, window, 0.42);
    tok["hover"] = mix(panel, accent, 0.12);
    tok["pressed"] = mix(panel, accent, 0.25);
    tok["accenttext"] = accent.lightness() > 150 ? "#000000" : "#FFFFFF";
    tok["accent2"] = accent2.name();
    if (!tok.contains("radius")) tok["radius"] = "4";
    QString qss = m_qss;
    for (auto it = tok.begin(); it != tok.end(); ++it) qss.replace("${" + it.key() + "}", it.value());

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setPalette(p);
    qApp->setStyleSheet(qss);
}

}  // namespace ud
