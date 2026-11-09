#include "CANsubConnection.hxx"

#include <algorithm>
#include <memory>
#include <QMetaObject>
#include <QNetworkProxy>
#include <QPointer>
#include <QRandomGenerator>
#include <QStringList>
#include <QTcpSocket>

#include "Logging.hxx"
#include "TcpKeepAlive.hxx"

namespace CANsubQt
{

/* Timeouts, see "Timeouts" in cansub_ffi.h. */

/* From open() until CANSUB_STATE_OPEN: The connect, the TLS handshake and the WebSocket upgrade. */
static constexpr int kDataOpenTimeoutMs = 5000;
/* From starting a control transport for outstanding requests until a request is in flight (connect and TLS). */
static constexpr int kControlOpenTimeoutMs = 5000;
/* From a request coming into flight until its response arrives. */
static constexpr int kRequestTimeoutMs = 5000;
/* Each stage of the close escalation: cansub_close(), cansub_close_force() and ending the transports. */
static constexpr int kCloseStageTimeoutMs = 3000;
/* How long the socket of an ended transport may take to close gracefully before it is aborted. */
static constexpr int kSocketLingerMs = 2000;
/* Delay before repeating a call which failed with CANSUB_RESULT_MEMORY. */
static constexpr int kMemoryRetryDelayMs = 20;

/* "409 Conflict" handling: Attempts in total, and the first delay, doubled per attempt plus up to half of it at random. */
static constexpr quint32 kHttpConflict = 409;
static constexpr int kRequestMaxAttempts = 5;
static constexpr int kConflictRetryBaseDelayMs = 50;

/* TCP keep-alive: A dead connection is detected after about 2 s + 3 * 1 s without data. Windows ignores the probe
 * count and always sends 10 probes, so there it takes about 2 s + 10 * 1 s, see enableTcpKeepAlive(). */
static constexpr int kKeepAliveIdleMs = 2000;
static constexpr int kKeepAliveIntervalMs = 1000;
static constexpr int kKeepAliveProbeCount = 3;

/* Stop draining while this much waits in the write buffer of a socket. Queued frames then stay in the library, so
 * CANSUB_RESULT_AGAIN from cansub_frame_send() reflects the link. */
static constexpr qint64 kWriteBufferLimit = 20 * 1024;
/* Read buffer of the sockets, so a library which accepts no data back-pressures the remote through TCP. */
static constexpr qint64 kReadBufferSize = 64 * 1024;
/* Size of the buffer moving bytes between the sockets and the library. */
static constexpr int kIoBufferSize = 16 * 1024;

static cansub_stream const kStreams[] = {CANSUB_STREAM_DATA, CANSUB_STREAM_CONTROL};
/* Order for ending both transports, see the close escalation. */
static cansub_stream const kStreamsControlFirst[] = {CANSUB_STREAM_CONTROL, CANSUB_STREAM_DATA};

/* Every state check goes through a mask, also for a single state. */
static constexpr quint32 stateBit(cansub_state state)
{
    return state < 32u ? quint32(1) << state : 0u;
}

static constexpr bool stateIn(cansub_state state, quint32 mask)
{
    return (stateBit(state) & mask) != 0u;
}

static constexpr quint32 kStateOpen = stateBit(CANSUB_STATE_OPEN);
static constexpr quint32 kStateClosing = stateBit(CANSUB_STATE_CLOSING);
static constexpr quint32 kStateClosed = stateBit(CANSUB_STATE_CLOSED);

static char const *streamName(cansub_stream stream)
{
    return stream == CANSUB_STREAM_DATA ? "data" : "control";
}

static QString resultName(cansub_result result)
{
    char buffer[64];
    cansub_result const size = cansub_result_name(result, buffer, sizeof(buffer));
    if (size > 0 && static_cast<size_t>(size) <= sizeof(buffer))
    {
        return QString::fromLatin1(buffer);
    }
    return QString::number(result);
}

static QString stateName(cansub_state state)
{
    char buffer[64];
    cansub_result const size = cansub_state_name(state, buffer, sizeof(buffer));
    if (size > 0 && static_cast<size_t>(size) <= sizeof(buffer))
    {
        return QString::fromLatin1(buffer);
    }
    return QString::number(state);
}

/* A user-given option as text for a message. An embedded NUL is shown as "\0". The size is passed, as Qt 5 stops at
 * the first NUL of a QByteArray. */
static QString optionText(QByteArray const &value)
{
    return QString::fromLatin1(value.constData(), value.size()).replace(QLatin1Char('\0'), QStringLiteral("\\0"));
}

/* Translated reasons for invalid context options, one sentence each, see cansub_options_fault_code. */
static QStringList describeOptionsFaults(cansub_options_faults faults, Connection::Options const &options)
{
    QStringList reasons;
    if ((faults & CANSUB_OPTIONS_FAULT_STRUCT_SIZE) != 0)
    {
        reasons << Connection::tr(
            "The CANsub library does not match the plugin (the size of the context options differs).");
    }
    if ((faults & CANSUB_OPTIONS_FAULT_HOST_MISSING) != 0)
    {
        reasons << Connection::tr("No host is given.");
    }
    if ((faults & CANSUB_OPTIONS_FAULT_HOST) != 0)
    {
        reasons << Connection::tr(
            "The host \"%1\" is invalid. It must have 1 to 255 characters, each a letter, a digit, '-', '.' or '_'.")
            .arg(optionText(options.host));
    }
    if ((faults & CANSUB_OPTIONS_FAULT_DEVICE_ID) != 0)
    {
        reasons << Connection::tr("The device ID \"%1\" is invalid. It must be 8 hex digits.")
            .arg(optionText(options.deviceId));
    }
    if ((faults & CANSUB_OPTIONS_FAULT_CHANNEL) != 0)
    {
        reasons << Connection::tr("The CAN channel must be 1 or higher.");
    }
    if ((faults & CANSUB_OPTIONS_FAULT_MTLS_PAIR) != 0)
    {
        reasons << Connection::tr(
            "Mutual TLS needs both a client certificate and a client key, but only one of them is given.");
    }
    if ((faults & CANSUB_OPTIONS_FAULT_MTLS_CERTIFICATE) != 0)
    {
        reasons << Connection::tr(
            "The client certificate cannot be read. It must be a certificate in PEM or DER format.");
    }
    if ((faults & CANSUB_OPTIONS_FAULT_MTLS_KEY) != 0)
    {
        reasons << Connection::tr(
            "The client key cannot be read. It must be an unencrypted private key in PEM or DER format.");
    }
    if ((faults & CANSUB_OPTIONS_FAULT_MTLS_MISMATCH) != 0)
    {
        reasons << Connection::tr("The client key does not belong to the client certificate.");
    }
    return reasons;
}

static QString describeInitFailure(cansub_result result)
{
    return Connection::tr("The CANsub library could not be initialized (%1).").arg(resultName(result));
}

/* The library options for options. The pointers refer to the data of options. */
static cansub_context_options toContextOptions(Connection::Options const &options)
{
    cansub_context_options contextOptions{};
    contextOptions.struct_size = sizeof(cansub_context_options);
    contextOptions.host = options.host.constData();
    contextOptions.device_id = options.deviceId.isEmpty() ? nullptr : options.deviceId.constData();
    contextOptions.channel = options.channel;
    contextOptions.accept_any_device = options.acceptAnyDevice ? 1 : 0;
    if (!options.clientCertificate.isEmpty())
    {
        contextOptions.client_certificate = options.clientCertificate.constData();
        contextOptions.client_certificate_size = static_cast<size_t>(options.clientCertificate.size());
    }
    if (!options.clientKey.isEmpty())
    {
        contextOptions.client_key = options.clientKey.constData();
        contextOptions.client_key_size = static_cast<size_t>(options.clientKey.size());
    }
    return contextOptions;
}

/**
 * Validate the options with cansub_context_options_validate(). Requires the library to be initialized.
 *
 * @param options Options to validate. The address is not checked.
 * @param[out] errorMessage If not NULL, set on failure to a translated reason.
 * @return True if the options are valid.
 */
static bool checkOptions(Connection::Options const &options, QString *errorMessage)
{
    cansub_context_options const contextOptions = toContextOptions(options);
    cansub_options_faults faults = 0;
    cansub_result result = cansub_context_options_validate(&contextOptions, &faults);

    /* The library takes C strings, so it would validate a host or device ID with an embedded NUL in truncated form.
     * A struct size fault is reported alone, as the library does. */
    bool const structSizeFault = result == CANSUB_RESULT_INVALID_ARG && (faults & CANSUB_OPTIONS_FAULT_STRUCT_SIZE) != 0;
    if ((result == CANSUB_RESULT_OK || result == CANSUB_RESULT_INVALID_ARG) && !structSizeFault)
    {
        if (options.host.contains('\0'))
        {
            faults |= CANSUB_OPTIONS_FAULT_HOST;
            result = CANSUB_RESULT_INVALID_ARG;
        }
        if (options.deviceId.contains('\0'))
        {
            faults |= CANSUB_OPTIONS_FAULT_DEVICE_ID;
            result = CANSUB_RESULT_INVALID_ARG;
        }
    }

    if (result == CANSUB_RESULT_OK)
    {
        return true;
    }
    qCDebug(lcCansub).noquote() << "cansub_context_options_validate() failed with" << resultName(result)
                                << "and the faults" << Qt::hex << faults;
    if (errorMessage != nullptr)
    {
        if (result == CANSUB_RESULT_INVALID_ARG && faults != 0)
        {
            *errorMessage = describeOptionsFaults(faults, options).join(QLatin1Char(' '));
        }
        else if (result == CANSUB_RESULT_MEMORY)
        {
            *errorMessage = Connection::tr(
                "The CANsub library ran out of memory while checking the connection options.");
        }
        else
        {
            *errorMessage = Connection::tr(
                "The CANsub library could not check the connection options: %1").arg(resultDescription(result));
        }
    }
    return false;
}

LibraryReference::LibraryReference()
    : initResult(cansub_library_init())
{
}

LibraryReference::~LibraryReference()
{
    if (initResult == CANSUB_RESULT_OK)
    {
        cansub_library_deinit();
    }
}

QString resultDescription(cansub_result result)
{
    switch (result)
    {
    case CANSUB_RESULT_OK:
    {
        return Connection::tr("No error.");
    }
    case CANSUB_RESULT_AGAIN:
    {
        return Connection::tr("The operation cannot be completed now.");
    }
    case CANSUB_RESULT_MEMORY:
    {
        return Connection::tr("The CANsub library ran out of memory.");
    }
    case CANSUB_RESULT_INVALID_ARG:
    {
        return Connection::tr("The CANsub library rejected an invalid argument.");
    }
    case CANSUB_RESULT_CLOSED:
    {
        return Connection::tr("The connection to the device is closed.");
    }
    case CANSUB_RESULT_INVALID_STATE:
    {
        return Connection::tr(
            "The CANsub library was called in an invalid state. This is an internal error of the plugin.");
    }
    case CANSUB_RESULT_CONNECTION_REJECTED:
    {
        return Connection::tr(
            "The device rejected the WebSocket connection. The CAN channel may be in use by another client, or it "
            "does not exist.");
    }
    case CANSUB_RESULT_CONNECTION_TLS:
    {
        return Connection::tr(
            "The TLS handshake with the device failed. The device certificate does not match the device ID or the "
            "host, or the device requires a client certificate, which the plugin does not support.");
    }
    case CANSUB_RESULT_CONNECTION_PROTOCOL:
    {
        return Connection::tr("The device sent an invalid response or violated the WebSocket protocol.");
    }
    case CANSUB_RESULT_CONNECTION_LOST:
    {
        return Connection::tr("The connection to the device was lost or could not be established.");
    }
    case CANSUB_RESULT_NOT_FOUND:
    {
        return Connection::tr("The requested item does not exist.");
    }
    case CANSUB_RESULT_NOT_SUPPORTED:
    {
        return Connection::tr("The CANsub library does not support this item.");
    }
    case CANSUB_RESULT_ERROR:
    {
        return Connection::tr("An internal error occurred in the CANsub library.");
    }
    default:
    {
        return Connection::tr("The CANsub library reported the error %1.").arg(resultName(result));
    }
    }
}

bool Connection::validate(Options const &options, QString *errorMessage)
{
    LibraryReference const reference;
    if (!reference.isInitialized())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = describeInitFailure(reference.result());
        }
        return false;
    }
    return checkOptions(options, errorMessage);
}

