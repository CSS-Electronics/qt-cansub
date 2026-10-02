#include <algorithm>
#include <functional>
#include <optional>
#include <QDateTime>
#include <QDebug>
#include <QPointer>
#include <QSslSocket>
#include <QTimer>

#include "CANsub.hxx"
#include "CANsubProtocol.hxx"
#include "Certificate.hxx"
#include "GuardedTransition.hxx"
#include "HostResolver.hxx"
#include "ResolverFactory.hxx"
#include "QtVersionHelper.hxx"
#include "BatchTracker.hxx"
#include "LogicTransition.hxx"
#include "NetworkManagerWithPeerInfo.hxx"

/* qint64 to match QWebSocket::bytesToWrite(), keeping the comparisons free of sign-conversion warnings */
constexpr qint64 outstandingBytesLimit = 2048 * 10;
constexpr unsigned closeHandshakeTimeoutMs = 2000;

constexpr bool CANsubConfigurationKeyListenOnly_default = false;
constexpr bool CANsubConfigurationKeyAutoReset_default = true;
constexpr bool CANsubConfigurationKeyErrorFramesEnabled_default = false;
constexpr bool CANsubConfigurationKeyUpdateTime_default = true;
constexpr bool CANsubConfigurationKeyBlockingWrite_default = false;
constexpr bool CANsubConfigurationKeyReceiveOwn_default = false;

/* Default timeout (in ms) for API calls */
#define CANSUB_API_DEFAULT_TIMEOUT_MS                           10000

/* Default timeout (in ms) for the websocket call */
#define CANSUB_WEBSOCKET_TIMEOUT_MS                             10000

/* Default timeout (in ms) for the mDNS lookup */
#define CANSUB_MDNS_TIMEOUT_MS                                  10000

/* Settle delay (in ms) after the websocket connects, before reporting the bus as connected. The remote opens the
 * hardware CAN bus asynchronously after accepting the websocket, leaving a brief window in which the websocket is
 * not yet associated with the bus. */
#define CANSUB_WS_SETTLE_MS                                     10

/* Helper struct for storing all bit-timing parameters */
struct CombinedBitTiming
{
    uint brp;
    uint tseg1;
    uint tseg2;
    uint sjw;
    uint tseg1_data;
    uint tseg2_data;
    uint sjw_data;
};

/* When storing the CombinedBitTiming in a QMap, we need to support sorting. This combined key class handles this. */
struct CombinedBitTimingKey
{
    uint samplePoint;
    uint bitrate;
    uint bitrateData;

    bool operator<(CombinedBitTimingKey const& other) const
    {
        return std::tie(samplePoint, bitrate, bitrateData) < std::tie(other.samplePoint, other.bitrate, other.bitrateData);
    }

    bool operator==(CombinedBitTimingKey const& other) const
    {
        return std::tie(samplePoint, bitrate, bitrateData) == std::tie(other.samplePoint, other.bitrate, other.bitrateData);
    }
};

struct CANsubConnectionStates
{
    QState* lookupHost;
    QState* getVersion;
    QState* getTime;
    QState* setTime;
    QState* setBusPhy;
    QState* getBusFilters;
    QState* setBusFilters;
    QState* openWebSocket;
    QFinalState* connected;
    QFinalState* error;
};

static CANsubConnectionStates allocateStates(QStateMachine* stateMachine)
{
    CANsubConnectionStates result = {nullptr};

    result.lookupHost = new QState(stateMachine);
    result.getVersion = new QState(stateMachine);
    result.getTime = new QState(stateMachine);
    result.setTime = new QState(stateMachine);
    result.setBusPhy = new QState(stateMachine);
    result.getBusFilters = new QState(stateMachine);
    result.setBusFilters = new QState(stateMachine);
    result.openWebSocket = new QState(stateMachine);
    result.connected = new QFinalState(stateMachine);
    result.error = new QFinalState(stateMachine);

    return result;
}

/**
 * Internal helper function to map bit-rates to bit-timings via a sample point.
 * @param bitrate Target bitrate
 * @param bitrateData Target data bit-rate
 * @param samplePoint Target sample point
 * @return Matching bit-timing parameters or empty if none found
 */
static std::optional<CombinedBitTiming> getBitTiming(uint bitrate, uint bitrateData, uint samplePoint);


CANsub::CANsub(QString const& address, uint8_t channel, ResolverFactory const* resolverFactory, QObject* parent) :
    QCanBusDevice(parent),
    deviceAddress(address),
    deviceChannel(channel),
    networkAccessManager(new NetworkManagerWithPeerInfo(this)),
    bufferRx(100 * 1024),
    resolverFactory(resolverFactory),
    connectionStateMachine(nullptr),
    blockingWrites(false)
{
    api.setNetworkAccessManager(networkAccessManager);

    connect(networkAccessManager, &QNetworkAccessManager::sslErrors, this,
            [this](QNetworkReply* reply, const QList<QSslError>& errors)
            {
                if (acceptSslErrors(errors))
                {
                    /* The no-argument overload is used deliberately, as it is the only variant that works on every
                     * supported Qt release, and the check function has already vetted the
                     * complete error set of this handshake. This handling is only valid for this transaction. */
                    reply->ignoreSslErrors();
                }
            });

    /* Set the default values for the static configuration parameters */
    QCanBusDevice::setConfigurationParameter(QCanBusDevice::LoopbackKey, false);

    /* Set the default values for the non-static configuration parameters */
    QCanBusDevice::setConfigurationParameter(QCanBusDevice::CanFdKey, true);
    QCanBusDevice::setConfigurationParameter(QCanBusDevice::ReceiveOwnKey, false);

    /* Set the default sample point */
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeySamplePoint, 8000);

    /* Set other default values */
    QCanBusDevice::setConfigurationParameter(
        CANsubConfigurationKeyListenOnly, CANsubConfigurationKeyListenOnly_default);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyAutoReset, CANsubConfigurationKeyAutoReset_default);
    QCanBusDevice::setConfigurationParameter(
        CANsubConfigurationKeyErrorFramesEnabled, CANsubConfigurationKeyErrorFramesEnabled_default);
    QCanBusDevice::setConfigurationParameter(
        CANsubConfigurationKeyUpdateTime, CANsubConfigurationKeyUpdateTime_default);
    QCanBusDevice::setConfigurationParameter(
        CANsubConfigurationKeyBlockingWrite, CANsubConfigurationKeyBlockingWrite_default);

    webSocketCloseTimeoutTimer.setSingleShot(true);
    connect(&webSocketCloseTimeoutTimer, &QTimer::timeout, this, [this]()
    {
        qWarning() << "WebSocket close handshake timed out, aborting connection";
        QPointer<CANsub> alive(this);
        webSocket->abort();

        /* Below is a safeguard in case abort does not handle re-setting the state correctly */
        if (alive && state() == ClosingState)
        {
            setState(UnconnectedState);
        };
    });

    /* Create and wire the initial transport websocket */
    createWebSocket();

    /* Connect default signals */
    connect(&api, &OpenAPI::OAIDefaultApi::apiVersionGetSignal, this, &CANsub::versionCheckSlot);
    connect(&api, &OpenAPI::OAIDefaultApi::apiVersionGetSignalError, this, &CANsub::versionCheckErrorSlot);

    api.setTimeOut(CANSUB_API_DEFAULT_TIMEOUT_MS);
}

