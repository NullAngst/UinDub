// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"

#include "App.h"
#include "core/Analyze.h"
#include "core/Pipeline.h"
#include "core/Qtgmc.h"
#include "core/Warble.h"
#include "shared/Checksums.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace ud {

std::vector<QColor> referenceColors() { return {QColor(0x5B, 0x9B, 0xFF), QColor(0xE8, 0x6B, 0xD2), QColor(0x3F, 0xD0, 0xD0), QColor(0xFF, 0x9F, 0x43)}; }

namespace {

// Each tab scrolls, so small screens can reach everything.
QVBoxLayout* scrolled(QWidget* tab) {
    auto* outer = new QVBoxLayout(tab);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* sa = new QScrollArea(tab);
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget(sa);
    auto* l = new QVBoxLayout(inner);
    l->setContentsMargins(8, 8, 8, 8);
    sa->setWidget(inner);
    outer->addWidget(sa);
    return l;
}

QString rate(const Rational& r) {
    if (r.den == 1) return QString::number(r.num);
    return QString::number(r.toDouble(), 'f', 3);
}

}  // namespace

// ---------------------------------------------------------------------------
// Source

SourceTab::SourceTab(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    QVBoxLayout* l = scrolled(this);
    auto* g1 = new QGroupBox(tr("File"), this);
    auto* g1l = new QVBoxLayout(g1);
    m_info = new QLabel(g1);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    g1l->addWidget(m_info);
    l->addWidget(g1);

    auto* g2 = new QGroupBox(tr("PingDub capture"), this);
    auto* g2l = new QVBoxLayout(g2);
    m_pingdub = new QLabel(g2);
    m_pingdub->setWordWrap(true);
    m_pingdub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    g2l->addWidget(m_pingdub);
    auto* lf = new QFormLayout;
    m_label = new QLineEdit(g2);
    m_label->setPlaceholderText(tr("written to the output as its title"));
    lf->addRow(tr("Tape label"), m_label);
    g2l->addLayout(lf);
    l->addWidget(g2);
    connect(m_label, &QLineEdit::editingFinished, this, [this] {
        m_s->project().tapeLabel = m_label->text().toStdString();
        m_s->changed(ChSource);
    });

    auto* g3 = new QGroupBox(tr("Field order"), this);
    auto* g3l = new QVBoxLayout(g3);
    m_field = new QComboBox(g3);
    m_field->addItem(tr("Automatic"), 0);
    m_field->addItem(tr("Top field first (TFF)"), 1);
    m_field->addItem(tr("Bottom field first (BFF)"), 2);
    g3l->addWidget(m_field);
    m_fieldInfo = new QLabel(g3);
    m_fieldInfo->setWordWrap(true);
    g3l->addWidget(m_fieldInfo);
    auto* rerun = new QPushButton(tr("Run the comb test again"), g3);
    g3l->addWidget(rerun);
    g3l->addWidget(noteLabel(tr("Getting it wrong makes motion judder back and forth. The tag, PingDub's JSON and a comb test on the first "
                                "moving frames are compared; the preview warns when they disagree."),
                             g3));
    l->addWidget(g3);
    connect(m_field, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        Project& p = m_s->project();
        p.fieldOverride = i != 0;
        p.fieldOrder = i == 1 ? FieldOrder::TFF : (i == 2 ? FieldOrder::BFF : m_s->fieldCheck().chosen);
        m_s->changed(ChSource);
    });
    connect(rerun, &QPushButton::clicked, this, [this] { m_s->rerunFieldCheck(); });

    auto* g4 = new QGroupBox(tr("Verify source"), this);
    auto* g4l = new QVBoxLayout(g4);
    g4l->addWidget(noteLabel(tr("Checks the master against PingDub's .sha256 and .framemd5 sidecars: every byte and every decoded frame."), g4));
    m_verify = new QPushButton(tr("Verify now"), g4);
    g4l->addWidget(m_verify);
    m_verifyBefore = new QCheckBox(tr("Verify before every render"), g4);
    m_verifyBefore->setChecked(AppSettings::instance().verifyBeforeRender);
    g4l->addWidget(m_verifyBefore);
    m_verifyResult = new QLabel(g4);
    m_verifyResult->setWordWrap(true);
    g4l->addWidget(m_verifyResult);
    l->addWidget(g4);
    l->addStretch(1);
    connect(m_verify, &QPushButton::clicked, this, &SourceTab::verify);
    connect(m_verifyBefore, &QCheckBox::toggled, this, [](bool on) {
        AppSettings::instance().verifyBeforeRender = on;
        AppSettings::instance().save();
    });
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & ChSource) refresh();
    });
    connect(s, &Session::closed, this, &SourceTab::refresh);
}

void SourceTab::refresh() {
    if (!m_s->isOpen()) {
        m_info->setText(tr("Nothing open."));
        m_pingdub->clear();
        m_fieldInfo->clear();
        return;
    }
    const SourceInfo& s = m_s->source();
    const Project& p = m_s->project();
    QString t;
    for (const SourceSegment& g : s.segments)
        t += QString("<b>%1</b> (%2 frames)<br>").arg(QFileInfo(QString::fromStdString(g.file)).fileName().toHtmlEscaped()).arg(g.frames);
    if (!s.otherSegments.empty())
        t += tr("<i>%1 more segments of this capture are not loaded.</i><br>").arg(s.otherSegments.size());
    const char* pix = av_get_pix_fmt_name(AVPixelFormat(s.pixfmt));
    t += tr("%1, %2, %3x%4, %5 fps, SAR %6:%7, %8<br>")
             .arg(QString::fromStdString(s.codec), pix ? pix : "?")
             .arg(s.width)
             .arg(s.height)
             .arg(rate(s.fps))
             .arg(s.sar.num)
             .arg(s.sar.den)
             .arg(s.lines525 ? tr("525-line (NTSC), smpte170m") : tr("625-line (PAL), bt470bg"));
    t += tr("%1 frames, %2<br>").arg(s.frames).arg(QString::fromStdString(clockString(s.duration())));
    if (s.hasAudio) t += tr("Audio: %1, %2 Hz, %3 channels, %4-bit<br>").arg(QString::fromStdString(s.audioCodec)).arg(s.sampleRate).arg(s.channels).arg(s.audioBits);
    else t += tr("No audio<br>");
    t += tr("%1 chapters, %2 caption tracks, %3 logged events").arg(s.chapters.size()).arg(s.captions.size()).arg(s.events.size());
    m_info->setText(t);
    if (s.havePingDub)
        m_pingdub->setText(tr("PingDub %1<br>Device: %2 (%3), input %4<br>Standard %5, captured %6")
                               .arg(QString::fromStdString(s.pingdubVersion), QString::fromStdString(s.device), QString::fromStdString(s.driver),
                                    QString::fromStdString(s.input), QString::fromStdString(s.standard), QString::fromStdString(s.captureStart)));
    else
        m_pingdub->setText(tr("No PingDub sidecars: chapters, events, captions and checksums come only from the file itself."));
    {
        const QSignalBlocker b(m_label);
        m_label->setText(QString::fromStdString(p.tapeLabel));
    }
    const FieldOrderCheck& c = m_s->fieldCheck();
    {
        const QSignalBlocker b(m_field);
        m_field->setCurrentIndex(!p.fieldOverride ? 0 : (p.fieldOrder == FieldOrder::BFF ? 2 : 1));
        m_field->setItemText(0, tr("Automatic (%1)").arg(fieldOrderName(c.chosen)));
    }
    QString f = tr("Container tag: %1<br>PingDub JSON: %2<br>Comb test: %3 (confidence %4)<br>In use: <b>%5</b>")
                    .arg(fieldOrderName(c.tag), fieldOrderName(c.json), fieldOrderName(c.comb))
                    .arg(c.combConfidence, 0, 'f', 2)
                    .arg(fieldOrderName(p.renderFieldOrder()));
    for (const std::string& n : c.notes) f += "<br><span style='color:#D29922'>" + QString::fromStdString(n).toHtmlEscaped() + "</span>";
    m_fieldInfo->setText(f);
}

