#ifndef CANSUB_QT_CANSUB_HXX
#define CANSUB_QT_CANSUB_HXX

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtStateMachine/QStateMachine>
#else
#include <QStateMachine>
#endif

#include <optional>
#include <QObject>
#include <QTimer>
#include <QtSerialBus/QCanBusDevice>

#include "QtVersionHelper.hxx"
#include "OAIDefaultApi.h"
#include "FrameBuffer.hxx"
#include "TlsPolicy.hxx"
#include "WebSocket.hxx"

/* Expected version string from the remote API */
#define CANSUB_EXPECTED_VERSION                                 "05.00"
#define CANSUB_EXPECTED_VERSION_JSON                            "\"" CANSUB_EXPECTED_VERSION "\""

/* Forward declaration of the resolver factory interface */
struct ResolverFactory;

struct CANsubBitTiming
{
    quint16 brp;
    quint16 tseg1;
    quint16 tseg2;
    quint16 sjw;
};

Q_DECLARE_METATYPE(CANsubBitTiming);

/* Additional configuration parameters */
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#define CANsubConfigurationKey(x) static_cast<QCanBusDevice::ConfigurationKey>(static_cast<int>(QCanBusDevice::ConfigurationKey::UserKey) + (x))
#else
#define CANsubConfigurationKey(x) (QCanBusDevice::ConfigurationKey::UserKey + (x))
#endif

/* Configuration key for controlling silent operation (boolean) */
#define CANsubConfigurationKeyListenOnly            CANsubConfigurationKey(1)

/* Configuration key for controlling auto recover (boolean) */
#define CANsubConfigurationKeyAutoReset             CANsubConfigurationKey(2)

/* Configuration key for controlling the reception of error frames (boolean) */
#define CANsubConfigurationKeyErrorFramesEnabled    CANsubConfigurationKey(3)

/* Configuration of the target sample point when using bitrates. 80 % is written as 8000, 81.5 % is written as 8150.
 * Sample point is used to look for a proper bit timing configuration. */
#define CANsubConfigurationKeySamplePoint           CANsubConfigurationKey(4)

/* Bit-timing configuration for classical/non-BRS section (CANsubBitTiming) */
#define CANsubConfigurationKeyBitTiming             CANsubConfigurationKey(5)

/* Bit-timing configuration for BRS section (CANsubBitTiming) */
#define CANsubConfigurationKeyBitTimingData         CANsubConfigurationKey(6)

/* Should the timebase on the remote CANsub be updated if too out of sync (boolean)? */
#define CANsubConfigurationKeyUpdateTime            CANsubConfigurationKey(7)

/* Should writes to the CANsub remote block if the network is stuck (boolean)? */
#define CANsubConfigurationKeyBlockingWrite         CANsubConfigurationKey(8)

/* Hostname to validate the remote certificate against when connecting via a raw IP address (QString).
 * The CANsub presents a certificate issued for its device ID and mDNS hostnames, so when the user connects by IP there
 * is no name to verify against unless one is supplied here. Ignored when connecting by hostname. */
#define CANsubConfigurationKeyExpectedHostname      CANsubConfigurationKey(9)

/* Deprecated */
#define CANsubConfigurationKeyDisableCertificateValidation CANsubConfigurationKey(10)

struct CANsubConnectionStates;

struct CANsub : public QCanBusDevice
{
    Q_OBJECT;

public:
    /**
     * Create a new CANsub QCanBusDevice
     *
     * @param address Address for the remote CANsub device to connect to
     * @param channel Channel at the remote to use
     * @param resolverFactory Factory used to create mDNS host resolvers (provided by the plugin)
     * @param parent Qt parent
     */
    explicit CANsub(QString const& address, uint8_t channel, ResolverFactory const* resolverFactory,
                    QObject* parent = nullptr);

    ~CANsub() override;

    static QCanBusDeviceInfo createInfo(QString const& hostname, int channel);

    bool writeFrame(const QCanBusFrame& frame) override;

    QString interpretErrorFrame(const QCanBusFrame& errorFrame) override;

    void setConfigurationParameter(CANBUS_CONFIGURATION_KEY key, const QVariant& value) override;

protected:
    bool open() override;
    void close() override;

private:
    /* Device address information */
    QString const deviceAddress;
    uint8_t const deviceChannel;

