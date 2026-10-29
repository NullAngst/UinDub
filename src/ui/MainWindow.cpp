// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include "App.h"
#include "Controls.h"
#include "Dialogs.h"
#include "Docks.h"
#include "OffsetStrip.h"
#include "PreviewEngine.h"
#include "PreviewWidget.h"
#include "Tabs.h"
#include "Timeline.h"
#include "core/Analyze.h"
#include "core/Qtgmc.h"
#include "core/Warble.h"
#include "shared/Theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>

namespace ud {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("UinDub");
    setAcceptDrops(true);
    resize(1500, 950);
    m_s = new Session(this);
    m_engine = new PreviewEngine(this);

    // Center: preview with the offset strip beside it, timeline under both.
    auto* center = new QWidget(this);
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(4, 4, 4, 4);
    auto* top = new QSplitter(Qt::Horizontal, center);
    m_preview = new PreviewWidget(top);
    m_strip = new OffsetStrip(top);
    top->addWidget(m_preview);
    top->addWidget(m_strip);
    top->setStretchFactor(0, 1);
    top->setStretchFactor(1, 0);
    m_strip->setVisible(false);
    m_timeline = new Timeline(m_s, center);
    cl->addWidget(top, 1);
    cl->addWidget(m_timeline);

    // Side panel, tabs in pipeline order.
    auto* tabs = new QTabWidget(this);
    tabs->setMinimumWidth(360);
    tabs->setUsesScrollButtons(false);  // all six tabs stay visible; the panel widens to fit
    m_sourceTab = new SourceTab(m_s, tabs);
    m_edgeTab = new EdgeLockTab(m_s, tabs);
    m_deintTab = new DeinterlaceTab(m_s, tabs);
    m_colorTab = new ColorTab(m_s, tabs);
    m_cropTab = new CropTab(m_s, tabs);
    m_outputTab = new OutputTab(m_s, tabs);
    tabs->addTab(m_sourceTab, tr("Source"));
    tabs->addTab(m_edgeTab, tr("Edge Lock"));
    tabs->addTab(m_deintTab, tr("Deinterlace"));
    tabs->addTab(m_colorTab, tr("Color"));
    tabs->addTab(m_cropTab, tr("Crop"));
    tabs->addTab(m_outputTab, tr("Output"));
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(center);
    split->addWidget(tabs);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    setCentralWidget(split);