void SourceTab::verify() {
    if (!m_s->isOpen()) return;
    m_verify->setEnabled(false);
    m_verifyResult->setText(tr("Verifying..."));
    const SourceInfo s = m_s->source();
    auto result = std::make_shared<QString>();
    runInBackground(
        this,
        [s, result] {
            for (const SourceSegment& g : s.segments) {
                const VerifyResult v = verifyCapture(g.file, true);
                *result += QFileInfo(QString::fromStdString(g.file)).fileName() + ": " + QString::fromStdString(v.summary()) + "\n";
            }
        },
        [this, result] {
            m_verify->setEnabled(true);
            m_verifyResult->setText(result->trimmed());
        });
}

// ---------------------------------------------------------------------------
// Edge Lock

EdgeLockTab::EdgeLockTab(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    QVBoxLayout* l = scrolled(this);
    m_enable = new QCheckBox(tr("Edge Lock (software TBC)"), this);
    m_enable->setStyleSheet("font-weight: bold");
    l->addWidget(m_enable);
    l->addWidget(noteLabel(tr("Off by default. It straightens each line's horizontal position against a reference edge. It does nothing "
                              "until at least one reference is set, and nothing at all while off."),
                           this));

    auto* g = new QGroupBox(tr("References (up to 4)"), this);
    auto* gl = new QVBoxLayout(g);
    auto* row = new QHBoxLayout;
    m_border = new QPushButton(tr("Use left border"), g);
    m_border->setToolTip(tr("Finds the left blanking edge (and the right one when the capture has it). The best reference there is: "
                            "PingDub records the full sampled width and that edge is straight by definition."));
    m_draw = new QPushButton(tr("Draw edge..."), g);
    m_draw->setToolTip(tr("Pause on a good frame. The preview switches to field view; click along something that should be straight or "
                          "smoothly curved (a door frame, a TV bezel), then Enter. Drawing again at a later frame starts a new span."));
    row->addWidget(m_border);
    row->addWidget(m_draw);
    gl->addLayout(row);
    m_refs = new QListWidget(g);
    m_refs->setMinimumHeight(90);
    gl->addWidget(m_refs);
    auto* row2 = new QHBoxLayout;
    m_carry = new QPushButton(tr("Carry forward to playhead"), g);
    m_carry->setToolTip(tr("Copies the selected drawn reference to a new span starting at the playhead, for when the shot comes back after a cut."));
    m_remove = new QPushButton(tr("Remove"), g);
    row2->addWidget(m_carry);
    row2->addWidget(m_remove);
    gl->addLayout(row2);
    gl->addWidget(noteLabel(tr("I'd start with the border every time and add a drawn edge only where the border goes dark or the right side "
                               "still wobbles."),
                            g));
    l->addWidget(g);

    auto* sg = new QGroupBox(tr("Settings"), this);
    auto* f = new QFormLayout(sg);
    auto spin = [&](int lo, int hi, const QString& suffix) {
        auto* sp = new QSpinBox(sg);
        sp->setRange(lo, hi);
        sp->setSuffix(suffix);
        sp->setKeyboardTracking(false);
        return sp;
    };
    m_range = spin(2, 40, tr(" px"));
    f->addRow(tr("Search range (+-)"), m_range);
    m_topLines = spin(0, 120, tr(" lines"));
    m_topRange = spin(2, 60, tr(" px"));
    f->addRow(tr("Top zone"), m_topLines);
    f->addRow(tr("Top zone search (+-)"), m_topRange);
    m_tmpl = spin(8, 64, tr(" px"));
    f->addRow(tr("Template width"), m_tmpl);
    m_conf = new QDoubleSpinBox(sg);
    m_conf->setRange(0.1, 0.99);
    m_conf->setSingleStep(0.05);
    m_conf->setKeyboardTracking(false);
    f->addRow(tr("Confidence threshold"), m_conf);
    m_gap = spin(0, 200, tr(" lines"));
    f->addRow(tr("Max gap"), m_gap);
    m_smooth = spin(1, 61, tr(" fields"));
    f->addRow(tr("Motion smoothing"), m_smooth);
    m_strength = new SliderSpin(0, 100, 1, 0, "%", sg);
    m_strength->setDefault(100);
    f->addRow(tr("Strength"), m_strength);
    m_fill = new QComboBox(sg);
    m_fill->addItem(tr("Black (16)"));
    m_fill->addItem(tr("Repeat edge pixel"));
    f->addRow(tr("Fill"), m_fill);
    m_length = new QCheckBox(tr("Length correction (experimental)"), sg);
    m_length->setToolTip(tr("With two references across a line, also corrects the line's length, not just its start."));
    f->addRow(m_length);
    l->addWidget(sg);
    l->addWidget(noteLabel(tr("The defaults are a first guess, tuned against known damage (Tools > Warble test). Strength and fill apply at "
                              "render; the rest need Analyze again."),
                           this));

    m_analyze = new QPushButton(tr("Analyze (F5)"), this);
    m_analyze->setObjectName("primaryButton");
    l->addWidget(m_analyze);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    l->addWidget(m_progress);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    l->addWidget(m_status);
    m_score = new QLabel(this);
    m_score->setWordWrap(true);
    l->addWidget(m_score);
    l->addStretch(1);

    connect(m_enable, &QCheckBox::toggled, this, [this](bool on) {
        m_s->project().edgeEnabled = on;
        m_s->changed(ChEdge);
    });
    connect(m_border, &QPushButton::clicked, this, &EdgeLockTab::useBorderRequested);
    connect(m_draw, &QPushButton::clicked, this, &EdgeLockTab::drawRequested);
    connect(m_analyze, &QPushButton::clicked, this, [this] {
        if (m_running) emit stopRequested();
        else emit analyzeRequested();
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int i = m_refs->currentRow();
        Project& p = m_s->project();
        if (i < 0 || i >= int(p.refs.size())) return;
        p.refs.erase(p.refs.begin() + i);
        m_s->changed(ChRefs);
    });
    connect(m_carry, &QPushButton::clicked, this, [this] {
        const int i = m_refs->currentRow();
        Project& p = m_s->project();
        if (i < 0 || i >= int(p.refs.size()) || p.refs[size_t(i)].kind != RefKind::Drawn || p.refs[size_t(i)].spans.empty()) return;
        Reference& r = p.refs[size_t(i)];
        const int64_t f = m_s->frame();
        const RefSpan* base = nullptr;
        for (const RefSpan& s : r.spans)
            if (s.start <= f) base = &s;
        if (!base) base = &r.spans.front();
        RefSpan copy = *base;
        // The span it came from ends here; the copy runs to the next span or the end.
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
    connect(m_refs, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        const int i = m_refs->row(it);
        Project& p = m_s->project();
        if (i < 0 || i >= int(p.refs.size())) return;
        p.refs[size_t(i)].enabled = it->checkState() == Qt::Checked;
        const std::string name = it->text().trimmed().toStdString();
        if (!name.empty()) p.refs[size_t(i)].name = name;
        m_s->changed(ChRefs);
    });
    for (QSpinBox* sp : {m_range, m_topLines, m_topRange, m_tmpl, m_gap, m_smooth})
        connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, &EdgeLockTab::settingChanged);
    connect(m_conf, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &EdgeLockTab::settingChanged);
    connect(m_strength, &SliderSpin::valueChanged, this, &EdgeLockTab::settingChanged);
    connect(m_fill, qOverload<int>(&QComboBox::currentIndexChanged), this, &EdgeLockTab::settingChanged);
    connect(m_length, &QCheckBox::toggled, this, &EdgeLockTab::settingChanged);
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & (ChRefs | ChEdge | ChAnalysis | ChSource)) refresh();
    });
    connect(s, &Session::closed, this, &EdgeLockTab::refresh);
}

