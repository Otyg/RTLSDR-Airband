#include "WaveformWidget.h"

#include <QPainter>
#include <QPaintEvent>

WaveformWidget::WaveformWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(120);
}

void WaveformWidget::setSamples(QVector<float> const& samples) {
    samples_ = samples;
    update();
}

void WaveformWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), QColor(18, 24, 33));

    int const w = width();
    int const h = height();
    if (w <= 1 || h <= 1) {
        return;
    }

    int const midY = h / 2;
    p.setPen(QPen(QColor(70, 90, 110), 1));
    p.drawLine(0, midY, w, midY);

    if (samples_.isEmpty()) {
        return;
    }

    QPolygon poly;
    poly.reserve(w);

    int const n = samples_.size();
    for (int x = 0; x < w; ++x) {
        int idx = (x * n) / w;
        if (idx >= n) {
            idx = n - 1;
        }
        float v = samples_[idx];
        if (v < -1.0f) {
            v = -1.0f;
        } else if (v > 1.0f) {
            v = 1.0f;
        }
        int y = midY - static_cast<int>(v * (h * 0.45f));
        poly << QPoint(x, y);
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(80, 220, 170), 1.5));
    p.drawPolyline(poly);
}