    m_scopes = new ScopesDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_scopes);
    m_queue = new QueueDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_queue);
    m_queue->hide();
    m_scopes->hide();

    m_pos = new QLabel(this);
    m_msg = new QLabel(this);
    statusBar()->addWidget(m_pos);
    statusBar()->addWidget(m_msg, 1);

    const auto colors = referenceColors();
    m_preview->setRefColors(colors);
    m_timeline->setRefColors(colors);

    buildMenus();
    bindKeys();

    connect(m_s, &Session::opened, this, &MainWindow::onOpened);
    connect(m_s, &Session::closed, this, [this] {
        m_engine->clear();
        m_preview->clear(tr("Open a PingDub capture, or any interlaced SD file (File > Open)."));
        setWindowTitle("UinDub");
        updateStatus();
    });
    connect(m_s, &Session::message, this, &MainWindow::showMessage);
    connect(m_s, &Session::positionChanged, this, [this](qint64, int) {
        m_qtgmcPending = false;
        requestPreview();
        updateStatus();
    });
    connect(m_s, &Session::projectChanged, this, [this](int what) {
        m_engine->setProject(m_s->project(), what);
        const CropSettings& c = m_s->project().crop;
        m_preview->setCrop(c.left, c.right, c.top, c.bottom);
        int sl, sr, sb;
        m_cropTab->safeCrop(sl, sr, sb);
        m_preview->setSafeCrop(sl, sr, sb);
        m_strip->setTruth(m_s->project().hasTruth, m_s->project().truth, m_s->project().renderFieldOrder());
        m_strip->setVisible(m_s->project().edgeEnabled);
        requestPreview();
        updateStatus();
    });
    connect(m_engine, &PreviewEngine::ready, this, [this](PreviewFramePtr f) {
        if (!m_s->isOpen()) return;
        m_preview->showFrame(f);
        m_strip->showFrame(f, m_s->project().edge.strength);
        m_scopes->showFrame(f);
        m_colorTab->setClipped(f->clipped);
        if (!f->error.isEmpty()) showMessage(f->error);
        else if (m_qtgmcPending && f->qtgmc) {
            m_qtgmcPending = false;
            showMessage(tr("QTGMC preview of this frame. Scrubbing goes back to bwdif."));
        }
    });
    connect(m_preview, &PreviewWidget::drawn, this, &MainWindow::drawnEdge);
    connect(m_preview, &PreviewWidget::cropChanged, this, [this](int l, int r, int t, int b) {
        CropSettings& c = m_s->project().crop;
        c.left = l;
        c.right = r;
        c.top = t;
        c.bottom = b;
        m_s->changed(ChCrop);
    });
    connect(m_cropTab, &CropTab::editToggled, m_preview, &PreviewWidget::setCropEditing);
    connect(m_edgeTab, &EdgeLockTab::useBorderRequested, this, &MainWindow::useBorder);
    connect(m_edgeTab, &EdgeLockTab::drawRequested, this, &MainWindow::startDraw);
    connect(m_edgeTab, &EdgeLockTab::analyzeRequested, this, [this] { analyze(); });
    connect(m_edgeTab, &EdgeLockTab::stopRequested, this, &MainWindow::stopAll);
    connect(m_outputTab, &OutputTab::renderRequested, this, [this] { render(true); });
    connect(m_outputTab, &OutputTab::queueRequested, this, [this] { render(false); });
    connect(m_queue, &QueueDock::progress, m_timeline, &Timeline::setProgress);
    connect(m_queue, &QueueDock::finished, this, [this](const QString& path, bool ok) {
        showMessage(ok ? tr("Rendered %1").arg(path) : tr("Render did not finish; see the queue for why."));
    });

    // Timeline actions.
    connect(m_timeline, &Timeline::cutRequested, this, [this](qint64 a, qint64 b) {
        m_s->project().edit.cuts.push_back({a, b});
        m_timeline->clearSelection();
        m_s->changed(ChEdit);
    });
    connect(m_timeline, &Timeline::setInOutRequested, this, [this](qint64 a, qint64 b) {
        m_s->project().edit.in = a;
        m_s->project().edit.out = b;
        m_timeline->clearSelection();
        m_s->changed(ChEdit);
    });
    connect(m_timeline, &Timeline::dontCorrectRequested, this, [this](qint64 a, qint64 b) {
        EdgeOverride o;
        o.kind = EdgeOverride::Kind::DontCorrect;
        o.start = a;
        o.end = b;
        m_s->project().overrides.push_back(o);
        m_timeline->clearSelection();
        m_s->changed(ChEdge);
    });
    connect(m_timeline, &Timeline::reanalyzeRequested, this, [this](qint64 a, qint64 b) {
        const EdgeLockSettings s = m_s->project().edge;
        m_timeline->clearSelection();
        analyze(a, b, &s);
    });
    connect(m_timeline, &Timeline::deleteCutRequested, this, [this](qint64 f) {
        auto& cuts = m_s->project().edit.cuts;
        cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [f](const CutSpan& c) { return f >= c.start && f < c.end; }), cuts.end());
        m_s->changed(ChEdit);
    });
    connect(m_timeline, &Timeline::deleteOverrideRequested, this, [this](qint64 f) {
        auto& ov = m_s->project().overrides;
        ov.erase(std::remove_if(ov.begin(), ov.end(), [f](const EdgeOverride& o) { return f >= o.start && f < o.end; }), ov.end());
        m_s->changed(ChEdge);
    });
    connect(m_timeline, &Timeline::carryForwardRequested, this, [this](int slot, qint64 f) {
        Project& p = m_s->project();
        if (slot < 0 || slot >= int(p.refs.size()) || p.refs[size_t(slot)].spans.empty()) return;
        Reference& r = p.refs[size_t(slot)];
        const RefSpan* base = &r.spans.front();
        for (const RefSpan& s : r.spans)
            if (s.start <= f) base = &s;
        RefSpan copy = *base;
        for (RefSpan& s : r.spans)
            if (s.start < f && s.end > f) s.end = f;
        copy.start = f;
        copy.end = m_s->frames();
        for (const RefSpan& s : r.spans)
            if (s.start > f) copy.end = std::min(copy.end, s.start);
        r.spans.push_back(copy);
        std::sort(r.spans.begin(), r.spans.end(), [](const RefSpan& a, const RefSpan& b) { return a.start < b.start; });
        m_s->changed(ChRefs);
    });
    connect(m_timeline, &Timeline::endSpanRequested, this, [this](int slot, qint64 f) {
        Project& p = m_s->project();
        if (slot < 0 || slot >= int(p.refs.size())) return;
        for (RefSpan& s : p.refs[size_t(slot)].spans)
            if (s.start < f && s.end > f) s.end = f;
        m_s->changed(ChRefs);
    });

    m_play.setTimerType(Qt::PreciseTimer);
    connect(&m_play, &QTimer::timeout, this, &MainWindow::tick);

    AppSettings& as = AppSettings::instance();
    if (!as.geometry.isEmpty()) restoreGeometry(as.geometry);
    if (!as.state.isEmpty()) restoreState(as.state);
    updateStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openDialog);
    m_recentMenu = file->addMenu(tr("Open &recent"));
    updateRecent();
    file->addAction(tr("Save project &as..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addAction(tr("&Close"), QKeySequence::Close, this, [this] { m_s->close(); });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    auto* g = new QActionGroup(this);
    m_actProcessed = view->addAction(tr("&Processed"), this, [this] { m_preview->setView(PreviewWidget::View::Processed); });
    m_actSource = view->addAction(tr("&Source"), this, [this] { m_preview->setView(PreviewWidget::View::Source); });
    m_actSplit = view->addAction(tr("&A/B split"), this, [this] { m_preview->setView(PreviewWidget::View::Split); });
    for (QAction* a : {m_actProcessed, m_actSource, m_actSplit}) {
        a->setCheckable(true);
        g->addAction(a);
    }
    m_actProcessed->setChecked(true);
    m_actField = view->addAction(tr("&Field view (one field, line-doubled)"), this, [this] { requestPreview(); });
    m_actField->setCheckable(true);
    m_actField->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F));
    view->addSeparator();
    auto* zg = new QActionGroup(this);
    const std::pair<QString, int> zooms[] = {{tr("Zoom to &fit"), 0}, {tr("Zoom &100%"), 1}, {tr("Zoom &200%"), 2}, {tr("Zoom &400%"), 4}};
    const QKeySequence zkeys[] = {QKeySequence(Qt::CTRL | Qt::Key_0), QKeySequence(Qt::CTRL | Qt::Key_1), QKeySequence(Qt::CTRL | Qt::Key_2),
                                  QKeySequence(Qt::CTRL | Qt::Key_4)};
    for (int i = 0; i < 4; ++i) {
        const int z = zooms[i].second;
        QAction* a = view->addAction(zooms[i].first, this, [this, z] { m_preview->setZoom(z); });
        a->setCheckable(true);
        a->setChecked(z == 0);
        a->setShortcut(zkeys[i]);
        zg->addAction(a);
    }
    view->addSeparator();
    m_actQtgmc = view->addAction(tr("Preview with &QTGMC (paused frame)"), this, [this] {
        if (!m_s->isOpen()) return;
        setPlaying(0);
        m_qtgmcPending = true;
        showMessage(tr("Running QTGMC on this frame..."));
        m_engine->request(m_s->frame(), m_s->field(), false, true);
    });
    m_actQtgmc->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q));
    m_actOverlay = view->addAction(tr("Edge Lock &overlay"), this, [this](bool on) { m_preview->setOverlayVisible(on); });
    m_actOverlay->setCheckable(true);
    m_actOverlay->setChecked(true);
    QAction* strip = view->addAction(tr("Offset s&trip"), this, [this](bool on) { m_strip->setVisible(on); });
    strip->setCheckable(true);
    view->addAction(m_scopes->toggleViewAction());
    view->addAction(m_queue->toggleViewAction());
    connect(view, &QMenu::aboutToShow, this, [this, strip] { strip->setChecked(m_strip->isVisible()); });

    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(tr("&Warble test..."), this, [this] {
        if (!m_s->isOpen()) {
            showMessage(tr("Open a clean clip first."));
            return;
        }
        WarbleDialog d(m_s->source(), m_s->project().renderFieldOrder(), m_s->frame(), this);
        if (d.exec() == QDialog::Accepted && !d.createdProject().isEmpty()) {
            openPath(d.createdProject());
            showMessage(tr("Warble copy open. Its left border is set: run Analyze (F5) and the Edge Lock tab shows the score."));
        }
    });
    tools->addAction(tr("&Analyze"), this, [this] { analyze(); });
    tools->addAction(tr("&Render"), this, [this] { render(true); });
    QMenu* settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("&Preferences..."), this, [this] {
        PreferencesDialog d(this);
        connect(&d, &PreferencesDialog::keysChanged, this, &MainWindow::bindKeys);
        d.exec();
    });
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About UinDub"), this, [this] {
        const VsRuntimeInfo& rt = vapoursynthRuntime();
        QMessageBox::about(this, tr("About UinDub"),
                           tr("<b>UinDub %1</b><br>Post-processing for PingDub captures: Edge Lock (a software TBC), deinterlacing, color, "
                              "crop, cuts and encoding.<br><br>QTGMC: %2<br><br>GPL-3.0-or-later. github.com/NullAngst/UinDub")
                               .arg(UINDUB_VERSION, rt.ok ? tr("VapourSynth %1").arg(QString::fromStdString(rt.version)).toHtmlEscaped()
                                                          : QString::fromStdString(rt.error).toHtmlEscaped()));
    });
}

