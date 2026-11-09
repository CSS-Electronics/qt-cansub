#ifndef CANSUB_QT_CANSUB_HXX
#define CANSUB_QT_CANSUB_HXX

#include <optional>
#include <QtGlobal>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QtSerialBus/QCanBusDevice>
#include <QtSerialBus/QCanBusDeviceInfo>
#include <QtSerialBus/QCanBusFrame>

/* Additional configuration parameters. The values are part of the public interface and must never change. */
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

/* Bit-timing configuration for classical/non-BRS section (CANsubQt::BitTiming) */
#define CANsubConfigurationKeyBitTiming             CANsubConfigurationKey(5)

/* Bit-timing configuration for BRS section (CANsubQt::BitTiming) */
#define CANsubConfigurationKeyBitTimingData         CANsubConfigurationKey(6)

/* Should the timebase on the remote CANsub be updated if too out of sync (boolean)? */
#define CANsubConfigurationKeyUpdateTime            CANsubConfigurationKey(7)

/* Should writes to the CANsub remote block if the network is stuck (boolean)? */
#define CANsubConfigurationKeyBlockingWrite         CANsubConfigurationKey(8)

/* Hostname to validate the remote certificate against when connecting via a raw IP address (QString).
 * The CANsub presents a certificate issued for its device ID and mDNS hostnames, so when the user connects by IP there
 * is no name to verify against unless one is supplied, here or as the device ID in the interface name
 * (`<IPv4 address>:<device ID>@<channel>`), which is the default of this key. Accepted are the device ID and the device
 * hostnames (`<device ID>-usb.local` and `<device ID>-eth.local`), as the certificate is verified against the device ID
 * derived from them. An empty string only validates the root certificate. Ignored when connecting by hostname. */
#define CANsubConfigurationKeyExpectedHostname      CANsubConfigurationKey(9)

/* Deprecated. Has no effect. */
#define CANsubConfigurationKeyDisableCertificateValidation CANsubConfigurationKey(10)

struct cansub_frame;
class QHostAddress;
class QTimer;

namespace CANsubQt
{

class Connection;
class HostLookup;

/**
 * Bit timing of one CAN phase, as used by the REST API of the device (80 MHz CAN clock).
 *
 * tseg1 excludes the sync segment: A bit is 1 + tseg1 + tseg2 time quanta, and the sample point is after
 * 1 + tseg1 time quanta. E.g. brp 4, tseg1 63, tseg2 16 is 250 kbit/s with an 80 % sample point.
 */
struct BitTiming
{
    quint16 brp;
    quint16 tseg1;
    quint16 tseg2;
    quint16 sjw;
};

class CANsub : public QCanBusDevice
{
    Q_OBJECT

public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using ConfigurationKeyType = QCanBusDevice::ConfigurationKey;
#else
    using ConfigurationKeyType = int;
#endif

    /**
     * Connection target, parsed from an interface name.
     *
     * The interface name is one of:
     * - `<device ID>-usb.local@<channel>` or `<device ID>-eth.local@<channel>`: The device certificate is verified
     *   against the hostname. The short forms without `.local` are rejected.
     * - `<IPv4 address>:<device ID>@<channel>`: The device certificate is verified against the device ID.
     * - `<IPv4 address>@<channel>`: Only the root certificate is verified.
     *
     * The device ID is 8 hex digits, the IPv4 address is in dotted decimal (e.g. `192.168.1.100`) and the channel is 1
     * to 4. The device is always reached at port 443. For an IPv4 address, CANsubConfigurationKeyExpectedHostname
     * overrides the device ID to verify the certificate against.
     *
     * Examples: `aabbccdd-usb.local@1`, `192.168.1.100:aabbccdd@1`, `192.168.1.100@1`.
     */
    struct Target
    {
        QString interfaceName;
        QString host;
        /* True if host is an IPv4 address, false for a device hostname */
        bool ipv4 = false;
        quint32 channel = 0;
        /* Device ID after the ':' (lower case), or empty */
        QString deviceId;
    };

    /**
     * Parse an interface name, see Target for the format.
     *
     * @return The target, or nothing with errorMessage set (if not NULL) to the reason.
     */
    static std::optional<Target> parseInterfaceName(QString const &interfaceName, QString *errorMessage);

    /**
     * Create a device for a target.
     *
     * Validates the connection options with the CANsub library.
     *
     * @return The device, or nullptr with errorMessage set (if not NULL) if the target cannot be used.
     */
    static CANsub *create(Target const &target, QString *errorMessage);

    /**
     * Device info for one channel of a discovered device.
     *
     * @param hostname mDNS hostname of the device. A trailing "." is removed.
     * @param channel CAN channel, the first is 1.
     * @return Device info with the interface name `<hostname>@<channel>`.
     */
    static QCanBusDeviceInfo createInfo(QString const &hostname, int channel);

