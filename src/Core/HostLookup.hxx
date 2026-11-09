#ifndef CANSUB_QT_HOST_LOOKUP_HXX
#define CANSUB_QT_HOST_LOOKUP_HXX

#include <QByteArray>
#include <QHostAddress>
#include <QHostInfo>
#include <QObject>
#include <QString>
#include <QTimer>

namespace QMdnsEngine
{
class Resolver;
class Server;
}

namespace CANsubQt
{

/**
 * Resolves the host of a CANsub target to an IPv4 address. The CANsub only speaks IPv4.
 *
 * - IPv4 literal: Used as is.
 * - Name ending in ".local": Resolved with mDNS (qmdnsengine) and with the system resolver (QHostInfo) in parallel.
 *   The first IPv4 address from either wins. The system resolver covers platforms with their own mDNS responder
 *   (e.g. Avahi with nss-mdns, Windows 10+), qmdnsengine the platforms without.
 * - Any other name: Resolved with the system resolver.
 *
 * Lives in the thread it was created in, which must run an event loop.
 */
class HostLookup : public QObject
{
    Q_OBJECT

public:
    explicit HostLookup(QString host, QObject *parent = nullptr);
    ~HostLookup() override;

    /**
     * Start the lookup.
     *
     * Exactly one of resolved() or failed() is emitted afterwards, never from within start(), unless abort() is
     * called or the object is destroyed first. Call only once.
     *
     * @param timeoutMs Time until failed() is emitted if no IPv4 address was found.
     */
    void start(int timeoutMs);

    /** Stop the lookup. No signal is emitted afterwards. */
    void abort();

signals:
    /** The host resolved to an IPv4 address. */
    void resolved(QHostAddress const &address);

    /** The lookup failed or timed out. The message is translated and names the host. */
    void failed(QString const &errorMessage);

private:
    enum class State
    {
        Idle,      /* start() not called yet */
        Running,   /* Lookups in progress */
        Finishing, /* Result decided, its signal is queued */
        Done,      /* Signal emitted or aborted */
    };

    void startMdns(QByteArray const &name);
    void sendMdnsQuery();
    void onMdnsResolved(QHostAddress const &address);
    void onTimeout();
    void succeed(QHostAddress const &address);
    void fail(QString const &errorMessage);
    void stopLookups();

    QString host;
    State state = State::Idle;
    int timeoutMs = 0;
    QTimer timeoutTimer;

    /* System resolver, -1 if no lookup is pending */
    int hostInfoId = -1;

    /* mDNS resolver, only for ".local" names. Both are children of this object, and nullptr when not running. */
    QByteArray mdnsName;
    QMdnsEngine::Server *mdnsServer = nullptr;
    QMdnsEngine::Resolver *mdnsResolver = nullptr;
    QTimer mdnsQueryTimer;

private slots:
    /* Result of the system resolver. A slot for the string-based QHostInfo::lookupHost(), see start(). */
    void onHostInfo(QHostInfo const &info);
};

} // namespace CANsubQt

#endif // CANSUB_QT_HOST_LOOKUP_HXX
