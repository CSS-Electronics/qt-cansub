#include "Logging.hxx"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QPointer>
#include <QTimer>

#include "CANsub.hxx"
#include "CANsubConnection.hxx"
#include "CANsubFrames.hxx"
#include "CANsubRest.hxx"
#include "HostLookup.hxx"

namespace CANsubQt
{

Q_LOGGING_CATEGORY(lcCansub, "cansub")

using Response = Connection::Response;

/* Time until the host lookup gives up. */
static constexpr int lookupTimeoutMs = 5000;

/* Delay between the opened data stream and ConnectedState. The device opens the CAN controller asynchronously after
 * accepting the WebSocket, and frames written before are lost. */
static constexpr int settleTimeMs = 10;

/* Total time a blocking writeFrame() waits for space. */
static constexpr int blockingWriteTimeoutMs = 1000;

/* The device time is set if it differs this much or more from the local time. */
static constexpr qint64 timeToleranceMs = 1000;

/* Sample point used if none is configured, in 0.01 %. */
static constexpr quint32 defaultSamplePoint = 8000;

/* Highest sample point, in 0.01 %. */
static constexpr quint32 maximumSamplePoint = 10000;

/* Lowest data bit rate used if none is configured. The device always runs CAN FD, so a data phase is always needed. */
static constexpr quint32 minimumDefaultDataBitRate = 1000000;

/* The key as an int, so one switch works with the int keys of Qt5 and the enum keys of Qt6, including user keys. */
static constexpr int keyValue(CANsub::ConfigurationKeyType key)
{
    return static_cast<int>(key);
}

/* Name of a configuration key for messages. */
static QString keyName(CANsub::ConfigurationKeyType key)
{
    switch (keyValue(key))
    {
    case keyValue(QCanBusDevice::RawFilterKey):
    {
        return QStringLiteral("RawFilterKey");
    }
    case keyValue(QCanBusDevice::ErrorFilterKey):
    {
        return QStringLiteral("ErrorFilterKey");
    }
    case keyValue(QCanBusDevice::LoopbackKey):
    {
        return QStringLiteral("LoopbackKey");
    }
    case keyValue(QCanBusDevice::ReceiveOwnKey):
    {
        return QStringLiteral("ReceiveOwnKey");
    }
    case keyValue(QCanBusDevice::BitRateKey):
    {
        return QStringLiteral("BitRateKey");
    }
    case keyValue(QCanBusDevice::CanFdKey):
    {
        return QStringLiteral("CanFdKey");
    }
    case keyValue(QCanBusDevice::DataBitRateKey):
    {
        return QStringLiteral("DataBitRateKey");
    }
    case keyValue(CANsubConfigurationKeyListenOnly):
    {
        return QStringLiteral("CANsubConfigurationKeyListenOnly");
    }
    case keyValue(CANsubConfigurationKeyAutoReset):
    {
        return QStringLiteral("CANsubConfigurationKeyAutoReset");
    }
    case keyValue(CANsubConfigurationKeyErrorFramesEnabled):
    {
        return QStringLiteral("CANsubConfigurationKeyErrorFramesEnabled");
    }
    case keyValue(CANsubConfigurationKeySamplePoint):
    {
        return QStringLiteral("CANsubConfigurationKeySamplePoint");
    }
    case keyValue(CANsubConfigurationKeyBitTiming):
    {
        return QStringLiteral("CANsubConfigurationKeyBitTiming");
    }
    case keyValue(CANsubConfigurationKeyBitTimingData):
    {
        return QStringLiteral("CANsubConfigurationKeyBitTimingData");
    }
    case keyValue(CANsubConfigurationKeyUpdateTime):
    {
        return QStringLiteral("CANsubConfigurationKeyUpdateTime");
    }
    case keyValue(CANsubConfigurationKeyBlockingWrite):
    {
        return QStringLiteral("CANsubConfigurationKeyBlockingWrite");
    }
    case keyValue(CANsubConfigurationKeyExpectedHostname):
    {
        return QStringLiteral("CANsubConfigurationKeyExpectedHostname");
    }
    case keyValue(CANsubConfigurationKeyDisableCertificateValidation):
    {
        return QStringLiteral("CANsubConfigurationKeyDisableCertificateValidation");
    }
    default:
    {
        break;
    }
    }

    return QString::number(keyValue(key));
}

/* A configuration value as an unsigned 32-bit number, or nothing if it is not one. */
static std::optional<quint32> toUnsigned(QVariant const &value)
{
    bool ok = false;
    qlonglong const number = value.toLongLong(&ok);
    if (!ok || number < 0 || number > std::numeric_limits<quint32>::max())
    {
        return std::nullopt;
    }

    return static_cast<quint32>(number);
}

/* An ErrorFilterKey value, or nothing if it is not one. Qt documents QCanBusFrame::FrameErrors, and an integer with the
 * same bits is accepted as well. */
static std::optional<QCanBusFrame::FrameErrors> toFrameErrors(QVariant const &value)
{
    if (value.userType() == qMetaTypeId<QCanBusFrame::FrameErrors>())
    {
        return value.value<QCanBusFrame::FrameErrors>();
    }

    std::optional<quint32> const number = toUnsigned(value);
    if (number)
    {
        return QCanBusFrame::FrameErrors(QFlag(static_cast<int>(*number)));
    }

    return std::nullopt;
}

/* Connection options of a target, without the address. Used for the validation in create() and for every connection,
 * so both see the same options. The device is always reached at the default port, without mutual TLS. */
static Connection::Options connectionOptions(CANsub::Target const &target, QString const &deviceId,
                                             bool acceptAnyDevice)
{
    Connection::Options options;
    options.host = target.host.toUtf8();
    options.channel = target.channel;
    options.deviceId = deviceId.toUtf8();
    options.acceptAnyDevice = acceptAnyDevice;
    return options;
}

CANsub *CANsub::create(Target const &target, QString *errorMessage)
{
    /* The options of a connection with the default CANsubConfigurationKeyExpectedHostname, see open() */
    bool const acceptAnyDevice = target.ipv4 && target.deviceId.isEmpty();

    QString error;
    if (!Connection::validate(connectionOptions(target, target.deviceId, acceptAnyDevice), &error))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = tr("Invalid interface name \"%1\": %2").arg(target.interfaceName, error);
        }
        return nullptr;
    }

    return new CANsub(target);
}

