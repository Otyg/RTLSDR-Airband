#ifndef THEME_H
#define THEME_H

#include <QtGlobal>
#include <QString>

namespace Theme {

inline constexpr qint64 kDefaultSessionTrafficHighlightThresholdMs = 2000;

inline QString const kDefaultChannelColor = QStringLiteral("#1e3227");
inline QString const kSessionTrafficChannelColor = QStringLiteral("#5a6b2b");
inline QString const kScannedChannelColor = QStringLiteral("#2aa54a");
inline QString const kActiveChannelColor = QStringLiteral("#39ff14");

inline QString const kChannelBoxBackgroundColor = QStringLiteral("#111827");
inline QString const kChannelBoxBorderColor = QStringLiteral("#2a3345");

inline QString const kChannelBoxStyle =
    QStringLiteral("QFrame { background:%1; border:1px solid %2; border-radius:8px; }");
inline QString const kChannelLabelStyle = QStringLiteral("font-weight:600; color:%1;");
inline QString const kChannelFrequencyStyle = QStringLiteral("color:%1;");

}  // namespace Theme

#endif
