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

#include "qmdnsengine/cache.h"
#include "qmdnsengine/server.h"

#include "ResolverFactory.hxx"

class HostResolver;

class Plugin : public QObject, public CAN_BUS_FACTORY_BASE, public ResolverFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QCanBusFactory" FILE "plugin.json" )
    Q_INTERFACES (CAN_BUS_FACTORY_BASE)

public:
    Plugin();
    ~Plugin() override = default;

    QList<QCanBusDeviceInfo> availableDevices(QString* errorMessage) const override;
    QCanBusDevice* createDevice(QString const& interfaceName, QString* errorMessage) const override;

    HostResolver* createResolver(QByteArray const& name, QObject* parent) const override;

private:
    QMdnsEngine::Server* mdnsServer;
};

#endif // CANSUB_QT_CANSUB_PLUGIN_HXX