QCanBusDeviceInfo CANsub::createInfo(QString const &hostname, int channel)
{
    QString name = hostname;

    /* mDNS names end with a '.', which the user should not see */
    if (name.endsWith(QLatin1Char('.')))
    {
        name.chop(1);
    }

    QString const interfaceName = QStringLiteral("%1@%2").arg(name, QString::number(channel));
    QString const serialNumber = name.left(8);
    QString const description = tr("CANsub device %1, channel %2").arg(serialNumber, QString::number(channel));

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QCanBusDevice::createDeviceInfo(QStringLiteral("CANsub"), interfaceName, serialNumber, description,
                                           QString(), channel, false, true);
#else
    return QCanBusDevice::createDeviceInfo(interfaceName, serialNumber, description, channel, false, true);
#endif
}

CANsub::CANsub(Target target) :
    QCanBusDevice(nullptr),
    target(std::move(target)),
    settleTimer(new QTimer(this))
{
    /* Set default values for all configuration keys. */
    QCanBusDevice::setConfigurationParameter(QCanBusDevice::LoopbackKey, false);
    QCanBusDevice::setConfigurationParameter(QCanBusDevice::CanFdKey, true);
    QCanBusDevice::setConfigurationParameter(QCanBusDevice::ReceiveOwnKey, false);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeySamplePoint, defaultSamplePoint);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyListenOnly, false);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyAutoReset, true);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyErrorFramesEnabled, false);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyUpdateTime, true);
    QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyBlockingWrite, false);

    /* A connection by IPv4 address verifies the certificate against the device ID of the interface name, or only the
     * root certificate without one, until the user sets another expected hostname */
    if (this->target.ipv4)
    {
        QCanBusDevice::setConfigurationParameter(CANsubConfigurationKeyExpectedHostname, this->target.deviceId);
    }

    settleTimer->setSingleShot(true);
    settleTimer->setInterval(settleTimeMs);
    connect(settleTimer, &QTimer::timeout, this, &CANsub::onSettled);
}

CANsub::~CANsub()
{
    /* The lookup handlers forget the lookup before they emit anything, so it is never on the stack here */
    delete lookup;

    /* Destroys the contexts at once, without emitting anything or calling the handlers of outstanding requests.
     * This is allowed from within a slot or handler of the connection, e.g. if a client slot deletes the device from
     * within a signal emitted by the device in onResponse() or onClosed(). */
    qDeleteAll(closingConnections);
    delete connection;
}