Connection *Connection::create(Options const &options, QObject *parent, QString *errorMessage)
{
    /* Parented only on success, so a failed creation is simply deleted. */
    std::unique_ptr<Connection> connection(new Connection(options, nullptr));
    if (!connection->library.isInitialized())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = describeInitFailure(connection->library.result());
        }
        return nullptr;
    }

    Options const &stored = connection->options;
    if (!checkOptions(stored, errorMessage))
    {
        return nullptr;
    }

    cansub_context_options const contextOptions = toContextOptions(stored);
    cansub_result const result = cansub_context_create(&connection->context, &contextOptions);
    if (result != CANSUB_RESULT_OK)
    {
        qCWarning(lcCansub).noquote() << "cansub_context_create() failed with" << resultName(result);
        connection->context = nullptr;
        if (errorMessage != nullptr)
        {
            if (result == CANSUB_RESULT_MEMORY)
            {
                *errorMessage = tr("The CANsub library ran out of memory while creating the connection.");
            }
            else
            {
                *errorMessage = tr("The CANsub library could not create the connection: %1")
                    .arg(resultDescription(result));
            }
        }
        return nullptr;
    }

    connection->setParent(parent);
    return connection.release();
}

Connection::Connection(Options const &options, QObject *parent)
    : QObject(parent)
    , options(options)
    , ioBuffer(kIoBufferSize, Qt::Uninitialized)
{
    /* The timers only record what happened and schedule a pump. */
    retryTimer.setSingleShot(true);
    retryTimer.setInterval(kMemoryRetryDelayMs);
    connect(&retryTimer, &QTimer::timeout, this, &Connection::schedulePump);

    dataOpenTimer.setSingleShot(true);
    dataOpenTimer.setInterval(kDataOpenTimeoutMs);
    connect(&dataOpenTimer, &QTimer::timeout, this, &Connection::onDataOpenTimeout);

    controlOpenTimer.setSingleShot(true);
    controlOpenTimer.setInterval(kControlOpenTimeoutMs);
    connect(&controlOpenTimer, &QTimer::timeout, this, &Connection::onControlOpenTimeout);

    requestTimer.setSingleShot(true);
    requestTimer.setInterval(kRequestTimeoutMs);
    connect(&requestTimer, &QTimer::timeout, this, &Connection::onRequestTimeout);

    closeTimer.setSingleShot(true);
    closeTimer.setInterval(kCloseStageTimeoutMs);
    connect(&closeTimer, &QTimer::timeout, this, &Connection::onCloseTimeout);
}

