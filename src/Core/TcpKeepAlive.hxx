#ifndef CANSUB_QT_TCP_KEEP_ALIVE_HXX
#define CANSUB_QT_TCP_KEEP_ALIVE_HXX

class QAbstractSocket;

namespace CANsubQt
{

/**
 * Enable TCP keep-alive on a connected socket and tune its timing.
 *
 * @param socket Connected socket.
 * @param idleMs Idle time before the first probe.
 * @param intervalMs Time between probes.
 * @param probeCount Unanswered probes before the connection is dropped.
 * @note probeCount Is ignored on Windows, which uses a fixed count.
 * @return True if keep-alive is enabled with the requested timing. False if only the OS defaults could be applied, or
 *         keep-alive could not be enabled at all.
 */
bool enableTcpKeepAlive(QAbstractSocket *socket, int idleMs, int intervalMs, int probeCount);

} // namespace CANsubQt

#endif // CANSUB_QT_TCP_KEEP_ALIVE_HXX
