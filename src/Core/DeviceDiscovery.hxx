#ifndef CANSUB_QT_DEVICE_DISCOVERY_HXX
#define CANSUB_QT_DEVICE_DISCOVERY_HXX

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtSerialBus/QCanBusDeviceInfo>

namespace QMdnsEngine
{
class Service;
}

namespace CANsubQt
{

/** A CANsub service found with mDNS. */
struct DiscoveredService
{
    /** mDNS hostname of the device, e.g. "aabbccdd-usb.local." */
    QByteArray hostname;
    /** Number of CAN channels. Only set if the service is accepted. */
    int channels = 0;
    /** Why the service is not listed, empty if it is accepted. Only logged. */
    QString rejectReason;
};

/**
 * Check a CANsub service: Its "api" TXT record must name an API version this library supports, and its "channels"
 * TXT record must be 1, 2 or 4.
 */
DiscoveredService evaluateService(QMdnsEngine::Service const &service);

/**
 * Discover CANsub devices with mDNS, for QCanBusFactory::availableDevices().
 *
 * Browses for about 2 s, processing the events of the calling thread, so it works from any thread with an event
 * dispatcher. Each device accepted by evaluateService() is listed once (by device ID), with one entry per channel from
 * CANsub::createInfo().
 *
 * @param[out] errorMessage If not NULL, set to a translated reason on failure, and to an empty string otherwise.
 * @return The devices found, empty on failure.
 */
QList<QCanBusDeviceInfo> discoverDevices(QString *errorMessage);

} // namespace CANsubQt

#endif // CANSUB_QT_DEVICE_DISCOVERY_HXX
