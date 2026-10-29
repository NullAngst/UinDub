// SPDX-License-Identifier: GPL-3.0-or-later
#include "Session.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace ud {

Session::Session(QObject* parent) : QObject(parent) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Session::saveNow);
}

Session::~Session() { saveNow(); }

void Session::close() {
    if (!m_open) return;
    saveNow();
    m_open = false;
    m_project = Project{};
    m_source = SourceInfo{};
    m_check = FieldOrderCheck{};
    m_tracks = TrackStore{};
    m_offsets.reset();
    m_frame = 0;
    m_field = 0;
    emit closed();
}

bool Session::open(const QString& pathIn, bool join, QString& err) {
    close();
    const QFileInfo fi(pathIn);
    if (fi.isDir()) return openProjectFile(QDir(pathIn).filePath("project.json"), err);
    if (fi.fileName() == "project.json") return openProjectFile(pathIn, err);
    return openSourcePath(pathIn, join, err);
}

bool Session::openProjectFile(const QString& file, QString& err) {
    std::string e;
    Project p;
    if (!Project::load(file.toStdString(), p, e)) {
        err = QString::fromStdString(e);
        return false;
    }
    m_project = p;
    if (!probeSource(m_project.sourcePath, m_project.joinSegments, m_source, e)) {
        err = tr("The project's source cannot be opened: %1").arg(QString::fromStdString(e));
        return false;
    }
    return finishOpen(err);
}

bool Session::openSourcePath(const QString& path, bool join, QString& err) {
    std::string e;
    SourceInfo s;
    if (!probeSource(path.toStdString(), join, s, e)) {
        err = QString::fromStdString(e);
        return false;
    }
    // A project for this source already in its default folder opens as is.
    const std::string folder = defaultProjectFolder(s.segments.size() > 1 ? s.basePath : path.toStdString());
    Project existing;
    if (QFileInfo::exists(QString::fromStdString(folder + "/project.json")) && Project::load(folder, existing, e)) {
        const QString a = QFileInfo(QString::fromStdString(existing.sourcePath)).canonicalFilePath();
        const QString b = QFileInfo(path).canonicalFilePath();
        if (a == b && existing.joinSegments == join) {
            m_project = existing;
            m_source = s;
            return finishOpen(err);
        }
    }
    Project p;
    p.folder = folder;
    // A second, different source with the same name gets its own folder.
    for (int n = 2; QFileInfo::exists(QString::fromStdString(p.folder + "/project.json")); ++n) p.folder = folder + "-" + std::to_string(n);
    p.sourcePath = path.toStdString();
    p.joinSegments = join;
    p.tapeLabel = s.tapeLabel;
    m_project = p;
    m_source = s;
    return finishOpen(err);
}

bool Session::finishOpen(QString& err) {
    std::string e;
    SourceReader r;
    if (!r.open(m_source, e)) {
        err = QString::fromStdString(e);
        return false;
    }
    m_check = checkFieldOrder(r, m_source);
    if (!m_project.fieldOverride) m_project.fieldOrder = m_check.chosen;
    m_open = true;
    m_frame = std::clamp<int64_t>(m_project.edit.in, 0, std::max<int64_t>(0, m_source.frames - 1));
    m_field = 0;
    reloadAnalysis();
    saveNow();
    emit opened();
    emit projectChanged(ChAll);
    emit positionChanged(m_frame, m_field);
    for (const std::string& n : m_check.notes) emit message(QString::fromStdString(n));
    return true;
}

void Session::rerunFieldCheck() {
    if (!m_open) return;
    std::string e;
    SourceReader r;
    if (!r.open(m_source, e)) return;
    m_check = checkFieldOrder(r, m_source);
    if (!m_project.fieldOverride) m_project.fieldOrder = m_check.chosen;
    changed(ChSource);
}

void Session::reloadAnalysis() {
    m_offsets.reset();
    m_tracks = TrackStore{};
    if (!m_project.analysisCurrent()) return;
    std::string e;
    auto st = std::make_unique<OffsetStore>();
    if (st->open(m_project.offsetsPath(), e)) m_offsets = std::move(st);
    m_tracks.load(m_project.tracksPath(), e);
}

void Session::changed(int what) {
    if (!m_open) return;
    if (what & ChAnalysis) reloadAnalysis();
    emit projectChanged(what);
    m_saveTimer.start();
}

void Session::saveNow() {
    m_saveTimer.stop();
    if (!m_open) return;
    std::string e;
    if (!m_project.save(e)) emit message(tr("Could not save the project: %1").arg(QString::fromStdString(e)));
}

void Session::seek(int64_t frame, int field) {
    if (!m_open) return;
    const int64_t f = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, m_source.frames - 1));
    const int fl = std::clamp(field, 0, 1);
    if (f == m_frame && fl == m_field) return;
    m_frame = f;
    m_field = fl;
    emit positionChanged(m_frame, m_field);
}

void Session::stepField(int delta) {
    int64_t f = m_frame;
    int fl = m_field + delta;
    while (fl > 1) {
        fl -= 2;
        ++f;
    }
    while (fl < 0) {
        fl += 2;
        --f;
    }
    if (f < 0) {
        f = 0;
        fl = 0;
    }
    seek(f, fl);
}

}  // namespace ud