Connection::~Connection()
{
    if (context != nullptr)
    {
        cansub_context_destroy(context);
        context = nullptr;
    }

    stopTimers();

    /* Disconnected first, as abort() emits synchronously. Sockets still closing gracefully are children, aborted when
     * ~QObject() deletes them. */
    for (Transport &transport : transports)
    {
        if (transport.socket != nullptr)
        {
            QObject::disconnect(transport.socket, nullptr, this, nullptr);
            transport.socket->abort();
            delete transport.socket;
            transport.socket = nullptr;
        }
    }
}

/* Public interface. Never emits and never runs handlers, but schedules a pump. */

void Connection::request(QByteArray const &method, QByteArray const &path, QByteArray const &body,
                         ResponseHandler handler)
{
    RequestEntry entry;
    entry.id = nextRequestId++;
    entry.method = method;
    entry.path = path;
    entry.body = body;
    entry.handler = std::move(handler);
    if (closeRequested || finished || fatal)
    {
        qCDebug(lcCansub).noquote() << "Rejecting a request after close:" << method << path;
        entry.phase = RequestPhase::Done;
        entry.response.result = CANSUB_RESULT_CLOSED;
    }
    requests.push_back(std::move(entry));
    schedulePump();
}

bool Connection::open()
{
    if (fatal || finished || closeRequested)
    {
        return false;
    }
    if (openRequested)
    {
        return true;
    }

    cansub_result const result = cansub_open(context);
    if (isFatal(result, "cansub_open"))
    {
        schedulePump();
        return false;
    }
    if (result == CANSUB_RESULT_MEMORY)
    {
        openPending = true;
        retryTimer.start();
    }
    else if (result != CANSUB_RESULT_OK)
    {
        qCWarning(lcCansub).noquote() << "cansub_open() failed with" << resultName(result);
        return false;
    }

    qCDebug(lcCansub) << "Opening the data stream";
    openRequested = true;
    dataOpenTimer.start();
    schedulePump();
    return true;
}

void Connection::close()
{
    if (fatal || finished || closeRequested)
    {
        return;
    }
    closeRequested = true;
    qCDebug(lcCansub) << "Closing";

    /* Requests are accepted until close() is called, so hand the queued ones to the library while it still accepts
     * them. Their handlers are called from the next pump. */
    pumpRequests();
    if (fatal)
    {
        schedulePump();
        return;
    }

    cansub_result const result = cansub_close(context);
    if (isFatal(result, "cansub_close"))
    {
        schedulePump();
        return;
    }
    if (result == CANSUB_RESULT_MEMORY)
    {
        closePending = true;
        retryTimer.start();
    }

    if (closeStage == CloseStage::None)
    {
        closeStage = CloseStage::Graceful;
        closeTimer.start();
    }
    schedulePump();
}

cansub_result Connection::sendFrame(cansub_frame const &frame)
{
    if (fatal || finished)
    {
        return CANSUB_RESULT_CLOSED;
    }
    if (!openRequested || openPending)
    {
        /* cansub_frame_send() returns CANSUB_RESULT_INVALID_STATE until cansub_open() succeeded, which is retried after
         * CANSUB_RESULT_MEMORY while openPending is set. Treat it as still opening. */
        sendBlocked = true;
        return CANSUB_RESULT_AGAIN;
    }

    cansub_result const result = cansub_frame_send(context, &frame);
    if (result == CANSUB_RESULT_OK)
    {
        schedulePump();
        return CANSUB_RESULT_OK;
    }
    if (result == CANSUB_RESULT_AGAIN)
    {
        sendBlocked = true;
        return CANSUB_RESULT_AGAIN;
    }
    if (result == CANSUB_RESULT_MEMORY)
    {
        /* Not queued. Ask the caller to wait for readyToSend(), which the retry pump emits. */
        sendBlocked = true;
        retryTimer.start();
        return CANSUB_RESULT_AGAIN;
    }
    if (isFatal(result, "cansub_frame_send"))
    {
        schedulePump();
        return CANSUB_RESULT_CLOSED;
    }
    return result;
}

bool Connection::canSend() const
{
    if (fatal || finished)
    {
        return false;
    }
    return (cansub_get_ready(context) & CANSUB_READY_CAN_TX) != 0;
}

cansub_state Connection::state() const
{
    /* Also after the context was abandoned at the end of the close escalation. */
    if (fatal || finished)
    {
        return CANSUB_STATE_CLOSED;
    }
    return cansub_get_state(context);
}

/* Pump, see "Driving a context" in cansub_ffi.h. */

void Connection::schedulePump()
{
    if (pumpScheduled)
    {
        return;
    }
    pumpScheduled = true;
    /* Queued, so no public method and no socket signal pumps or emits synchronously. The call is dropped if the
     * connection is deleted first. */
    QMetaObject::invokeMethod(this, [this]() { pump(); }, Qt::QueuedConnection);
}