void MainWindow::bindKeys() {
    for (QAction* a : m_keyed) {
        removeAction(a);
        a->deleteLater();
    }
    m_keyed.clear();
    auto bind = [&](const QString& id, auto fn) {
        auto* a = new QAction(this);
        a->setShortcut(AppSettings::instance().key(id));
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        m_keyed[id] = a;
    };
    bind("play", [this] { setPlaying(m_speed == 0 ? 1 : 0); });
    bind("prev_frame", [this] {
        setPlaying(0);
        m_s->seek(m_s->frame() - 1, 0);
    });
    bind("next_frame", [this] {
        setPlaying(0);
        m_s->seek(m_s->frame() + 1, 0);
    });
    bind("prev_field", [this] {
        setPlaying(0);
        m_s->stepField(-1);
    });
    bind("next_field", [this] {
        setPlaying(0);
        m_s->stepField(1);
    });
    bind("shuttle_back", [this] { setPlaying(m_speed >= 0 ? -1 : std::max(-8, m_speed * 2)); });
    bind("shuttle_stop", [this] { setPlaying(0); });
    bind("shuttle_fwd", [this] { setPlaying(m_speed <= 0 ? 1 : std::min(8, m_speed * 2)); });
    bind("mark_in", [this] {
        m_s->project().edit.in = m_timeline->snap(m_s->frame());
        m_s->changed(ChEdit);
    });
    bind("mark_out", [this] {
        m_s->project().edit.out = m_timeline->snap(m_s->frame()) + 1;
        m_s->changed(ChEdit);
    });
    bind("mark_cut", [this] { markCut(); });
    bind("analyze", [this] { analyze(); });
    bind("render", [this] { render(true); });
    bind("stop", [this] { stopAll(); });
}

