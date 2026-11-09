#include "CANsubPlugin.hxx"

#include "CANsub.hxx"
#include "DeviceDiscovery.hxx"
#include "MetaTypes.hxx"

namespace CANsubQt
{

Plugin::Plugin()
{
    registerMetaTypes();
}

QList<QCanBusDeviceInfo> Plugin::availableDevices(QString *errorMessage) const
{
    return discoverDevices(errorMessage);
}

QCanBusDevice *Plugin::createDevice(QString const &interfaceName, QString *errorMessage) const
{
    if (errorMessage != nullptr)
    {
        *errorMessage = QString();
    }

    auto const target = CANsub::parseInterfaceName(interfaceName, errorMessage);
    if (!target)
    {
        return nullptr;
    }

    return CANsub::create(*target, errorMessage);
}

} // namespace CANsubQt
