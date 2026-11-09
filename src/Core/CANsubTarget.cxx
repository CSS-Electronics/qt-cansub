/* Parsing of interface names, see CANsub::Target for the format */

#include <optional>
#include <QString>
#include <QStringList>

#include "CANsub.hxx"

namespace CANsubQt
{

/* True for exactly 8 hex digits. */
static bool isDeviceId(QString const &text)
{
    if (text.size() != 8)
    {
        return false;
    }

    for (QChar const character : text)
    {
        ushort const c = character.unicode();
        bool const isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!isHex)
        {
            return false;
        }
    }

    return true;
}

/* True for a non-empty string of the digits 0-9 only. */
static bool isDecimal(QString const &text)
{
    if (text.isEmpty())
    {
        return false;
    }

    for (QChar const character : text)
    {
        ushort const c = character.unicode();
        if (c < '0' || c > '9')
        {
            return false;
        }
    }

    return true;
}

/* True for an IPv4 address in dotted decimal with four parts, e.g. "192.168.1.100". QHostAddress also accepts short
 * forms such as "1", which are not IPv4 addresses here. */
static bool isDottedIpv4(QString const &host)
{
    QStringList const parts = host.split(QLatin1Char('.'));
    if (parts.size() != 4)
    {
        return false;
    }

    for (QString const &part : parts)
    {
        if (!isDecimal(part) || (part.size() > 3) || (part.toUInt() > 255))
        {
            return false;
        }
    }

    return true;
}

/* True for "<device ID>-usb.local" or "<device ID>-eth.local", the hostnames of a device. The suffix must be lower
 * case. */
static bool isDeviceHostname(QString const &host)
{
    QString const suffix = host.mid(8);
    return isDeviceId(host.left(8))
           && ((suffix == QLatin1String("-usb.local")) || (suffix == QLatin1String("-eth.local")));
}

/* True for "<device ID>-usb" or "<device ID>-eth", the hostnames of a device without ".local". */
static bool isShortDeviceHostname(QString const &host)
{
    QString const suffix = host.mid(8);
    return isDeviceId(host.left(8))
           && ((suffix.compare(QLatin1String("-usb"), Qt::CaseInsensitive) == 0)
               || (suffix.compare(QLatin1String("-eth"), Qt::CaseInsensitive) == 0));
}

/* Parse an interface name. Always sets reason on failure, without naming the interface name. */
static std::optional<CANsub::Target> parseTarget(QString const &interfaceName, QString &reason)
{
    CANsub::Target target;
    target.interfaceName = interfaceName;

    /* The channel follows the '@' */
    qsizetype const at = interfaceName.lastIndexOf(QLatin1Char('@'));
    if (at < 0)
    {
        reason = CANsub::tr("The channel is missing. Use the format <host>@<channel>, e.g. aabbccdd-usb.local@1.");
        return std::nullopt;
    }
    if (interfaceName.indexOf(QLatin1Char('@')) != at)
    {
        reason = CANsub::tr("It contains more than one '@'.");
        return std::nullopt;
    }

    QString const channelText = interfaceName.mid(at + 1);
    if ((channelText.size() != 1) || (channelText.at(0) < QLatin1Char('1')) || (channelText.at(0) > QLatin1Char('4')))
    {
        reason = CANsub::tr("The channel \"%1\" is not 1, 2, 3 or 4.").arg(channelText);
        return std::nullopt;
    }
    target.channel = channelText.toUInt();

    /* An IPv4 address can be followed by ':' and the device ID to verify the device certificate against */
    QString host = interfaceName.left(at);
    qsizetype const colon = host.indexOf(QLatin1Char(':'));
    if (colon >= 0)
    {
        QString const deviceId = host.mid(colon + 1);
        host = host.left(colon);

        if (!isDottedIpv4(host))
        {
            reason = CANsub::tr("A device ID after ':' is only supported after an IPv4 address, e.g. "
                                "192.168.1.100:aabbccdd@1.");
            return std::nullopt;
        }
        if (!isDeviceId(deviceId))
        {
            reason = CANsub::tr("The device ID \"%1\" after the ':' is not 8 hex digits.").arg(deviceId);
            return std::nullopt;
        }
        target.deviceId = deviceId.toLower();
    }

    if (host.isEmpty())
    {
        reason = CANsub::tr("The host before the '@' is missing.");
        return std::nullopt;
    }

    /* The device hostnames only resolve with mDNS, which needs the ".local" suffix. Rejecting the short form here gives
     * a clearer message than the generic one below. */
    if (isShortDeviceHostname(host))
    {
        reason = CANsub::tr("The host \"%1\" is missing the .local suffix. Use \"%1.local\" instead.").arg(host);
        return std::nullopt;
    }

    target.ipv4 = isDottedIpv4(host);
    if (!target.ipv4 && !isDeviceHostname(host))
    {
        reason = CANsub::tr("The host \"%1\" is neither the hostname of a CANsub device (<device ID>-usb.local or "
                            "<device ID>-eth.local) nor an IPv4 address.").arg(host);
        return std::nullopt;
    }
    target.host = host;

    return target;
}

std::optional<CANsub::Target> CANsub::parseInterfaceName(QString const &interfaceName, QString *errorMessage)
{
    QString reason;
    std::optional<Target> target = parseTarget(interfaceName, reason);
    if (!target && (errorMessage != nullptr))
    {
        *errorMessage = tr("Invalid interface name \"%1\": %2").arg(interfaceName, reason);
    }

    return target;
}

std::optional<QString> CANsub::expectedDeviceId(QString const &expectedHostname)
{
    /* A hostname may be given fully qualified, with a trailing '.' */
    QString name = expectedHostname;
    if (name.endsWith(QLatin1Char('.')))
    {
        name.chop(1);
    }

    if (isDeviceId(name))
    {
        return name.toLower();
    }

    /* Host names are not case-sensitive, so the suffix is not either */
    QString const suffix = name.mid(8);
    if (isDeviceId(name.left(8))
        && ((suffix.compare(QLatin1String("-usb.local"), Qt::CaseInsensitive) == 0)
            || (suffix.compare(QLatin1String("-eth.local"), Qt::CaseInsensitive) == 0)))
    {
        return name.left(8).toLower();
    }

    return std::nullopt;
}

} // namespace CANsubQt
