#ifndef CANSUB_QT_QTVERSIONHELPER_H
#define CANSUB_QT_QTVERSIONHELPER_H

#include <QtGlobal>

/* Attempt at centralization of some Qt 5/6 compatibility checks */

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#define CANBUS_CONFIGURATION_KEY QCanBusDevice::ConfigurationKey
#define GET_VARIANT_TYPE(v) (v).typeId()
#elif QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#define CANBUS_CONFIGURATION_KEY QCanBusDevice::ConfigurationKey
#define GET_VARIANT_TYPE(v) (v).typeId()
#else
#define CANBUS_CONFIGURATION_KEY int
#define GET_VARIANT_TYPE(v) (v).userType()
#endif

/* Re-label a QDateTime as UTC without converting its calendar digits. QDateTime::setTimeSpec is deprecated from
 * Qt 6.9, the lightweight QTimeZone::UTC representation it defers to exists from Qt 6.5. */
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QTimeZone>
#define CANSUB_SET_UTC(dateTime) (dateTime).setTimeZone(QTimeZone::UTC)
#else
#define CANSUB_SET_UTC(dateTime) (dateTime).setTimeSpec(Qt::UTC)
#endif

#endif //CANSUB_QT_QTVERSIONHELPER_H
