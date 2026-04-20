#include "WaterfallWidget.h"

#include <QPainter>
#include <QPaintEvent>

WaterfallWidget::WaterfallWidget(QWidget* parent) : QWidget(parent), bins_(192) {
    setMinimumHeight(140);
    image_ = QImage(bins_, 160, QImage::Format_RGB32);
    image_.fill(QColor(10, 14, 22));
}

void WaterfallWidget::appendFrame(QVector<float> const& bins) {
    if (bins.isEmpty()) {
        return;
    }

    if (image_.width() != bins_) {
        image_ = QImage(bins_, image_.height(), QImage::Format_RGB32);
        image_.fill(QColor(10, 14, 22));
    }

    if (image_.height() > 1) {
        for (int y = image_.height() - 1; y > 0; --y) {
            memcpy(image_.scanLine(y), image_.scanLine(y - 1), static_cast<size_t>(image_.bytesPerLine()));
        }
    }

    QRgb* row = reinterpret_cast<QRgb*>(image_.scanLine(0));
    int const n = bins.size();
    for (int x = 0; x < bins_; ++x) {
        int idx = (x * n) / bins_;
        if (idx >= n) {
            idx = n - 1;
        }
        float v = bins[idx];
        if (v < 0.0f) {
            v = 0.0f;
        } else if (v > 1.0f) {
            v = 1.0f;
        }
        row[x] = colorForLevel(v);
    }

    update();
}

void WaterfallWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), QColor(10, 14, 22));
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(rect(), image_);
}

QRgb WaterfallWidget::colorForLevel(float level) const {
    if (level < 0.2f) {
        int c = static_cast<int>(level / 0.2f * 70.0f);
        return qRgb(0, c, 90 + c / 2);
    }
    if (level < 0.5f) {
        float t = (level - 0.2f) / 0.3f;
        return qRgb(static_cast<int>(t * 40.0f), 80 + static_cast<int>(t * 120.0f), 140 + static_cast<int>(t * 60.0f));
    }
    if (level < 0.8f) {
        float t = (level - 0.5f) / 0.3f;
        return qRgb(40 + static_cast<int>(t * 180.0f), 200 + static_cast<int>(t * 40.0f), 200 - static_cast<int>(t * 120.0f));
    }

    float t = (level - 0.8f) / 0.2f;
    return qRgb(220 + static_cast<int>(t * 35.0f), 220 - static_cast<int>(t * 180.0f), 80 - static_cast<int>(t * 80.0f));
}