void CANsub::createWebSocket()
{
    /* Dispose of any previous instance. Sever our handlers first, so tearing down the old socket cannot emit into
     * the device. */
    if (webSocket != nullptr)
    {
        disconnect(webSocket, nullptr, this, nullptr);
        webSocket->abort();
        webSocket->deleteLater();
    }

    webSocket = new WebSocket(this);

    connect(webSocket, &WebSocket::disconnected, this, [this]
    {
        webSocketCloseTimeoutTimer.stop();
        bool const wasConnected = (state() == ConnectedState);

        /* setError() may re-enter and delete this device, so update our own state first and emit last */
        QPointer<CANsub> alive(this);
        setState(UnconnectedState);
        if (alive && wasConnected)
        {
            setError(QStringLiteral("Connection to remote CANsub device lost"), ConnectionError);
        }
    });

    /* Wire the receive handler once for the lifetime of the socket. The connection dies with it on replacement.
     * The forwarding policy is read from the forwardFd member, which are refreshed from configuration
     * on every open(). */
    connect(webSocket, &WebSocket::binaryMessageReceived, this, [this](QByteArray const& data)
    {
        wsDataReceived(data);
    });

    /* The socket is captured explicitly in the handlers below, so they always act on the instance that emitted,
     * never on a successor. */
    connect(webSocket, &WebSocket::error, this,
            [this, socket = webSocket](QAbstractSocket::SocketError error)
            {
                qCritical() << "WebSocket error: " << error;
                recordPendingError(QStringLiteral("WebSocket error: %1").arg(socket->errorString()), QString(), ConnectionError);
            });

    connect(webSocket, &WebSocket::sslErrors, this, [this, socket = webSocket](const QList<QSslError>& errors)
    {
        if (acceptSslErrors(errors))
        {
            /* The no-argument overload is used deliberately, as it is the only variant that reaches the in-flight
             * socket on every supported Qt release, and the check function has already vetted the complete error set of
             * this handshake. It does latch ignore-everything on this QWebSocket instance for the rest of its life,
             * which is contained: createWebSocket() gives every connection attempt a fresh instance, so the latch
             * never outlives the attempt it was vetted for. */
            socket->ignoreSslErrors();
        }
    });
}

CANsub::~CANsub()
{
    /* Sever the websocket handlers first, as closing the socket below can emit synchronously, and those handlers emit
     * client-visible signals (stateChanged/errorOccurred). */
    disconnect(webSocket, nullptr, this, nullptr);

    /* Close the websocket if open. The instance itself is parented to this device and dies with it. */
    if (webSocket->isValid())
    {
        webSocket->close();

        /* NOTE: It may be that an abort is necessary here. Kept for reference. */
        //webSocket->abort();
    }

    /* Abort any pending request via the OpenAPI wrapper */
    api.abortRequests();

    /* Ensure to also stop and delete the statemachine if it is present */
    if (connectionStateMachine)
    {
        connectionStateMachine->stop();
        stateMachineDestroy();
    }
}

QCanBusDeviceInfo CANsub::createInfo(const QString& hostname, int channel)
{
    /* We need to bastardize the name attribute, since we need to encode both the device address and the channel in one
     * variable, which is also used as an identifier in the GUIs. */

    const QString pluginName = QStringLiteral("CANsub");
    const QString serialNumber = hostname.left(8);
    const QString description = QString("CANsub Device %1 Channel %2").arg(serialNumber).arg(channel);

    QString interfaceName = hostname;

    /* The hostname we receive from mDNS lookup has a trailing ".". Remove it when presenting it to the end user. */
    if (interfaceName.endsWith("."))
    {
        interfaceName.truncate(interfaceName.lastIndexOf("."));
    }
    interfaceName.append(QString("@%1").arg(QString::number(channel)));

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QCanBusDevice::createDeviceInfo(
        pluginName,
        interfaceName,
        serialNumber,
        description,
        QString(),
        channel,
        false,
        true
        );
#else
    return QCanBusDevice::createDeviceInfo(
        interfaceName,
        serialNumber,
        description,
        channel,
        false,
        true
        );
#endif
}

bool CANsub::open()
{
    /* Clear the internal error trackers and state */
    clearError();
    pendingErrorCode = NoError;
    pendingErrorString.clear();
    bufferRx.clear();

    qDebug() << "Starting connection attempt";

    /* Some Windows builds of the Qt framework, e.g. the ones shipped with SavvyCAN and PlotJuggler, does not
     * support TLS */
    if (!QSslSocket::supportsSsl())
    {
        pendingErrorString = "TLS/SSL support is missing on this platform, cannot connect to CANsub device";
        pendingErrorCode = ConnectionError;
        qCritical() << pendingErrorString;
        abortOpen();
        return false;
    }

    if (this->connectionStateMachine != nullptr)
    {
        /* Connection attempt already in progress */
        qWarning() << "Attempting to connect while already connecting";
        return true;
    }
    /* NOTE: We do not check for ConnectedState/ClosingState, as this already should have been handled by the Qt
     *       framework. */

    /* Resolve how the remote's TLS identity should be verified before we start talking to it. */
    if (!resolveTlsPolicy())
    {
        abortOpen();
        return false;
    }

    /* Never reuse the previous attempt's websocket. Replace it with a freshly wired instance so no TLS-ignore
     * state can leak between attempts. The state-machine transitions below bind to the new instance. */
    createWebSocket();

    /* We handle the entire connection sequence via a state machine, as there is a lot of communication back and forth
     * with the CANsub remote, and the latency may be too long to handle without handing control back to the control
     * loop.
     * NOTE: The state storage can reside as a stack allocated struct here, since it only stores pointers and all the
     *       state pointers will have the state machine as parent, thus having a life-time identical to the state
     *       machine. */
    connectionStateMachine = new QStateMachine(this);
    auto connectionStates = allocateStates(connectionStateMachine);

    configureLookupState(connectionStates);
    configureGetVersionState(connectionStates);
    configureGetTimeState(connectionStates);
    configureSetTimeState(connectionStates);
    if (!configureSetBusPhyState(connectionStates))
    {
        abortOpen();
        return false;
    }
    configureGetBusFiltersState(connectionStates);
    configureSetBusFiltersState(connectionStates);
    configureOpenWebsocketState(connectionStates);
    configureTerminalStates(connectionStates);

    connectionStateMachine->setInitialState(connectionStates.lookupHost);
    qDebug() << "Starting state machine";
    connectionStateMachine->start();

    return true;
}

void CANsub::close()
{
    clearError();

    /* Disconnect the websocket to close the bus and abort any in-flight requests */
    api.abortRequests();
    if (webSocket->state() == QAbstractSocket::ConnectedState)
    {
        webSocket->close();
    }
    else if (webSocket->state() == QAbstractSocket::ConnectingState)
    {
        webSocket->abort();
    }

    /* We may have closed the bus without ever opening the websocket. If it is already closed, mark the CAN bus as
     * closed. */
    if (webSocket->state() == QAbstractSocket::UnconnectedState)
    {
        setState(UnconnectedState);
    }
    else
    {
        webSocketCloseTimeoutTimer.start(closeHandshakeTimeoutMs);
    }

    /* Ensure to also stop and delete the statemachine if it is present */
    if (connectionStateMachine)
    {
        connectionStateMachine->stop();
        stateMachineDestroy();
    }
}

