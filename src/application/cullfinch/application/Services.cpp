// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/Services.h>

namespace cullfinch::application {

QString sessionLifecycleToken(SessionLifecycle lifecycle) {
    using enum SessionLifecycle;
    switch (lifecycle) {
    case Active:
        return QStringLiteral("active");
    case Paused:
        return QStringLiteral("paused");
    case Finished:
        return QStringLiteral("finished");
    case Discarded:
        break;
    }
    return QStringLiteral("discarded");
}

SessionLifecycle sessionLifecycleFromToken(const QString& token) {
    using enum SessionLifecycle;
    if (token == QLatin1String("active")) {
        return Active;
    }
    if (token == QLatin1String("paused")) {
        return Paused;
    }
    if (token == QLatin1String("finished")) {
        return Finished;
    }
    return Discarded;
}

} // namespace cullfinch::application