void EdgeLockTab::settingChanged() {
    EdgeLockSettings& e = m_s->project().edge;
    e.searchRange = m_range->value();
    e.topLines = m_topLines->value();
    e.topRange = m_topRange->value();
    e.templateWidth = m_tmpl->value();
    e.confidence = m_conf->value();
    e.maxGap = m_gap->value();
    e.motionSmoothing = m_smooth->value();
    e.strength = m_strength->value() / 100.0;
    e.fillRepeat = m_fill->currentIndex() == 1;
    e.lengthCorrection = m_length->isChecked();
    m_s->changed(ChEdge);
}

void EdgeLockTab::refresh() {
    const bool open = m_s->isOpen();
    for (QWidget* w : std::initializer_list<QWidget*>{m_enable, m_border, m_draw, m_refs, m_remove, m_carry, m_analyze}) w->setEnabled(open);
    if (!open) {
        m_refs->clear();
        m_status->clear();
        m_score->clear();
        return;
    }
    const Project& p = m_s->project();
    {
        const QSignalBlocker b(m_enable);
        m_enable->setChecked(p.edgeEnabled);
    }
    {
        const QSignalBlocker b(m_refs);
        const int keep = m_refs->currentRow();
        m_refs->clear();
        const auto colors = referenceColors();
        for (size_t i = 0; i < p.refs.size(); ++i) {
            const Reference& r = p.refs[i];
            QString kind = r.kind == RefKind::LeftBorder ? tr("left border") : (r.kind == RefKind::RightBorder ? tr("right border") : tr("drawn"));
            QString name = r.name.empty() ? tr("Reference %1").arg(i + 1) : QString::fromStdString(r.name);
            auto* it = new QListWidgetItem(name);
            it->setToolTip(tr("%1, %n span(s). Double-click to rename.", nullptr, int(r.spans.size())).arg(kind));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
            it->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);
            QPixmap pm(12, 12);
            pm.fill(colors[i % colors.size()]);
            it->setIcon(QIcon(pm));
            m_refs->addItem(it);
        }
        if (keep >= 0 && keep < m_refs->count()) m_refs->setCurrentRow(keep);
    }
    m_border->setEnabled(open && int(p.refs.size()) < kMaxReferences);
    m_draw->setEnabled(open && int(p.refs.size()) <= kMaxReferences);
    const EdgeLockSettings& e = p.edge;
    const QSignalBlocker b1(m_range), b2(m_topLines), b3(m_topRange), b4(m_tmpl), b5(m_conf), b6(m_gap), b7(m_smooth), b9(m_fill), b10(m_length);
    m_range->setValue(e.searchRange);
    m_topLines->setValue(e.topLines);
    m_topRange->setValue(e.topRange);
    m_tmpl->setValue(e.templateWidth);
    m_conf->setValue(e.confidence);
    m_gap->setValue(e.maxGap);
    m_smooth->setValue(e.motionSmoothing);
    m_strength->setValue(e.strength * 100.0);
    m_fill->setCurrentIndex(e.fillRepeat ? 1 : 0);
    m_length->setChecked(e.lengthCorrection);
    if (!m_running) {
        if (p.refs.empty()) m_status->setText(tr("No references yet."));
        else if (p.analysisCurrent()) m_status->setText(tr("<span style='color:#3FB950'>Analyzed.</span> Render uses these offsets."));
        else if (!p.analysisKey.empty()) m_status->setText(tr("<span style='color:#D29922'>References or settings changed since the last Analyze.</span> Run it again before rendering."));
        else m_status->setText(tr("Not analyzed yet. Analyze measures every field; it takes about as long as decoding the clip."));
    }
    m_score->clear();
    if (p.hasTruth && m_s->offsets()) {
        const WarbleScore sc = scoreWarble(*m_s->offsets(), p.truth, p.renderFieldOrder(), m_s->frames());
        if (sc.ok) {
            double worst = 0;
            for (double v : sc.perField) worst = std::max(worst, v);
            m_score->setText(tr("<b>Warble test score</b><br>Residual %1 px RMS on %2 measured lines (%3% of all)<br>All lines, interpolated "
                                "included: %4 px RMS<br>Worst field: %5 px RMS<br>Clip constant taken out: %6 px")
                                 .arg(sc.rmsMeasured, 0, 'f', 3)
                                 .arg(sc.measured)
                                 .arg(100.0 * sc.measured / std::max(1, sc.lines), 0, 'f', 1)
                                 .arg(sc.rmsAll, 0, 'f', 3)
                                 .arg(worst, 0, 'f', 3)
                                 .arg(sc.bias, 0, 'f', 2));
        }
    }
}