void CANsub::setConfigurationParameter(ConfigurationKeyType key, QVariant const &value)
{
    bool valid = false;
    QString expected;

    switch (keyValue(key))
    {
    case keyValue(QCanBusDevice::LoopbackKey):
    {
        if (value.isValid() && value.canConvert<bool>() && !value.toBool())
        {
            return;
        }
        qCWarning(lcCansub) << "LoopbackKey is fixed to false, as the CANsub device has no loopback. Ignoring" << value;
        return;
    }
    case keyValue(QCanBusDevice::RawFilterKey):
    {
        qCWarning(lcCansub) << "RawFilterKey is not supported, as the CANsub plugin does not filter frames. Ignoring"
                            << value;
        return;
    }
    case keyValue(CANsubConfigurationKeyDisableCertificateValidation):
    {
        qCWarning(lcCansub).noquote() << keyName(key) << "is deprecated and has no effect.";
        return;
    }
    case keyValue(CANsubConfigurationKeyExpectedHostname):
    {
        /* The CANsub library verifies the certificate against a device ID, so only names it can be derived from are
         * accepted. An empty name only verifies the root certificate. */
        valid = value.canConvert<QString>()
                && (value.toString().isEmpty() || expectedDeviceId(value.toString()).has_value());
        expected = tr("a device ID (8 hex digits), a device hostname (<device ID>-usb.local or <device ID>-eth.local) "
                      "or an empty string");
        break;
    }
    case keyValue(QCanBusDevice::CanFdKey):
    {
        valid = value.canConvert<bool>();
        expected = tr("a boolean");
        if (valid && !value.toBool())
        {
            qCWarning(lcCansub) << "The CANsub device always runs CAN FD. With CanFdKey false, CAN FD frames are "
                                   "dropped on receive and rejected on transmit.";
        }
        break;
    }
    case keyValue(QCanBusDevice::ReceiveOwnKey):
    case keyValue(CANsubConfigurationKeyListenOnly):
    case keyValue(CANsubConfigurationKeyAutoReset):
    case keyValue(CANsubConfigurationKeyErrorFramesEnabled):
    case keyValue(CANsubConfigurationKeyUpdateTime):
    case keyValue(CANsubConfigurationKeyBlockingWrite):
    {
        valid = value.canConvert<bool>();
        expected = tr("a boolean");
        break;
    }
    case keyValue(QCanBusDevice::BitRateKey):
    case keyValue(QCanBusDevice::DataBitRateKey):
    {
        valid = toUnsigned(value).has_value();
        expected = tr("a bit rate in bit/s (unsigned integer)");
        break;
    }
    case keyValue(CANsubConfigurationKeySamplePoint):
    {
        std::optional<quint32> const samplePoint = toUnsigned(value);
        valid = samplePoint.has_value() && (*samplePoint <= maximumSamplePoint);
        expected = tr("a sample point in 0.01 % from 0 to 10000 (unsigned integer)");
        break;
    }
    case keyValue(CANsubConfigurationKeyBitTiming):
    case keyValue(CANsubConfigurationKeyBitTimingData):
    {
        valid = value.canConvert<BitTiming>();
        expected = tr("a CANsubQt::BitTiming");
        break;
    }
    case keyValue(QCanBusDevice::ErrorFilterKey):
    {
        valid = toFrameErrors(value).has_value();
        expected = tr("QCanBusFrame::FrameErrors");
        break;
    }
    default:
    {
        QCanBusDevice::setConfigurationParameter(key, value);
        return;
    }
    }

    /* An invalid QVariant clears the key, as in QCanBusDevice */
    if (value.isValid() && !valid)
    {
        QString const message = tr("Invalid value \"%1\" of type %2 for the configuration key %3. Expected %4.")
                                .arg(value.toString(), QString::fromLatin1(value.typeName()), keyName(key), expected);
        qCWarning(lcCansub).noquote() << message;
        setError(message, ConfigurationError);
        return;
    }

    QCanBusDevice::setConfigurationParameter(key, value);

    /* The keys handled here are only read by open() */
    if (state() != UnconnectedState)
    {
        qCInfo(lcCansub).noquote() << keyName(key) << "changed while connected. It takes effect on the next connect.";
    }
}

bool CANsub::boolParameter(ConfigurationKeyType key, bool fallback) const
{
    QVariant const value = configurationParameter(key);
    if (!value.isValid())
    {
        return fallback;
    }

    return value.toBool();
}

/* JSON object of one bit timing phase for the PHY request. */
/* A sample point in 0.01 % as a percentage for messages, e.g. "81.5". */
static QString samplePointText(quint32 samplePoint)
{
    return QString::number(samplePoint / 100.0);
}

