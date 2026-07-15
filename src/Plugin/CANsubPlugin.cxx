#include <QSslSocket>
#include <QTimer>

#include "CANsub.hxx"
#include "CANsubPlugin.hxx"
#include "Certificate.hxx"
#include "DeferredMdnsHostResolver.hxx"
#include "MdnsHostResolver.hxx"
#include "OAIHelpers.h"

#include "qmdnsengine/browser.h"
#include "qmdnsengine/dns.h"
#include "qmdnsengine/message.h"
#include "qmdnsengine/query.h"
#include "qmdnsengine/record.h"
#include "qmdnsengine/server.h"
#include "qmdnsengine/service.h"

static bool isMetaTypeRegistered(char const* const name)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    auto const mt = QMetaType::fromName(name);
    return mt.isValid();
#else
    auto const typeId = QMetaType::type(name);
    return !((typeId == QMetaType::UnknownType) || (!QMetaType::isRegistered(typeId)));
#endif
}

Plugin::Plugin() :
    mdnsServer(new QMdnsEngine::Server(this))
{
    if (!QSslSocket::supportsSsl())
    {
        qCritical() << "TLS/SSL support is missing on this platform, but required for correct CANsub operation";
    }

    /* A lot of the communication in this plugin is done through QCanBusFrame over signals and slots. To ensure we can
     * enqueue across thread borders, we need to register the type in the metaobject type system. */
    if (!isMetaTypeRegistered("QCanBusFrame"))
    {
        qRegisterMetaType<QCanBusFrame>("QCanBusFrame");
    }
    if (!isMetaTypeRegistered("QVector<QCanBusFrame>"))
    {
        qRegisterMetaType<QVector<QCanBusFrame>>("QVector<QCanBusFrame>");
    }

    /* QMdnsEngine also passes objects around. As some programs may use this across threads, these need to also be
     * registered in the Qt type system.*/
    if (!isMetaTypeRegistered("Message"))
    {
        qRegisterMetaType<QMdnsEngine::Message>("Message");
    }

    /* Also register the custom types used by the CANsub device */
    if (!isMetaTypeRegistered("CANsubBitTiming"))
    {
        qRegisterMetaType<CANsubBitTiming>("CANsubBitTiming");
    }

    /* Also register the custom types used by the host resolver */
    if (!isMetaTypeRegistered("QHostAddress"))
    {
        qRegisterMetaType<QHostAddress>("QHostAddress");
    }

    /* Set default OpenAPI parameters */
    /* NOTE: The default generated OpenAPI helpers cannot figure out the string termination */
    OpenAPI::setDateTimeFormat("\"yyyy-MM-ddTHH:mm:ss.zzzZ\"");

    /* Point the OpenAPI client at the shared, statically-stored SSL configuration that trusts only the embedded CANsub
     * root certificate. The configuration outlives the plugin, so the raw pointer kept by  the worker can never
     * dangle. */
    OpenAPI::OAIHttpRequestWorker::sslDefaultConfiguration = &coreSslConfiguration();
}

HostResolver* Plugin::createResolver(QByteArray const& name, QObject* parent) const
{
    if (QThread::currentThread() == mdnsServer->thread())
    {
        /* Ensure we have any new interface discovered here */
        mdnsServer->forceRefresh();

        return new MdnsHostResolver(mdnsServer, name, nullptr, parent);
    }
    return new DeferredMdnsHostResolver(mdnsServer, name, nullptr, parent);
}

