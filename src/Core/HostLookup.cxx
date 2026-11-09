#include "HostLookup.hxx"

#include <utility>

#include <QAbstractSocket>
#include <QHostInfo>
#include <QMetaObject>

#include "Logging.hxx"

#include "qmdnsengine/dns.h"
#include "qmdnsengine/message.h"
#include "qmdnsengine/query.h"
#include "qmdnsengine/resolver.h"
#include "qmdnsengine/server.h"

namespace CANsubQt
{

/* Interval of the repeated mDNS queries. See startMdns() for why they are needed. */
static constexpr int mdnsQueryIntervalMs = 500;

HostLookup::HostLookup(QString host, QObject *parent) :
    QObject(parent),
    host(std::move(host))
{
    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, this, &HostLookup::onTimeout);

    mdnsQueryTimer.setInterval(mdnsQueryIntervalMs);
    connect(&mdnsQueryTimer, &QTimer::timeout, this, &HostLookup::sendMdnsQuery);
}

HostLookup::~HostLookup()
{
    state = State::Done;
    stopLookups();
}

void HostLookup::start(int timeoutMs)
{
    Q_ASSERT(state == State::Idle);
    state = State::Running;
    this->timeoutMs = timeoutMs;

    /* IPv4 literal: Nothing to resolve. Other address literals are rejected, the CANsub only speaks IPv4. */
    QHostAddress literal;
    if (literal.setAddress(host))
    {
        if (literal.protocol() == QAbstractSocket::IPv4Protocol)
        {
            succeed(literal);
        }
        else
        {
            fail(tr("The address %1 is not an IPv4 address. The CANsub only supports IPv4.").arg(host));
        }
        return;
    }

    /* A trailing "." marks a fully qualified name. Accept it for the ".local" check. */
    QString name = host;
    if (name.endsWith(QLatin1Char('.')))
    {
        name.chop(1);
    }

    timeoutTimer.start(timeoutMs);

    if (name.endsWith(QStringLiteral(".local"), Qt::CaseInsensitive))
    {
        /* qmdnsengine compares the names of received records byte by byte with the queried name, and they always end
         * in ".". mDNS names are case-insensitive and the CANsub announces its hostname in lower case, so query it in
         * lower case to match the records. */
        startMdns(name.toLower().toUtf8() + '.');
    }

    /* The string-based overload connects the result to a slot. The functor overload allocates a slot object, which Qt 5
     * only frees when it delivers the result. An aborted lookup (see stopLookups()) never delivers, so it would leak
     * the slot object every time mDNS or the timeout wins. The connection is dropped with this object. */
    hostInfoId = QHostInfo::lookupHost(host, this, SLOT(onHostInfo(QHostInfo)));
}

void HostLookup::abort()
{
    state = State::Done;
    stopLookups();
}

void HostLookup::startMdns(QByteArray const &name)
{
    Q_ASSERT(mdnsServer == nullptr);
    Q_ASSERT(mdnsResolver == nullptr);

    mdnsName = name;

    /* The server binds its sockets and joins the mDNS group on all interfaces present now, and sends on each of them.
     * Creating it per lookup picks up interfaces that appeared since the last lookup, e.g. the USB CDC-NCM link of a
     * freshly plugged CANsub. It reports failures to bind through error() from within its constructor already, and
     * retries them only after a minute. A lookup without working mDNS ends with the timeout. */
    mdnsServer = new QMdnsEngine::Server(this);
    connect(mdnsServer, &QMdnsEngine::Server::error, this,
            [this](QString const &message)
            {
                qCWarning(lcCansub) << "mDNS server error while resolving" << host << "-" << message;
            });

    /* The resolver expects the name with a trailing ".". It sends a single query for A and AAAA records when it is
     * created and never repeats it. That query can be lost before the multicast filter of a freshly appeared interface
     * is programmed (on USB CDC-NCM this is an asynchronous control transfer to the adapter) or to plain UDP loss.
     * sendMdnsQuery() re-queries until resolved. The resolver handles all responses, also those to the repeated
     * queries. */
    mdnsResolver = new QMdnsEngine::Resolver(mdnsServer, mdnsName, nullptr, this);
    connect(mdnsResolver, &QMdnsEngine::Resolver::resolved, this, &HostLookup::onMdnsResolved);

    mdnsQueryTimer.start();
}