std::optional<QByteArray> CANsub::buildPhyConfiguration(QString *errorMessage) const
{
    Q_ASSERT(errorMessage != nullptr);

    Rest::PhyConfig phy;

    QVariant const bitTiming = configurationParameter(CANsubConfigurationKeyBitTiming);
    QVariant const bitTimingData = configurationParameter(CANsubConfigurationKeyBitTimingData);
    if (bitTiming.isValid() && bitTimingData.isValid())
    {
        BitTiming const nominal = bitTiming.value<BitTiming>();
        BitTiming const data = bitTimingData.value<BitTiming>();
        phy.timing = {nominal.brp, nominal.tseg1, nominal.tseg2, nominal.sjw};
        phy.timingData = {data.brp, data.tseg1, data.tseg2, data.sjw};
    }
    else
    {
        if (bitTiming.isValid() || bitTimingData.isValid())
        {
            qCWarning(lcCansub) << "Only one of the bit timing keys is set, so the bit timing is calculated from the "
                                   "bit rates. Set both CANsubConfigurationKeyBitTiming and "
                                   "CANsubConfigurationKeyBitTimingData to use them.";
        }

        std::optional<quint32> const nominalBitRate = toUnsigned(configurationParameter(QCanBusDevice::BitRateKey));
        if (!nominalBitRate)
        {
            *errorMessage = tr("No bit rate is configured for %1. Set the BitRateKey (and optionally the "
                               "DataBitRateKey), or both CANsub bit timing keys.").arg(target.interfaceName);
            return std::nullopt;
        }

        std::optional<quint32> const configuredDataBitRate =
            toUnsigned(configurationParameter(QCanBusDevice::DataBitRateKey));
        quint32 const dataBitRate = configuredDataBitRate.has_value()
                                        ? *configuredDataBitRate
                                        : std::max(minimumDefaultDataBitRate, *nominalBitRate);

        std::optional<quint32> const configuredSamplePoint =
            toUnsigned(configurationParameter(CANsubConfigurationKeySamplePoint));
        quint32 const samplePoint = configuredSamplePoint.value_or(defaultSamplePoint);

        cansub_bittiming calculated{};
        cansub_result const result = cansub_calculate_bittiming(&calculated, *nominalBitRate, samplePoint, dataBitRate,
                                                                samplePoint);
        if (result == CANSUB_RESULT_NOT_FOUND)
        {
            *errorMessage = tr("No bit timing of the CANsub device matches a nominal bit rate of %1 bit/s and a data "
                               "bit rate of %2 bit/s with a sample point of %3 %.")
                            .arg(QString::number(*nominalBitRate), QString::number(dataBitRate),
                                 samplePointText(samplePoint));
            return std::nullopt;
        }
        if (result != CANSUB_RESULT_OK)
        {
            *errorMessage = tr("The nominal bit rate of %1 bit/s, the data bit rate of %2 bit/s or the sample point of "
                               "%3 % is invalid. The bit rates must be above 0, the data bit rate must be at least the "
                               "nominal bit rate and the sample point must be at most 100 %.")
                            .arg(QString::number(*nominalBitRate), QString::number(dataBitRate),
                                 samplePointText(samplePoint));
            return std::nullopt;
        }

        phy.timing = {calculated.nominal.brp, calculated.nominal.seg1, calculated.nominal.seg2, calculated.nominal.sjw};
        phy.timingData = {calculated.data.brp, calculated.data.seg1, calculated.data.seg2, calculated.data.sjw};
    }

    phy.listenOnly = boolParameter(CANsubConfigurationKeyListenOnly, false);
    phy.autoReset = boolParameter(CANsubConfigurationKeyAutoReset, true);
    phy.errorFrames = boolParameter(CANsubConfigurationKeyErrorFramesEnabled, false);
    phy.txAckFrames = boolParameter(QCanBusDevice::ReceiveOwnKey, false);

    return Rest::phyBody(phy);
}

bool CANsub::open()
{
    /* close() and every failure end the connect before the device becomes unconnected */
    Q_ASSERT(step == Step::Idle);
    Q_ASSERT(lookup == nullptr);
    Q_ASSERT(connection == nullptr);

    clearError();

    QString errorMessage;
    std::optional<QByteArray> const phy = buildPhyConfiguration(&errorMessage);
    if (!phy)
    {
        qCWarning(lcCansub).noquote() << errorMessage;
        setError(errorMessage, ConfigurationError);
        return false;
    }
    phyConfiguration = *phy;

    session = Session();
    if (target.ipv4)
    {
        /* setConfigurationParameter() only accepts values expectedDeviceId() can map, or an empty one */
        QVariant const expectedHostname = configurationParameter(CANsubConfigurationKeyExpectedHostname);
        session.deviceId = expectedDeviceId(expectedHostname.toString()).value_or(QString());
        session.acceptAnyDevice = session.deviceId.isEmpty();
    }
    session.canFd = boolParameter(QCanBusDevice::CanFdKey, true);
    session.receiveOwn = boolParameter(QCanBusDevice::ReceiveOwnKey, false);
    session.errorFrames = boolParameter(CANsubConfigurationKeyErrorFramesEnabled, false);
    session.blockingWrite = boolParameter(CANsubConfigurationKeyBlockingWrite, false);
    session.updateTime = boolParameter(CANsubConfigurationKeyUpdateTime, true);
    session.errorFilter = toFrameErrors(configurationParameter(QCanBusDevice::ErrorFilterKey));

    qCInfo(lcCansub).noquote() << "Connecting to" << target.interfaceName;

    enterStep(Step::Lookup);
    return true;
}