QString CANsub::interpretErrorFrame(const QCanBusFrame& errorFrame)
{
    QString result;

    if (errorFrame.frameType() == QCanBusFrame::ErrorFrame)
    {
        auto const errorType = errorFrame.error();

        if (errorType & QCanBusFrame::FrameError::TransmissionTimeoutError)
        {
            result += "Failed to transmit requested frame.\n";
        }
        if (errorType & QCanBusFrame::FrameError::LostArbitrationError)
        {
            result += "Lost CAN bus arbitration during frame transmission.\n";
        }
        if (errorType & QCanBusFrame::FrameError::ControllerError)
        {
            result += "Remote CAN controller error.\n";
        }
        if (errorType & QCanBusFrame::FrameError::ProtocolViolationError)
        {
            result += "CAN protocol violation error.\n";
        }
        if (errorType & QCanBusFrame::FrameError::TransceiverError)
        {
            result += "Failed to write requested payload.\n";
        }
        if (errorType & QCanBusFrame::FrameError::MissingAcknowledgmentError)
        {
            result += "Frame was not acknowledged on the CAN bus.\n";
        }
        if (errorType & QCanBusFrame::FrameError::BusOffError)
        {
            result += "CAN bus is offline.\n";
        }
        if (errorType & QCanBusFrame::FrameError::BusError)
        {
            result += "An error occurred on the CAN bus.\n";
        }
        if (errorType & QCanBusFrame::FrameError::ControllerRestartError)
        {
            result += "The remote CAN controller has restarted.\n";
        }
        if (errorType & QCanBusFrame::FrameError::UnknownError)
        {
            result += "Unknown error.\n";
        }

        if (!result.isEmpty())
        {
            result.prepend("The following errors occurred:\n");
        }
    }

    return result;
}

template <typename T>
bool CANsub::configurationHelperForwardIfValid(CANBUS_CONFIGURATION_KEY key, QVariant const& value)
{
    if ((value.isValid() && value.canConvert<T>()) || !value.isValid())
    {
        /* Forward to the base class */
        QCanBusDevice::setConfigurationParameter(key, value);
        return true;
    }

    return false;
}

void CANsub::setConfigurationParameter(CANBUS_CONFIGURATION_KEY key, const QVariant& value)
{
    switch (key)
    {
        case QCanBusDevice::ConfigurationKey::LoopbackKey:
        {
            /* The device will never be able to perform loopback, so this option cannot be set. */
            qWarning() << "Attempting to set fixed key LoopbackKey with value" << value.toString()
                << ". Value is fixed to false";
            break;
        }
        case QCanBusDevice::ConfigurationKey::CanFdKey:
        {
            if (value.isValid() && value.canConvert<bool>())
            {
                if (!value.value<bool>())
                {
                    qWarning() <<
                        "Attempting to disable CAN-FD operation. CANsub will always enable CAN-FD on the PHY, but can filter out CAN-FD frames in the data stream.";
                }
                QCanBusDevice::setConfigurationParameter(key, value);
            }
            else
            {
                QString const errorMsg = QString("Invalid value for key CanFd with type %1 and value %2").
                                         arg(value.typeName()).arg(value.toString());
                qCritical() << errorMsg;
                setError(errorMsg, ConfigurationError);
            }
            break;
        }
        case QCanBusDevice::ConfigurationKey::ReceiveOwnKey:
        case CANsubConfigurationKeyListenOnly:
        case CANsubConfigurationKeyAutoReset:
        case CANsubConfigurationKeyErrorFramesEnabled:
        {
            if (!configurationHelperForwardIfValid<bool>(key, value))
            {
                QString const errorMsg = QString("Invalid value for key %1 with type %2").arg(key).arg(
                    GET_VARIANT_TYPE(value));
                setError(errorMsg, ConfigurationError);
            }
            break;
        }
        case CANsubConfigurationKeyDisableCertificateValidation:
        {
            qWarning() << "The DisableCertificateValidation configuration key has been deprecated and no longer has any effect";
            break;
        }
        case CANsubConfigurationKeySamplePoint:
        {
            if ((value.isValid() && value.canConvert<uint>() && (value.value<uint>() <= 9999)) || !value.isValid())
            {
                /* Forward to the base class */
                QCanBusDevice::setConfigurationParameter(key, value);
            }
            else
            {
                QString const errorMsg = QString("Invalid value for key SamplePoint with type %1").arg(
                    GET_VARIANT_TYPE(value));
                qCritical() << errorMsg;
                setError(errorMsg, ConfigurationError);
            }
            break;
        }
        case CANsubConfigurationKeyExpectedHostname:
        {
            if (!configurationHelperForwardIfValid<QString>(key, value))
            {
                QString const errorMsg = QString("Invalid value for key ExpectedHostname with type %1").arg(GET_VARIANT_TYPE(value));
                qCritical() << errorMsg;
                setError(errorMsg, ConfigurationError);
            }
            break;
        }
        case CANsubConfigurationKeyBitTiming:
        case CANsubConfigurationKeyBitTimingData:
        {
            if (!configurationHelperForwardIfValid<CANsubBitTiming>(key, value))
            {
                QString const keyName = (key == CANsubConfigurationKeyBitTiming) ? "BitTiming" : "BitTimingData";
                QString const errorMsg = QString("Invalid value for key %1 with type %2").arg(keyName).arg(
                    GET_VARIANT_TYPE(value));
                qCritical() << errorMsg;
                setError(errorMsg, ConfigurationError);
            }
            break;
        }
        default:
        {
            /* Forward all other requests to the base class */
            QCanBusDevice::setConfigurationParameter(key, value);
            break;
        }
    }
}

bool CANsub::writeFrame(const QCanBusFrame& frame)
{
    /* Gate on the device state rather than the websocket: the websocket is already valid during the settle window
     * (ConnectingState), before the remote has associated it with the hardware CAN bus. */
    if (state() != QCanBusDevice::ConnectedState)
    {
        setError("Attempting to write a frame to a CANsub device that is not connected", QCanBusDevice::WriteError);
        return false;
    }
    if (!frame.isValid())
    {
        setError("Attempting to write an invalid frame", QCanBusDevice::WriteError);
        return false;
    }
    if (frame.hasFlexibleDataRateFormat() && !forwardFd)
    {
        setError("Attempting to write an FD frame to a non-FD bus", QCanBusDevice::WriteError);
        return false;
    }
    if (frame.frameType() == QCanBusFrame::ErrorFrame)
    {
        setError("Attempting to write an error frame", QCanBusDevice::WriteError);
        return false;
    }

    if (webSocket->bytesToWrite() > outstandingBytesLimit)
    {
        /* Try a flush now */
        webSocket->flush();
        if (webSocket->bytesToWrite() > outstandingBytesLimit)
        {
            /* Re-entrant blocking writes (a framesReceived emission dispatched from the nested loop below can lead a
             * client slot back into writeFrame) fall through to the non-blocking drop: Nesting another wait would
             * invert transmit order, consume the outer call's deadline and allow unbounded recursion. */
            if (blockingWrites && !inBlockingWrite)
            {
                /* Block until the write can happen, up to a timeout of 1 s. A bytesWritten emission may reflect only
                 * a partial flush of the backlog, so keep waiting until the backlog drops below the limit or the
                 * deadline expires, instead of giving up on the first emission.
                 * WARNING: This can potentially block the event loop */
                QPointer<CANsub> alive(this);
                QEventLoop loop;
                QTimer timeoutTimer;
                bool timedOut = false;
                timeoutTimer.setSingleShot(true);
                timeoutTimer.setInterval(1000);
                connect(webSocket, &WebSocket::bytesWritten, &loop, &QEventLoop::quit);
                connect(&timeoutTimer, &QTimer::timeout, &loop, [&loop, &timedOut]
                {
                    timedOut = true;
                    loop.quit();
                });
                timeoutTimer.start();

                inBlockingWrite = true;
                while (!timedOut)
                {
                    loop.exec();

                    /* The nested event loop dispatches arbitrary events. A connection loss handled inside it may have
                     * led a client slot to delete this device. Touch no member in that case. (Deleting the device also
                     * kills the bytesWritten connection, so the loop then exits via the stack-owned timeout timer.) */
                    if (alive.isNull())
                    {
                        return false;
                    }

                    if (webSocket->bytesToWrite() <= outstandingBytesLimit)
                    {
                        break;
                    }
                }
                inBlockingWrite = false;

                /* The nested loop may have dispatched a connection loss. An aborted socket reports an empty backlog,
                 * so the backlog re-check below would pass. Verify the device is still connected before
                 * transmitting. */
                if (state() != QCanBusDevice::ConnectedState)
                {
                    setError("Connection lost while waiting to transmit frame to remote CANsub device",
                             QCanBusDevice::WriteError);
                    return false;
                }

                /* Re-check condition. Do not block permanently, just drop this frame here. */
                if (webSocket->bytesToWrite() > outstandingBytesLimit)
                {
                    setError("Unable to transmit frame to remote CANsub device", QCanBusDevice::WriteError);
                    return false;
                }
            }
            else
            {
                setError("Unable to transmit frame to remote CANsub device", QCanBusDevice::WriteError);
                return false;
            }
        }
    }

    QByteArray data;
    CANsubProtocolEncode(data, frame);
    qint64 const bytesSent = webSocket->sendBinaryMessage(data);
    if (bytesSent == data.size())
    {
        /* Ensure no previous write error has survived to here */
        if (error() == QCanBusDevice::WriteError)
        {
            clearError();
        }

        framesWritten(1);
        return true;
    }

    setError("Unable to transmit the whole frame to remote CANsub device", QCanBusDevice::WriteError);
    return false;
}