void EdgeLockTab::setAnalyzing(bool running, double fraction, const QString& status) {
    m_running = running;
    m_analyze->setText(running ? tr("Stop (Esc)") : tr("Analyze (F5)"));
    m_analyze->setProperty("running", running);
    m_analyze->style()->unpolish(m_analyze);
    m_analyze->style()->polish(m_analyze);
    m_progress->setVisible(running);
    m_progress->setValue(int(fraction * 1000));
    if (!status.isEmpty()) m_status->setText(status);
}

// ---------------------------------------------------------------------------
// Deinterlace

DeinterlaceTab::DeinterlaceTab(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    QVBoxLayout* l = scrolled(this);
    auto* f = new QFormLayout;
    m_mode = new QComboBox(this);
    m_mode->addItem(tr("QTGMC (best for tape)"), int(DeintMode::Qtgmc));
    m_mode->addItem(tr("bwdif (fast)"), int(DeintMode::Bwdif));
    m_mode->addItem(tr("yadif"), int(DeintMode::Yadif));
    m_mode->addItem(tr("IVTC (telecined film to 23.976p)"), int(DeintMode::Ivtc));
    m_mode->addItem(tr("None, keep interlaced"), int(DeintMode::None));
    f->addRow(tr("Mode"), m_mode);
    m_rate = new QComboBox(this);
    m_rate->addItem(tr("Double rate (every field)"));
    m_rate->addItem(tr("Single rate"));
    f->addRow(tr("Rate"), m_rate);
    m_preset = new QComboBox(this);
    for (const std::string& p : qtgmcPresets()) m_preset->addItem(QString::fromStdString(p));
    f->addRow(tr("QTGMC preset"), m_preset);
    l->addLayout(f);
    auto* adv = new QGroupBox(tr("QTGMC advanced"), this);
    adv->setCheckable(true);
    adv->setChecked(false);
    auto* af = new QFormLayout(adv);
    m_sourceMatch = new QSpinBox(adv);
    m_sourceMatch->setRange(0, 3);
    m_sourceMatch->setSpecialValueText(tr("off"));
    af->addRow(tr("Source match"), m_sourceMatch);
    m_sharp = new QDoubleSpinBox(adv);
    m_sharp->setRange(-0.05, 2.0);
    m_sharp->setSingleStep(0.05);
    m_sharp->setSpecialValueText(tr("preset's own"));
    af->addRow(tr("Sharpness"), m_sharp);
    af->addRow(noteLabel(tr("Noise options stay off: denoising is out of scope here."), adv));
    l->addWidget(adv);
    m_out = new QLabel(this);
    l->addWidget(m_out);
    m_warn = new QLabel(this);
    m_warn->setObjectName("warn");
    m_warn->setWordWrap(true);
    l->addWidget(m_warn);
    l->addWidget(noteLabel(tr("The preview uses bwdif so scrubbing stays live. View > Preview with QTGMC shows the real result on a paused "
                              "frame."),
                           this));
    m_vs = noteLabel(QString(), this);
    l->addWidget(m_vs);
    l->addStretch(1);
    connect(m_mode, qOverload<int>(&QComboBox::activated), this, &DeinterlaceTab::changed);
    connect(m_rate, qOverload<int>(&QComboBox::activated), this, &DeinterlaceTab::changed);
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, &DeinterlaceTab::changed);
    connect(m_sourceMatch, qOverload<int>(&QSpinBox::valueChanged), this, &DeinterlaceTab::changed);
    connect(m_sharp, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &DeinterlaceTab::changed);
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & (ChDeint | ChSource)) refresh();
    });
}

void DeinterlaceTab::changed() {
    DeintSettings& d = m_s->project().deint;
    d.mode = DeintMode(m_mode->currentData().toInt());
    d.doubleRate = m_rate->currentIndex() == 0;
    d.qtgmcPreset = m_preset->currentText().toStdString();
    d.sourceMatch = m_sourceMatch->value();
    d.sharpness = m_sharp->value() < 0 ? -1 : m_sharp->value();
    m_s->changed(ChDeint);
}

void DeinterlaceTab::refresh() {
    const bool open = m_s->isOpen();
    setEnabled(open);
    if (!open) return;
    const DeintSettings& d = m_s->project().deint;
    const QSignalBlocker a(m_mode), b(m_rate), c(m_preset), e(m_sourceMatch), g(m_sharp);
    m_mode->setCurrentIndex(m_mode->findData(int(d.mode)));
    m_rate->setCurrentIndex(d.doubleRate ? 0 : 1);
    m_preset->setCurrentText(QString::fromStdString(d.qtgmcPreset));
    m_sourceMatch->setValue(d.sourceMatch);
    m_sharp->setValue(d.sharpness < 0 ? -0.05 : d.sharpness);
    const bool rateApplies = d.mode == DeintMode::Qtgmc || d.mode == DeintMode::Bwdif || d.mode == DeintMode::Yadif;
    m_rate->setEnabled(rateApplies);
    m_preset->setEnabled(d.mode == DeintMode::Qtgmc);
    const Rational out = deintOutputRate(d, m_s->source().fps);
    m_out->setText(d.mode == DeintMode::None ? tr("Output stays interlaced at %1 fps, tagged %2.").arg(rate(out)).arg(fieldOrderName(m_s->project().renderFieldOrder()))
                                             : tr("Output: %1p").arg(rate(out)));
    const FieldOrderCheck& fc = m_s->fieldCheck();
    m_warn->setText(fc.comb != FieldOrder::Unknown && fc.comb != m_s->project().renderFieldOrder() && fc.combConfidence > 0.25
                        ? tr("The comb test disagrees with the field order in use (%1). Check it in the Source tab.").arg(fieldOrderName(m_s->project().renderFieldOrder()))
                        : QString());
    const VsRuntimeInfo& rt = vapoursynthRuntime();
    m_vs->setText(rt.ok ? tr("QTGMC: VapourSynth %1, in-process.").arg(QString::fromStdString(rt.version))
                        : tr("QTGMC is not available: %1").arg(QString::fromStdString(rt.error)));
}