void CANsub::enterStep(Step next)
{
    step = next;

    /* NOTE: Signal results are forwarded to functions instead of being inlined as lambdas to improve readability. */

    switch (next)
    {
    case Step::Idle:
    {
        break;
    }
    case Step::Lookup:
    {
        HostLookup *const started = new HostLookup(target.host);
        lookup = started;
        connect(started, &HostLookup::resolved, this, [this, started](QHostAddress const &address)
        {
            onLookupResolved(started, address);
        });
        connect(started, &HostLookup::failed, this, [this, started](QString const &message)
        {
            onLookupFailed(started, message);
        });
        started->start(lookupTimeoutMs);
        break;
    }
    case Step::WaitForClosing:
    {
        /* onRetiredClosed() continues with the Version step */
        qCDebug(lcCansub) << "Waiting for" << closingConnections.size() << "earlier connections to close";
        break;
    }
    case Step::Version:
    {
        Q_ASSERT(connection == nullptr);
        Q_ASSERT(closingConnections.isEmpty());

        Connection::Options options = connectionOptions(target, session.deviceId, session.acceptAnyDevice);
        options.address = QHostAddress(resolvedAddress);

        QString errorMessage;
        Connection *const created = Connection::create(options, nullptr, &errorMessage);
        if (created == nullptr)
        {
            failOpen(tr("Connecting to %1 failed: %2").arg(target.interfaceName, errorMessage), ConnectionError);
            break;
        }

        connection = created;
        connect(created, &Connection::opened, this, [this, created]()
        {
            onOpened(created);
        });
        connect(created, &Connection::framesReceived, this, [this, created](QVector<cansub_frame> const &frames)
        {
            onFramesReceived(created, frames);
        });
        connect(created, &Connection::closed, this, [this, created](cansub_result error, QString const &message)
        {
            onClosed(created, error, message);
        });

        /* The API version is checked before any other request, see the API conventions in cansub_ffi.h */
        sendRequest("GET", Rest::versionPath, QByteArray());
        break;
    }
    case Step::PtpStatus:
    {
        sendRequest("GET", Rest::ptpPath, QByteArray());
        break;
    }
    case Step::GetTime:
    {
        sendRequest("GET", Rest::timePath, QByteArray());
        break;
    }
    case Step::SetTime:
    {
        sendRequest("PUT", Rest::timePath, Rest::timeBody(QDateTime::currentDateTimeUtc()));
        break;
    }
    case Step::SetPhy:
    {
        sendRequest("PUT", Rest::phyPath(target.channel), phyConfiguration);
        break;
    }
    case Step::GetFilters:
    {
        sendRequest("GET", Rest::filterPath(target.channel), QByteArray());
        break;
    }
    case Step::DeleteFilters:
    {
        if (filtersToDelete.isEmpty())
        {
            enterStep(Step::AddStandardFilter);
            break;
        }

        sendRequest("DELETE", Rest::filterPath(target.channel, filtersToDelete.first()), QByteArray());
        break;
    }
    case Step::AddStandardFilter:
    {
        sendRequest("POST", Rest::filterPath(target.channel), Rest::acceptAllFilterBody(false));
        break;
    }
    case Step::AddExtendedFilter:
    {
        sendRequest("POST", Rest::filterPath(target.channel), Rest::acceptAllFilterBody(true));
        break;
    }
    case Step::OpenData:
    {
        /* onOpened() continues with the Settle step */
        if (!connection->open())
        {
            failOpen(tr("Opening the data stream of CAN channel %1 of %2 failed.")
                     .arg(QString::number(target.channel), target.host), ConnectionError);
        }
        break;
    }
    case Step::Settle:
    {
        /* onSettled() called from the settleTimer continues with the Connected step */
        qCDebug(lcCansub) << "Data stream open, settling";
        settleTimer->start();
        break;
    }
    case Step::Connected:
    {
        qCInfo(lcCansub).noquote() << "Connected to" << target.interfaceName;
        setState(ConnectedState);
        break;
    }
    }
}

void CANsub::sendRequest(QByteArray const &method, QByteArray const &path, QByteArray const &body)
{
    Q_ASSERT(connection != nullptr);

    qCDebug(lcCansub) << method << path << body;

    /* The answer belongs to this step and this connection, see onResponse() */
    Connection *const source = connection;
    Step const requestStep = step;
    connection->request(method, path, body, [this, source, requestStep](Response const &response)
    {
        onResponse(source, requestStep, response.result, response.httpStatus, response.body);
    });
}

/* Message for a failed request: The step, the channel and either the transport error or the HTTP status and body. */
static QString requestFailureMessage(QString const &step, quint32 channel, qint32 result, quint32 httpStatus,
                                     QByteArray const &body)
{
    if (result < 0)
    {
        return CANsub::tr("%1 failed for CAN channel %2: %3")
               .arg(step, QString::number(channel), resultDescription(result));
    }

    QString const text = QString::fromUtf8(body).trimmed();
    if (text.isEmpty())
    {
        return CANsub::tr("%1 failed for CAN channel %2 with HTTP status %3.")
               .arg(step, QString::number(channel), QString::number(httpStatus));
    }

    return CANsub::tr("%1 failed for CAN channel %2 with HTTP status %3: %4")
           .arg(step, QString::number(channel), QString::number(httpStatus), text);
}