static std::optional<CombinedBitTiming> getBitTiming(uint bitrate, uint bitrateData, uint samplePoint)
{
    std::optional<CombinedBitTiming> result;

    /* Map of default bit-timings. Currently only populated for a sample point of 80 %. */
    static QMap<CombinedBitTimingKey, CombinedBitTiming> bitrates(
    {
        {{8000, 50000, 1000000}, {8, 159, 40, 4, 7, 2, 2}},
        {{8000, 100000, 1000000}, {4, 159, 40, 4, 15, 4, 4}},
        {{8000, 100000, 2000000}, {4, 159, 40, 4, 7, 2, 2}},
        {{8000, 125000, 1000000}, {4, 127, 32, 4, 15, 4, 4}},
        {{8000, 125000, 2000000}, {2, 255, 64, 4, 15, 4, 4}},
        {{8000, 125000, 4000000}, {2, 255, 64, 4, 7, 2, 2}},
        {{8000, 200000, 1000000}, {4, 79, 20, 4, 15, 4, 4}},
        {{8000, 200000, 2000000}, {2, 159, 40, 4, 15, 4, 4}},
        {{8000, 200000, 4000000}, {2, 159, 40, 4, 7, 2, 2}},
        {{8000, 250000, 1000000}, {4, 63, 16, 4, 15, 4, 4}},
        {{8000, 250000, 2000000}, {2, 127, 32, 4, 15, 4, 4}},
        {{8000, 250000, 4000000}, {1, 255, 64, 4, 15, 4, 4}},
        {{8000, 500000, 1000000}, {4, 31, 8, 4, 15, 4, 4}},
        {{8000, 500000, 2000000}, {2, 63, 16, 4, 15, 4, 4}},
        {{8000, 500000, 4000000}, {1, 127, 32, 4, 15, 4, 4}},
        {{8000, 800000, 1000000}, {4, 19, 5, 4, 15, 4, 4}},
        {{8000, 800000, 2000000}, {2, 39, 10, 4, 15, 4, 4}},
        {{8000, 800000, 4000000}, {1, 79, 20, 4, 15, 4, 4}},
        {{8000, 1000000, 1000000}, {4, 15, 4, 4, 15, 4, 4}},
        {{8000, 1000000, 2000000}, {2, 31, 8, 4, 15, 4, 4}},
        {{8000, 1000000, 4000000}, {1, 63, 16, 4, 15, 4, 4}},
    });

    CombinedBitTimingKey const key = {samplePoint, bitrate, bitrateData};

    if (bitrates.contains(key))
    {
        result.emplace(bitrates[key]);
    }

    return result;
}

void CANsub::wsDataReceived(QByteArray const& data)
{
    if (!bufferRx.append(reinterpret_cast<std::byte const*>(data.constData()), static_cast<std::size_t>(data.size())))
    {
        /* The receive buffer is full and could not be reclaimed: Drop the current buffer and attempt again. */
        qWarning() << "Receive buffer overflow, dropping the current buffer";
        bufferRx.clear();

        if (!bufferRx.append(reinterpret_cast<std::byte const*>(data.constData()), static_cast<std::size_t>(data.size())))
        {
            /* The receive buffer is not large enough to handle this payload at all, drop it all. */
            qWarning() << "Receive buffer not large enough to handle received payload";
            return;
        }
    }

    /* Clear local storage and attempt to decode as many frames as possible from the rx buffer into the frame buffer */
    receivedFrames.clear();
    CANsubProtocolDecode(bufferRx, receivedFrames);

    /* Filter out all the frames we do not want to see */
    receivedFrames.erase(
        std::remove_if(
            receivedFrames.begin(),
            receivedFrames.end(),
            [forwardFd = forwardFd](QCanBusFrame const& v)
            {
                if (!forwardFd && (v.frameType() == QCanBusFrame::FrameType::DataFrame) && (v.
                    hasFlexibleDataRateFormat()))
                {
                    return true;
                }
                return false;
            }
            ), receivedFrames.end());
    if (!receivedFrames.empty())
    {
        enqueueReceivedFrames(receivedFrames);
    }
}

bool CANsub::acceptSslErrors(QList<QSslError> const& errors) const
{
    bool validCertificateChain = false;

    /* We always connect to the resolved IP, so a HostNameMismatch is expected. Verify the certificate's identity
     * against the verification hostname (the mDNS hostname, or the user-supplied expected hostname when connecting by
     * IP) instead. If no hostname is supplied, accept a hostname mismatch, but ensure that the certificate *is* the
     * correct root certificate. */
    if ((errors.size() == 1) && (errors.first().error() == QSslError::HostNameMismatch))
    {
        /* If no hostname is set, we are connecting by IP and only require the root certificate to be valid. */
        if (tlsPolicy.verificationHostname.isEmpty())
        {
            qWarning() << "Hostname validation is disabled, but correct root found";
            return true;
        }

        /* The hostname must either be part of the common name or one of the SANs */
        auto const& certificate = errors.first().certificate();
        QStringList commonNames = certificate.subjectInfo(QSslCertificate::SubjectInfo::CommonName);
        if (commonNames.length() != 1)
        {
            /* There should never be more than one common name in the subject field */
            qWarning() << "Invalid device certificate";
            return false;
        }
        if (commonNames.first().compare("CANsub " + tlsPolicy.verificationHostname, Qt::CaseInsensitive) == 0)
        {
            /* Full match */
            return true;
        }

        /* Check if any of the Subject Alternative Names matches */
        for (auto const& san : certificate.subjectAlternativeNames().values(QSsl::DnsEntry))
        {
            if (san.compare(tlsPolicy.verificationHostname, Qt::CaseInsensitive) == 0)
            {
                return true;
            }
        }
    }

    qCritical() << "SSL connection failed with non-recoverable errors:" << errors;
    return false;
}

void CANsub::versionCheckSlot(QString const& value)
{
    /* Cache the version value. It cannot change while the bus is open. */
    version = value;

    if (version.value<QString>() == CANSUB_EXPECTED_VERSION_JSON)
    {
        emit versionCheckComplete(QPrivateSignal());
    }
    else
    {
        recordPendingError(
            QStringLiteral("Unsupported remote firmware version %1 (expected %2)").arg(
                QString(version.toString()).remove('"') , CANSUB_EXPECTED_VERSION),
            QString(), ConnectionError);

        emit versionCheckError(QPrivateSignal());
    }
}

