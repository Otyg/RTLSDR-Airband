#ifndef WAVEFORM_WIDGET_H
#define WAVEFORM_WIDGET_H

#include <QVector>
#include <QWidget>

class WaveformWidget : public QWidget {
    Q_OBJECT

   public:
    explicit WaveformWidget(QWidget* parent = nullptr);

   public slots:
    void setSamples(QVector<float> const& samples);

   protected:
    void paintEvent(QPaintEvent* event) override;

   private:
    QVector<float> samples_;
};

#endif
