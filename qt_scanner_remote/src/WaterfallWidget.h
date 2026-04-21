#ifndef WATERFALL_WIDGET_H
#define WATERFALL_WIDGET_H

#include <QImage>
#include <QList>
#include <QVector>
#include <QWidget>

class WaterfallWidget : public QWidget {
    Q_OBJECT

   public:
    explicit WaterfallWidget(QWidget* parent = nullptr);
    QSize sizeHint() const override;

   public slots:
    void appendFrame(QVector<float> const& bins);
    void setFrames(QList<QVector<float>> const& frames);

   protected:
    void paintEvent(QPaintEvent* event) override;

   private:
    QRgb colorForLevel(float level) const;
    void paintBinsToRow(QRgb* row, QVector<float> const& bins) const;

    QImage image_;
    int bins_;
};

#endif
