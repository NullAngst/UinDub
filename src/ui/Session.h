// SPDX-License-Identifier: GPL-3.0-or-later
// The open project and everything derived from it: the source description,
// the field order check, Analyze's offsets and tracks, the playhead. Every
// change saves the project a moment later.
#pragma once

#include "core/OffsetStore.h"
#include "core/Project.h"
#include "core/Source.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace ud {

enum Change : int {
    ChSource = 1 << 0,
    ChRefs = 1 << 1,
    ChEdge = 1 << 2,
    ChDeint = 1 << 3,
    ChColor = 1 << 4,
    ChCrop = 1 << 5,
    ChEdit = 1 << 6,
    ChOutput = 1 << 7,
    ChAnalysis = 1 << 8,
    ChAll = 0xFFFF,
};

class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;

    // A media file, a PingDub sidecar, a project.json or a project folder.
    bool open(const QString& path, bool joinSegments, QString& err);
    void close();
    bool isOpen() const { return m_open; }

    Project& project() { return m_project; }
    const Project& project() const { return m_project; }
    const SourceInfo& source() const { return m_source; }
    const FieldOrderCheck& fieldCheck() const { return m_check; }
    const TrackStore& tracks() const { return m_tracks; }
    const OffsetStore* offsets() const { return m_offsets && m_offsets->isOpen() ? m_offsets.get() : nullptr; }
    bool analyzed() const { return m_project.analysisCurrent(); }

    // Tell everyone what changed; the project saves shortly after.
    void changed(int what);
    void saveNow();
    void reloadAnalysis();
    void rerunFieldCheck();

    int64_t frame() const { return m_frame; }
    int field() const { return m_field; }
    void seek(int64_t frame, int field = 0);
    void stepField(int delta);

    // Last frame index, for the timeline.
    int64_t frames() const { return m_source.frames; }

signals:
    void opened();
    void closed();
    void projectChanged(int what);
    void positionChanged(qint64 frame, int field);
    void message(const QString& text);

private:
    bool openProjectFile(const QString& file, QString& err);
    bool openSourcePath(const QString& path, bool join, QString& err);
    bool finishOpen(QString& err);

    bool m_open = false;
    Project m_project;
    SourceInfo m_source;
    FieldOrderCheck m_check;
    TrackStore m_tracks;
    std::unique_ptr<OffsetStore> m_offsets;
    int64_t m_frame = 0;
    int m_field = 0;
    QTimer m_saveTimer;
};

}  // namespace ud