// ---------------------------------------------------------------------------
// Color

ColorTab::ColorTab(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    QVBoxLayout* l = scrolled(this);
    auto* top = new QHBoxLayout;
    m_perChapter = new QCheckBox(tr("Per chapter"), this);
    m_perChapter->setToolTip(tr("A tape is often several recordings with different levels; PingDub marks recording boundaries as chapters."));
    m_scope = new QComboBox(this);
    top->addWidget(m_perChapter);
    top->addWidget(m_scope, 1);
    l->addLayout(top);
    auto* g = new QGroupBox(tr("Levels"), this);
    auto* f = new QFormLayout(g);
    m_bright = new SliderSpin(-50, 50, 1, 0, "", g);
    m_contrast = new SliderSpin(0.5, 2.0, 0.01, 2, "x", g);
    m_contrast->setDefault(1);
    m_gamma = new SliderSpin(0.5, 2.0, 0.01, 2, "", g);
    m_gamma->setDefault(1);
    m_black = new SliderSpin(0, 255, 1, 0, "", g);
    m_black->setDefault(16);
    m_white = new SliderSpin(0, 255, 1, 0, "", g);
    m_white->setDefault(235);
    f->addRow(tr("Brightness"), m_bright);
    f->addRow(tr("Contrast"), m_contrast);
    f->addRow(tr("Gamma"), m_gamma);
    f->addRow(tr("Black point"), m_black);
    f->addRow(tr("White point"), m_white);
    m_auto = new QPushButton(tr("Auto levels"), g);
    m_auto->setToolTip(tr("Measures the 0.1st and 99.9th luma percentiles over the trim range (active picture only) and suggests black and "
                          "white points. Nothing is applied on its own beyond those two."));
    f->addRow(m_auto);
    l->addWidget(g);
    auto* gc = new QGroupBox(tr("Color"), this);
    auto* fc = new QFormLayout(gc);
    m_sat = new SliderSpin(0, 200, 1, 0, "%", gc);
    m_sat->setDefault(100);
    m_hue = new SliderSpin(-30, 30, 0.5, 1, QString::fromUtf8("°"), gc);
    fc->addRow(tr("Saturation"), m_sat);
    fc->addRow(tr("Hue"), m_hue);
    const char* ch[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i) {
        m_lift[i] = new SliderSpin(-0.25, 0.25, 0.005, 3, "", gc);
        fc->addRow(tr("Lift %1").arg(ch[i]), m_lift[i]);
    }
    for (int i = 0; i < 3; ++i) {
        m_gammaRgb[i] = new SliderSpin(0.5, 2.0, 0.01, 2, "", gc);
        m_gammaRgb[i]->setDefault(1);
        fc->addRow(tr("Gamma %1").arg(ch[i]), m_gammaRgb[i]);
    }
    for (int i = 0; i < 3; ++i) {
        m_gain[i] = new SliderSpin(0.5, 2.0, 0.01, 2, "", gc);
        m_gain[i]->setDefault(1);
        fc->addRow(tr("Gain %1").arg(ch[i]), m_gain[i]);
    }
    l->addWidget(gc);
    auto* gch = new QGroupBox(tr("VHS chroma"), this);
    auto* fch = new QFormLayout(gch);
    m_delayX = new SliderSpin(-8, 8, 0.25, 2, tr(" px"), gch);
    m_delayX->setToolTip(tr("VHS chroma often lags luma by a few pixels. Negative moves the color left, back onto the picture."));
    m_delayY = new SliderSpin(-2, 2, 0.25, 2, tr(" lines"), gch);
    m_blur = new SliderSpin(0, 3, 0.1, 1, tr(" px"), gch);
    fch->addRow(tr("Chroma delay"), m_delayX);
    fch->addRow(tr("Chroma delay, vertical"), m_delayY);
    fch->addRow(tr("Chroma blur"), m_blur);
    l->addWidget(gch);
    auto* reset = new QPushButton(tr("Reset"), this);
    l->addWidget(reset);
    m_clip = new QLabel(this);
    l->addWidget(m_clip);
    l->addStretch(1);
    std::vector<SliderSpin*> all = {m_bright, m_contrast, m_gamma, m_black, m_white, m_sat, m_hue, m_delayX, m_delayY, m_blur};
    for (int i = 0; i < 3; ++i) {
        all.push_back(m_lift[i]);
        all.push_back(m_gammaRgb[i]);
        all.push_back(m_gain[i]);
    }
    for (SliderSpin* w : all) connect(w, &SliderSpin::valueChanged, this, &ColorTab::push);
    connect(reset, &QPushButton::clicked, this, [this] {
        current() = ColorSettings{};
        m_s->changed(ChColor);
    });
    connect(m_perChapter, &QCheckBox::toggled, this, [this](bool on) {
        m_s->project().colorPerChapter = on;
        m_s->changed(ChColor);
    });
    connect(m_scope, qOverload<int>(&QComboBox::activated), this, [this] { refresh(); });
    connect(m_auto, &QPushButton::clicked, this, &ColorTab::autoLevels);
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & (ChColor | ChSource)) refresh();
    });
}

ColorSettings& ColorTab::current() {
    Project& p = m_s->project();
    const int ch = m_scope->currentData().toInt();
    if (!p.colorPerChapter || ch < 0) return p.color;
    auto it = p.chapterColors.find(ch);
    if (it == p.chapterColors.end()) it = p.chapterColors.emplace(ch, p.color).first;
    return it->second;
}