bool MainWindow::openPath(const QString& path) {
    QString err;
    bool join = true;
    // Opening one segment of a split capture offers to load the rest.
    SourceInfo probe;
    std::string e;
    const QFileInfo fi(path);
    if (!fi.isDir() && fi.fileName() != "project.json" && probeSource(path.toStdString(), false, probe, e) && !probe.otherSegments.empty()) {
        const auto r = QMessageBox::question(this, tr("Segmented capture"),
                                             tr("This file is one of %1 segments of a capture. Open the whole capture as one source?")
                                                 .arg(probe.otherSegments.size() + 1),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        join = r == QMessageBox::Yes;
    }
    if (!m_s->open(path, join, err)) {
        QMessageBox::warning(this, tr("Open"), err);
        return false;
    }
    AppSettings::instance().lastOpenFolder = fi.absolutePath();
    AppSettings::instance().addRecent(QString::fromStdString(m_s->project().folder));
    AppSettings::instance().save();
    updateRecent();
    return true;
}

void MainWindow::onOpened() {
    m_engine->setSource(m_s->source());
    m_engine->setProject(m_s->project(), ChAll);
    const QString name = QString::fromStdString(!m_s->project().tapeLabel.empty() ? m_s->project().tapeLabel
                                                                                  : QFileInfo(QString::fromStdString(m_s->source().path)).fileName().toStdString());
    setWindowTitle(tr("UinDub - %1").arg(name));
    m_sourceTab->refresh();
    m_edgeTab->refresh();
    m_deintTab->refresh();
    m_colorTab->refresh();
    m_cropTab->refresh();
    m_outputTab->refresh();
    requestPreview();
    updateStatus();
}

void MainWindow::openDialog() {
    const QString f = QFileDialog::getOpenFileName(this, tr("Open a capture or a project"), AppSettings::instance().lastOpenFolder,
                                                   tr("Captures and projects (*.mkv *.avi *.mov *.mp4 *.json);;All files (*)"));
    if (!f.isEmpty()) openPath(f);
}

void MainWindow::saveAs() {
    if (!m_s->isOpen()) return;
    const QString d = QFileDialog::getExistingDirectory(this, tr("Move the project to a new, empty folder"), QString::fromStdString(m_s->project().folder));
    if (d.isEmpty()) return;
    m_s->saveNow();
    std::string err;
    if (!moveProject(m_s->project(), d.toStdString(), err)) {
        QMessageBox::warning(this, tr("Save project as"), QString::fromStdString(err));
        return;
    }
    AppSettings::instance().addRecent(d);
    AppSettings::instance().save();
    updateRecent();
    m_s->changed(ChAll);
    showMessage(tr("Project moved to %1").arg(d));
}

void MainWindow::updateRecent() {
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    for (const QString& r : AppSettings::instance().recent) m_recentMenu->addAction(r, this, [this, r] { openPath(r); });
    m_recentMenu->setEnabled(!AppSettings::instance().recent.isEmpty());
}

void MainWindow::requestPreview() {
    if (!m_s->isOpen()) return;
    m_engine->request(m_s->frame(), m_s->field(), m_actField->isChecked(), false);
}

void MainWindow::useBorder() {
    if (!m_s->isOpen()) return;
    Project& p = m_s->project();
    SourceReader r;
    std::string e;
    if (!r.open(m_s->source(), e)) return;
    FramePtr f = r.read(m_s->frame());
    Picture pic;
    if (!f || !toPicture(f.get(), pic, true)) return;
    const BorderFind b = findBorders(pic, p.edge);
    if (!b.left && !b.right) {
        QMessageBox::information(this, tr("Use left border"),
                                 tr("No blanking edge found on this frame. Try a frame with picture reaching the left side, or draw an edge."));
        return;
    }
    // One border reference per side; a second click updates them from this frame.
    auto add = [&](RefKind kind, double x, const TemplateBuild& t, const char* name) {
        for (Reference& ref : p.refs)
            if (ref.kind == kind) {
                ref.spans = {RefSpan{0, m_s->frames(), {{x, 0}}, t.profile, t.step, m_s->frame()}};
                return;
            }
        if (int(p.refs.size()) >= kMaxReferences) return;
        Reference ref;
        ref.id = int(p.refs.size()) + 1;
        ref.kind = kind;
        ref.name = name;
        ref.spans.push_back(RefSpan{0, m_s->frames(), {{x, 0}}, t.profile, t.step, m_s->frame()});
        p.refs.push_back(ref);
    };
    if (b.left) add(RefKind::LeftBorder, b.leftX, b.leftT, "Left border");
    if (b.right) add(RefKind::RightBorder, b.rightX, b.rightT, "Right border");
    // The border comes first in the list, since it wins wherever it is seen.
    std::stable_sort(p.refs.begin(), p.refs.end(), [](const Reference& a, const Reference& c) { return int(a.kind) < int(c.kind); });
    if (!p.edgeEnabled) p.edgeEnabled = true;
    m_s->changed(ChRefs | ChEdge);
    showMessage(b.right ? tr("Left border at %1 px and right border at %2 px.").arg(b.leftX, 0, 'f', 1).arg(b.rightX, 0, 'f', 1)
                        : tr("Left border at %1 px.").arg(b.leftX, 0, 'f', 1));
}

void MainWindow::startDraw() {
    if (!m_s->isOpen()) return;
    setPlaying(0);
    m_actField->setChecked(true);
    requestPreview();
    m_preview->startDrawing();
    showMessage(tr("Field view: click along the edge, Enter to finish."));
}

void MainWindow::drawnEdge(const std::vector<PointF>& pts) {
    Project& p = m_s->project();
    SourceReader r;
    std::string e;
    if (!r.open(m_s->source(), e)) return;
    FramePtr f = r.read(m_s->frame());
    Picture pic;
    if (!f || !toPicture(f.get(), pic, true)) return;
    const int64_t at = m_s->frame();
    // The span runs from here to the next cut PingDub or Analyze found, or the end.
    int64_t end = m_s->frames();
    for (int64_t c : p.detectedCuts)
        if (c > at) end = std::min(end, c);
    RefSpan sp;
    if (!makeDrawnSpan(pic, pts, at, end, p.edge, sp, e)) {
        QMessageBox::information(this, tr("Draw edge"), QString::fromStdString(e));
        return;
    }
    // Drawing again at a later frame starts a new span of the same reference,
    // when a drawn reference is already tracking here.
    Reference* target = nullptr;
    for (Reference& ref : p.refs)
        if (ref.kind == RefKind::Drawn && !ref.spans.empty() && ref.spans.front().start < at) target = &ref;
    if (target) {
        for (RefSpan& s : target->spans)
            if (s.start < at && s.end > at) s.end = at;
        for (const RefSpan& s : target->spans)
            if (s.start > at) sp.end = std::min(sp.end, s.start);
        target->spans.push_back(sp);
        std::sort(target->spans.begin(), target->spans.end(), [](const RefSpan& a, const RefSpan& b) { return a.start < b.start; });
        showMessage(tr("New span for %1 from frame %2.").arg(QString::fromStdString(target->name)).arg(at));
    } else {
        if (int(p.refs.size()) >= kMaxReferences) {
            QMessageBox::information(this, tr("Draw edge"), tr("Four references is the most; remove one first."));
            return;
        }
        Reference ref;
        ref.id = int(p.refs.size()) + 1;
        ref.kind = RefKind::Drawn;
        ref.name = "Drawn edge " + std::to_string(std::count_if(p.refs.begin(), p.refs.end(), [](const Reference& x) { return x.kind == RefKind::Drawn; }) + 1);
        ref.spans.push_back(sp);
        p.refs.push_back(ref);
        showMessage(tr("Drawn edge set from frame %1.").arg(at));
    }
    if (!p.edgeEnabled) p.edgeEnabled = true;
    m_s->changed(ChRefs | ChEdge);
}

void MainWindow::analyze(int64_t start, int64_t end, const EdgeLockSettings* override) {
    if (!m_s->isOpen() || m_analyzing) return;
    Project& p = m_s->project();
    if (p.refs.empty()) {
        showMessage(tr("Set a reference first: Use left border, or Draw edge."));
        return;
    }
    const bool partial = start > 0 || (end >= 0 && end < m_s->frames());
    if (partial && !p.analysisCurrent()) {
        QMessageBox::information(this, tr("Re-analyze"), tr("Analyze the whole clip with these references first; then a span can be re-analyzed on its own."));
        return;
    }
    m_s->saveNow();
    AnalyzeJob job;
    job.source = m_s->source();
    job.order = p.renderFieldOrder();
    job.settings = override ? *override : p.edge;
    job.refs = p.refs;
    job.start = start;
    job.end = end;
    job.offsetsPath = p.offsetsPath();
    job.tracksPath = p.tracksPath();
    const std::string key = p.currentAnalysisKey();
    m_analyzing = true;
    m_analyzeCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_analyzeCancel;
    auto out = std::make_shared<AnalyzeOutcome>();
    QPointer<MainWindow> self(this);
    m_edgeTab->setAnalyzing(true, 0, tr("Analyzing..."));
    runInBackground(
        this,
        [job, cancel, out, self] {
            *out = runAnalyze(job, [cancel, self](double f, int64_t frame) {
                QMetaObject::invokeMethod(
                    self,
                    [self, f, frame] {
                        if (self) self->m_edgeTab->setAnalyzing(true, f, tr("Analyzing frame %1 (%2%)").arg(frame).arg(f * 100, 0, 'f', 1));
                    },
                    Qt::QueuedConnection);
                return !*cancel;
            });
        },
        [this, out, key, partial, start, end, override] {
            m_analyzing = false;
            Project& p = m_s->project();
            if (out->ok) {
                if (!partial) {
                    p.analysisKey = key;
                    p.detectedCuts = out->tracker.cuts;
                    p.autoEnds = out->tracker.autoEnds;
                } else if (override) {
                    EdgeOverride o;
                    o.kind = EdgeOverride::Kind::Reanalyze;
                    o.start = start;
                    o.end = end;
                    o.settings = *override;
                    p.overrides.push_back(o);
                }
                m_edgeTab->setAnalyzing(false, 1, QString());
                m_s->changed(ChAnalysis | ChEdge);
                QString msg = tr("Analyzed %1 frames in %2 s.").arg(out->frames).arg(out->seconds, 0, 'f', 1);
                if (!out->tracker.cuts.empty()) msg += tr(" %1 scene cuts ended drawn edges.").arg(out->tracker.cuts.size());
                if (p.hasTruth && m_s->offsets()) {
                    const WarbleScore sc = scoreWarble(*m_s->offsets(), p.truth, p.renderFieldOrder(), m_s->frames());
                    if (sc.ok) msg += tr(" Warble test: %1 px RMS residual on measured lines.").arg(sc.rmsMeasured, 0, 'f', 3);
                }
                showMessage(msg);
            } else {
                m_edgeTab->setAnalyzing(false, 0, out->cancelled ? tr("Analyze stopped. The previous offsets are untouched.") : QString::fromStdString(out->error));
                m_s->changed(ChEdge);
            }
        });
}

void MainWindow::stopAll() {
    if (m_preview->drawing()) {
        m_preview->cancelDrawing();
        return;
    }
    setPlaying(0);
    if (m_analyzeCancel) *m_analyzeCancel = true;
    if (m_queue->running()) m_queue->stop();
    if (m_timeline->cutStart() >= 0) m_timeline->setCutStart(-1);
}

void MainWindow::render(bool startNow) {
    if (!m_s->isOpen()) return;
    Project& p = m_s->project();
    if (p.edgeActive() && !p.analysisCurrent()) {
        const auto r = QMessageBox::question(this, tr("Render"), tr("Edge Lock is on but this clip has not been analyzed with the current references "
                                                                     "and settings. Analyze now (and render after)?"));
        if (r == QMessageBox::Yes) analyze();
        return;
    }
    if (p.deint.mode == DeintMode::Qtgmc && !vapoursynthRuntime().ok) {
        QMessageBox::warning(this, tr("Render"), tr("QTGMC is not available in this build: %1\nPick bwdif in the Deinterlace tab, or run the AppImage.")
                                                     .arg(QString::fromStdString(vapoursynthRuntime().error)));
        return;
    }
    if (p.out.folder.empty()) {
        // Nothing is written next to the master unless that folder is picked on purpose.
        QString start = AppSettings::instance().lastOutputFolder;
        if (start.isEmpty()) start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        const QString d = QFileDialog::getExistingDirectory(this, tr("Folder for the rendered file"), start);
        if (d.isEmpty()) return;
        p.out.folder = d.toStdString();
        AppSettings::instance().lastOutputFolder = d;
        AppSettings::instance().save();
        m_s->changed(ChOutput);
        m_outputTab->refresh();
    }
    m_s->saveNow();
    RenderJob job;
    job.project = p;
    job.source = m_s->source();
    job.outPath = outputPath(p.out, m_s->source());
    job.verifySource = AppSettings::instance().verifyBeforeRender;
    const QString title = QFileInfo(QString::fromStdString(job.outPath)).fileName();
    if (!m_queue->isVisible()) {
        m_queue->show();
        resizeDocks({m_queue}, {220}, Qt::Vertical);  // keep the preview the biggest thing on screen
    }
    m_queue->add(job, title, startNow);
}

void MainWindow::markCut() {
    if (!m_s->isOpen()) return;
    const int64_t f = m_timeline->snap(m_s->frame());
    if (m_timeline->cutStart() < 0) {
        m_timeline->setCutStart(f);
        showMessage(tr("Cut starts here. Move to its end and press C again (Esc cancels)."));
        return;
    }
    const int64_t a = std::min(m_timeline->cutStart(), f), b = std::max(m_timeline->cutStart(), f);
    m_timeline->setCutStart(-1);
    if (b <= a) return;
    m_s->project().edit.cuts.push_back({a, b});
    m_s->changed(ChEdit);
    showMessage(tr("Cut %1 frames.").arg(b - a));
}

void MainWindow::setPlaying(int speed) {
    m_speed = speed;
    if (!m_s->isOpen() || speed == 0) {
        m_play.stop();
        updateStatus();
        return;
    }
    const double fps = m_s->source().fps.toDouble();
    m_play.start(int(std::lround(1000.0 / std::max(1.0, fps))));
    updateStatus();
}

void MainWindow::tick() {
    const int64_t next = m_s->frame() + m_speed;
    if (next < 0 || next >= m_s->frames()) {
        setPlaying(0);
        return;
    }
    m_s->seek(next, 0);
}

void MainWindow::updateStatus() {
    if (!m_s->isOpen()) {
        m_pos->setText(QString());
        return;
    }
    const SourceInfo& s = m_s->source();
    const int64_t f = m_s->frame();
    QString t = tr("%1  frame %2 / %3  field %4").arg(QString::fromStdString(clockString(frameSeconds(f, s.fps)))).arg(f).arg(s.frames - 1).arg(m_s->field() + 1);
    if (m_speed) t += tr("  playing %1x").arg(m_speed);
    m_pos->setText(t);
}

void MainWindow::showMessage(const QString& m) { m_msg->setText(m); }

void MainWindow::closeEvent(QCloseEvent* e) {
    if (m_queue->running()) {
        const auto r = QMessageBox::question(this, tr("Quit"), tr("A render is running. Stop it and quit?"));
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        m_queue->stop();
    }
    if (m_analyzeCancel) *m_analyzeCancel = true;
    m_s->saveNow();
    AppSettings& as = AppSettings::instance();
    as.geometry = saveGeometry();
    as.state = saveState();
    as.save();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const QList<QUrl> urls = e->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) openPath(urls.first().toLocalFile());
}

}  // namespace ud