QString CANsub::requestDescription() const
{
    switch (step)
    {
    case Step::Version:
    {
        return tr("Version query");
    }
    case Step::PtpStatus:
    {
        return tr("Reading the PTP status of the device");
    }
    case Step::GetTime:
    {
        return tr("Reading the time of the device");
    }
    case Step::SetTime:
    {
        return tr("Setting the time of the device");
    }
    case Step::SetPhy:
    {
        return tr("Configuring the CAN bus");
    }
    case Step::GetFilters:
    {
        return tr("Reading the CAN filters");
    }
    case Step::DeleteFilters:
    {
        return tr("Removing CAN filter %1").arg(filtersToDelete.first());
    }
    case Step::AddStandardFilter:
    {
        return tr("Adding the accept-all filter for standard IDs");
    }
    case Step::AddExtendedFilter:
    {
        return tr("Adding the accept-all filter for extended IDs");
    }
    case Step::Idle:
    case Step::Lookup:
    case Step::WaitForClosing:
    case Step::OpenData:
    case Step::Settle:
    case Step::Connected:
    {
        break;
    }
    }

    /* No request is sent in the other steps */
    Q_ASSERT(false);
    return tr("A request");
}

void CANsub::onResponse(Connection *source, Step requestStep, qint32 result, quint32 httpStatus,
                        QByteArray const &body)
{
    /* Answers for a retired connection or an earlier connect are dropped. Only one request is outstanding at a time. */
    if ((source != connection) || (requestStep != step))
    {
        return;
    }

    qCDebug(lcCansub) << "Response" << result << httpStatus << body;

    /* A transport failure ends every step the same way */
    if (result < 0)
    {
        failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                 ConnectionError);
        return;
    }

    switch (step)
    {
    case Step::Version:
    {
        if (httpStatus != 200)
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConnectionError);
            return;
        }

        quint32 remote = 0;
        cansub_result const check = cansub_api_version_check(body.constData(), static_cast<size_t>(body.size()),
                                                              &remote);
        if (check == CANSUB_RESULT_NOT_SUPPORTED)
        {
            QString const version = QStringLiteral("%1.%2").arg(remote >> 16, 2, 10, QLatin1Char('0'))
                                    .arg(remote & 0xFFFFu, 2, 10, QLatin1Char('0'));
            failOpen(tr("The device %1 uses the API version %2, which this plugin does not support")
                .arg(target.host, version), ConnectionError);
            return;
        }
        if (check != CANSUB_RESULT_OK)
        {
            failOpen(tr("The device %1 answered the API version request with the unexpected version \"%2\".")
                     .arg(target.host, QString::fromUtf8(body).trimmed()), ConnectionError);
            return;
        }

        if (session.updateTime)
        {
            enterStep(Step::PtpStatus);
        }
        else
        {
            enterStep(Step::SetPhy);
        }
        return;
    }
    case Step::PtpStatus:
    {
        /* Setting the time disables PTP on the device for the rest of its power cycle, so the time of a device that
         * tracks a PTP master is left alone. Any other answer falls back to the time comparison. */
        std::optional<QString> const ptpState = (httpStatus == 200) ? Rest::parsePtpState(body) : std::nullopt;
        if (ptpState == QLatin1String("tracking"))
        {
            qCDebug(lcCansub) << "Not setting the device time, as it tracks a PTP master";
            enterStep(Step::SetPhy);
        }
        else
        {
            qCDebug(lcCansub).noquote() << "PTP status" << httpStatus << ptpState.value_or(QStringLiteral("unknown"))
                                        << "- comparing the device time";
            enterStep(Step::GetTime);
        }
        return;
    }
    case Step::GetTime:
    {
        if (httpStatus != 200)
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConnectionError);
            return;
        }

        /* An unreadable device time is set like a wrong one */
        std::optional<QDateTime> const deviceTime = Rest::parseTime(body);
        QDateTime const now = QDateTime::currentDateTimeUtc();
        if (deviceTime && (qAbs(deviceTime->msecsTo(now)) < timeToleranceMs))
        {
            enterStep(Step::SetPhy);
        }
        else
        {
            qCInfo(lcCansub).noquote() << "Setting the device time, as it is"
                                       << (deviceTime ? deviceTime->toString(Qt::ISODateWithMs)
                                                      : QStringLiteral("unreadable"))
                                       << "and the local time is" << now.toString(Qt::ISODateWithMs);
            enterStep(Step::SetTime);
        }
        return;
    }
    case Step::SetTime:
    {
        if (httpStatus != 200)
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConnectionError);
            return;
        }

        enterStep(Step::SetPhy);
        return;
    }
    case Step::SetPhy:
    {
        if (httpStatus == 200)
        {
            enterStep(Step::GetFilters);
            return;
        }

        QString const failure = requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body);
        if (httpStatus == 500)
        {
            failOpen(tr("CAN channel %1 of %2 is in use by another client (the device rejected the PHY "
                        "configuration while the channel is open). %3")
                     .arg(QString::number(target.channel), target.host, failure), ConfigurationError);
        }
        else if (httpStatus == 404)
        {
            failOpen(tr("The device %1 has no CAN channel %2. %3")
                     .arg(target.host, QString::number(target.channel), failure), ConfigurationError);
        }
        else
        {
            failOpen(failure, ConfigurationError);
        }
        return;
    }
    case Step::GetFilters:
    {
        if (httpStatus != 200)
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConfigurationError);
            return;
        }

        std::optional<QList<quint32>> const filterIds = Rest::parseFilterIds(body);
        if (!filterIds)
        {
            failOpen(tr("The device %1 answered the filter request of CAN channel %2 with an unexpected response.")
                     .arg(target.host, QString::number(target.channel)), ConfigurationError);
            return;
        }

        /* Whatever another client left behind is replaced by the two accept-all filters, as the plugin does not
         * filter frames */
        filtersToDelete = *filterIds;
        enterStep(Step::DeleteFilters);
        return;
    }
    case Step::DeleteFilters:
    {
        /* A filter removed in the meantime is fine */
        if ((httpStatus != 200) && (httpStatus != 404))
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConfigurationError);
            return;
        }

        filtersToDelete.removeFirst();
        enterStep(Step::DeleteFilters);
        return;
    }
    case Step::AddStandardFilter:
    {
        if (httpStatus != 201)
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConfigurationError);
            return;
        }

        enterStep(Step::AddExtendedFilter);
        return;
    }
    case Step::AddExtendedFilter:
    {
        if (httpStatus != 201)
        {
            failOpen(requestFailureMessage(requestDescription(), target.channel, result, httpStatus, body),
                     ConfigurationError);
            return;
        }

        enterStep(Step::OpenData);
        return;
    }
    case Step::Idle:
    case Step::Lookup:
    case Step::WaitForClosing:
    case Step::OpenData:
    case Step::Settle:
    case Step::Connected:
    {
        /* No request is sent in these steps */
        Q_ASSERT(false);
        return;
    }
    }
}

