#include "DeviceDiscovery.hxx"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMap>
#include <QSet>
#include <QTimer>

#include <qmdnsengine/browser.h>
#include <qmdnsengine/dns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/server.h>
#include <qmdnsengine/service.h>

#include "cansub/cansub_ffi.h"

#include "CANsub.hxx"
#include "Logging.hxx"

namespace CANsubQt
{

/* mDNS service type of the CANsub */
static char const *const serviceType = "_cansub._tcp.local.";

/* Duration of a discovery, and interval of its repeated queries */
static constexpr int discoveryDurationMs = 2000;
static constexpr int discoveryQueryIntervalMs = 500;

DiscoveredService evaluateService(QMdnsEngine::Service const &service)
{
    DiscoveredService result;
    result.hostname = service.hostname();

    auto const attributes = service.attributes();

    auto const api = attributes.constFind("api");
    if (api == attributes.constEnd())
    {
        result.rejectReason = QStringLiteral("No \"api\" TXT record");
        return result;
    }

    /* The TXT record carries the unquoted 5-byte form, e.g. 05.00 */
    QByteArray const &apiVersion = api.value();
    if (cansub_api_version_check(apiVersion.constData(), static_cast<size_t>(apiVersion.size()), nullptr) !=
        CANSUB_RESULT_OK)
    {
        result.rejectReason = QStringLiteral("Unsupported API version \"%1\"").arg(QString::fromUtf8(apiVersion));
        return result;
    }

    auto const channels = attributes.constFind("channels");
    if (channels == attributes.constEnd())
    {
        result.rejectReason = QStringLiteral("No \"channels\" TXT record");
        return result;
    }

    bool ok = false;
    int const numberOfChannels = channels.value().toInt(&ok);
    if (!ok || ((numberOfChannels != 1) && (numberOfChannels != 2) && (numberOfChannels != 4)))
    {
        result.rejectReason =
            QStringLiteral("Unsupported number of channels \"%1\"").arg(QString::fromUtf8(channels.value()));
        return result;
    }

    result.channels = numberOfChannels;
    return result;
}

QList<QCanBusDeviceInfo> discoverDevices(QString *errorMessage)
{
    /* The sockets and timers below need the event dispatcher of the calling thread */
    if (QAbstractEventDispatcher::instance() == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = CANsub::tr("CANsub device discovery needs a thread with an event dispatcher, e.g. one with "
                                       "a running event loop.");
        }
        return {};
    }

    /* Services by their name. An update may replace an accepted service with a rejected one and vice versa. */
    QMap<QByteArray, DiscoveredService> services;

    auto const onService = [&services](QMdnsEngine::Service const &service)
    {
        services.insert(service.name(), evaluateService(service));
    };

    {
        /* Local to this call, so it works from any thread. The server binds its sockets and joins the mDNS group on
         * all interfaces present now, e.g. the USB CDC-NCM link of a freshly plugged CANsub, and sends on each of
         * them. The browser must be destroyed before the server. */
        QMdnsEngine::Server server;
        QMdnsEngine::Browser browser(&server, serviceType);

        /* The browser emits serviceAdded as soon as the PTR and SRV records are known. If the TXT record arrives in
         * a later message, the attributes are missing at that point and only delivered with serviceUpdated. */
        QObject::connect(&browser, &QMdnsEngine::Browser::serviceAdded, onService);
        QObject::connect(&browser, &QMdnsEngine::Browser::serviceUpdated, onService);
        QObject::connect(&browser, &QMdnsEngine::Browser::serviceRemoved,
                         [&services](QMdnsEngine::Service const &service)
                         {
                             services.remove(service.name());
                         });

        QEventLoop loop;

        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

        /* The browser only re-sends its query after 60 s, far outside this window, and its initial query may be lost
         * before the multicast filter of a freshly appeared interface is programmed (on USB CDC-NCM this is an
         * asynchronous control transfer to the adapter). Re-query to cover this and plain UDP loss. These queries
         * carry no known-answer section, so devices whose records are already cached still respond. */
        QMdnsEngine::Query ptrQuery;
        ptrQuery.setName(serviceType);
        ptrQuery.setType(QMdnsEngine::PTR);
        QMdnsEngine::Message queryMessage;
        queryMessage.addQuery(ptrQuery);

        QTimer requery;
        requery.setInterval(discoveryQueryIntervalMs);
        QObject::connect(&requery, &QTimer::timeout, &loop,
                         [&server, &queryMessage, &loop]()
                         {
                             /* Do not block an application that is closing down any longer */
                             if (QCoreApplication::closingDown())
                             {
                                 loop.quit();
                                 return;
                             }
                             server.sendMessageToAll(queryMessage);
                         });

        deadline.start(discoveryDurationMs);
        requery.start();

        if (!QCoreApplication::closingDown())
        {
            loop.exec();
        }
    }

    /* List each device once, by its device ID, e.g. when it is reachable over both USB and Ethernet */
    QList<QCanBusDeviceInfo> devices;
    QSet<QByteArray> deviceIds;
    for (auto it = services.constBegin(); it != services.constEnd(); ++it)
    {
        DiscoveredService const &service = it.value();
        if (!service.rejectReason.isEmpty())
        {
            qCInfo(lcCansub) << "Ignoring CANsub service" << it.key() << "on" << service.hostname << "-"
                             << service.rejectReason;
            continue;
        }

        QByteArray const deviceId = service.hostname.left(8);
        if (deviceIds.contains(deviceId))
        {
            continue;
        }
        deviceIds.insert(deviceId);

        for (int channel = 1; channel <= service.channels; ++channel)
        {
            devices.append(CANsub::createInfo(QString::fromUtf8(service.hostname), channel));
        }
    }

    if (errorMessage != nullptr)
    {
        *errorMessage = QString();
    }

    return devices;
}

} // namespace CANsubQt