void ColorTab::push() {
    ColorSettings& c = current();
    c.brightness = m_bright->value();
    c.contrast = m_contrast->value();
    c.gamma = m_gamma->value();
    c.blackPoint = m_black->value();
    c.whitePoint = m_white->value();
    c.saturation = m_sat->value() / 100.0;
    c.hue = m_hue->value();
    for (int i = 0; i < 3; ++i) {
        c.lift[size_t(i)] = m_lift[i]->value();
        c.gammaRgb[size_t(i)] = m_gammaRgb[i]->value();
        c.gain[size_t(i)] = m_gain[i]->value();
    }
    c.chromaDelayX = m_delayX->value();
    c.chromaDelayY = m_delayY->value();
    c.chromaBlur = m_blur->value();
    m_s->changed(ChColor);
}

void ColorTab::refresh() {
    const bool open = m_s->isOpen();
    setEnabled(open);
    if (!open) return;
    const Project& p = m_s->project();
    {
        const QSignalBlocker a(m_perChapter), b(m_scope);
        m_perChapter->setChecked(p.colorPerChapter);
        const int keep = m_scope->currentData().isValid() ? m_scope->currentData().toInt() : -1;
        m_scope->clear();
        m_scope->addItem(tr("Whole clip"), -1);
        for (size_t i = 0; i < m_s->source().chapters.size(); ++i)
            m_scope->addItem(QString::fromStdString(m_s->source().chapters[i].title.empty() ? "Chapter " + std::to_string(i + 1)
                                                                                           : m_s->source().chapters[i].title),
                             int(i));
        const int idx = m_scope->findData(keep);
        m_scope->setCurrentIndex(idx < 0 ? 0 : idx);
        m_scope->setEnabled(p.colorPerChapter && !m_s->source().chapters.empty());
        m_perChapter->setEnabled(!m_s->source().chapters.empty());
    }
    const ColorSettings& c = const_cast<ColorTab*>(this)->current();
    m_bright->setValue(c.brightness);
    m_contrast->setValue(c.contrast);
    m_gamma->setValue(c.gamma);
    m_black->setValue(c.blackPoint);
    m_white->setValue(c.whitePoint);
    m_sat->setValue(c.saturation * 100.0);
    m_hue->setValue(c.hue);
    for (int i = 0; i < 3; ++i) {
        m_lift[i]->setValue(c.lift[size_t(i)]);
        m_gammaRgb[i]->setValue(c.gammaRgb[size_t(i)]);
        m_gain[i]->setValue(c.gain[size_t(i)]);
    }
    m_delayX->setValue(c.chromaDelayX);
    m_delayY->setValue(c.chromaDelayY);
    m_blur->setValue(c.chromaBlur);
}

void ColorTab::setClipped(double share) {
    if (share <= 0) {
        m_clip->setText(tr("<span style='color:#3FB950'>Nothing pushed past legal range.</span>"));
        return;
    }
    const QString col = share > 0.005 ? "#F85149" : "#D29922";
    m_clip->setText(tr("<span style='color:%1'>%2% of pixels pushed past legal range by these settings (clipped on the way out).</span>")
                        .arg(col)
                        .arg(share * 100.0, 0, 'f', 2));
}

void ColorTab::autoLevels() {
    if (!m_s->isOpen()) return;
    m_auto->setEnabled(false);
    m_auto->setText(tr("Measuring..."));
    const SourceInfo s = m_s->source();
    const Project p = m_s->project();
    // Over the trim range, or the selected chapter.
    int64_t a = std::max<int64_t>(0, p.edit.in), b = p.edit.out < 0 ? s.frames : p.edit.out;
    const int ch = m_scope->currentData().toInt();
    if (p.colorPerChapter && ch >= 0 && ch < int(s.chapters.size())) {
        a = std::max(a, s.chapters[size_t(ch)].start);
        b = std::min(b, s.chapters[size_t(ch)].end);
    }
    auto hist = std::make_shared<LevelHistogram>();
    runInBackground(
        this,
        [s, p, a, b, hist] {
            SourceReader r;
            std::string err;
            if (!r.open(s, err) || b <= a) return;
            const int64_t step = std::max<int64_t>(1, (b - a) / 200);
            for (int64_t n = a; n < b; n += step) {
                bool cut = false;
                for (const CutSpan& c : p.edit.cuts) cut |= n >= c.start && n < c.end;
                if (cut) continue;
                FramePtr f = r.read(n);
                Picture pic;
                if (f && toPicture(f.get(), pic, true)) hist->add(pic, 16, 8);
            }
        },
        [this, hist] {
            m_auto->setEnabled(true);
            m_auto->setText(tr("Auto levels"));
            double bl = 16, wh = 235;
            if (!hist->suggest(bl, wh)) return;
            ColorSettings& c = current();
            c.blackPoint = bl;
            c.whitePoint = wh;
            m_s->changed(ChColor);
        });
}

// ---------------------------------------------------------------------------
// Crop

CropTab::CropTab(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    QVBoxLayout* l = scrolled(this);
    auto* f = new QFormLayout;
    auto spin = [&] {
        auto* sp = new QSpinBox(this);
        sp->setRange(0, 400);
        sp->setSuffix(tr(" px"));
        sp->setKeyboardTracking(false);
        return sp;
    };
    m_l = spin();
    m_r = spin();
    m_t = spin();
    m_b = spin();
    f->addRow(tr("Left"), m_l);
    f->addRow(tr("Right"), m_r);
    f->addRow(tr("Top"), m_t);
    f->addRow(tr("Bottom"), m_b);
    l->addLayout(f);
    m_edit = new QCheckBox(tr("Drag the edges on the preview"), this);
    l->addWidget(m_edit);
    auto* sg = new QGroupBox(tr("Safe crop"), this);
    auto* sl = new QVBoxLayout(sg);
    m_safe = new QLabel(sg);
    m_safe->setWordWrap(true);
    sl->addWidget(m_safe);
    m_toSafe = new QPushButton(tr("Crop to the safe guide"), sg);
    sl->addWidget(m_toSafe);
    l->addWidget(sg);
    auto* af = new QFormLayout;
    m_aspect = new QComboBox(this);
    m_aspect->addItem(tr("Keep pixel aspect as a tag"));
    m_aspect->addItem(tr("Resize to square pixels"));
    m_aspect->setToolTip(tr("The tag (10:11 or 12:11) makes players show the right shape with no pixels resampled. Resize only for targets "
                            "that ignore it."));
    af->addRow(tr("Pixel aspect"), m_aspect);
    l->addLayout(af);
    m_result = new QLabel(this);
    m_result->setWordWrap(true);
    l->addWidget(m_result);
    l->addWidget(noteLabel(tr("Crop happens after deinterlacing, so any top and bottom value works. With a 4:2:0 output the size snaps to "
                              "even numbers; with None deinterlacing, top and bottom snap to even to keep field parity."),
                           this));
    l->addStretch(1);
    for (QSpinBox* sp : {m_l, m_r, m_t, m_b}) connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, &CropTab::changed);
    connect(m_aspect, qOverload<int>(&QComboBox::activated), this, &CropTab::changed);
    connect(m_edit, &QCheckBox::toggled, this, &CropTab::editToggled);
    connect(m_toSafe, &QPushButton::clicked, this, [this] {
        int sl = 0, sr = 0, sb = 0;
        safeCrop(sl, sr, sb);
        CropSettings& c = m_s->project().crop;
        c.left = std::max(c.left, sl);
        c.right = std::max(c.right, sr);
        c.bottom = std::max(c.bottom, sb);
        m_s->changed(ChCrop);
    });
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & (ChCrop | ChAnalysis | ChEdge | ChOutput | ChDeint | ChSource)) refresh();
    });
}