void Connection::pump()
{
    /* Cleared first, so a socket signal emitted from within this pump, e.g. by connectToHost(), schedules another. */
    pumpScheduled = false;

    if (!finished && !fatal)
    {
        bool progress = true;
        while (progress && !fatal)
        {
            progress = pumpPass();
            if (!fatal && stateIn(cansub_get_state(context), kStateOpen))
            {
                /* OPEN can be passed within one pump, e.g. opened and closed by the device. */
                reachedOpen = true;
            }
            if (!progress && !fatal)
            {
                /* Everything received after a remote close is processed now, so the EOF loses nothing. */
                progress = pumpRemoteClosedTransports();
            }
        }
        if (!fatal)
        {
            updateRequestTimers();
        }
        if (!fatal)
        {
            pumpState();
        }
        if (!fatal && !finished && sendBlocked && (cansub_get_ready(context) & CANSUB_READY_CAN_TX) != 0)
        {
            sendBlocked = false;
            readyToSendPending = true;
        }
    }

    if (fatal && !finished)
    {
        finish(CANSUB_RESULT_ERROR, CANSUB_RESULT_ERROR, CANSUB_RESULT_ERROR,
               tr("An internal error occurred in the CANsub library (%1 from %2()). The connection was closed.")
                   .arg(resultName(fatalResult), QString::fromLatin1(fatalCall)));
    }

    /* Must stay the last call of pump(): A slot may delete the connection, and then nothing of it may be touched.
     * A slot may also run a nested event loop which pumps again, so every event is taken before it is emitted. */

    emitEvents();
}

void Connection::emitEvents()
{
    QPointer<Connection> const self(this);

    if (reachedOpen && !openedEmitted)
    {
        openedEmitted = true;
        emit opened();
        if (self.isNull())
        {
            return;
        }
    }

    if (!receivedFrames.isEmpty())
    {
        QVector<cansub_frame> frames;
        frames.swap(receivedFrames);
        emit framesReceived(frames);
        if (self.isNull())
        {
            return;
        }
    }

    /* Handlers are called in request order, so a request waiting for a retry holds back the later ones. */
    while (!requests.empty() && requests.front().phase == RequestPhase::Done)
    {
        ResponseHandler const handler = std::move(requests.front().handler);
        Response const response = std::move(requests.front().response);
        requests.pop_front();
        if (handler)
        {
            handler(response);
            if (self.isNull())
            {
                return;
            }
        }
    }

    if (readyToSendPending)
    {
        readyToSendPending = false;
        emit readyToSend();
        if (self.isNull())
        {
            return;
        }
    }

    if (finished && !closedEmitted && requests.empty())
    {
        closedEmitted = true;
        /* Copied, as a slot may delete the connection while further slots still use the arguments. */
        cansub_result const error = closeError;
        QString const message = closeMessage;
        emit closed(error, message);
    }
}

bool Connection::pumpPass()
{
    bool progress = false;

    progress |= pumpPendingCalls();
    if (fatal)
    {
        return false;
    }

    progress |= pumpTransportAborts();
    if (fatal)
    {
        return false;
    }

    for (cansub_stream const stream : kStreams)
    {
        progress |= pumpTransport(stream);
        if (fatal)
        {
            return false;
        }
    }

    progress |= pumpFrames();
    if (fatal)
    {
        return false;
    }

    progress |= pumpResponses();
    if (fatal)
    {
        return false;
    }

    progress |= pumpRequests();
    if (fatal)
    {
        return false;
    }

    return progress;
}

bool Connection::pumpPendingCalls()
{
    bool progress = false;

    if (openPending)
    {
        cansub_result const result = cansub_open(context);
        if (isFatal(result, "cansub_open"))
        {
            return false;
        }
        if (result == CANSUB_RESULT_MEMORY)
        {
            retryTimer.start();
        }
        else
        {
            if (result != CANSUB_RESULT_OK)
            {
                qCWarning(lcCansub).noquote() << "cansub_open() failed with" << resultName(result);
            }
            openPending = false;
            progress = true;
        }
    }

    if (closePending)
    {
        cansub_result const result = cansub_close(context);
        if (isFatal(result, "cansub_close"))
        {
            return false;
        }
        if (result == CANSUB_RESULT_MEMORY)
        {
            retryTimer.start();
        }
        else
        {
            closePending = false;
            progress = true;
        }
    }

    if (forcePending)
    {
        cansub_result const result = cansub_close_force(context);
        if (isFatal(result, "cansub_close_force"))
        {
            return false;
        }
        if (result == CANSUB_RESULT_MEMORY)
        {
            retryTimer.start();
        }
        else
        {
            if (result == CANSUB_RESULT_AGAIN)
            {
                qCDebug(lcCansub) << "An abort of the queued CAN frames is already in progress";
            }
            forcePending = false;
            progress = true;
        }
    }

    return progress;
}

bool Connection::pumpTransportAborts()
{
    bool progress = false;
    for (cansub_stream const stream : kStreamsControlFirst)
    {
        if (transports[stream].abortRequested)
        {
            qCDebug(lcCansub) << "Ending the" << streamName(stream) << "transport without draining";
            endTransport(stream);
            if (fatal)
            {
                return false;
            }
            progress = true;
        }
    }
    return progress;
}

bool Connection::pumpTransport(cansub_stream stream)
{
    Transport &transport = transports[stream];
    cansub_ready const flags = cansub_ready_stream(cansub_get_ready(context), stream);

    if ((flags & CANSUB_READY_DATA_TRANSPORT_CLOSE) != 0)
    {
        if (transport.phase == TransportPhase::Connected)
        {
            closeTransportGracefully(stream);
        }
        else
        {
            /* Never connected: Abort a connect in progress and only signal the EOF. */
            qCDebug(lcCansub) << "Ending the" << streamName(stream) << "transport before it was connected";
            endTransport(stream);
        }
        return true;
    }

    switch (transport.phase)
    {
    case TransportPhase::Idle:
    {
        if ((flags & CANSUB_READY_DATA_TRANSPORT_OPEN) == 0)
        {
            return false;
        }
        if (endingTransports)
        {
            /* The close escalation ends every transport, so answer new requests for one as not opened. */
            qCDebug(lcCansub) << "Refusing a new" << streamName(stream) << "transport while ending the transports";
            endTransport(stream);
            return true;
        }
        startTransport(stream);
        return true;
    }

    case TransportPhase::Connecting:
    {
        if (!transport.tcpConnected || transport.abortRequested)
        {
            return false;
        }
        cansub_result const result = cansub_data_connected(context, stream);
        if (isFatal(result, "cansub_data_connected"))
        {
            return false;
        }
        if (result == CANSUB_RESULT_MEMORY)
        {
            retryTimer.start();
            return false;
        }
        if (result != CANSUB_RESULT_OK)
        {
            qCWarning(lcCansub).noquote() << "cansub_data_connected() failed with" << resultName(result);
            transport.abortRequested = true;
            return true;
        }
        qCDebug(lcCansub) << "The" << streamName(stream) << "transport is connected";
        transport.phase = TransportPhase::Connected;
        return true;
    }

    case TransportPhase::Connected:
    {
        break;
    }
    }

    bool progress = false;
    if (pumpDrain(stream))
    {
        progress = true;
    }
    if (fatal)
    {
        return false;
    }
    if (pumpSink(stream))
    {
        progress = true;
    }
    return progress;
}