void CANsub::versionCheckErrorSlot(QString const& summary, QNetworkReply::NetworkError error, const QString& errorStr)
{
    Q_UNUSED(summary);
    Q_UNUSED(error);

    recordPendingError(QStringLiteral("Version query failed"), errorStr, ConnectionError);

    emit versionCheckError(QPrivateSignal());
}

void CANsub::stateMachineDestroy()
{
    if (!connectionStateMachine)
    {
        return;
    }

    /* Disconnect the API from the state machine (so they don't use the soon-to-be nullptr) */
    disconnect(&api, nullptr, connectionStateMachine, nullptr);

    connectionStateMachine->deleteLater();
    connectionStateMachine = nullptr;
}

template <typename OkSignal, typename ErrSignal>
void CANsub::wireRequestState(QState* state, std::function<void()> onEntered,
                              OkSignal ok, QAbstractState* onOk,
                              ErrSignal err, QAbstractState* onError)
{
    wireGenericState(state, onEntered, &api, ok, onOk, &api, err, onError);
}

template <typename OkSignal, typename ErrSignal>
void CANsub::wireRequestState(QState* state, std::function<void()> onEntered,
                              OkSignal ok, QAbstractState* onOk,
                              ErrSignal err, QAbstractState* onError,
                              QString errorContext, CanBusError errorCode)
{
    wireRequestState(state, std::move(onEntered), ok, onOk, err, onError);
    wireErrorCapture(err, std::move(errorContext), errorCode);
}

void CANsub::recordPendingError(QString const& context, QString const& detail, CanBusError code)
{
    if (!pendingErrorString.isEmpty())
    {
        /* Only keep track of the first error, such that later failures does not hide the root cause. */
        return;
    }

    pendingErrorString = detail.isEmpty() ? context : QStringLiteral("%1: %2").arg(context, detail);
    pendingErrorCode = code;
}

void CANsub::reportPendingError()
{
    if (pendingErrorCode == NoError)
    {
        pendingErrorCode = ConnectionError;
    }
    if (pendingErrorString.isEmpty())
    {
        pendingErrorString = QStringLiteral("Error during connection attempt");
    }

    setError(pendingErrorString, pendingErrorCode);
}

void CANsub::abortOpen()
{
    /* A synchronous failure may happen before or after the state machine has been allocated */
    if (connectionStateMachine)
    {
        stateMachineDestroy();
    }

    /* Report synchronously so error()/errorString() are populated once connectDevice() returns, matching the
     * plugins shipped with Qt (they call setError() directly in their open() paths). The emission runs while
     * QCanBusDevice::connectDevice() is still on the stack, so a client slot deleting this device directly,
     * instead of via deleteLater() as Qt requires when deleting a signal's sender, leaves connectDevice() on a
     * dangling this. Consequently nothing may touch this object after the report, and callers must return
     * immediately. */
    reportPendingError();
}

void CANsub::wireErrorCapture(void (OpenAPI::OAIDefaultApi::*err)(QNetworkReply::NetworkError, QString const&),
                              QString errorContext, CanBusError errorCode)
{
    connect(&api, err, connectionStateMachine,
            [this, errorContext = std::move(errorContext), errorCode](QNetworkReply::NetworkError, QString const& detail)
            {
                recordPendingError(errorContext, detail, errorCode);
            });
}

template <typename Summary>
void CANsub::wireErrorCapture(void (OpenAPI::OAIDefaultApi::*err)(Summary, QNetworkReply::NetworkError, QString const&),
                              QString errorContext, CanBusError errorCode)
{
    connect(&api, err, connectionStateMachine,
            [this, errorContext = std::move(errorContext), errorCode](Summary const&, QNetworkReply::NetworkError,
                                                                      QString const& detail)
            {
                recordPendingError(errorContext, detail, errorCode);
            });
}

template <typename OkSender, typename OkSignal, typename ErrSender, typename ErrSignal>
void CANsub::wireGenericState(
    QState* state,
    std::function<void()> onEntered,
    OkSender okSender, OkSignal ok, QAbstractState* onOk,
    ErrSender errSender, ErrSignal err, QAbstractState* onError)
{
    connect(state, &QState::entered, this, std::move(onEntered));
    state->addTransition(okSender, ok, onOk);
    state->addTransition(errSender, err, onError);
}

bool CANsub::addressIsIp() const
{
    return !QHostAddress(deviceAddress).isNull();
}

bool CANsub::resolveTlsPolicy()
{
    auto* const nam = dynamic_cast<NetworkManagerWithPeerInfo*>(networkAccessManager);
    if (!nam)
    {
        recordPendingError(QStringLiteral("Internal error: unexpected network access manager type"), QString(),
                           ConnectionError);
        return false;
    }

    if (!addressIsIp())
    {
        /* Hostname connection. Verify the certificate against the hostname. */
        tlsPolicy.verificationHostname = deviceAddress;
        nam->setTlsPolicy(tlsPolicy);
        return true;
    }

    /* IP connection. The certificate is issued for the mDNS hostname and cannot match the IP, so the user ought to
     * tell us how to handle verification via the expected hostname. */
    QVariant const expectedHostNameRaw = configurationParameter(CANsubConfigurationKeyExpectedHostname);
    QString const expectedHostname = (expectedHostNameRaw.isValid() && expectedHostNameRaw.canConvert<QString>()) ? expectedHostNameRaw.toString() : QString();

    if (!expectedHostname.isEmpty())
    {
        /* Verify the certificate against the user-supplied hostname instead of the IP. */
        tlsPolicy.verificationHostname = expectedHostname;
        nam->setTlsPolicy(tlsPolicy);
        return true;
    }

    /* Fallback. No hostname supplied but we still verify the certificate chain. */
    tlsPolicy.verificationHostname.clear();
    nam->setTlsPolicy(tlsPolicy);
    return true;
}

void CANsub::configureLookupState(CANsubConnectionStates const& connectionStates)
{
    /* The lookup state's success/error are CANsub signals emitted from this object, not from the api */
    wireGenericState(
        connectionStates.lookupHost,
        [this]()
        {
            qDebug() << "Entering lookupHostState";

            /* If the user supplied a raw IP there is nothing to resolve, use it directly and continue */
            if (addressIsIp())
            {
                resolvedHost.setValue(deviceAddress);
                emit dnsLookupComplete(QPrivateSignal());
                return;
            }

            /* We want to perform an mDNS lookup. As such, it is important that the hostname we look up ends
             * with a "." */
            QString const lookupName = deviceAddress + ".";

            auto resolver = resolverFactory->createResolver(lookupName.toUtf8(), connectionStateMachine);
            if (!resolver)
            {
                emit dnsLookupError(QPrivateSignal());
                return;
            }
            auto* const resolverTimer = new QTimer(connectionStateMachine);

            resolverTimer->setInterval(CANSUB_MDNS_TIMEOUT_MS);
            resolverTimer->setSingleShot(true);

            connect(resolverTimer, &QTimer::timeout, resolver, &QObject::deleteLater);
            connect(resolverTimer, &QTimer::timeout, resolverTimer, &QTimer::deleteLater);
            connect(resolverTimer, &QTimer::timeout, this, [this, resolver]
            {
                /* Both deleteLater() calls above are deferred, so the resolver can still emit resolved() until its
                 * deletion is processed, and the resolved handler dereferences the (equally deferred-deleted) timer.
                 * Cut the connection so a late result cannot reach it. */
                disconnect(resolver, nullptr, this, nullptr);
                emit dnsLookupError(QPrivateSignal());
            });

            std::shared_ptr<bool> resolved = std::make_shared<bool>(false);
            connect(resolver, &HostResolver::resolved, this,
                    [this, resolved, resolver, resolverTimer](QHostAddress const& address)
                    {
                        /* CANsub only supports IPv4 */
                        if ((*resolved == false) && (address.protocol() == QAbstractSocket::IPv4Protocol))
                        {
                            *resolved = true;
                            resolverTimer->stop();
                            resolverTimer->deleteLater();
                            resolver->deleteLater();
                            resolvedHost.setValue(address.toString());
                            emit dnsLookupComplete(QPrivateSignal());
                        }
                    });

            resolverTimer->start();
        },
        this, &CANsub::dnsLookupComplete, connectionStates.getVersion,
        this, &CANsub::dnsLookupError, connectionStates.error
        );
}