    /* Connectivity. The websocket is owned via QObject parenting and is replaced by createWebSocket() on every
     * connection attempt. */
    WebSocket* webSocket = nullptr;
    QTimer webSocketCloseTimeoutTimer;
    QNetworkAccessManager* const networkAccessManager;
    OpenAPI::OAIDefaultApi api;

    /* Storage */
    FrameBuffer bufferRx;
    QVector<QCanBusFrame> receivedFrames;

    ResolverFactory const* const resolverFactory;
    QVariant version;
    QVariant resolvedHost;
    QStateMachine* connectionStateMachine;

    /* True while a blocking writeFrame() is inside its nested wait loop. Used to reject re-entrant blocking
     * writes (a framesReceived emission dispatched from the nested loop can lead a client slot back into
     * writeFrame), which would otherwise invert transmit order, consume the outer call's deadline and allow
     * unbounded recursion. */
    bool blockingWrites = false;
    bool inBlockingWrite = false;

    /* Per-connection receive-forwarding policy. Refreshed from configuration on every open(), read by the
     * binaryMessageReceived handler that is wired once for the device's lifetime. */
    bool forwardFd = false;

    /* TLS identity policy, resolved once per open() from the device address and configuration parameters and shared
     * by both transports: a copy is pushed to the network access manager for the REST API, and the same policy is
     * read directly by the WebSocket sslErrors handler. */
    TlsPolicy tlsPolicy;

    /* Create and wire a fresh transport websocket, disposing of any previous instance. A websocket must never be
     * reused across connection attempts. */
    void createWebSocket();

    /* True when the device address is a raw IP literal rather than an mDNS hostname. */
    [[nodiscard]] bool addressIsIp() const;

    /* Resolve the TLS identity policy (tlsPolicy) and push it to the network access manager. Returns false and sets a
     * ConfigurationError if the combination of address and configuration cannot be validated. */
    bool resolveTlsPolicy();

    /* Error state tracking */
    QString pendingErrorString;
    QCanBusDevice::CanBusError pendingErrorCode = NoError;

    /* Record a connection-phase error for the terminal error state to report. The first error recorded in an attempt
     * wins, so a cascade of follow-up failures cannot clobber the root cause. `detail` is the network error string and
     * may be empty, in which case `context` is used verbatim. */
    void recordPendingError(QString const& context, QString const& detail, QCanBusDevice::CanBusError code);

    /* Flush the recorded pending error (applying sensible fallbacks) to the QCanBusDevice error state. Shared by
     * the terminal error state and the synchronous open() abort path. */
    void reportPendingError();

    /* Abort an in-progress open() call. Tear down the partially built state machine if one was allocated and flush the
     * pending error. */
    void abortOpen();

    void versionCheckSlot(QString const& value);
    void versionCheckErrorSlot(QString const& summary, QNetworkReply::NetworkError error, const QString& errorStr);

    void wsDataReceived(QByteArray const& data);

    bool acceptSslErrors(QList<QSslError> const& errors) const;

    /* The following is all connect state-machine related */
    void stateMachineDestroy();
    void configureLookupState(CANsubConnectionStates const& connectionStates);
    void configureGetVersionState(CANsubConnectionStates const& connectionStates);
    void configureGetTimeState(CANsubConnectionStates const& connectionStates);
    void configureSetTimeState(CANsubConnectionStates const& connectionStates);
    bool configureSetBusPhyState(CANsubConnectionStates const& connectionStates);
    void configureGetBusFiltersState(CANsubConnectionStates const& connectionStates);
    void configureSetBusFiltersState(CANsubConnectionStates const& connectionStates);
    void configureOpenWebsocketState(CANsubConnectionStates const& connectionStates);
    void configureTerminalStates(CANsubConnectionStates const& connectionStates);

    /**
     * Statemachine wiring helper to simplify the connection state machine.
     * Hardwired to use the OpenAPI as event emitter.
     * @tparam OkSignal Type of signal emitted on Ok
     * @tparam ErrSignal Type of signal emitted on Error
     * @param state Starting state
     * @param onEntered Callback to invoke on entering the starting state
     * @param ok Signal emitted on Ok
     * @param onOk Target state on Ok emitted
     * @param err Signal emitted on Error
     * @param onError Target state on Error emitted
     */
    template <typename OkSignal, typename ErrSignal>
    void wireRequestState(QState* state, std::function<void()> onEntered, OkSignal ok, QAbstractState* onOk,
                          ErrSignal err, QAbstractState* onError);