bool Connection::pumpDrain(cansub_stream stream)
{
    Transport &transport = transports[stream];
    Q_ASSERT(transport.phase == TransportPhase::Connected);
    if ((cansub_ready_stream(cansub_get_ready(context), stream) & CANSUB_READY_DATA_DRAIN) == 0)
    {
        return false;
    }

    /* After a remote close or a socket error, the drained bytes have nowhere to go and are discarded. */
    QTcpSocket *const socket = transport.socket;
    bool const writable = !transport.remoteClosed && socket->state() == QAbstractSocket::ConnectedState;
    bool progress = false;
    for (;;)
    {
        if (writable && socket->bytesToWrite() >= kWriteBufferLimit)
        {
            /* Resumed by bytesWritten(). */
            break;
        }
        cansub_result const result = cansub_data_drain(context, stream, ioBuffer.data(),
                                                       static_cast<size_t>(ioBuffer.size()));
        if (isFatal(result, "cansub_data_drain"))
        {
            return false;
        }
        if (result <= 0)
        {
            break;
        }
        progress = true;
        if (writable)
        {
            qint64 const written = socket->write(ioBuffer.constData(), result);
            if (written != result)
            {
                qCWarning(lcCansub).noquote() << "Writing to the" << streamName(stream) << "transport failed:"
                                              << socket->errorString();
                transport.abortRequested = true;
                break;
            }
        }
    }
    return progress;
}

bool Connection::pumpSink(cansub_stream stream)
{
    Transport &transport = transports[stream];
    Q_ASSERT(transport.phase == TransportPhase::Connected);

    /* The received bytes stay in the read buffer of the socket until the library accepts them, also after a remote
     * close. So nothing is copied or allocated, and a library which accepts nothing back-pressures the device. */
    QTcpSocket *const socket = transport.socket;
    bool progress = false;
    for (;;)
    {
        qint64 const available = socket->bytesAvailable();
        if (available <= 0)
        {
            break;
        }
        if ((cansub_ready_stream(cansub_get_ready(context), stream) & CANSUB_READY_DATA_SINK) == 0)
        {
            /* Resumed once the library processed its data, e.g. after the frames are read. */
            break;
        }
        cansub_result const space = cansub_data_sink_space(context, stream);
        if (isFatal(space, "cansub_data_sink_space"))
        {
            return false;
        }
        if (space <= 0)
        {
            /* CANSUB_RESULT_CLOSED: TRANSPORT_CLOSE is set, the next pass closes the transport. */
            break;
        }

        qint64 const size = std::min({static_cast<qint64>(space), available, static_cast<qint64>(ioBuffer.size())});
        qint64 const peeked = socket->peek(ioBuffer.data(), size);
        if (peeked <= 0)
        {
            break;
        }
        cansub_result const accepted = cansub_data_sink(context, stream, ioBuffer.constData(),
                                                        static_cast<size_t>(peeked));
        if (isFatal(accepted, "cansub_data_sink"))
        {
            return false;
        }
        if (accepted <= 0)
        {
            /* CANSUB_RESULT_AGAIN, or CANSUB_RESULT_CLOSED with TRANSPORT_CLOSE set for the next pass. */
            break;
        }
        progress = true;
        if (socket->skip(accepted) != accepted)
        {
            qCWarning(lcCansub).noquote() << "Skipping the sunk bytes of the" << streamName(stream)
                                          << "transport failed:" << socket->errorString();
            transport.abortRequested = true;
            break;
        }
    }
    return progress;
}

bool Connection::pumpFrames()
{
    if ((cansub_get_ready(context) & CANSUB_READY_CAN_RX) == 0)
    {
        return false;
    }

    bool progress = false;
    for (;;)
    {
        cansub_frame frame;
        cansub_result const result = cansub_frame_recv(context, &frame);
        if (isFatal(result, "cansub_frame_recv"))
        {
            return false;
        }
        if (result != CANSUB_RESULT_OK)
        {
            break;
        }
        receivedFrames.append(frame);
        progress = true;
    }
    if (progress)
    {
        /* Frames only exist from CANSUB_STATE_OPEN on. */
        reachedOpen = true;
    }
    return progress;
}

bool Connection::pumpResponses()
{
    bool progress = false;
    while ((cansub_get_ready(context) & CANSUB_READY_CONTROL_RESPONSE) != 0)
    {
        uint64_t tag = 0;
        uint32_t httpStatus = 0;
        cansub_result const size = cansub_response_peek(context, &tag, &httpStatus);
        if (isFatal(size, "cansub_response_peek"))
        {
            return false;
        }
        if (size == CANSUB_RESULT_AGAIN || size == CANSUB_RESULT_CLOSED || size == CANSUB_RESULT_INVALID_ARG)
        {
            break;
        }

        /* A failed request (CONNECTION_*) has no body. */
        QByteArray body;
        if (size > 0)
        {
            body.resize(size);
        }
        cansub_result const result = cansub_response_recv(context, &tag, &httpStatus,
                                                          size > 0 ? body.data() : nullptr,
                                                          size > 0 ? static_cast<size_t>(size) : 0);
        if (isFatal(result, "cansub_response_recv"))
        {
            return false;
        }
        if (result == CANSUB_RESULT_AGAIN || result == CANSUB_RESULT_CLOSED || result == CANSUB_RESULT_INVALID_ARG)
        {
            qCWarning(lcCansub).noquote() << "cansub_response_recv() failed with" << resultName(result)
                                          << "after a successful peek";
            break;
        }
        if (result >= 0 && result < body.size())
        {
            body.truncate(result);
        }
        progress = true;
        handleResponse(tag, httpStatus, result, body);
    }
    return progress;
}

