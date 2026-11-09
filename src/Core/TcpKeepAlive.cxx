#include "TcpKeepAlive.hxx"

/* The Winsock headers must come before any header which may include windows.h. */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <mstcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <QAbstractSocket>
#include <QtGlobal>
#include <QVariant>

#include "Logging.hxx"

namespace CANsubQt
{

#ifndef _WIN32
/* The socket options take whole seconds. Round up, so the timing is never shorter than requested. */
static int toSeconds(int milliseconds)
{
    return qMax(1, (milliseconds + 999) / 1000);
}

static bool setTcpOption(qintptr descriptor, int option, int value, char const *name)
{
    if (setsockopt(static_cast<int>(descriptor), IPPROTO_TCP, option, &value, sizeof(value)) != 0)
    {
        qCWarning(lcCansub) << "setsockopt" << name << "failed";
        return false;
    }
    return true;
}
#endif

bool enableTcpKeepAlive(QAbstractSocket *socket, int idleMs, int intervalMs, int probeCount)
{
    Q_ASSERT(socket != nullptr);

    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    if (socket->socketOption(QAbstractSocket::KeepAliveOption).toInt() == 0)
    {
        qCWarning(lcCansub) << "TCP keep-alive could not be enabled";
        return false;
    }

    qintptr const descriptor = socket->socketDescriptor();
    if (descriptor == -1)
    {
        return false;
    }

#ifdef _WIN32
    /* The probe count is fixed by Windows (10 probes). */
    Q_UNUSED(probeCount);

    tcp_keepalive settings = {};
    settings.onoff = 1;
    settings.keepalivetime = static_cast<ULONG>(qMax(1, idleMs));
    settings.keepaliveinterval = static_cast<ULONG>(qMax(1, intervalMs));
    DWORD bytesReturned = 0;
    int const result = WSAIoctl(static_cast<SOCKET>(descriptor), SIO_KEEPALIVE_VALS, &settings, sizeof(settings),
                                nullptr, 0, &bytesReturned, nullptr, nullptr);
    if (result != 0)
    {
        qCWarning(lcCansub) << "WSAIoctl SIO_KEEPALIVE_VALS failed with" << WSAGetLastError();
        return false;
    }
    return true;
#else
    bool tuned = true;

#if defined(TCP_KEEPIDLE)
    if (!setTcpOption(descriptor, TCP_KEEPIDLE, toSeconds(idleMs), "TCP_KEEPIDLE"))
    {
        tuned = false;
    }
#elif defined(TCP_KEEPALIVE)
    /* macOS names the idle time TCP_KEEPALIVE. */
    if (!setTcpOption(descriptor, TCP_KEEPALIVE, toSeconds(idleMs), "TCP_KEEPALIVE"))
    {
        tuned = false;
    }
#else
    Q_UNUSED(idleMs);
    tuned = false;
#endif

#if defined(TCP_KEEPINTVL)
    if (!setTcpOption(descriptor, TCP_KEEPINTVL, toSeconds(intervalMs), "TCP_KEEPINTVL"))
    {
        tuned = false;
    }
#else
    Q_UNUSED(intervalMs);
    tuned = false;
#endif

#if defined(TCP_KEEPCNT)
    if (!setTcpOption(descriptor, TCP_KEEPCNT, qMax(1, probeCount), "TCP_KEEPCNT"))
    {
        tuned = false;
    }
#else
    Q_UNUSED(probeCount);
    tuned = false;
#endif

    return tuned;
#endif
}

} // namespace CANsubQt
