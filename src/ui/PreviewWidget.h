// SPDX-License-Identifier: GPL-3.0-or-later
// The preview: YUV planes go to the GPU and become RGB in a shader (as in
// PingDub), shown processed, as the source, or split with a divider you
// drag. Zoom to 200% and 400% is nearest-neighbor, so you see real pixels.
// The surround is neutral gray in every theme.
//
// On top, drawn with QPainter: the Edge Lock overlay (green measured, amber
// interpolated, red lost), a drawn reference being made, and the crop with
// its safe crop guide.
#pragma once

#include "PreviewEngine.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPointF>

#include <vector>

namespace ud {

class PreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class View { Processed, Source, Split };
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;

    void showFrame(PreviewFramePtr f);
    void clear(const QString& message);
    PreviewFramePtr frame() const { return m_frame; }

    void setView(View v);
    View view() const { return m_view; }
    // 0: fit, else 1, 2 or 4 times.
    void setZoom(int z);
    int zoom() const { return m_zoom; }

    // Drawing a reference edge: clicks add points, Enter or a double click finishes.
    void startDrawing();
    void cancelDrawing();
    bool drawing() const { return m_drawing; }

    void setCrop(int l, int r, int t, int b);
    void setSafeCrop(int l, int r, int bottom);
    void setCropEditing(bool on);
    void setOverlayVisible(bool on);
    void setRefColors(const std::vector<QColor>& c) { m_refColors = c; update(); }

signals:
    void drawn(std::vector<ud::PointF> points);
    void cropChanged(int l, int r, int t, int b);
    void zoomChanged(int z);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    QRectF imageRect() const;  // where the picture sits, widget px
    QPointF toImage(const QPointF& w) const;
    QPointF toWidget(double x, double y) const;
    void upload(int set, const PreviewFrame& f);
    void paintOverlay(QPainter& p);

    QOpenGLShaderProgram m_prog;
    unsigned m_tex[6] = {0, 0, 0, 0, 0, 0};
    int m_tw[6] = {}, m_th[6] = {};
    bool m_gl = false;
    PreviewFramePtr m_frame;
    bool m_uploaded = false;
    QString m_message;
    View m_view = View::Processed;
    double m_split = 0.5;
    int m_zoom = 0;
    QPointF m_pan;
    bool m_panning = false, m_draggingSplit = false;
    QPointF m_lastMouse;
    bool m_drawing = false;
    std::vector<PointF> m_points;
    QPointF m_hover;
    bool m_overlay = true;
    bool m_cropEdit = false;
    int m_crop[4] = {0, 0, 0, 0};  // l r t b
    int m_safe[3] = {0, 0, 0};     // l r bottom
    int m_cropDrag = -1;           // edge being dragged
    std::vector<QColor> m_refColors;
};

}  // namespace ud
