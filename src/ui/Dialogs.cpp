// SPDX-License-Identifier: GPL-3.0-or-later
#include "Dialogs.h"

#include "App.h"
#include "core/Warble.h"
#include "shared/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QPointer>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <atomic>

namespace ud {

WarbleDialog::WarbleDialog(const SourceInfo& clean, FieldOrder order, int64_t start, QWidget* parent)
    : QDialog(parent), m_clean(clean), m_order(order) {
    setWindowTitle(tr("Warble test"));
    auto* l = new QVBoxLayout(this);
    l->addWidget(noteLabel(tr("Takes a clean clip (a real PingDub capture of a good tape is best) and writes a damaged FFV1 copy with the same "
                              "damage model as the synthetic tests. The true offsets go into the copy's project: open it, run Analyze, and "
                              "UinDub scores itself against them. That is how the Edge Lock defaults get tuned, and how you check them on "
                              "your own footage before a bad tape ever shows up."),
                           this));
    auto* g = new QGroupBox(tr("Damage"), this);
    auto* f = new QFormLayout(g);
    const DamageModel d;
    m_jitter = new SliderSpin(0, 8, 0.1, 1, tr(" px"), g);
    m_jitter->setValue(d.jitter);
    m_jitter->setDefault(d.jitter);
    f->addRow(tr("Per-line jitter (+-)"), m_jitter);
    m_wobble = new SliderSpin(0, 10, 0.1, 1, tr(" px"), g);
    m_wobble->setValue(d.wobble);
    m_wobble->setDefault(d.wobble);
    f->addRow(tr("Wobble (+-)"), m_wobble);
    m_period = new SliderSpin(8, 240, 1, 0, tr(" lines"), g);
    m_period->setValue(d.wobblePeriod);
    m_period->setDefault(d.wobblePeriod);
    f->addRow(tr("Wobble period"), m_period);
    m_crawl = new SliderSpin(0, 1, 0.01, 2, tr(" rad/field"), g);
    m_crawl->setValue(d.wobbleCrawl);
    m_crawl->setDefault(d.wobbleCrawl);
    f->addRow(tr("Wobble crawl"), m_crawl);
    m_hook = new SliderSpin(0, 22, 0.5, 1, tr(" px"), g);
    m_hook->setValue(d.hook);
    m_hook->setDefault(d.hook);
    f->addRow(tr("Top hook"), m_hook);
    m_hookLines = new SliderSpin(2, 60, 1, 0, tr(" lines"), g);
    m_hookLines->setValue(d.hookLines);
    m_hookLines->setDefault(d.hookLines);
    f->addRow(tr("Top hook fades by"), m_hookLines);
    m_field = new SliderSpin(0, 4, 0.1, 1, tr(" px"), g);
    m_field->setValue(d.fieldOffset);
    m_field->setDefault(d.fieldOffset);
    f->addRow(tr("Field offset"), m_field);
    l->addWidget(g);
    auto* rf = new QFormLayout;
    m_start = new QSpinBox(this);
    m_start->setRange(0, int(std::max<int64_t>(0, clean.frames - 1)));
    m_start->setValue(int(start));
    m_frames = new QSpinBox(this);
    m_frames->setRange(1, int(std::max<int64_t>(1, clean.frames)));
    m_frames->setValue(int(std::min<int64_t>(clean.frames - start, int64_t(std::llround(clean.fps.toDouble() * 20)))));
    rf->addRow(tr("From frame"), m_start);
    rf->addRow(tr("Frames"), m_frames);
    auto* orow = new QHBoxLayout;
    m_out = new QLineEdit(this);
    const QFileInfo src(QString::fromStdString(clean.segments.size() > 1 ? clean.basePath + ".mkv" : clean.path));
    m_out->setText(QDir(QString::fromStdString(defaultProjectFolder(clean.path))).absoluteFilePath("../" + src.completeBaseName() + ".warble.mkv"));
    m_out->setText(QFileInfo(m_out->text()).absoluteFilePath());
    auto* browse = new QPushButton(tr("..."), this);
    orow->addWidget(m_out, 1);
    orow->addWidget(browse);
    rf->addRow(tr("Damaged copy"), orow);
    l->addLayout(rf);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    l->addWidget(m_progress);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_go = bb->addButton(tr("Make the copy"), QDialogButtonBox::AcceptRole);
    l->addWidget(bb);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Damaged copy"), m_out->text(), tr("Matroska (*.mkv)"));
        if (!f.isEmpty()) m_out->setText(f);
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_go, &QPushButton::clicked, this, &WarbleDialog::make);
}

