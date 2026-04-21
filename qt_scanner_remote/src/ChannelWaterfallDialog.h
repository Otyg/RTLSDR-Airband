#ifndef CHANNEL_WATERFALL_DIALOG_H
#define CHANNEL_WATERFALL_DIALOG_H

#include <QDialog>
#include <QList>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVector>

#include "WaterfallWidget.h"

class ChannelWaterfallDialog : public QDialog {
    Q_OBJECT

   public:
    explicit ChannelWaterfallDialog(QWidget* parent = nullptr);

    void setChannelInfo(qint64 freqHz, QString const& label);
    void setFrames(QList<QVector<float>> const& frames);

   protected:
    void resizeEvent(QResizeEvent* event) override;

   private:
    QString buildWindowTitle() const;
    void syncWaterfallWidth();

    WaterfallWidget* waterfall_;
    QScrollArea* scrollArea_;
    qint64 freqHz_;
    QString label_;
};

#endif
