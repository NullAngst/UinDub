// SPDX-License-Identifier: GPL-3.0-or-later
// Builds preview frames on its own thread so scrubbing never blocks the
// window: decode, Edge Lock, bwdif (QTGMC on request, on a paused frame),
// color, scopes, and the Edge Lock overlay measured on the shown field.
#pragma once

#include "ScopeTypes.h"
#include "core/OffsetStore.h"
#include "core/Pipeline.h"
#include "core/Project.h"
#include "core/Source.h"

#include <QMutex>
#include <QObject>
#include <QWaitCondition>

#include <atomic>
#include <map>
#include <memory>
#include <thread>

namespace ud {

struct OverlayRef {
    int slot = 0;
    RefKind kind = RefKind::Drawn;
    RefState state = RefState::Inactive;
    std::vector<double> pos, conf;  // per field line; NaN where not measured
    std::vector<PointF> shape;      // the span's drawn shape, moved by the real motion
};

struct PreviewFrame {
    int64_t frame = 0;
    int field = 0;      // temporal field shown (0 first)
    int parity = 0;     // its parity (0 top)
    bool fieldView = false;
    bool qtgmc = false;
    int width = 0, height = 0;
    Rational sar{10, 11};
    std::vector<uint8_t> y, u, v;     // processed, 8-bit 4:4:4
    std::vector<uint8_t> sy, su, sv;  // source, the same size
    ScopeFrame scope, scopeSource;
    std::shared_ptr<LumaPlane> luma, lumaSource;
    double clipped = 0;
    FrameOffsets offsets;
    bool haveOffsets = false;
    std::vector<OverlayRef> overlay;
    QString error;
};
using PreviewFramePtr = std::shared_ptr<const PreviewFrame>;

class PreviewEngine : public QObject {
    Q_OBJECT
public:
    explicit PreviewEngine(QObject* parent = nullptr);
    ~PreviewEngine() override;

    void setSource(const SourceInfo& s);
    void setProject(const Project& p, int changes);
    void clear();
    // Only the newest request is worked on; older ones are dropped.
    void request(int64_t frame, int field, bool fieldView, bool qtgmc = false);

signals:
    void ready(ud::PreviewFramePtr frame);

private:
    void run();
    void build(int64_t frame, int field, bool fieldView, bool qtgmc);
    bool raw(int64_t n, std::shared_ptr<Picture>& out);
    bool corrected(int64_t n, std::shared_ptr<Picture>& out);

    std::thread m_thread;
    QMutex m_m;
    QWaitCondition m_cv;
    bool m_stop = false;
    bool m_pending = false;
    int64_t m_reqFrame = 0;
    int m_reqField = 0;
    bool m_reqFieldView = false, m_reqQtgmc = false;

    // guarded by m_m, copied into the worker before each build
    SourceInfo m_sourceIn;
    Project m_projectIn;
    int m_dirty = 0;
    bool m_haveSource = false;

    // worker-only
    SourceInfo m_source;
    Project m_project;
    std::unique_ptr<SourceReader> m_reader;
    InputStage m_input;
    bool m_inputOk = false;
    TrackStore m_tracks;
    std::map<int64_t, std::shared_ptr<Picture>> m_raw, m_corr;
    std::vector<int64_t> m_rawOrder, m_corrOrder;
};

}  // namespace ud

Q_DECLARE_METATYPE(ud::PreviewFramePtr)
