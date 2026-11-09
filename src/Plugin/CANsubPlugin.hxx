#ifndef CANSUB_QT_CANSUB_PLUGIN_HXX
#define CANSUB_QT_CANSUB_PLUGIN_HXX

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QCanBusFactory>
#define CAN_BUS_FACTORY_BASE QCanBusFactory
#else
#include <QCanBusFactoryV2>
#define CAN_BUS_FACTORY_BASE QCanBusFactoryV2
#endif

#include <QList>
#include <QObject>
#include <QString>
#include <QtPlugin>

namespace CANsubQt
{

/**
 * Qt CAN bus plugin "CANsub". Only the factory: Everything else is in Core.
 *
 * The constructor registers the metatypes used across threads, see registerMetaTypes().
 */
class Plugin : public QObject, public CAN_BUS_FACTORY_BASE
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QCanBusFactory" FILE "plugin.json")
    Q_INTERFACES(CAN_BUS_FACTORY_BASE)

public:
    Plugin();
    ~Plugin() override = default;

    /** Discover devices with mDNS, see discoverDevices(). */
    QList<QCanBusDeviceInfo> availableDevices(QString *errorMessage) const override;

    /** Create a device from an interface name, see CANsub::Target for the format. */
    QCanBusDevice *createDevice(QString const &interfaceName, QString *errorMessage) const override;
};

} // namespace CANsubQt

#endif // CANSUB_QT_CANSUB_PLUGIN_HXX