bool Connection::pumpRequests()
{
    bool progress = false;
    for (RequestEntry &entry : requests)
    {
        if (entry.phase != RequestPhase::Queued)
        {
            continue;
        }
        if ((cansub_get_ready(context) & CANSUB_READY_CONTROL_REQUEST) == 0)
        {
            break;
        }
        if (!submitRequest(entry))
        {
            break;
        }
        progress = true;
    }
    if (fatal)
    {
        return false;
    }
    return progress;
}

bool Connection::pumpRemoteClosedTransports()
{
    bool progress = false;
    for (cansub_stream const stream : kStreams)
    {
        Transport const &transport = transports[stream];
        if (transport.phase != TransportPhase::Connected || !transport.remoteClosed)
        {
            continue;
        }
        qint64 const unprocessed = transport.socket->bytesAvailable();
        if (unprocessed > 0)
        {
            qCWarning(lcCansub) << "The library did not accept the last" << unprocessed << "bytes received on the"
                                << streamName(stream) << "transport";
        }
        qCDebug(lcCansub) << "Signalling the EOF of the" << streamName(stream) << "transport closed by the device";
        endTransport(stream);
        if (fatal)
        {
            return false;
        }
        progress = true;
    }
    return progress;
}

void Connection::pumpState()
{
    cansub_state const current = cansub_get_state(context);
    if (!stateIn(current, stateBit(lastState)))
    {
        qCDebug(lcCansub).noquote() << "State" << stateName(lastState) << "->" << stateName(current);
        lastState = current;
    }

    if (stateIn(current, kStateOpen | kStateClosing | kStateClosed))
    {
        dataOpenTimer.stop();
    }
    if (stateIn(current, kStateOpen))
    {
        reachedOpen = true;
    }
    if (stateIn(current, kStateClosing) && closeStage == CloseStage::None)
    {
        /* Closing without close(), e.g. after a remote close or a fatal error. Bound it like close() does. */
        closeStage = CloseStage::Graceful;
        closeTimer.start();
    }
    if (stateIn(current, kStateClosed))
    {
        finishClosed();
    }
}

void Connection::updateRequestTimers()
{
    uint64_t tag = 0;
    cansub_result const result = cansub_request_in_flight(context, &tag);
    if (result == CANSUB_RESULT_OK)
    {
        controlOpenTimer.stop();
        if (tag != inFlightTag)
        {
            inFlightTag = tag;
            requestTimer.start();
        }
    }
    else if (result == CANSUB_RESULT_NOT_FOUND)
    {
        inFlightTag = 0;
        requestTimer.stop();
    }
    else
    {
        qCWarning(lcCansub).noquote() << "cansub_request_in_flight() failed with" << resultName(result);
    }
}

/* Transports */

void Connection::startTransport(cansub_stream stream)
{
    Transport &transport = transports[stream];
    Q_ASSERT(transport.phase == TransportPhase::Idle);
    Q_ASSERT(transport.socket == nullptr);

    QTcpSocket *const socket = new QTcpSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(kReadBufferSize);
    transport.socket = socket;
    transport.phase = TransportPhase::Connecting;

    /* The handlers only record what happened and schedule a pump, which makes every library call. */
    connect(socket, &QAbstractSocket::connected, this, [this, stream, socket]() { onSocketConnected(stream, socket); });
    connect(socket, &QAbstractSocket::disconnected, this,
            [this, stream, socket]() { onSocketDisconnected(stream, socket); });
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, stream, socket](QAbstractSocket::SocketError) { onSocketError(stream, socket); });
    connect(socket, &QIODevice::readyRead, this, &Connection::schedulePump);
    connect(socket, &QIODevice::bytesWritten, this, &Connection::schedulePump);

    if (stream == CANSUB_STREAM_CONTROL && outstandingRequests() > 0)
    {
        /* Not for a transport which only carries the abort of cansub_close_force(), see "Timeouts". */
        controlOpenTimer.start();
    }

    qCDebug(lcCansub).noquote() << "Connecting the" << streamName(stream) << "transport to"
                                << options.address.toString() << "port" << options.port;
    socket->connectToHost(options.address, options.port);
}

/* Ends a transport early, without draining, see "Timeouts" in cansub_ffi.h. Also answers a TRANSPORT_OPEN or
 * TRANSPORT_CLOSE of a transport which was never connected. */
void Connection::endTransport(cansub_stream stream)
{
    bool const connected = transports[stream].phase == TransportPhase::Connected;
    retireTransport(stream, false);

    cansub_ready const flags = cansub_ready_stream(cansub_get_ready(context), stream);
    bool const requested = (flags & (CANSUB_READY_DATA_TRANSPORT_OPEN | CANSUB_READY_DATA_TRANSPORT_CLOSE)) != 0;
    if (connected || requested)
    {
        cansub_result const result = cansub_data_eof(context, stream);
        isFatal(result, "cansub_data_eof");
    }
}

/* TRANSPORT_CLOSE on a connected transport: Drain until CANSUB_RESULT_CLOSED, close the socket, then the EOF. */
void Connection::closeTransportGracefully(cansub_stream stream)
{
    Transport const &transport = transports[stream];
    Q_ASSERT(transport.phase == TransportPhase::Connected);
    QTcpSocket *const socket = transport.socket;
    bool writable = !transport.remoteClosed && socket->state() == QAbstractSocket::ConnectedState;

    /* Drain everything, including the TLS close_notify, and write it before the graceful disconnect. */
    for (;;)
    {
        cansub_result const result = cansub_data_drain(context, stream, ioBuffer.data(),
                                                       static_cast<size_t>(ioBuffer.size()));
        if (isFatal(result, "cansub_data_drain"))
        {
            return;
        }
        if (result <= 0)
        {
            break;
        }
        if (writable && socket->write(ioBuffer.constData(), result) != result)
        {
            qCWarning(lcCansub).noquote() << "Writing to the closing" << streamName(stream) << "transport failed:"
                                          << socket->errorString();
            writable = false;
        }
    }

    qCDebug(lcCansub) << "Closing the" << streamName(stream) << "transport";
    retireTransport(stream, writable);

    cansub_result const result = cansub_data_eof(context, stream);
    isFatal(result, "cansub_data_eof");
}

/**
 * Detach the socket of a transport and reset the transport to idle. Does not call cansub_data_eof().
 *
 * @param stream Stream of the transport.
 * @param graceful Let the socket flush its written bytes (e.g. the TLS close_notify) and close, bounded by
 *        kSocketLingerMs. Otherwise the socket is aborted.
 */