void HostLookup::sendMdnsQuery()
{
    Q_ASSERT(mdnsServer != nullptr);

    /* Only the A record is of interest. Without a known-answer section, the device always responds. */
    QMdnsEngine::Query query;
    query.setName(mdnsName);
    query.setType(QMdnsEngine::A);

    QMdnsEngine::Message message;
    message.addQuery(query);
    mdnsServer->sendMessageToAll(message);
}

void HostLookup::onHostInfo(QHostInfo const &info)
{
    hostInfoId = -1;

    if (state != State::Running)
    {
        return;
    }

    auto const addresses = info.addresses();
    for (QHostAddress const &address : addresses)
    {
        if (address.protocol() == QAbstractSocket::IPv4Protocol)
        {
            qCDebug(lcCansub) << "System resolver resolved" << host << "to" << address;
            succeed(address);
            return;
        }
    }

    /* For ".local" names, the mDNS lookup continues until it resolves or times out */
    if (mdnsResolver != nullptr)
    {
        qCDebug(lcCansub) << "System resolver found no IPv4 address for" << host << "- waiting for mDNS. Error:"
                          << info.errorString();
        return;
    }

    if (info.error() != QHostInfo::NoError)
    {
        fail(tr("Could not resolve the host %1 to an IPv4 address: %2").arg(host, info.errorString()));
    }
    else
    {
        fail(tr("The host %1 has no IPv4 address. The CANsub only supports IPv4.").arg(host));
    }
}

void HostLookup::onMdnsResolved(QHostAddress const &address)
{
    if (state != State::Running)
    {
        return;
    }

    if (address.protocol() != QAbstractSocket::IPv4Protocol)
    {
        return;
    }

    qCDebug(lcCansub) << "mDNS resolved" << host << "to" << address;
    succeed(address);
}

void HostLookup::onTimeout()
{
    if (state != State::Running)
    {
        return;
    }

    fail(tr("Could not resolve the host %1 to an IPv4 address within %2 ms.").arg(host).arg(timeoutMs));
}

void HostLookup::succeed(QHostAddress const &address)
{
    Q_ASSERT(state == State::Running);
    state = State::Finishing;
    stopLookups();

    /* Always queued: Never from within start(), and never from within a qmdnsengine signal, so the receiver may delete
     * this object in its slot. abort() and the destructor cancel the emission through the state. */
    QMetaObject::invokeMethod(
        this,
        [this, address]()
        {
            if (state != State::Finishing)
            {
                return;
            }
            state = State::Done;
            emit resolved(address);
        },
        Qt::QueuedConnection);
}

void HostLookup::fail(QString const &errorMessage)
{
    Q_ASSERT(state == State::Running);
    state = State::Finishing;
    stopLookups();

    /* Queued for the same reasons as in succeed() */
    QMetaObject::invokeMethod(
        this,
        [this, errorMessage]()
        {
            if (state != State::Finishing)
            {
                return;
            }
            state = State::Done;
            emit failed(errorMessage);
        },
        Qt::QueuedConnection);
}

void HostLookup::stopLookups()
{
    timeoutTimer.stop();
    mdnsQueryTimer.stop();

    if (hostInfoId != -1)
    {
        QHostInfo::abortHostLookup(hostInfoId);
        hostInfoId = -1;
    }

    /* This may run inside a signal of the resolver or the server, e.g. resolved(). Delete them later, so they are not
     * destroyed while their code is still on the stack. As children, they are still deleted with this object if that
     * comes first. */
    if (mdnsResolver != nullptr)
    {
        disconnect(mdnsResolver, nullptr, this, nullptr);
        mdnsResolver->deleteLater();
        mdnsResolver = nullptr;
    }
    if (mdnsServer != nullptr)
    {
        disconnect(mdnsServer, nullptr, this, nullptr);
        mdnsServer->deleteLater();
        mdnsServer = nullptr;
    }
}

} // namespace CANsubQt