void CANsub::configureGetVersionState(CANsubConnectionStates const& connectionStates)
{
    QVariant const updateTimeRaw = configurationParameter(CANsubConfigurationKeyUpdateTime);
    bool updateTime = false;
    if (updateTimeRaw.isValid() && updateTimeRaw.canConvert<bool>() && updateTimeRaw.value<bool>())
    {
        updateTime = true;
    }

    /* The version state's success/error are CANsub signals emitted from this object (via versionCheckSlot), not
     * directly from the api. */
    wireGenericState(
        connectionStates.getVersion,
        [this]()
        {
            qDebug() << "Entering getVersionState";

            if (!resolvedHost.isValid() || !resolvedHost.canConvert<QString>())
            {
                qCritical() << "Attempting to resolve invalid name";
                emit versionCheckError(QPrivateSignal());
                return;
            }

            QUrl serverUrl;
            serverUrl.setScheme("https");
            serverUrl.setHost(resolvedHost.toString());
            serverUrl.setPort(443);

            /* Store the default REST endpoint */
            api.setNewServerForAllOperations(serverUrl);

            /* Check if we previously resolved the version to a supported instance */
            if (version.isValid() && version.canConvert<QString>() && version.value<QString>() ==
                CANSUB_EXPECTED_VERSION_JSON)
            {
                qDebug() << "Version is already valid, skipping";
                emit versionCheckComplete(QPrivateSignal());
            }
            else if (version.isValid())
            {
                qDebug() << "Version is already invalid, skipping" << version.toString();
                recordPendingError(QStringLiteral("Unsupported remote firmware version %1 (expected %2)").arg(QString(version.toString()).remove('"') , CANSUB_EXPECTED_VERSION), QString(), ConnectionError);
                emit versionCheckError(QPrivateSignal());
            }
            else
            {
                qDebug() << "Requesting version via API";
                api.apiVersionGet();
            }
        },
        this, &CANsub::versionCheckComplete, updateTime ? connectionStates.getTime : connectionStates.setBusPhy,
        this, &CANsub::versionCheckError, connectionStates.error
        );
}

/**
 * Compute the absolute difference (in ms) between the remote clock reported in a apiTimeGetSignal and the local
 * clock. Returns an empty optional if the signal arguments are malformed or the reported time is invalid, in which
 * case neither sync transition fires and the invalid-time guard routes the machine to the error state.
 */
static std::optional<qint64> remoteClockDeltaMs(QList<QVariant> const& args)
{
    if (args.size() != 1)
    {
        qWarning() << "Invalid number of arguments";
        return std::nullopt;
    }
    if (GET_VARIANT_TYPE(args.at(0)) != qMetaTypeId<OpenAPI::OAITime>())
    {
        qWarning() << "Invalid type";
        return std::nullopt;
    }

    const auto time = args.at(0).value<OpenAPI::OAITime>();
    QDateTime value = time.getUtc();

    /* The generated client emits the success signal with a default-constructed QDateTime when an HTTP 200 body
     * fails to parse. toMSecsSinceEpoch() on it is undefined, so reject it here. */
    if (!value.isValid())
    {
        qWarning() << "Invalid remote time";
        return std::nullopt;
    }

    /* The device reports its time in UTC (RFC3339 'Z'). Re-label rather than convert, in case the generated
     * deserializer dropped the timespec: The calendar digits are UTC digits either way, so this is a no-op on a
     * correctly labelled value. */
    CANSUB_SET_UTC(value);

    return qAbs(value.toMSecsSinceEpoch() - QDateTime::currentMSecsSinceEpoch());
}

void CANsub::configureGetTimeState(CANsubConnectionStates const& connectionStates)
{
    /* The getTime state needs custom handling: the same apiTimeGetSignal drives one of two transitions depending on
     * how far the remote clock has drifted, so it cannot use the generic ok/error helper. */
    connect(connectionStates.getTime, &QState::entered, this, [this]
    {
        qDebug() << "Entering getTimeState";
        api.apiTimeGet();
    });

    auto const addTimeGuard = [this, &connectionStates](std::function<bool(qint64)> const& accept,
                                                        QAbstractState* target)
    {
        auto* const transition = new GuardedTransition(connectionStates.getTime, [accept](QList<QVariant> const& args)
        {
            auto const delta = remoteClockDeltaMs(args);
            return delta.has_value() && accept(delta.value());
        });
        transition->setSenderObject(&api);
        transition->setSignal(
            QMetaMethod::fromSignal(&OpenAPI::OAIDefaultApi::apiTimeGetSignal).methodSignature().constData());
        transition->setTargetState(target);
        connectionStates.getTime->addTransition(transition);
    };

    /* Out of sync (>= 1 s): push the current time before configuring the bus. */
    addTimeGuard([](qint64 delta) { return delta >= 1000; }, connectionStates.setTime);

    /* In sync (< 1 s): skip straight to the bus configuration. */
    addTimeGuard([](qint64 delta) { return delta < 1000; }, connectionStates.setBusPhy);

    /* Malformed or invalid time payload (e.g. an HTTP 200 whose body failed to parse): Fail the attempt instead of
     * sitting in getTime with no pending request. Transitions are tested in the order they were added, so this
     * guard only runs after both sync guards have rejected, and its recording side effect only fires when the
     * transition is actually taken. */
    auto* const invalidTimeTransition = new GuardedTransition(connectionStates.getTime,
                                                              [this](QList<QVariant> const& args)
    {
        if (remoteClockDeltaMs(args).has_value())
        {
            return false;
        }
        recordPendingError(QStringLiteral("Remote returned an invalid time (channel %1)").arg(deviceChannel),
                           QString(), ConfigurationError);
        return true;
    });
    invalidTimeTransition->setSenderObject(&api);
    invalidTimeTransition->setSignal(
        QMetaMethod::fromSignal(&OpenAPI::OAIDefaultApi::apiTimeGetSignal).methodSignature().constData());
    invalidTimeTransition->setTargetState(connectionStates.error);
    connectionStates.getTime->addTransition(invalidTimeTransition);

    connectionStates.getTime->addTransition(&api, &OpenAPI::OAIDefaultApi::apiTimeGetSignalError, connectionStates.error);
    wireErrorCapture(&OpenAPI::OAIDefaultApi::apiTimeGetSignalError,
                     QStringLiteral("Getting remote time failed (channel %1)").arg(deviceChannel), ConfigurationError);
}

void CANsub::configureSetTimeState(CANsubConnectionStates const& connectionStates)
{
    wireRequestState(
        connectionStates.setTime,
        [this]()
        {
            qDebug() << "Entering setTimeState";

            OpenAPI::OAITime time;
            time.setUtc(QDateTime::currentDateTimeUtc());
            time.setRatePpb(0);
            api.apiTimePut(time);
        },
        &OpenAPI::OAIDefaultApi::apiTimePutSignal, connectionStates.setBusPhy,
        &OpenAPI::OAIDefaultApi::apiTimePutSignalError, connectionStates.error,
        QStringLiteral("Setting remote time failed (channel %1)").arg(deviceChannel), ConfigurationError
        );
}