bool CropTab::editing() const { return m_edit->isChecked(); }

void CropTab::safeCrop(int& l, int& r, int& bottom) const {
    l = r = 0;
    // PingDub's bottom 8 head-switching lines go no matter what.
    bottom = 8;
    if (m_s->isOpen() && m_s->project().edgeActive() && !m_s->tracks().fields.empty())
        m_s->tracks().safeCrop(m_s->project().edge.strength, l, r);
}

void CropTab::changed() {
    CropSettings& c = m_s->project().crop;
    c.left = m_l->value();
    c.right = m_r->value();
    c.top = m_t->value();
    c.bottom = m_b->value();
    c.squarePixels = m_aspect->currentIndex() == 1;
    m_s->changed(ChCrop);
}

void CropTab::refresh() {
    const bool open = m_s->isOpen();
    setEnabled(open);
    if (!open) return;
    const CropSettings& c = m_s->project().crop;
    const QSignalBlocker a(m_l), b(m_r), d(m_t), e(m_b), f(m_aspect);
    m_l->setValue(c.left);
    m_r->setValue(c.right);
    m_t->setValue(c.top);
    m_b->setValue(c.bottom);
    m_aspect->setCurrentIndex(c.squarePixels ? 1 : 0);
    int sl, sr, sb;
    safeCrop(sl, sr, sb);
    m_safe->setText(tr("Left %1 px, right %2 px (the largest Edge Lock shift per side), bottom %3 lines (head switching).").arg(sl).arg(sr).arg(sb));
    const SourceInfo& s = m_s->source();
    const OutputGeometry g = outputGeometry(m_s->project(), s, encoderPixelFormat(effectiveOutput(m_s->project().out), s));
    QString t = tr("Output: <b>%1x%2</b>, pixel aspect %3:%4").arg(g.width).arg(g.height).arg(g.sar.num).arg(g.sar.den);
    for (const std::string& n : g.notes) t += "<br>" + QString::fromStdString(n).toHtmlEscaped();
    m_result->setText(t);
}

// ---------------------------------------------------------------------------
// Output

OutputTab::OutputTab(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    QVBoxLayout* l = scrolled(this);
    auto* gv = new QGroupBox(tr("Video"), this);
    auto* f = new QFormLayout(gv);
    m_codec = new QComboBox(gv);
    m_codec->addItem(tr("x265 (HEVC)"), int(VideoCodec::X265));
    m_codec->addItem(tr("x264 (AVC)"), int(VideoCodec::X264));
    m_codec->addItem(tr("SVT-AV1"), int(VideoCodec::SvtAv1));
    m_codec->addItem(tr("FFV1 (lossless)"), int(VideoCodec::Ffv1));
    f->addRow(tr("Encoder"), m_codec);
    m_rf = new SliderSpin(0, 51, 1, 0, "", gv);
    m_rf->setToolTip(tr("RF, the same idea and scale as HandBrake's: lower is better and bigger. It maps straight to the encoder's CRF."));
    f->addRow(tr("RF"), m_rf);
    m_preset = new QComboBox(gv);
    f->addRow(tr("Preset"), m_preset);
    m_422 = new QCheckBox(tr("4:2:2 chroma"), gv);
    m_422->setToolTip(tr("4:2:0 is the default because every player handles it, and VHS chroma is soft enough that halving its vertical "
                         "resolution costs nothing you can see."));
    f->addRow(m_422);
    l->addWidget(gv);
    auto* ga = new QGroupBox(tr("Audio"), this);
    auto* fa = new QFormLayout(ga);
    m_audio = new QComboBox(ga);
    m_audio->addItem(tr("FLAC (lossless)"), int(AudioCodec::Flac));
    m_audio->addItem(tr("Opus"), int(AudioCodec::Opus));
    m_audio->addItem(tr("AAC"), int(AudioCodec::Aac));
    m_audio->addItem(tr("Copy (PCM untouched)"), int(AudioCodec::Copy));
    fa->addRow(tr("Codec"), m_audio);
    m_kbps = new QSpinBox(ga);
    m_kbps->setRange(32, 512);
    m_kbps->setSuffix(tr(" kb/s"));
    m_kbps->setKeyboardTracking(false);
    fa->addRow(tr("Bitrate"), m_kbps);
    fa->addRow(noteLabel(tr("Audio is never resampled or re-timed. PingDub already made it line up."), ga));
    l->addWidget(ga);
    auto* gc = new QGroupBox(tr("File"), this);
    auto* fc = new QFormLayout(gc);
    m_container = new QComboBox(gc);
    m_container->addItem(tr("MKV"));
    m_container->addItem(tr("MP4"));
    m_container->setToolTip(tr("MP4 players handle FLAC and subtitle tracks badly, so with MP4 those are off and audio is AAC or Opus."));
    fc->addRow(tr("Container"), m_container);
    auto* fr = new QHBoxLayout;
    m_folder = new QLineEdit(gc);
    auto* browse = new QPushButton(tr("..."), gc);
    browse->setMaximumWidth(36);
    fr->addWidget(m_folder, 1);
    fr->addWidget(browse);
    fc->addRow(tr("Folder"), fr);
    m_name = new QLineEdit(gc);
    fc->addRow(tr("Name"), m_name);
    m_chapters = new QCheckBox(tr("Chapters"), gc);
    m_captions = new QCheckBox(tr("Captions (from PingDub's SRT sidecars)"), gc);
    fc->addRow(m_chapters);
    fc->addRow(m_captions);
    l->addWidget(gc);
    m_path = new QLabel(this);
    m_path->setWordWrap(true);
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(m_path);
    auto* br = new QHBoxLayout;
    m_render = new QPushButton(tr("Render (F6)"), this);
    m_render->setObjectName("primaryButton");
    m_queue = new QPushButton(tr("Add to queue"), this);
    br->addWidget(m_render, 1);
    br->addWidget(m_queue);
    l->addLayout(br);
    l->addWidget(noteLabel(tr("Hardware encoders are left out: VAAPI on AMD has no RF-style constant quality, so the RF you pick would mean "
                              "something else."),
                           this));
    l->addStretch(1);

    connect(m_codec, qOverload<int>(&QComboBox::activated), this, [this] {
        OutputSettings& o = m_s->project().out;
        const VideoCodec c = VideoCodec(m_codec->currentData().toInt());
        if (c != o.codec) {
            o.codec = c;
            o.rf = rfDefault(c);
            o.preset = presetDefault(c);
        }
        m_s->changed(ChOutput);
    });
    connect(m_rf, &SliderSpin::valueChanged, this, &OutputTab::changed);
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, &OutputTab::changed);
    connect(m_422, &QCheckBox::toggled, this, &OutputTab::changed);
    connect(m_audio, qOverload<int>(&QComboBox::activated), this, [this] {
        OutputSettings& o = m_s->project().out;
        o.audio = AudioCodec(m_audio->currentData().toInt());
        m_s->changed(ChOutput);
    });
    connect(m_kbps, qOverload<int>(&QSpinBox::valueChanged), this, &OutputTab::changed);
    connect(m_container, qOverload<int>(&QComboBox::activated), this, &OutputTab::changed);
    connect(m_folder, &QLineEdit::editingFinished, this, &OutputTab::changed);
    connect(m_name, &QLineEdit::editingFinished, this, &OutputTab::changed);
    connect(m_chapters, &QCheckBox::toggled, this, &OutputTab::changed);
    connect(m_captions, &QCheckBox::toggled, this, &OutputTab::changed);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Output folder"), m_folder->text());
        if (d.isEmpty()) return;
        m_folder->setText(d);
        AppSettings::instance().lastOutputFolder = d;
        AppSettings::instance().save();
        changed();
    });
    connect(m_render, &QPushButton::clicked, this, &OutputTab::renderRequested);
    connect(m_queue, &QPushButton::clicked, this, &OutputTab::queueRequested);
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & (ChOutput | ChSource | ChEdit | ChDeint)) refresh();
    });
}

