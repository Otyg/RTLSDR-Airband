#include "SpectrumWidget.h"

#include <QPainter>
#include <QPaintEvent>

SpectrumWidget::SpectrumWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(72);
}

void SpectrumWidget::setBins(QVector<float> const& bins) {
    bins_ = bins;
    update();
}

void SpectrumWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), QColor(10, 14, 22));

    int const w = width();
    int const h = height();
    if (w <= 1 || h <= 1) {
        return;
    }

    p.setPen(QPen(QColor(36, 58, 83), 1));
    for (int i = 1; i < 4; ++i) {
        int const y = (h * i) / 4;
        p.drawLine(0, y, w, y);
    }

    if (bins_.isEmpty()) {
        return;
    }

    QPolygon spectrum;
    spectrum.reserve(w + 2);
    spectrum << QPoint(0, h);

    int const n = bins_.size();
    for (int x = 0; x < w; ++x) {
        int idx = (x * n) / w;
        if (idx >= n) {
            idx = n - 1;
        }

        float v = bins_[idx];
        if (v < 0.0f) {
            v = 0.0f;
        } else if (v > 1.0f) {
            v = 1.0f;
        }

        int const y = h - 1 - static_cast<int>(v * (h - 8));
        spectrum << QPoint(x, y);
    }

    spectrum << QPoint(w - 1, h);

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(88, 220, 255), 1.5));
    p.setBrush(QColor(30, 156, 186, 90));
    p.drawPolygon(spectrum);
}