void WarbleDialog::make() {
    WarbleJob job;
    job.clean = m_clean;
    job.order = m_order;
    job.damage.jitter = m_jitter->value();
    job.damage.wobble = m_wobble->value();
    job.damage.wobblePeriod = m_period->value();
    job.damage.wobbleCrawl = m_crawl->value();
    job.damage.hook = m_hook->value();
    job.damage.hookLines = m_hookLines->value();
    job.damage.fieldOffset = m_field->value();
    job.damage.seed = uint32_t(QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF);
    job.start = m_start->value();
    job.frames = m_frames->value();
    job.outPath = m_out->text().toStdString();
    job.projectFolder = defaultProjectFolder(job.outPath);
    m_go->setEnabled(false);
    m_progress->setVisible(true);
    auto res = std::make_shared<WarbleResult>();
    auto frac = std::make_shared<std::atomic<int>>(0);
    QPointer<WarbleDialog> self(this);
    runInBackground(
        this,
        [job, res, frac, self] {
            *res = makeWarbleCopy(job, [frac, self](double f) {
                frac->store(int(f * 1000));
                QMetaObject::invokeMethod(
                    self, [self, frac] { if (self) self->m_progress->setValue(frac->load()); }, Qt::QueuedConnection);
                return true;
            });
        },
        [this, res] {
            m_go->setEnabled(true);
            if (!res->ok) {
                QMessageBox::warning(this, tr("Warble test"), QString::fromStdString(res->error));
                return;
            }
            m_created = QString::fromStdString(res->project.folder);
            accept();
        });
}

// ---------------------------------------------------------------------------

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Preferences"));
    auto* l = new QVBoxLayout(this);
    auto* f = new QFormLayout;
    auto* theme = new QComboBox(this);
    ThemeManager& tm = ThemeManager::instance();
    for (const QString& id : tm.ids()) theme->addItem(tm.nameOf(id), id);
    theme->setCurrentIndex(std::max(0, theme->findData(tm.current())));
    f->addRow(tr("Theme"), theme);
    l->addLayout(f);
    l->addWidget(noteLabel(tr("Themes use PingDub's format: a file copied from ~/.config/pingdub/themes/ to ~/.config/uindub/themes/ works as is."), this));
    auto* kg = new QGroupBox(tr("Keys"), this);
    auto* kf = new QFormLayout(kg);
    for (const KeyAction& a : keyActions()) {
        auto* e = new QKeySequenceEdit(AppSettings::instance().key(a.id), kg);
        m_keys[a.id] = e;
        kf->addRow(a.label, e);
    }
    auto* reset = new QPushButton(tr("Default keys"), kg);
    kf->addRow(reset);
    l->addWidget(kg);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    l->addWidget(bb);
    connect(theme, qOverload<int>(&QComboBox::activated), this, [theme] {
        const QString id = theme->currentData().toString();
        ThemeManager::instance().apply(id);
        AppSettings::instance().theme = id;
        AppSettings::instance().save();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        for (const KeyAction& a : keyActions()) m_keys[a.id]->setKeySequence(a.fallback);
    });
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        for (auto it = m_keys.begin(); it != m_keys.end(); ++it) AppSettings::instance().keys[it.key()] = it.value()->keySequence();
        AppSettings::instance().save();
        emit keysChanged();
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

}  // namespace ud
