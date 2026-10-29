// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreviewWidget.h"

#include "shared/Theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVector2D>
#include <QWheelEvent>

#include <cmath>
#include <cstring>

namespace ud {

static const char* kVert = R"(
attribute vec2 aPos;
attribute vec2 aTex;
varying vec2 vTex;
void main() {
    vTex = aTex;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Two sets of 4:4:4 planes, processed and source; `split` picks per column.
static const char* kFrag = R"(
#ifdef GL_ES
precision highp float;
#endif
varying vec2 vTex;
uniform sampler2D tY0;
uniform sampler2D tU0;
uniform sampler2D tV0;
uniform sampler2D tY1;
uniform sampler2D tU1;
uniform sampler2D tV1;
uniform float split;   // processed left of it, source right; 2 all processed, -1 all source
void main() {
    vec3 yuv;
    if (vTex.x < split) yuv = vec3(texture2D(tY0, vTex).r, texture2D(tU0, vTex).r, texture2D(tV0, vTex).r);
    else yuv = vec3(texture2D(tY1, vTex).r, texture2D(tU1, vTex).r, texture2D(tV1, vTex).r);
    // BT.601, limited range
    float Y = (yuv.x * 255.0 - 16.0) / 219.0;
    float U = (yuv.y * 255.0 - 128.0) / 224.0;
    float V = (yuv.z * 255.0 - 128.0) / 224.0;
    vec3 rgb = vec3(Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U);
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

PreviewWidget::PreviewWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_message = tr("Open a PingDub capture, or any interlaced SD file (File > Open).");
}

PreviewWidget::~PreviewWidget() {
    makeCurrent();
    if (m_gl) glDeleteTextures(6, m_tex);
    doneCurrent();
}

void PreviewWidget::initializeGL() {
    initializeOpenGLFunctions();
    m_prog.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    m_prog.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    m_prog.bindAttributeLocation("aPos", 0);
    m_prog.bindAttributeLocation("aTex", 1);
    m_prog.link();
    glGenTextures(6, m_tex);
    for (unsigned t : m_tex) {
        glBindTexture(GL_TEXTURE_2D, t);
        // Nearest: zoomed in, every sample is a crisp block.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    m_gl = true;
}

void PreviewWidget::showFrame(PreviewFramePtr f) {
    m_frame = std::move(f);
    m_uploaded = false;
    update();
}

void PreviewWidget::clear(const QString& message) {
    m_frame.reset();
    m_message = message;
    update();
}

void PreviewWidget::setView(View v) {
    m_view = v;
    update();
}

void PreviewWidget::setZoom(int z) {
    m_zoom = z;
    m_pan = {};
    update();
    emit zoomChanged(z);
}

void PreviewWidget::startDrawing() {
    m_drawing = true;
    m_points.clear();
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void PreviewWidget::cancelDrawing() {
    m_drawing = false;
    m_points.clear();
    unsetCursor();
    update();
}

void PreviewWidget::setCrop(int l, int r, int t, int b) {
    m_crop[0] = l;
    m_crop[1] = r;
    m_crop[2] = t;
    m_crop[3] = b;
    update();
}

void PreviewWidget::setSafeCrop(int l, int r, int bottom) {
    m_safe[0] = l;
    m_safe[1] = r;
    m_safe[2] = bottom;
    update();
}

void PreviewWidget::setCropEditing(bool on) {
    m_cropEdit = on;
    update();
}

void PreviewWidget::setOverlayVisible(bool on) {
    m_overlay = on;
    update();
}

QRectF PreviewWidget::imageRect() const {
    if (!m_frame || m_frame->width <= 0) return {};
    const double sar = m_frame->sar.valid() ? m_frame->sar.toDouble() : 1.0;
    const double iw = m_frame->width * sar, ih = m_frame->height;
    double scale;
    if (m_zoom <= 0) scale = std::min(width() / iw, height() / ih);
    else scale = double(m_zoom) / devicePixelRatioF();
    const double w = iw * scale, h = ih * scale;
    return QRectF((width() - w) / 2 + m_pan.x(), (height() - h) / 2 + m_pan.y(), w, h);
}

QPointF PreviewWidget::toImage(const QPointF& p) const {
    const QRectF r = imageRect();
    if (r.isEmpty()) return {};
    return {(p.x() - r.left()) / r.width() * m_frame->width, (p.y() - r.top()) / r.height() * m_frame->height};
}

QPointF PreviewWidget::toWidget(double x, double y) const {
    const QRectF r = imageRect();
    if (r.isEmpty()) return {};
    return {r.left() + x / m_frame->width * r.width(), r.top() + y / m_frame->height * r.height()};
}

void PreviewWidget::upload(int set, const PreviewFrame& f) {
    const std::vector<uint8_t>* planes[3] = {set ? &f.sy : &f.y, set ? &f.su : &f.u, set ? &f.sv : &f.v};
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 3; ++i) {
        const int idx = set * 3 + i;
        glBindTexture(GL_TEXTURE_2D, m_tex[idx]);
        if (planes[i]->size() < size_t(f.width) * size_t(f.height)) continue;
        if (m_tw[idx] != f.width || m_th[idx] != f.height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, f.width, f.height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]->data());
            m_tw[idx] = f.width;
            m_th[idx] = f.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]->data());
        }
    }
}