void CANsub::onLookupResolved(HostLookup *source, QHostAddress const &address)
{
    if ((source != lookup) || (step != Step::Lookup))
    {
        return;
    }
    lookup = nullptr;
    source->deleteLater();

    qCDebug(lcCansub).noquote() << target.host << "resolved to" << address.toString();
    resolvedAddress = address.toIPv4Address();

    if (closingConnections.isEmpty())
    {
        enterStep(Step::Version);
    }
    else
    {
        enterStep(Step::WaitForClosing);
    }
}

void CANsub::onLookupFailed(HostLookup *source, QString const &message)
{
    if ((source != lookup) || (step != Step::Lookup))
    {
        return;
    }
    lookup = nullptr;
    source->deleteLater();

    failOpen(message, ConnectionError);
}

void CANsub::failOpen(QString const &message, CanBusError errorCode)
{
    /* The lookup is only set in the Lookup step, and its handlers forget it before they fail */
    Q_ASSERT(lookup == nullptr);

    qCWarning(lcCansub).noquote() << message;

    /* End the connect first, as the client may react to the error, e.g. with a new connect. A connection finishes its
     * close in the background. */
    step = Step::Idle;
    filtersToDelete.clear();
    if (connection != nullptr)
    {
        retireConnection();
    }

    QPointer<CANsub> alive(this);
    setError(message, errorCode);
    if (alive)
    {
        setState(UnconnectedState);
    }
}

void CANsub::retireConnection()
{
    Q_ASSERT(connection != nullptr);

    Connection *const retired = connection;
    connection = nullptr;
    settleTimer->stop();

    /* Only the end of the close matters now. It has not been emitted yet, as the connection is still current. */
    disconnect(retired, nullptr, this, nullptr);
    closingConnections.append(retired);
    connect(retired, &Connection::closed, this, [this, retired]()
    {
        onRetiredClosed(retired);
    });

    retired->close();
}

void CANsub::onRetiredClosed(Connection *source)
{
    if (!closingConnections.removeOne(source))
    {
        return;
    }

    /* Allowed from within its own signal, see Connection */
    delete source;

    qCDebug(lcCansub) << "Earlier connection closed," << closingConnections.size() << "still closing";

    /* A connect waiting for the earlier connections to close continues now */
    if (closingConnections.isEmpty() && (step == Step::WaitForClosing))
    {
        enterStep(Step::Version);
    }
}

void CANsub::onOpened(Connection *source)
{
    if ((source != connection) || (step != Step::OpenData))
    {
        return;
    }

    enterStep(Step::Settle);
}

void CANsub::onSettled()
{
    if (step != Step::Settle)
    {
        return;
    }

    enterStep(Step::Connected);
}

void CANsub::onFramesReceived(Connection *source, QVector<cansub_frame> const &frames)
{
    /* Frames of the settle window are dropped */
    if ((source != connection) || (step != Step::Connected))
    {
        return;
    }

    QVector<QCanBusFrame> received;
    received.reserve(frames.size());

    for (cansub_frame const &in : frames)
    {
        std::optional<QCanBusFrame> const frame = fromCansubFrame(in);
        if (!frame)
        {
            continue;
        }

        /* Transmit acknowledgements */
        if (frame->hasLocalEcho() && !session.receiveOwn)
        {
            continue;
        }

        if (frame->frameType() == QCanBusFrame::ErrorFrame)
        {
            /* The device only sends error frames if enabled. Never pass one on otherwise. */
            if (!session.errorFrames)
            {
                continue;
            }
            if (session.errorFilter && !(frame->error() & *session.errorFilter))
            {
                continue;
            }
        }
        else if (frame->hasFlexibleDataRateFormat() && !session.canFd)
        {
            continue;
        }

        received.append(*frame);
    }

    if (!received.isEmpty())
    {
        enqueueReceivedFrames(received);
    }
}