QList<QCanBusDeviceInfo> Plugin::availableDevices(QString* errorMessage) const
{
    if (QThread::currentThread() != mdnsServer->thread())
    {
        qWarning() << "availableDevices called from a different thread than the CANsub plugin, mDNS discovery is only supported from the plugin's thread";
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("CANsub device discovery must run in the plugin's thread");
        }
        return {};
    }

    /* Bind sockets and join the multicast groups on all currently available interfaces (e.g. a USB CDC-NCM interface
     * that appeared after the plugin was loaded) *before* the browser sends its initial query. Group membership must
     * be in place on an interface before responses arriving on it can be received. */
    mdnsServer->forceRefresh();

    auto* mdnsBrowser = new QMdnsEngine::Browser(mdnsServer, "_cansub._tcp.local.", nullptr);
    QList<QCanBusDeviceInfo> discoveredDevices;

    /* NOTE: The following connected lambdas have a warning about the capture of a local variable might be out of
     *       scope by the time the lambda is called. As the lambdas are disconnected before the variable runs out of
     *       scope, this should not be an issue. */

    auto const onServiceDiscovered = [&discoveredDevices](const QMdnsEngine::Service& service)
                          {
                              /* Look for the required keys */
                              if (service.attributes().count("api") != 1)
                              {
                                  return;
                              }
                              if (service.attributes().count("channels") != 1)
                              {
                                  return;
                              }

                              QString const remoteApiVersion = QString::fromUtf8(
                                  service.attributes().constFind("api").value());
                              if (remoteApiVersion != CANSUB_EXPECTED_VERSION)
                              {
                                  qInfo() << "Unsupported API version" << remoteApiVersion;
                                  return;
                              }

                              QString const channels = QString::fromUtf8(service.attributes().constFind("channels").value());
                              const uint numberOfChannels = channels.toUInt();

                              /* Sanity check */
                              if ((numberOfChannels != 1) && (numberOfChannels != 2) && (numberOfChannels != 4))
                              {
                                  return;
                              }

                              /* Ensure we have not already registered this device */
                              QString serial = service.hostname().left(8);

                              QListIterator it(discoveredDevices);
                              while (it.hasNext())
                              {
                                  if (it.next().serialNumber() == serial)
                                  {
                                      return;
                                  }
                              }

                              for (uint i = 1; i < numberOfChannels + 1; ++i)
                              {
                                  discoveredDevices.append(CANsub::createInfo(
                                      service.hostname(),
                                      static_cast<int>(i)
                                      ));
                              }
                          };

    /* The browser emits serviceAdded as soon as the PTR and SRV records are known. If the TXT record arrives in a
     * later message, the attributes are missing at that point and the service is rejected above. The attributes are
     * then only delivered through the subsequent serviceUpdated, so handle it with the same logic. */
    auto conn_a = connect(mdnsBrowser, &QMdnsEngine::Browser::serviceAdded, this, onServiceDiscovered);
    auto conn_b = connect(mdnsBrowser, &QMdnsEngine::Browser::serviceUpdated, this, onServiceDiscovered);
    auto conn_c = connect(mdnsBrowser, &QMdnsEngine::Browser::serviceRemoved, this,
                          [&discoveredDevices](const QMdnsEngine::Service& service)
                          {
                              QString serial = service.hostname().left(8);
                              QMutableListIterator it(discoveredDevices);

                              while (it.hasNext())
                              {
                                  if (it.next().serialNumber() == serial)
                                  {
                                      it.remove();
                                  }
                              }
                          });

    /* Wait for services to become available */
    {
        /* The browser only re-sends its query after 60 s, far outside this window, and its initial query may be lost
         * before the multicast filter of a freshly appeared interface is fully programmed (on USB CDC-NCM this is an
         * asynchronous control transfer to the adapter). Re-query manually a few times to cover this and plain UDP
         * loss. These queries carry no known-answer section, so devices whose records are already cached must still
         * respond. */
        QMdnsEngine::Query ptrQuery;
        ptrQuery.setName("_cansub._tcp.local.");
        ptrQuery.setType(QMdnsEngine::PTR);

        QElapsedTimer timer;
        timer.start();

        qint64 nextQueryMs = 500;

        while (timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

            /* If we are closing down, do not block here any longer */
            if (QCoreApplication::closingDown())
            {
                break;
            }

            if (timer.elapsed() >= nextQueryMs)
            {
                QMdnsEngine::Message queryMessage;
                queryMessage.addQuery(ptrQuery);
                mdnsServer->sendMessageToAll(queryMessage);

                nextQueryMs += 500;
            }

            QThread::msleep(10);
        }
    }

    disconnect(conn_a);
    disconnect(conn_b);
    disconnect(conn_c);

    mdnsBrowser->deleteLater();

    return discoveredDevices;
}

QCanBusDevice* Plugin::createDevice(QString const& interfaceName, QString* errorMessage) const
{
    /* We expect the address format to be hostname@channel or ip@channel */
    static auto reList = QVector<QPair<QRegularExpression, std::function<bool(QString const &address)>>>({
        QPair(
            QRegularExpression(R"(^([\d]{1,3}\.[\d]{1,3}\.[\d]{1,3}\.[\d]{1,3})@([1-4]{1})$)"),
            [](QString const &address)
            {
                /* Ensure this is a valid IPv4 range */
                return QHostAddress(address).protocol() == QAbstractSocket::IPv4Protocol;
            }
            ),
        QPair(
            QRegularExpression(R"(^([a-fA-F\d]{8}-(?:eth|usb)\.local)@([1-4]{1})$)"),
            [](QString const &address)
            {
                /* No need for additional address verification, the regex already matches it */
                return true;
            }
            ),
    });

    if (errorMessage != nullptr)
    {
        *errorMessage = QString();
    }

    CANsub* result = nullptr;

    for (auto const& re : reList)
    {
        QRegularExpressionMatch m = re.first.match(interfaceName);
        if (!m.hasMatch())
        {
            continue;
        }

        auto const address = m.captured(1);
        if (!re.second(address))
        {
            continue;
        }

        /* NOTE: The only valid channels at the time of release are the values in (1, 2, 3, 4). However, we leave it
         *       up to the state machine to catch this during connection attempts, as there is no good way to here limit
         *       a CANsub.2 device to not take channel 4 as an argument. */
        result = new CANsub(address, m.captured(2).toUInt(), this);

        break;
    }

    if ((result == nullptr) && (errorMessage != nullptr) && errorMessage->isEmpty())
    {
        *errorMessage = "Invalid address " + interfaceName;
    }

    return result;
}