    ~CANsub() override;

    void setConfigurationParameter(ConfigurationKeyType key, QVariant const &value) override;
    bool writeFrame(QCanBusFrame const &frame) override;
    QString interpretErrorFrame(QCanBusFrame const &errorFrame) override;

protected:
    bool open() override;
    void close() override;

private:
    explicit CANsub(Target target);

    /**
     * The device ID for a CANsubConfigurationKeyExpectedHostname value.
     *
     * @param expectedHostname A device ID, or a device hostname (`<device ID>-usb.local` or `<device ID>-eth.local`,
     *        optionally with a trailing '.').
     * @return The device ID in lower case, or nothing if the value is neither.
     */
    static std::optional<QString> expectedDeviceId(QString const &expectedHostname);

    /* Settings of one connection, snapshotted by open(). Later changes of their configuration keys take effect on the
     * next open(). */
    struct Session
    {
        /* Device ID to verify the device certificate against, or empty */
        QString deviceId;
        /* Only verify the root certificate */
        bool acceptAnyDevice = false;
        bool canFd = true;
        bool receiveOwn = false;
        bool errorFrames = false;
        bool blockingWrite = false;
        bool updateTime = true;
        std::optional<QCanBusFrame::FrameErrors> errorFilter;
    };

    /* Steps of a connect, in order. enterStep() starts a step, onResponse() evaluates the answer to its request. */
    enum class Step
    {
        /* Not connecting or connected */
        Idle,
        /* Resolving the host with lookup */
        Lookup,
        /* Waiting for closingConnections to finish, as the device rejects the PHY configuration while the WebSocket
         * of an earlier connection is still open */
        WaitForClosing,
        /* Creating the connection and querying the API version */
        Version,
        /* Querying the PTP status (UpdateTime only) */
        PtpStatus,
        /* Reading the device time (UpdateTime only, the device does not track a PTP master) */
        GetTime,
        /* Setting the device time (UpdateTime only, the device time is off) */
        SetTime,
        /* Configuring the PHY */
        SetPhy,
        /* Reading the filter IDs */
        GetFilters,
        /* Removing the filters in filtersToDelete, one at a time, the first one first */
        DeleteFilters,
        /* Adding the accept-all filter for standard IDs */
        AddStandardFilter,
        /* Adding the accept-all filter for extended IDs */
        AddExtendedFilter,
        /* Opening the data stream */
        OpenData,
        /* Waiting for the device to open the CAN controller, see settleTimer */
        Settle,
        /* ConnectedState */
        Connected,
    };

    std::optional<QByteArray> buildPhyConfiguration(QString *errorMessage) const;
    bool boolParameter(ConfigurationKeyType key, bool fallback) const;

    void enterStep(Step next);
    void sendRequest(QByteArray const &method, QByteArray const &path, QByteArray const &body);
    void onResponse(Connection *source, Step requestStep, qint32 result, quint32 httpStatus,
                    QByteArray const &body);
    QString requestDescription() const;
    void failOpen(QString const &message, CanBusError errorCode);
    void retireConnection();

    void onLookupResolved(HostLookup *source, QHostAddress const &address);
    void onLookupFailed(HostLookup *source, QString const &message);
    void onRetiredClosed(Connection *source);
    void onOpened(Connection *source);
    void onSettled();
    void onFramesReceived(Connection *source, QVector<cansub_frame> const &frames);
    void onClosed(Connection *source, qint32 error, QString const &message);

    Target target;

    /* Current step of the connect. */
    Step step = Step::Idle;
    /* PHY configuration (JSON) of the connection being opened. */
    QByteArray phyConfiguration;
    Session session;
    /* Lookup of the connection being opened, or nullptr. Owned. */
    HostLookup *lookup = nullptr;
    /* IPv4 address the host resolved to, for the connection created in the Version step. */
    quint32 resolvedAddress = 0;
    /* Filters still to remove in the DeleteFilters step. The first one is being removed, and stays in the list until
     * the device confirms it. */
    QList<quint32> filtersToDelete;
    /* Current connection, or nullptr. Owned, and deleted once closed. */
    Connection *connection = nullptr;
    /* Earlier connections still closing in the background. Owned, and deleted once closed. */
    QVector<Connection *> closingConnections;
    /* Delays ConnectedState after the data stream opened, see onOpened(). */
    QTimer *settleTimer = nullptr;
    /* A blocking writeFrame() waits in a nested event loop. */
    bool inBlockingWrite = false;
};

} // namespace CANsubQt

Q_DECLARE_METATYPE(CANsubQt::BitTiming)

#endif // CANSUB_QT_CANSUB_HXX
