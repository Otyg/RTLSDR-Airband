#include "ChannelWaterfallDialog.h"

#include <QLabel>
#include <QSizePolicy>
#include <QScrollBar>
#include <QVBoxLayout>

ChannelWaterfallDialog::ChannelWaterfallDialog(QWidget* parent)
    : QDialog(parent), waterfall_(new WaterfallWidget(this)), scrollArea_(new QScrollArea(this)), freqHz_(0) {
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(520, 640);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* hint = new QLabel("Nyast overst, aldst langst ner. Scrolla ned for aldre historik.", this);
    waterfall_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setWidget(waterfall_);

    layout->addWidget(hint);
    layout->addWidget(scrollArea_, 1);
}

void ChannelWaterfallDialog::setChannelInfo(qint64 freqHz, QString const& label) {
    freqHz_ = freqHz;
    label_ = label;
    setWindowTitle(buildWindowTitle());
}

void ChannelWaterfallDialog::setFrames(QList<QVector<float>> const& frames) {
    waterfall_->setFrames(frames);
    syncWaterfallWidth();
    scrollArea_->verticalScrollBar()->setValue(0);
}

QString ChannelWaterfallDialog::buildWindowTitle() const {
    QString const freqText = freqHz_ > 0 ? QString::number(static_cast<double>(freqHz_) / 1000000.0, 'f', 3) + " MHz" : "-";
    QString const shownLabel = label_.isEmpty() ? "-" : label_;
    return QString("Kanalvattenfall - %1 - %2").arg(shownLabel, freqText);
}

void ChannelWaterfallDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    syncWaterfallWidth();
}

void ChannelWaterfallDialog::syncWaterfallWidth() {
    int const viewportWidth = scrollArea_->viewport()->width();
    if (viewportWidth > 0) {
        waterfall_->setMinimumWidth(viewportWidth);
    }
}
