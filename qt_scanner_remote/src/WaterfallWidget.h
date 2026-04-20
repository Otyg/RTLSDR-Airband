#ifndef WATERFALL_WIDGET_H
#define WATERFALL_WIDGET_H

#include <QImage>
#include <QVector>
#include <QWidget>

class WaterfallWidget : public QWidget {
    Q_OBJECT

   public:
    explicit WaterfallWidget(QWidget* parent = nullptr);

   public slots:
    void appendFrame(QVector<float> const& bins);

   protected:
    void paintEvent(QPaintEvent* event) override;

   private:
    QRgb colorForLevel(float level) const;

    QImage image_;
    int bins_;
};

#endif