void Connection::retireTransport(cansub_stream stream, bool graceful)
{
    QTcpSocket *const socket = transports[stream].socket;
    transports[stream] = Transport();
    if (stream == CANSUB_STREAM_CONTROL)
    {
        controlOpenTimer.stop();
    }
    if (socket == nullptr)
    {
        return;
    }

    /* First, as abort() and disconnectFromHost() emit synchronously. */
    QObject::disconnect(socket, nullptr, this, nullptr);
    if (!graceful || socket->state() != QAbstractSocket::ConnectedState)
    {
        socket->abort();
        socket->deleteLater();
        return;
    }

    /* Connected before disconnectFromHost(), which emits disconnected() synchronously if nothing is left to write. The
     * socket stays a child of the connection, so deleting the connection aborts it. */
    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
    QTimer::singleShot(kSocketLingerMs, socket, [socket]()
    {
        if (socket->state() != QAbstractSocket::UnconnectedState)
        {
            qCDebug(lcCansub) << "Aborting a socket which did not close in time";
            socket->abort();
        }
        socket->deleteLater();
    });
    socket->disconnectFromHost();
}

void Connection::onSocketConnected(cansub_stream stream, QTcpSocket *socket)
{
    Transport &transport = transports[stream];
    if (transport.socket != socket)
    {
        return;
    }

    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    if (!enableTcpKeepAlive(socket, kKeepAliveIdleMs, kKeepAliveIntervalMs, kKeepAliveProbeCount))
    {
        qCWarning(lcCansub) << "TCP keep-alive could not be configured for the" << streamName(stream) << "transport";
    }
    qCDebug(lcCansub) << "The TCP connection of the" << streamName(stream) << "transport is up";
    transport.tcpConnected = true;
    schedulePump();
}

void Connection::onSocketDisconnected(cansub_stream stream, QTcpSocket *socket)
{
    Transport &transport = transports[stream];
    if (transport.socket != socket)
    {
        return;
    }

    if (transport.phase == TransportPhase::Connected && !transport.abortRequested)
    {
        if (!transport.remoteClosed)
        {
            qCDebug(lcCansub) << "The device closed the" << streamName(stream) << "transport";
        }
        /* The bytes received before the close still belong to the stream, e.g. the WebSocket close. They stay
         * readable in the socket and are sunk before the EOF. */
        transport.remoteClosed = true;
    }
    else
    {
        if (stream == CANSUB_STREAM_DATA && dataEndReason.isEmpty())
        {
            dataEndReason = tr("The device closed the connection while it was being opened.");
        }
        transport.abortRequested = true;
    }
    schedulePump();
}

void Connection::onSocketError(cansub_stream stream, QTcpSocket *socket)
{
    Transport &transport = transports[stream];
    if (transport.socket != socket)
    {
        return;
    }

    if (socket->error() == QAbstractSocket::RemoteHostClosedError && transport.phase == TransportPhase::Connected
        && !transport.abortRequested)
    {
        if (!transport.remoteClosed)
        {
            qCDebug(lcCansub) << "The device closed the" << streamName(stream) << "transport";
        }
        transport.remoteClosed = true;
    }
    else
    {
        qCWarning(lcCansub).noquote() << "The" << streamName(stream) << "transport failed:" << socket->errorString();
        if (stream == CANSUB_STREAM_DATA && dataEndReason.isEmpty())
        {
            dataEndReason = tr("The connection to the device failed: %1").arg(socket->errorString());
        }
        transport.abortRequested = true;
    }
    schedulePump();
}

void Connection::onDataOpenTimeout()
{
    if (finished || fatal || stateIn(cansub_get_state(context), kStateOpen | kStateClosing | kStateClosed))
    {
        return;
    }
    qCWarning(lcCansub) << "Opening the data stream timed out after" << kDataOpenTimeoutMs << "ms";
    if (dataEndReason.isEmpty())
    {
        dataEndReason = tr("The device did not open the CAN channel in time.");
    }
    transports[CANSUB_STREAM_DATA].abortRequested = true;
    schedulePump();
}

void Connection::onControlOpenTimeout()
{
    if (finished || fatal)
    {
        return;
    }
    qCWarning(lcCansub) << "Opening the control stream timed out after" << kControlOpenTimeoutMs << "ms";
    transports[CANSUB_STREAM_CONTROL].abortRequested = true;
    schedulePump();
}

void Connection::onRequestTimeout()
{
    if (finished || fatal)
    {
        return;
    }
    qCWarning(lcCansub) << "The request with tag" << inFlightTag << "timed out after" << kRequestTimeoutMs << "ms";
    transports[CANSUB_STREAM_CONTROL].abortRequested = true;
    schedulePump();
}

void Connection::onCloseTimeout()
{
    if (finished || fatal)
    {
        return;
    }

    switch (closeStage)
    {
    case CloseStage::None:
    {
        return;
    }
    case CloseStage::Graceful:
    {
        qCWarning(lcCansub) << "The device did not confirm the close in time, aborting its queued CAN frames";
        closeStage = CloseStage::Forced;
        forcePending = true;
        closeTimer.start();
        break;
    }
    case CloseStage::Forced:
    {
        qCWarning(lcCansub) << "The abort did not complete in time, ending both transports";
        if (dataEndReason.isEmpty())
        {
            dataEndReason = tr("The device did not complete the close in time.");
        }
        closeStage = CloseStage::EndingTransports;
        endingTransports = true;
        transports[CANSUB_STREAM_CONTROL].abortRequested = true;
        transports[CANSUB_STREAM_DATA].abortRequested = true;
        closeTimer.start();
        break;
    }
    case CloseStage::EndingTransports:
    {
        qCWarning(lcCansub) << "The context did not reach the closed state after ending the transports, abandoning it";
        cansub_result error = cansub_error(context, nullptr, 0, nullptr);
        if (error == CANSUB_RESULT_OK || error == CANSUB_RESULT_INVALID_ARG)
        {
            error = CANSUB_RESULT_CONNECTION_LOST;
        }
        finish(CANSUB_RESULT_CONNECTION_LOST, CANSUB_RESULT_CLOSED, error,
               tr("The connection did not close in time and was abandoned."));
        break;
    }
    }
    schedulePump();
}

/* Requests */