bool CANsub::configureSetBusPhyState(CANsubConnectionStates const& connectionStates)
{
    /* Create command */
    OpenAPI::OAIPhy command;
    {
        /* Check that we are either running with all parameters as bitrates or bit-timings */
        QVariant const bitRate = configurationParameter(ConfigurationKey::BitRateKey);
        QVariant const bitRateData = configurationParameter(ConfigurationKey::DataBitRateKey);
        QVariant const samplePoint = configurationParameter(CANsubConfigurationKeySamplePoint);
        QVariant const bitTiming = configurationParameter(CANsubConfigurationKeyBitTiming);
        QVariant const bitTimingData = configurationParameter(CANsubConfigurationKeyBitTimingData);

        if ((bitTiming.isValid() && bitTiming.canConvert<CANsubBitTiming>()) &&
            (bitTimingData.isValid() && bitTimingData.canConvert<CANsubBitTiming>()))
        {
            /* When using bit-timing, both regular and data timings must be present */
            {
                auto const bitTimingValue = bitTiming.value<CANsubBitTiming>();
                OpenAPI::OAIBittiming bt;
                bt.setBrp(bitTimingValue.brp);
                bt.setSeg1(bitTimingValue.tseg1);
                bt.setSeg2(bitTimingValue.tseg2);
                bt.setSjw(bitTimingValue.sjw);
                command.setTiming(bt);
            }

            {
                auto const bitTimingDataValue = bitTimingData.value<CANsubBitTiming>();
                OpenAPI::OAIBittiming bt;
                bt.setBrp(bitTimingDataValue.brp);
                bt.setSeg1(bitTimingDataValue.tseg1);
                bt.setSeg2(bitTimingDataValue.tseg2);
                bt.setSjw(bitTimingDataValue.sjw);
                command.setTimingData(bt);
            }
        }
        else if ((bitRate.isValid() && bitRate.canConvert<uint>()) &&
            (samplePoint.isValid() && samplePoint.canConvert<uint>()))
        {
            std::optional<CombinedBitTiming> timingParameters;

            if ((bitRateData.isValid() && bitRateData.canConvert<uint>()))
            {
                timingParameters = getBitTiming(bitRate.toUInt(), bitRateData.toUInt(), samplePoint.toUInt());
            }
            else
            {
                qWarning() <<
                    "Data bitrate not set, but CANsub will always run with FD enabled. Using default data rate of 1000000";
                timingParameters = getBitTiming(bitRate.toUInt(), 1000000, samplePoint.toUInt());
                if (!timingParameters.has_value())
                {
                    QString const errorMsg = QString("Can't get bitrate parameters for %1 - 1000000 at a sample point of %2").
                                         arg(bitRate.toUInt()).arg(samplePoint.toUInt());
                    qCritical() << errorMsg;
                    recordPendingError(errorMsg, QString(), ConfigurationError);
                    return false;
                }
            }

            if (!timingParameters.has_value())
            {
                QString const errorMsg = QString("Can't get bitrate parameters for %1 - %2 at a sample point of %3").
                                         arg(bitRate.toUInt()).arg(bitRateData.toUInt()).arg(samplePoint.toUInt());
                qCritical() << errorMsg;
                recordPendingError(errorMsg, QString(), ConfigurationError);
                return false;
            }

            {
                OpenAPI::OAIBittiming bt;
                bt.setBrp(static_cast<qint32>(timingParameters.value().brp));
                bt.setSeg1(static_cast<qint32>(timingParameters.value().tseg1));
                bt.setSeg2(static_cast<qint32>(timingParameters.value().tseg2));
                bt.setSjw(static_cast<qint32>(timingParameters.value().sjw));
                command.setTiming(bt);
            }

            {
                OpenAPI::OAIBittiming bt;
                bt.setBrp(static_cast<qint32>(timingParameters.value().brp));
                bt.setSeg1(static_cast<qint32>(timingParameters.value().tseg1_data));
                bt.setSeg2(static_cast<qint32>(timingParameters.value().tseg2_data));
                bt.setSjw(static_cast<qint32>(timingParameters.value().sjw_data));
                command.setTimingData(bt);
            }
        }
        else
        {
            QString const errorMsg = "Either bit-timings or bitrates and sample point must be configured";
            qCritical() << errorMsg;
            recordPendingError(errorMsg, QString(), ConfigurationError);
            return false;
        }

        QVariant const listenOnly = configurationParameter(CANsubConfigurationKeyListenOnly);
        if (!listenOnly.isValid())
        {
            command.setListenOnly(CANsubConfigurationKeyListenOnly_default);
        }
        else
        {
            command.setListenOnly(listenOnly.toBool());
        }

        QVariant const autoReset = configurationParameter(CANsubConfigurationKeyAutoReset);
        if (!autoReset.isValid())
        {
            command.setAutoReset(CANsubConfigurationKeyAutoReset_default);
        }
        else
        {
            command.setAutoReset(autoReset.toBool());
        }

        QVariant const errorFramesEnabled = configurationParameter(CANsubConfigurationKeyErrorFramesEnabled);
        if (!errorFramesEnabled.isValid())
        {
            command.setErrorFrames(CANsubConfigurationKeyErrorFramesEnabled_default);
        }
        else
        {
            command.setErrorFrames(errorFramesEnabled.toBool());
        }

        QVariant const receiveOwn = configurationParameter(QCanBusDevice::ConfigurationKey::ReceiveOwnKey);
        if (!receiveOwn.isValid())
        {
            command.setTxAckFrames(CANsubConfigurationKeyReceiveOwn_default);
        }
        else
        {
            command.setTxAckFrames(receiveOwn.toBool());
        }
    }

    wireRequestState(
        connectionStates.setBusPhy,
        [this, command]()
        {
            qDebug() << "Entering setBusPhyState";

            api.apiCanChannelPhyPut(deviceChannel, command);
        },
        &OpenAPI::OAIDefaultApi::apiCanChannelPhyPutSignal, connectionStates.getBusFilters,
        &OpenAPI::OAIDefaultApi::apiCanChannelPhyPutSignalError, connectionStates.error,
        QStringLiteral("Configuring CAN PHY failed (channel %1)").arg(deviceChannel), ConfigurationError
        );

    return true;
}

void CANsub::configureGetBusFiltersState(CANsubConnectionStates const& connectionStates)
{
    /* We need some custom logic to handle the filters. If there are any filters installed, we need to clear them.
     * If the list is already cleared, we can continue. */
    auto* const filtersEmptyTransition = new LogicTransition(connectionStates.setBusFilters);
    connectionStates.getBusFilters->addTransition(filtersEmptyTransition);

    connect(&api, &OpenAPI::OAIDefaultApi::apiCanChannelFilterGetSignal, connectionStateMachine,
            [this, connectionStates, filtersEmptyTransition](QList<qint32> summary)
            {
                if (summary.isEmpty())
                {
                    filtersEmptyTransition->trigger();
                }
                else
                {
                    /* Clear all remote filters */
                    auto* const tracker = new BatchTracker(summary.length(), connectionStateMachine);
                    connect(&api, &OpenAPI::OAIDefaultApi::apiCanChannelFilterFilterIdDeleteSignal, tracker,
                            &BatchTracker::requestFinishedOk);
                    connect(&api, &OpenAPI::OAIDefaultApi::apiCanChannelFilterFilterIdDeleteSignalError, tracker,
                            &BatchTracker::requestFinishedError);

                    wireErrorCapture(&OpenAPI::OAIDefaultApi::apiCanChannelFilterFilterIdDeleteSignalError,
                                     QStringLiteral("Clearing remote filters failed (channel %1)").arg(deviceChannel),
                                     ConfigurationError);

                    connectionStates.getBusFilters->addTransition(tracker, &BatchTracker::finishedOk,
                                                                  connectionStates.setBusFilters);
                    connectionStates.getBusFilters->addTransition(tracker, &BatchTracker::finishedWithErrors,
                                                                  connectionStates.error);

                    for (auto const& filterId : summary)
                    {
                        api.apiCanChannelFilterFilterIdDelete(deviceChannel, filterId);
                    }
                }
            });

    connect(connectionStates.getBusFilters, &QState::entered, this, [this]
    {
        qDebug() << "Entering getBusFilters";

        /* Request all installed filters */
        api.apiCanChannelFilterGet(deviceChannel);
    });

    connectionStates.getBusFilters->addTransition(&api, &OpenAPI::OAIDefaultApi::apiCanChannelFilterGetSignalError,
                                                  connectionStates.error);

    wireErrorCapture(&OpenAPI::OAIDefaultApi::apiCanChannelFilterGetSignalError,
                     QStringLiteral("Getting remote filters failed (channel %1)").arg(deviceChannel), ConfigurationError);
}