void PreviewWidget::paintGL() {
    QPainter painter(this);
    painter.beginNativePainting();
    const QColor s = StatusColor::PreviewSurround;
    glClearColor(float(s.redF()), float(s.greenF()), float(s.blueF()), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const bool have = m_frame && m_frame->width > 0 && !m_frame->y.empty();
    if (have) {
        if (!m_uploaded) {
            upload(0, *m_frame);
            upload(1, *m_frame);
            m_uploaded = true;
        }
        const QRectF r = imageRect();
        const double dpr = devicePixelRatioF();
        glViewport(int(std::lround(r.left() * dpr)), int(std::lround((height() - r.bottom()) * dpr)), int(std::lround(r.width() * dpr)),
                   int(std::lround(r.height() * dpr)));
        m_prog.bind();
        for (int i = 0; i < 6; ++i) {
            glActiveTexture(GL_TEXTURE0 + unsigned(i));
            glBindTexture(GL_TEXTURE_2D, m_tex[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        m_prog.setUniformValue("tY0", 0);
        m_prog.setUniformValue("tU0", 1);
        m_prog.setUniformValue("tV0", 2);
        m_prog.setUniformValue("tY1", 3);
        m_prog.setUniformValue("tU1", 4);
        m_prog.setUniformValue("tV1", 5);
        const float split = m_view == View::Processed ? 2.0f : (m_view == View::Source ? -1.0f : float(m_split));
        m_prog.setUniformValue("split", split);
        static const GLfloat pos[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        static const GLfloat tex[] = {0, 1, 1, 1, 0, 0, 1, 0};
        m_prog.enableAttributeArray(0);
        m_prog.enableAttributeArray(1);
        m_prog.setAttributeArray(0, GL_FLOAT, pos, 2);
        m_prog.setAttributeArray(1, GL_FLOAT, tex, 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_prog.disableAttributeArray(0);
        m_prog.disableAttributeArray(1);
        m_prog.release();
        glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    }
    painter.endNativePainting();
    if (!have) {
        painter.setPen(QColor(0xE0, 0xE0, 0xE0));
        painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, m_frame && !m_frame->error.isEmpty() ? m_frame->error : m_message);
        return;
    }
    paintOverlay(painter);
}

// White text on a dark plate, readable over any picture. `at` is the
// plate's top-left (AlignLeft) or top-right (AlignRight) corner.
static void tag(QPainter& p, QPointF at, Qt::Alignment align, const QString& text) {
    const QFontMetricsF fm(p.font());
    const QSizeF sz(fm.horizontalAdvance(text) + 12, fm.height() + 6);
    const QRectF box(align & Qt::AlignRight ? at.x() - sz.width() : at.x(), at.y(), sz.width(), sz.height());
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 170));
    p.drawRoundedRect(box, 3, 3);
    p.setPen(QColor(255, 255, 255));
    p.drawText(box, Qt::AlignCenter, text);
    p.restore();
}

void PreviewWidget::paintOverlay(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = imageRect();
    const PreviewFrame& f = *m_frame;
    // Crop: dim what goes, outline what stays.
    const bool anyCrop = m_crop[0] || m_crop[1] || m_crop[2] || m_crop[3];
    if (anyCrop || m_cropEdit) {
        const QPointF a = toWidget(m_crop[0], m_crop[2]), b = toWidget(f.width - m_crop[1], f.height - m_crop[3]);
        const QRectF keep(a, b);
        QPainterPath outside;
        outside.addRect(r);
        QPainterPath in;
        in.addRect(keep);
        p.fillPath(outside.subtracted(in), QColor(0, 0, 0, 120));
        p.setPen(QPen(QColor(255, 255, 255, 200), 1));
        p.drawRect(keep);
        if (m_cropEdit && (m_safe[0] || m_safe[1] || m_safe[2])) {
            // The safe crop guide: largest Edge Lock shift per side, PingDub's bottom lines.
            p.setPen(QPen(StatusColor::Warn, 1, Qt::DashLine));
            const QPointF sa = toWidget(m_safe[0], 0), sb = toWidget(f.width - m_safe[1], f.height - m_safe[2]);
            p.drawLine(QPointF(sa.x(), r.top()), QPointF(sa.x(), r.bottom()));
            p.drawLine(QPointF(sb.x(), r.top()), QPointF(sb.x(), r.bottom()));
            p.drawLine(QPointF(r.left(), sb.y()), QPointF(r.right(), sb.y()));
        }
    }
    if (m_view == View::Split) {
        const double x = r.left() + m_split * r.width();
        p.setPen(QPen(QColor(255, 255, 255, 220), 2));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        tag(p, QPointF(x - 6, r.top() + 6), Qt::AlignRight, tr("processed"));
        tag(p, QPointF(x + 6, r.top() + 6), Qt::AlignLeft, tr("source"));
    }
    if (m_overlay) {
        // Edge Lock: each line's tracked edge, green measured, amber
        // interpolated, red where the reference is lost.
        const double th = r.height() / f.height;  // widget px per display row
        const double rowStep = f.fieldView ? 2.0 : 1.0;
        for (const OverlayRef& o : f.overlay) {
            const QColor refc = o.slot < int(m_refColors.size()) ? m_refColors[size_t(o.slot)] : QColor(Qt::white);
            if (o.kind == RefKind::Drawn && o.shape.size() >= 2) {
                p.setPen(QPen(refc, 1, Qt::DashLine));
                QPolygonF poly;
                for (const PointF& q : o.shape) poly << toWidget(q.x, q.y);
                p.drawPolyline(poly);
            }
            const bool lost = o.state == RefState::Lost || o.state == RefState::Dark || o.state == RefState::Ended;
            for (size_t l = 0; l < o.pos.size(); ++l) {
                const int y = 2 * int(l) + f.parity;
                const bool measured = std::isfinite(o.pos[l]);
                const bool interp = !measured && f.haveOffsets && y < int(f.offsets.conf.size()) && f.offsets.conf[size_t(y)] == kConfInterpolated;
                QColor c;
                if (measured && !lost) c = StatusColor::Ok;
                else if (interp) c = StatusColor::Warn;
                else if (lost || !measured) c = StatusColor::Bad;
                double x = measured ? o.pos[l] : std::nan("");
                if (!measured) {
                    // Draw where the shape says the edge should be.
                    if (o.kind != RefKind::Drawn && !o.shape.empty()) x = o.shape[0].x;
                    else {
                        for (size_t k = 1; k < o.shape.size(); ++k)
                            if (y >= o.shape[k - 1].y && y <= o.shape[k].y) {
                                const double t = (y - o.shape[k - 1].y) / std::max(1e-9, o.shape[k].y - o.shape[k - 1].y);
                                x = o.shape[k - 1].x + t * (o.shape[k].x - o.shape[k - 1].x);
                            }
                    }
                }
                if (!std::isfinite(x)) continue;
                // In field view a field line covers two display rows.
                const double dispRow = f.fieldView ? 2.0 * l : y;
                const QPointF c0 = toWidget(x, dispRow);
                const double len = std::max(1.5, th * rowStep * 0.9);
                p.setPen(QPen(c, std::max(1.0, th * 0.6)));
                p.drawLine(QPointF(c0.x() - 3, c0.y() + len / 2), QPointF(c0.x() + 3, c0.y() + len / 2));
            }
        }
    }
    if (m_drawing) {
        p.setPen(QPen(QColor(0x5B, 0x9B, 0xFF), 2));
        QPolygonF poly;
        for (const PointF& q : m_points) poly << toWidget(q.x, q.y);
        if (!m_points.empty()) poly << m_hover;
        p.drawPolyline(poly);
        for (const PointF& q : m_points) p.drawEllipse(toWidget(q.x, q.y), 3, 3);
        tag(p, QPointF(8, 8), Qt::AlignLeft,
            tr("Click along an edge that should be straight or smoothly curved. Enter or double click to finish, Esc to cancel."));
    }
    if (f.qtgmc) {
        tag(p, QPointF(width() - 8, height() - 8 - p.fontMetrics().height() - 6), Qt::AlignRight, tr("QTGMC preview"));
    }
}

void PreviewWidget::mousePressEvent(QMouseEvent* e) {
    m_lastMouse = e->position();
    if (e->button() == Qt::MiddleButton) {
        m_panning = true;
        return;
    }
    if (e->button() != Qt::LeftButton || !m_frame) return;
    if (m_drawing) {
        const QPointF q = toImage(e->position());
        m_points.push_back({q.x(), q.y()});
        update();
        return;
    }
    const QRectF r = imageRect();
    if (m_view == View::Split && std::fabs(e->position().x() - (r.left() + m_split * r.width())) < 8) {
        m_draggingSplit = true;
        return;
    }
    if (m_cropEdit) {
        // Grab the nearest crop edge within reach.
        const QPointF a = toWidget(m_crop[0], m_crop[2]), b = toWidget(m_frame->width - m_crop[1], m_frame->height - m_crop[3]);
        const double d[4] = {std::fabs(e->position().x() - a.x()), std::fabs(e->position().x() - b.x()), std::fabs(e->position().y() - a.y()),
                             std::fabs(e->position().y() - b.y())};
        int best = -1;
        for (int i = 0; i < 4; ++i)
            if (d[i] < 10 && (best < 0 || d[i] < d[best])) best = i;
        m_cropDrag = best;
    }
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF delta = e->position() - m_lastMouse;
    m_lastMouse = e->position();
    if (m_panning) {
        m_pan += delta;
        update();
        return;
    }
    if (m_drawing) {
        m_hover = e->position();
        update();
        return;
    }
    if (m_draggingSplit) {
        const QRectF r = imageRect();
        m_split = std::clamp((e->position().x() - r.left()) / std::max(1.0, r.width()), 0.0, 1.0);
        update();
        return;
    }
    if (m_cropDrag >= 0 && m_frame) {
        const QPointF q = toImage(e->position());
        const int w = m_frame->width, h = m_frame->height;
        switch (m_cropDrag) {
        case 0: m_crop[0] = std::clamp(int(std::lround(q.x())), 0, w - m_crop[1] - 16); break;
        case 1: m_crop[1] = std::clamp(int(std::lround(w - q.x())), 0, w - m_crop[0] - 16); break;
        case 2: m_crop[2] = std::clamp(int(std::lround(q.y())), 0, h - m_crop[3] - 16); break;
        case 3: m_crop[3] = std::clamp(int(std::lround(h - q.y())), 0, h - m_crop[2] - 16); break;
        }
        emit cropChanged(m_crop[0], m_crop[1], m_crop[2], m_crop[3]);
        update();
    }
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent*) {
    m_panning = false;
    m_draggingSplit = false;
    m_cropDrag = -1;
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (m_drawing) {
        if (m_points.size() >= 2) emit drawn(m_points);
        cancelDrawing();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(e);
}

void PreviewWidget::keyPressEvent(QKeyEvent* e) {
    if (m_drawing) {
        if (e->key() == Qt::Key_Escape) {
            cancelDrawing();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (m_points.size() >= 2) emit drawn(m_points);
            cancelDrawing();
            return;
        }
        if (e->key() == Qt::Key_Backspace && !m_points.empty()) {
            m_points.pop_back();
            update();
            return;
        }
    }
    QOpenGLWidget::keyPressEvent(e);
}

void PreviewWidget::wheelEvent(QWheelEvent* e) {
    if (!(e->modifiers() & Qt::ControlModifier)) {
        QOpenGLWidget::wheelEvent(e);
        return;
    }
    static const int steps[] = {0, 1, 2, 4};
    int i = 0;
    while (i < 3 && steps[i] != m_zoom) ++i;
    i = std::clamp(i + (e->angleDelta().y() > 0 ? 1 : -1), 0, 3);
    setZoom(steps[i]);
}

}  // namespace ud