bool Connection::submitRequest(RequestEntry &entry)
{
    Q_ASSERT(entry.phase == RequestPhase::Queued);

    /* Never reuse a tag, as the request timeout detects a new request in flight by its tag. */
    quint64 const tag = nextTag++;
    char const *contentType = entry.body.isEmpty() ? nullptr : "application/json";
    void const *body = entry.body.isEmpty() ? nullptr : entry.body.constData();
    cansub_result const result = cansub_request_send(context, tag, entry.method.constData(), entry.path.constData(),
                                                     contentType, body, static_cast<size_t>(entry.body.size()));
    if (isFatal(result, "cansub_request_send"))
    {
        return false;
    }
    if (result == CANSUB_RESULT_OK)
    {
        qCDebug(lcCansub).noquote() << "Sent request" << entry.method << entry.path << "with tag" << tag;
        entry.phase = RequestPhase::Sent;
        entry.tag = tag;
        ++entry.attempts;
        return true;
    }
    if (result == CANSUB_RESULT_AGAIN)
    {
        return false;
    }
    if (result == CANSUB_RESULT_MEMORY)
    {
        retryTimer.start();
        return false;
    }

    qCDebug(lcCansub).noquote() << "Request" << entry.method << entry.path << "not sent:" << resultName(result);
    if (result == CANSUB_RESULT_CLOSED && entry.attempts > 0)
    {
        /* A retry after "409 Conflict" can no longer be sent. The conflict is the final response. */
        completeRequest(entry);
    }
    else
    {
        completeRequest(entry, result);
    }
    return true;
}

void Connection::handleResponse(quint64 tag, quint32 httpStatus, cansub_result result, QByteArray const &body)
{
    auto const it = std::find_if(requests.begin(), requests.end(), [tag](RequestEntry const &entry)
    {
        return entry.phase == RequestPhase::Sent && entry.tag == tag;
    });
    if (it == requests.end())
    {
        qCWarning(lcCansub) << "Discarding a response with the unknown tag" << tag;
        return;
    }
    RequestEntry &entry = *it;

    Response response;
    if (result >= 0)
    {
        response.result = CANSUB_RESULT_OK;
        response.httpStatus = httpStatus;
        response.body = body;
    }
    else
    {
        qCDebug(lcCansub).noquote() << "Request" << entry.method << entry.path << "failed:" << resultName(result);
        response.result = result;
    }

    if (result >= 0 && httpStatus == kHttpConflict && entry.attempts < kRequestMaxAttempts)
    {
        int const delay = kConflictRetryBaseDelayMs << (entry.attempts - 1);
        int const jitter = QRandomGenerator::global()->bounded(delay / 2 + 1);
        qCDebug(lcCansub).noquote() << "Request" << entry.method << entry.path << "got 409 Conflict, retrying in"
                                    << delay + jitter << "ms";
        entry.phase = RequestPhase::RetryWait;
        entry.tag = 0;
        entry.response = response;
        quint64 const id = entry.id;
        QTimer::singleShot(delay + jitter, this, [this, id]() { onRequestRetry(id); });
        return;
    }

    completeRequest(entry, response);
}

void Connection::onRequestRetry(quint64 id)
{
    if (finished || fatal)
    {
        return;
    }
    auto const it = std::find_if(requests.begin(), requests.end(), [id](RequestEntry const &entry)
    {
        return entry.id == id;
    });
    if (it == requests.end() || it->phase != RequestPhase::RetryWait)
    {
        return;
    }
    it->phase = RequestPhase::Queued;
    schedulePump();
}

void Connection::completeRequest(RequestEntry &entry)
{
    entry.phase = RequestPhase::Done;
    entry.tag = 0;
}

void Connection::completeRequest(RequestEntry &entry, Response const &response)
{
    entry.response = response;
    completeRequest(entry);
}

void Connection::completeRequest(RequestEntry &entry, cansub_result result)
{
    Response response;
    response.result = result;
    completeRequest(entry, response);
}

int Connection::outstandingRequests() const
{
    return static_cast<int>(std::count_if(requests.begin(), requests.end(), [](RequestEntry const &entry)
    {
        return entry.phase == RequestPhase::Sent;
    }));
}

/* End of the connection */

bool Connection::isFatal(cansub_result result, char const *call)
{
    if (result != CANSUB_RESULT_ERROR && result != CANSUB_RESULT_INVALID_STATE)
    {
        return false;
    }
    if (!fatal)
    {
        qCWarning(lcCansub).noquote() << QStringLiteral("%1() failed with %2, the library context is unusable")
                                             .arg(QString::fromLatin1(call), resultName(result));
        fatal = true;
        fatalResult = result;
        fatalCall = call;
    }
    return true;
}

void Connection::finishClosed()
{
    /* Frames and responses can still be read in CANSUB_STATE_CLOSED. */
    pumpFrames();
    if (fatal)
    {
        return;
    }
    pumpResponses();
    if (fatal)
    {
        return;
    }

    size_t messageSize = 0;
    cansub_result const error = cansub_error(context, nullptr, 0, &messageSize);
    QString libraryMessage;
    if (messageSize > 1)
    {
        QByteArray buffer(static_cast<int>(messageSize), '\0');
        cansub_error(context, buffer.data(), messageSize, nullptr);
        libraryMessage = QString::fromUtf8(buffer.constData());
    }

    QString message;
    if (error != CANSUB_RESULT_OK)
    {
        QString const description = error == CANSUB_RESULT_CONNECTION_LOST && !dataEndReason.isEmpty()
                                        ? dataEndReason
                                        : resultDescription(error);
        message = libraryMessage.isEmpty() ? description : tr("%1 (%2)").arg(description, libraryMessage);
        qCWarning(lcCansub).noquote() << "Closed with" << resultName(error) << libraryMessage;
    }
    else
    {
        qCDebug(lcCansub) << "Closed";
    }

    finish(CANSUB_RESULT_CONNECTION_LOST, CANSUB_RESULT_CLOSED, error, message);
}

/* Completes every request and records the arguments of closed(). Emits nothing, emitEvents() does. */
void Connection::finish(cansub_result sentFailure, cansub_result queuedFailure, cansub_result error,
                        QString const &message)
{
    Q_ASSERT(!finished);

    for (RequestEntry &entry : requests)
    {
        switch (entry.phase)
        {
        case RequestPhase::Queued:
        {
            if (entry.attempts > 0)
            {
                /* Re-queued after "409 Conflict". */
                completeRequest(entry);
            }
            else
            {
                completeRequest(entry, queuedFailure);
            }
            break;
        }
        case RequestPhase::Sent:
        {
            completeRequest(entry, sentFailure);
            break;
        }
        case RequestPhase::RetryWait:
        {
            completeRequest(entry);
            break;
        }
        case RequestPhase::Done:
        {
            break;
        }
        }
    }

    stopTimers();
    /* After CANSUB_STATE_CLOSED both transports are idle already. Sockets still closing gracefully finish by
     * themselves. */
    for (cansub_stream const stream : kStreams)
    {
        retireTransport(stream, false);
    }
    finished = true;
    sendBlocked = false;
    readyToSendPending = false;
    closeError = error;
    closeMessage = message;
}

void Connection::stopTimers()
{
    retryTimer.stop();
    dataOpenTimer.stop();
    controlOpenTimer.stop();
    requestTimer.stop();
    closeTimer.stop();
}

} // namespace CANsubQt