void CANsub::onClosed(Connection *source, qint32 error, QString const &message)
{
    if (source != connection)
    {
        return;
    }

    /* The current connection only closes by itself, as close() and failures retire it first. Forget it before
     * emitting anything. Deleting it is allowed from within its own signal, see Connection. */
    bool const wasConnected = (step == Step::Connected);
    step = Step::Idle;
    filtersToDelete.clear();
    settleTimer->stop();
    connection = nullptr;
    delete source;

    /* The message of the connection already holds the translated reason */
    QString text = message;
    if (text.isEmpty() && wasConnected)
    {
        text = tr("The device closed the connection to %1.").arg(target.interfaceName);
    }
    else if (text.isEmpty())
    {
        text = tr("Connecting to %1 failed: The device closed the connection.").arg(target.interfaceName);
    }

    qCWarning(lcCansub).noquote() << text << "(result" << error << ")";

    QPointer<CANsub> alive(this);
    setError(text, ConnectionError);
    if (alive)
    {
        setState(UnconnectedState);
    }
}

void CANsub::close()
{
    /* Qt expects UnconnectedState at once. A connection finishes its close in the background, and a following open()
     * waits for it in the WaitForClosing step. */
    step = Step::Idle;
    filtersToDelete.clear();

    if (lookup != nullptr)
    {
        lookup->abort();
        delete lookup;
        lookup = nullptr;
    }

    if (connection != nullptr)
    {
        retireConnection();
    }

    qCInfo(lcCansub).noquote() << "Disconnected from" << target.interfaceName;
    setState(UnconnectedState);
}

bool CANsub::writeFrame(QCanBusFrame const &frame)
{
    if (state() != ConnectedState)
    {
        setError(tr("Cannot write a frame to %1: The device is not connected.").arg(target.interfaceName), WriteError);
        return false;
    }
    Q_ASSERT(connection != nullptr);

    if (frame.hasFlexibleDataRateFormat() && !session.canFd)
    {
        setError(tr("Cannot write the frame: CAN FD frames are disabled with the CanFdKey."), WriteError);
        return false;
    }

    QString reason;
    std::optional<cansub_frame> const out = toCansubFrame(frame, reason);
    if (!out)
    {
        setError(tr("Cannot write the frame: %1").arg(reason), WriteError);
        return false;
    }

    cansub_result result = connection->sendFrame(*out);

    /* A write from a slot run by the nested event loop does not block again: It would invert the transmit order and
     * nest the waits without bound. */
    if ((result == CANSUB_RESULT_AGAIN) && session.blockingWrite && !inBlockingWrite)
    {
        /* WARNING: Runs a nested event loop, which may dispatch any event, including the deletion of this device */
        QPointer<CANsub> alive(this);
        Connection *const current = connection;

        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(current, &Connection::readyToSend, &loop, &QEventLoop::quit);
        connect(current, &Connection::closed, &loop, &QEventLoop::quit);
        connect(this, &QCanBusDevice::stateChanged, &loop, &QEventLoop::quit);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(blockingWriteTimeoutMs);

        inBlockingWrite = true;
        while ((result == CANSUB_RESULT_AGAIN) && deadline.isActive())
        {
            loop.exec();

            if (alive.isNull())
            {
                return false;
            }
            if ((current != connection) || (state() != ConnectedState))
            {
                inBlockingWrite = false;
                setError(tr("Cannot write the frame: The connection closed while waiting for space in the transmit "
                            "buffer."), WriteError);
                return false;
            }

            result = connection->sendFrame(*out);
        }
        inBlockingWrite = false;
    }

    switch (result)
    {
    case CANSUB_RESULT_OK:
    {
        /* Do not leave an earlier write error behind */
        if (error() == WriteError)
        {
            clearError();
        }

        emit framesWritten(1);
        return true;
    }
    case CANSUB_RESULT_AGAIN:
    {
        setError(tr("Cannot write the frame: The transmit buffer is full."), WriteError);
        return false;
    }
    case CANSUB_RESULT_CLOSED:
    {
        setError(tr("Cannot write the frame: The connection is closing."), WriteError);
        return false;
    }
    default:
    {
        setError(tr("Cannot write the frame: %1").arg(resultDescription(result)), WriteError);
        return false;
    }
    }
}

QString CANsub::interpretErrorFrame(QCanBusFrame const &errorFrame)
{
    return describeErrorFrame(errorFrame);
}

} // namespace CANsubQt