void CANsub::configureSetBusFiltersState(CANsubConnectionStates const& connectionStates)
{
    auto* const tracker = new BatchTracker(2, connectionStateMachine);

    /* The completion signals here originate from the BatchTracker, not the api, so use the generic helper. */
    wireGenericState(
        connectionStates.setBusFilters,
        [this, tracker]()
        {
            qDebug() << "Entering setBusFilters";

            connect(&api, &OpenAPI::OAIDefaultApi::apiCanChannelFilterPostSignal, tracker,
                    &BatchTracker::requestFinishedOk);
            connect(&api, &OpenAPI::OAIDefaultApi::apiCanChannelFilterPostSignalError, tracker,
                    &BatchTracker::requestFinishedError);

            wireErrorCapture(&OpenAPI::OAIDefaultApi::apiCanChannelFilterPostSignalError,
                             QStringLiteral("Setting remote filters failed (channel %1)").arg(deviceChannel),
                             ConfigurationError);

            /* Insert default filters */
            {
                OpenAPI::OAIFilter filter;
                filter.setF1(0x000);
                filter.setF2(0x000);
                filter.setIsExtended(false);
                filter.setIsRange(false);
                api.apiCanChannelFilterPost(deviceChannel, filter);
            }
            {
                OpenAPI::OAIFilter filter;
                filter.setF1(0x00000000);
                filter.setF2(0x00000000);
                filter.setIsExtended(true);
                filter.setIsRange(false);
                api.apiCanChannelFilterPost(deviceChannel, filter);
            }
        },
        tracker, &BatchTracker::finishedOk, connectionStates.openWebSocket,
        tracker, &BatchTracker::finishedWithErrors, connectionStates.error
        );
}

void CANsub::configureOpenWebsocketState(CANsubConnectionStates const& connectionStates)
{
    /* Resolve the per-connection forwarding policy. These feed the binaryMessageReceived handler that is wired once
     * in the constructor, so they must be set before the websocket opens. */
    QVariant const forwardFdRaw = configurationParameter(QCanBusDevice::ConfigurationKey::CanFdKey);
    QVariant const blockRaw = configurationParameter(CANsubConfigurationKeyBlockingWrite);
    blockingWrites = (blockRaw.isValid() && blockRaw.canConvert<bool>()) ? blockRaw.value<bool>() : false;

    forwardFd = (forwardFdRaw.isValid()) ? forwardFdRaw.value<bool>() : false;

    connect(connectionStates.openWebSocket, &QState::entered, this, [this]
    {
        qDebug() << "Entering openWebSocketState";

        QUrl remote;
        remote.setScheme("wss");
        remote.setHost(resolvedHost.toString());
        remote.setPort(443);
        remote.setPath(QString("/api/can/%1/ws").arg(deviceChannel));

        /* Start from the shared configuration that trusts only the embedded CANsub root certificate. The hostname
         * is verified against tlsPolicy.verificationHostname if set (a HostNameMismatch is expected, as we connect
         * by IP). */
        QSslConfiguration sslConfiguration = coreSslConfiguration();
        webSocket->setSslConfiguration(sslConfiguration);
        webSocket->setPeerVerifyName(tlsPolicy.verificationHostname);
        webSocket->open(remote);
    });

    /* There is a slight delay between the opening of the websocket and the actual hardware being ready. Introduce a
     * slight delay here to allow for state settling. */
    auto* const settleState = new QState(connectionStateMachine);
    auto* const settleTimer = new QTimer(connectionStateMachine);
    settleTimer->setSingleShot(true);
    settleTimer->setInterval(CANSUB_WS_SETTLE_MS);

    connect(settleState, &QState::entered, this, [settleTimer]
    {
        qDebug() << "Entering settleState";
        settleTimer->start();
    });

    /* Set a timeout on the websocket */
    auto* const connectTimeoutTimer = new QTimer(connectionStateMachine);
    connectTimeoutTimer->setSingleShot(true);
    connectTimeoutTimer->setInterval(CANSUB_WEBSOCKET_TIMEOUT_MS);

    connect(connectionStates.openWebSocket, &QState::entered, this, [connectTimeoutTimer]
    {
        connectTimeoutTimer->start();
    });
    connect(connectionStates.openWebSocket, &QState::exited, connectTimeoutTimer, &QTimer::stop);

    connect(connectTimeoutTimer, &QTimer::timeout, this, [this]
    {
        recordPendingError(QStringLiteral("Timed out opening websocket (channel %1)").arg(deviceChannel), QString(), ConnectionError);
    });

    connectionStates.openWebSocket->addTransition(webSocket, &WebSocket::connected, settleState);
    connectionStates.openWebSocket->addTransition(webSocket, &WebSocket::error, connectionStates.error);
    connectionStates.openWebSocket->addTransition(webSocket, &WebSocket::disconnected, connectionStates.error);
    connectionStates.openWebSocket->addTransition(connectTimeoutTimer, &QTimer::timeout, connectionStates.error);

    /* The settle timer drives the transition into the final connected state. A websocket error during the settle
     * window still routes to the error state. */
    settleState->addTransition(settleTimer, &QTimer::timeout, connectionStates.connected);
    settleState->addTransition(webSocket, &WebSocket::error, connectionStates.error);
    settleState->addTransition(webSocket, &WebSocket::disconnected, connectionStates.error);
}

void CANsub::configureTerminalStates(CANsubConnectionStates const& connectionStates)
{
    auto *connectionStateMachineCpy = connectionStateMachine;
    connect(connectionStates.connected, &QState::entered, this, [this, connectionStateMachineCpy]
    {
        qDebug() << "Entering connectedState";

        if (!connectionStateMachine || (connectionStateMachine != connectionStateMachineCpy))
        {
            /* The attempt was torn down (close()/destruction) before this deferred handler ran */
            return;
        }

        stateMachineDestroy();
        setState(ConnectedState);
    }, Qt::QueuedConnection);

    auto* const errorState = connectionStates.error;
    connect(errorState, &QState::entered, this, [this, connectionStateMachineCpy]
    {
        qDebug() << "Entering errorState";

        if (!connectionStateMachine || (connectionStateMachine != connectionStateMachineCpy))
        {
            /* The attempt was torn down (close()/destruction) before this deferred handler ran */
            return;
        }

        /* Ensure that all actions touching this object are handles before the outside-visible changes */
        QPointer<CANsub> alive(this);
        stateMachineDestroy();
        webSocket->abort();
        if (alive)
        {
            /* A client slot may delete this device in response to either emission (PlotJuggler's DataStreamCAN does exactly
             * that on ConnectionError), so guard the second against the first. */
            setState(UnconnectedState);
            if (alive)
            {
                reportPendingError();
            }
        }
    },  Qt::QueuedConnection);
}
