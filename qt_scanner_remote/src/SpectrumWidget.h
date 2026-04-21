#ifndef SPECTRUM_WIDGET_H
#define SPECTRUM_WIDGET_H

#include <QVector>
#include <QWidget>

class SpectrumWidget : public QWidget {
    Q_OBJECT

   public:
    explicit SpectrumWidget(QWidget* parent = nullptr);

   public slots:
    void setBins(QVector<float> const& bins);

   protected:
    void paintEvent(QPaintEvent* event) override;

   private:
    QVector<float> bins_;
};

#endif