void OutputTab::changed() {
    OutputSettings& o = m_s->project().out;
    o.rf = int(std::lround(m_rf->value()));
    o.preset = m_preset->currentText().toStdString();
    o.chroma422 = m_422->isChecked();
    if (o.audio == AudioCodec::Opus) o.opusKbps = m_kbps->value();
    if (o.audio == AudioCodec::Aac) o.aacKbps = m_kbps->value();
    o.mp4 = m_container->currentIndex() == 1;
    o.folder = m_folder->text().toStdString();
    o.name = m_name->text().toStdString();
    o.chapters = m_chapters->isChecked();
    o.captions = m_captions->isChecked();
    m_s->changed(ChOutput);
}

void OutputTab::refresh() {
    const bool open = m_s->isOpen();
    setEnabled(open);
    if (!open) return;
    OutputSettings& o = m_s->project().out;
    if (o.folder.empty() && !AppSettings::instance().lastOutputFolder.isEmpty()) o.folder = AppSettings::instance().lastOutputFolder.toStdString();
    const QSignalBlocker a(m_codec), b(m_preset), c(m_422), d(m_audio), e(m_kbps), f(m_container), g(m_folder), h(m_name), i(m_chapters), j(m_captions);
    m_codec->setCurrentIndex(m_codec->findData(int(o.codec)));
    const int mx = rfMax(o.codec);
    m_rf->setEnabled(mx > 0);
    m_rf->spin()->setRange(0, std::max(1, mx));
    m_rf->setValue(o.rf);
    m_rf->setDefault(rfDefault(o.codec));
    m_preset->clear();
    for (const std::string& p : presetsFor(o.codec)) m_preset->addItem(QString::fromStdString(p));
    m_preset->setEnabled(m_preset->count() > 0);
    m_preset->setCurrentText(QString::fromStdString(o.preset));
    m_422->setEnabled(o.codec == VideoCodec::X265 || o.codec == VideoCodec::X264);
    m_422->setChecked(o.chroma422);
    m_audio->setCurrentIndex(m_audio->findData(int(o.audio)));
    m_kbps->setEnabled(o.audio == AudioCodec::Opus || o.audio == AudioCodec::Aac);
    m_kbps->setValue(o.audio == AudioCodec::Aac ? o.aacKbps : o.opusKbps);
    m_container->setCurrentIndex(o.mp4 ? 1 : 0);
    // MP4 has no place for FLAC, untouched PCM or the caption tracks.
    for (int k = 0; k < m_audio->count(); ++k) {
        const AudioCodec ac = AudioCodec(m_audio->itemData(k).toInt());
        const bool ok = !o.mp4 || (ac != AudioCodec::Flac && ac != AudioCodec::Copy);
        if (auto* model = qobject_cast<QStandardItemModel*>(m_audio->model())) model->item(k)->setEnabled(ok);
    }
    m_captions->setEnabled(!o.mp4);
    m_folder->setText(QString::fromStdString(o.folder));
    m_folder->setPlaceholderText(tr("asked when you render"));
    m_name->setText(QString::fromStdString(o.name));
    m_name->setPlaceholderText(QString::fromStdString(defaultOutputName(m_s->source())));
    m_chapters->setChecked(o.chapters);
    m_captions->setChecked(o.captions && !o.mp4);
    const OutputSettings eff = effectiveOutput(o);
    QString note;
    if (eff.audio != o.audio) note = tr("<br><span style='color:#D29922'>MP4: audio will be AAC.</span>");
    if (o.folder.empty())
        m_path->setText(tr("Writes <b>%1</b> and a .uindub.json sidecar into a folder you pick. Nothing goes next to the master unless you pick its folder.")
                            .arg(QString::fromStdString(defaultOutputName(m_s->source()) + (eff.mp4 ? ".mp4" : ".mkv")).toHtmlEscaped()) +
                        note);
    else
        m_path->setText(tr("Writes <b>%1</b> and a .uindub.json sidecar with every setting next to it.").arg(QString::fromStdString(outputPath(o, m_s->source())).toHtmlEscaped()) + note);
}

}  // namespace ud