    /**
     * Statemachine wiring helper to simplify the connection statemachine.
     * Hardwired to use the OpenAPI as event emitter.
     * @tparam OkSignal Type of signal emitted on Ok
     * @tparam ErrSignal Type of signal emitted on Error
     * @param state Starting state
     * @param onEntered Callback to invoke on entering the starting state
     * @param ok Signal emitted on Ok
     * @param onOk Target state on Ok emitted
     * @param err Signal emitted on Error
     * @param onError Target state on Error emitted
     * @param errorContext Error context to append to the error state
     * @param errorCode Error code to target
     */
    template <typename OkSignal, typename ErrSignal>
    void wireRequestState(QState* state, std::function<void()> onEntered, OkSignal ok, QAbstractState* onOk,
                          ErrSignal err, QAbstractState* onError,
                          QString errorContext, QCanBusDevice::CanBusError errorCode = ConnectionError);

    /**
     * Generic statemachine wiring helper to simplify the connecting statemachine.
     * @tparam OkSender Ok signal sender class type
     * @tparam OkSignal Type of signal emitted on Ok
     * @tparam ErrSender Error signal sender class type
     * @tparam ErrSignal Type of signal emitted on Error
     * @param state Starting state
     * @param onEntered Callback to invoke on entering the starting state
     * @param okSender Handle to the Ok event sender
     * @param ok Signal emitted on Ok
     * @param onOk Target state on Ok emitted
     * @param errSender Handle to the Error event sender
     * @param err Signal emitted on error
     * @param onError Target state on Error emitted
     */
    template <typename OkSender, typename OkSignal, typename ErrSender, typename ErrSignal>
    void wireGenericState(QState* state, std::function<void()> onEntered, OkSender okSender, OkSignal ok,
                          QAbstractState* onOk, ErrSender errSender, ErrSignal err, QAbstractState* onError);

    /* Fold an OpenAPI error signal's detail string into the pending error message. Two overloads cover the two
     * shapes the generated client emits: (NetworkError, errorStr) for PUT/POST/DELETE and
     * (summary, NetworkError, errorStr) for GETs. The capture is a direct connection on the state machine, so it
     * runs before the machine processes the queued transition into the error state, and dies with the machine. */

    /**
     * Set the pending error state from an OpenAPI error signal
     * @param err OpenAPI error signal
     * @param errorContext Additional error context to set to the pending state
     * @param errorCode Error code to set tot he pending state
     */
    void wireErrorCapture(void (OpenAPI::OAIDefaultApi::*err)(QNetworkReply::NetworkError, QString const&),
                          QString errorContext, QCanBusDevice::CanBusError errorCode);

    /**
     * Set the pending error state from an OpenAPI error signal
     * @tparam Summary OpenAPI summary type for error signal
     * @param err OpenAPI error signal
     * @param errorContext Additional error context to set to the pending state
     * @param errorCode Error code to set to the pending state
     */
    template <typename Summary>
    void wireErrorCapture(void (OpenAPI::OAIDefaultApi::*err)(Summary, QNetworkReply::NetworkError, QString const&),
                          QString errorContext, QCanBusDevice::CanBusError errorCode);

    /**
     * Helper to simplify the storage of configuration settings
     * @tparam T Type of the value to store in this key
     * @param key Configuration key
     * @param value Configuration value
     * @return True if the value is set and of the correct type, or if the value is not set at all (to clear the value)
     */
    template <typename T>
    bool configurationHelperForwardIfValid(CANBUS_CONFIGURATION_KEY key, QVariant const& value);
signals:
    /* Signals used internally during the connection process */
    void versionCheckComplete(QPrivateSignal);
    void versionCheckError(QPrivateSignal);
    void dnsLookupComplete(QPrivateSignal);
    void dnsLookupError(QPrivateSignal);
};

#endif // CANSUB_QT_CANSUB_HXX
