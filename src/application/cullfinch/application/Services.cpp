// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/Services.h>

namespace cullfinch::application {

QString sessionLifecycleToken(SessionLifecycle lifecycle) {
    switch (lifecycle) {
    case SessionLifecycle::Active:
        return QStringLiteral("active");
    case SessionLifecycle::Paused:
        return QStringLiteral("paused");
    case SessionLifecycle::Finished:
        return QStringLiteral("finished");
    case SessionLifecycle::Discarded:
        break;
    }
    return QStringLiteral("discarded");
}

SessionLifecycle sessionLifecycleFromToken(const QString& token) {
    if (token == QLatin1String("active")) {
        return SessionLifecycle::Active;
    }
    if (token == QLatin1String("paused")) {
        return SessionLifecycle::Paused;
    }
    if (token == QLatin1String("finished")) {
        return SessionLifecycle::Finished;
    }
    return SessionLifecycle::Discarded;
}

} // namespace cullfinch::application
