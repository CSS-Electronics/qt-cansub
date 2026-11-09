#include "CANsubRest.hxx"

#include <cmath>
#include <limits>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>

namespace CANsubQt::Rest
{

/* Path of a channel endpoint, e.g. "/api/can/1/phy". */
static QByteArray channelPath(quint32 channel, char const *endpoint)
{
    return QByteArrayLiteral("/api/can/") + QByteArray::number(channel) + '/' + endpoint;
}

QByteArray phyPath(quint32 channel)
{
    return channelPath(channel, "phy");
}

QByteArray filterPath(quint32 channel)
{
    return channelPath(channel, "filter");
}

QByteArray filterPath(quint32 channel, quint32 filterId)
{
    return channelPath(channel, "filter/") + QByteArray::number(filterId);
}

static QJsonObject timingObject(Timing const &timing)
{
    QJsonObject object;
    object.insert(QStringLiteral("brp"), static_cast<qint64>(timing.brp));
    object.insert(QStringLiteral("seg1"), static_cast<qint64>(timing.seg1));
    object.insert(QStringLiteral("seg2"), static_cast<qint64>(timing.seg2));
    object.insert(QStringLiteral("sjw"), static_cast<qint64>(timing.sjw));
    return object;
}

QByteArray phyBody(PhyConfig const &config)
{
    QJsonObject phy;
    phy.insert(QStringLiteral("listen_only"), config.listenOnly);
    phy.insert(QStringLiteral("auto_reset"), config.autoReset);
    phy.insert(QStringLiteral("error_frames"), config.errorFrames);
    phy.insert(QStringLiteral("tx_ack_frames"), config.txAckFrames);
    phy.insert(QStringLiteral("timing"), timingObject(config.timing));
    phy.insert(QStringLiteral("timing_data"), timingObject(config.timingData));
    return QJsonDocument(phy).toJson(QJsonDocument::Compact);
}

QByteArray timeBody(QDateTime const &time)
{
    /* The C locale keeps the digits ASCII */
    QJsonObject body;
    body.insert(QStringLiteral("utc"),
                QLocale::c().toString(time.toUTC(), QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")));
    body.insert(QStringLiteral("rate_ppb"), 0);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray acceptAllFilterBody(bool extended)
{
    QJsonObject filter;
    filter.insert(QStringLiteral("is_extended"), extended);
    filter.insert(QStringLiteral("is_range"), false);
    filter.insert(QStringLiteral("f1"), 0);
    filter.insert(QStringLiteral("f2"), 0);
    return QJsonDocument(filter).toJson(QJsonDocument::Compact);
}

std::optional<QDateTime> parseTime(QByteArray const &body)
{
    QJsonValue const utc = QJsonDocument::fromJson(body).object().value(QStringLiteral("utc"));
    if (!utc.isString())
    {
        return std::nullopt;
    }

    QDateTime time = QDateTime::fromString(utc.toString(), Qt::ISODateWithMs);
    if (!time.isValid())
    {
        return std::nullopt;
    }

    if (time.timeSpec() == Qt::LocalTime)
    {
        /* Without a zone designator, the digits are still UTC */
        time.setTimeSpec(Qt::UTC);
    }
    return time.toUTC();
}

std::optional<QString> parsePtpState(QByteArray const &body)
{
    QJsonValue const state = QJsonDocument::fromJson(body).object().value(QStringLiteral("state"));
    if (!state.isString())
    {
        return std::nullopt;
    }
    return state.toString();
}

std::optional<QList<quint32>> parseFilterIds(QByteArray const &body)
{
    QJsonDocument const document = QJsonDocument::fromJson(body);
    if (!document.isArray())
    {
        return std::nullopt;
    }

    QJsonArray const array = document.array();
    QList<quint32> ids;
    for (QJsonValue const value : array)
    {
        if (!value.isDouble())
        {
            return std::nullopt;
        }

        double const id = value.toDouble();
        if ((id < 0.0) || (id > std::numeric_limits<quint32>::max()) || (id != std::floor(id)))
        {
            return std::nullopt;
        }
        ids.append(static_cast<quint32>(id));
    }
    return ids;
}

} // namespace CANsubQt::Rest
