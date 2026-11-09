/* Conversion of CAN frames between the CANsub library and Qt */

#include "CANsubFrames.hxx"

#include <algorithm>
#include <cstring>
#include <QByteArray>
#include <QStringList>

#include "CANsub.hxx"

namespace CANsubQt
{

/* QCanBusFrame errors of a CANsub error type. NoError for frames to drop. */
static QCanBusFrame::FrameErrors frameErrorsOfType(quint8 type)
{
    switch (type)
    {
    case CANSUB_ERROR_FRAME_TYPE_NO_ERROR:
    {
        return QCanBusFrame::NoError;
    }
    case CANSUB_ERROR_FRAME_TYPE_ACK_ERROR:
    {
        return QCanBusFrame::MissingAcknowledgmentError;
    }
    case CANSUB_ERROR_FRAME_TYPE_BIT_ERROR:
    case CANSUB_ERROR_FRAME_TYPE_FORM_ERROR:
    case CANSUB_ERROR_FRAME_TYPE_STUFF_ERROR:
    case CANSUB_ERROR_FRAME_TYPE_CRC_ERROR:
    {
        return QCanBusFrame::ProtocolViolationError;
    }
    default:
    {
        break;
    }
    }

    return QCanBusFrame::UnknownError;
}

std::optional<QCanBusFrame> fromCansubFrame(cansub_frame const &in)
{
    QCanBusFrame frame;

    if (in.flags & CANSUB_FRAME_FLAG_ERROR)
    {
        quint8 const type = in.data[0];
        QCanBusFrame::FrameErrors const errors = frameErrorsOfType(type);
        if (!errors)
        {
            return std::nullopt;
        }

        frame.setFrameType(QCanBusFrame::ErrorFrame);
        frame.setError(errors);
        /* The raw type, so describeErrorFrame() can name it */
        frame.setPayload(QByteArray(1, static_cast<char>(type)));
    }
    else
    {
        bool const isFd = (in.flags & CANSUB_FRAME_FLAG_FDF) != 0;

        frame.setExtendedFrameFormat((in.flags & CANSUB_FRAME_FLAG_IDE) != 0);
        frame.setFrameId(in.arbitration_id);

        if (in.flags & CANSUB_FRAME_FLAG_RTR)
        {
            /* Qt uses the payload size as the requested length: As many zero bytes as the DLC gives */
            int const requested = static_cast<int>(cansub_frame_dlc_to_nob(in.dlc, 0));
            frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
            frame.setPayload(QByteArray(requested, '\0'));
        }
        else
        {
            int const maximumLength = isFd ? 64 : 8;
            int const length = std::min(static_cast<int>(in.length), maximumLength);
            frame.setFrameType(QCanBusFrame::DataFrame);
            frame.setPayload(QByteArray(reinterpret_cast<char const *>(in.data), length));
        }

        if (isFd)
        {
            frame.setFlexibleDataRateFormat(true);
            frame.setBitrateSwitch((in.flags & CANSUB_FRAME_FLAG_BRS) != 0);
            frame.setErrorStateIndicator((in.flags & CANSUB_FRAME_FLAG_ESI) != 0);
        }
    }

    /* The whole timestamp goes in the microseconds field, with seconds left at 0. Tools such as SavvyCAN read only
     * microSeconds() and expect it to hold the full time */
    frame.setTimeStamp(QCanBusFrame::TimeStamp(0, static_cast<qint64>(in.timestamp_us)));
    frame.setLocalEcho((in.flags & CANSUB_FRAME_FLAG_TX) != 0);
    return frame;
}

std::optional<cansub_frame> toCansubFrame(QCanBusFrame const &frame, QString &reason)
{
    /* The specific checks come before QCanBusFrame::isValid(), which rejects most of these frames as well (in Qt 5.15
     * and Qt 6 alike), but without telling why */
    QCanBusFrame::FrameType const type = frame.frameType();
    if (type == QCanBusFrame::ErrorFrame)
    {
        reason = CANsub::tr("Error frames cannot be sent.");
        return std::nullopt;
    }
    if ((type != QCanBusFrame::DataFrame) && (type != QCanBusFrame::RemoteRequestFrame))
    {
        reason = CANsub::tr("Only data frames and remote request frames can be sent.");
        return std::nullopt;
    }

    bool const isFd = frame.hasFlexibleDataRateFormat();
    QByteArray const payload = frame.payload();
    qsizetype const length = payload.size();

    if (type == QCanBusFrame::RemoteRequestFrame)
    {
        if (isFd)
        {
            reason = CANsub::tr("A remote request frame cannot be a CAN FD frame.");
            return std::nullopt;
        }

        /* Qt uses the payload size as the requested length */
        if (length > 8)
        {
            reason = CANsub::tr("The requested length of %1 bytes of the remote request frame exceeds 8 bytes.")
                     .arg(QString::number(length));
            return std::nullopt;
        }
    }
    else if (isFd)
    {
        bool const isFdLength = (length <= 8) || (length == 12) || (length == 16) || (length == 20) || (length == 24)
                                || (length == 32) || (length == 48) || (length == 64);
        if (!isFdLength)
        {
            reason = CANsub::tr("A CAN FD frame cannot carry %1 bytes. The valid payload lengths are 0 to 8, 12, 16, "
                                "20, 24, 32, 48 and 64 bytes.").arg(QString::number(length));
            return std::nullopt;
        }
    }
    else if (length > 8)
    {
        reason = CANsub::tr("The payload of %1 bytes exceeds the 8 bytes of a classical CAN frame.")
                 .arg(QString::number(length));
        return std::nullopt;
    }

    if (!frame.hasExtendedFrameFormat() && (frame.frameId() > 0x7FFu))
    {
        reason = CANsub::tr("The frame ID 0x%1 exceeds the 11 bits of a standard frame. Use the extended frame "
                            "format.").arg(frame.frameId(), 0, 16);
        return std::nullopt;
    }

    /* Catches what is left, e.g. a frame ID beyond 29 bits, which QCanBusFrame::setFrameId() replaces by 0 */
    if (!frame.isValid())
    {
        reason = CANsub::tr("The frame is invalid, e.g. its frame ID exceeds 29 bits.");
        return std::nullopt;
    }

    cansub_frame out{};
    unsigned flags = 0;
    out.arbitration_id = frame.frameId();
    if (frame.hasExtendedFrameFormat())
    {
        flags |= CANSUB_FRAME_FLAG_IDE;
    }
    if (isFd)
    {
        flags |= CANSUB_FRAME_FLAG_FDF;
    }
    if (frame.hasBitrateSwitch())
    {
        flags |= CANSUB_FRAME_FLAG_BRS;
    }
    if (frame.hasErrorStateIndicator())
    {
        flags |= CANSUB_FRAME_FLAG_ESI;
    }

    /* The length is valid for the frame format here, so the DLC represents exactly that many bytes */
    out.length = static_cast<uint8_t>(length);
    out.dlc = static_cast<uint8_t>(cansub_frame_nob_to_dlc(static_cast<uint32_t>(length)));
    if (type == QCanBusFrame::RemoteRequestFrame)
    {
        flags |= CANSUB_FRAME_FLAG_RTR;
    }
    else
    {
        std::memcpy(out.data, payload.constData(), static_cast<size_t>(length));
    }
    out.flags = static_cast<uint8_t>(flags);

    cansub_frame_faults faults = 0;
    if (cansub_frame_validate(&out, &faults) != CANSUB_RESULT_OK)
    {
        QStringList problems;
        if (faults & CANSUB_FRAME_FAULT_RESERVED)
        {
            problems << CANsub::tr("Reserved bits of the frame are set.");
        }
        if (faults & CANSUB_FRAME_FAULT_RX_ONLY)
        {
            problems << CANsub::tr("The frame is marked as an error frame or a transmit acknowledgement, which are "
                                   "only received.");
        }
        if (faults & CANSUB_FRAME_FAULT_FD_ONLY)
        {
            problems << CANsub::tr("The bit rate switch or the error state indicator is set on a classical CAN "
                                   "frame.");
        }
        if (faults & CANSUB_FRAME_FAULT_RTR_FD)
        {
            problems << CANsub::tr("A remote request frame cannot be a CAN FD frame.");
        }
        if (faults & CANSUB_FRAME_FAULT_ID)
        {
            problems << CANsub::tr("The frame ID 0x%1 exceeds 11 bits, or 29 bits for an extended frame.")
                        .arg(out.arbitration_id, 0, 16);
        }
        if (faults & CANSUB_FRAME_FAULT_DLC)
        {
            problems << CANsub::tr("The DLC %1 is outside 0-15.").arg(QString::number(out.dlc));
        }
        if (faults & CANSUB_FRAME_FAULT_LENGTH)
        {
            problems << CANsub::tr("The length of %1 bytes does not match the DLC %2.")
                        .arg(QString::number(out.length), QString::number(out.dlc));
        }
        if (problems.isEmpty())
        {
            problems << CANsub::tr("The CANsub library rejected the frame (faults 0x%1).").arg(faults, 0, 16);
        }

        reason = problems.join(QLatin1Char(' '));
        return std::nullopt;
    }

    return out;
}

QString describeErrorFrame(QCanBusFrame const &frame)
{
    if (frame.frameType() != QCanBusFrame::ErrorFrame)
    {
        return QString();
    }

    QStringList lines;
    QCanBusFrame::FrameErrors const errors = frame.error();

    if (errors.testFlag(QCanBusFrame::TransmissionTimeoutError))
    {
        lines << CANsub::tr("The transmission timed out.");
    }
    if (errors.testFlag(QCanBusFrame::LostArbitrationError))
    {
        lines << CANsub::tr("The arbitration was lost.");
    }
    if (errors.testFlag(QCanBusFrame::ControllerError))
    {
        lines << CANsub::tr("The CAN controller reported an error.");
    }
    if (errors.testFlag(QCanBusFrame::ProtocolViolationError))
    {
        lines << CANsub::tr("The CAN protocol was violated.");
    }
    if (errors.testFlag(QCanBusFrame::TransceiverError))
    {
        lines << CANsub::tr("The CAN transceiver reported an error.");
    }
    if (errors.testFlag(QCanBusFrame::MissingAcknowledgmentError))
    {
        lines << CANsub::tr("The frame was not acknowledged.");
    }
    if (errors.testFlag(QCanBusFrame::BusOffError))
    {
        lines << CANsub::tr("The CAN controller is bus off.");
    }
    if (errors.testFlag(QCanBusFrame::BusError))
    {
        lines << CANsub::tr("A CAN bus error occurred.");
    }
    if (errors.testFlag(QCanBusFrame::ControllerRestartError))
    {
        lines << CANsub::tr("The CAN controller restarted.");
    }
    if (errors.testFlag(QCanBusFrame::UnknownError))
    {
        lines << CANsub::tr("An unknown error occurred.");
    }

    QByteArray const payload = frame.payload();
    if (!payload.isEmpty())
    {
        quint8 const type = static_cast<quint8>(payload.at(0));
        switch (type)
        {
        case CANSUB_ERROR_FRAME_TYPE_NO_ERROR:
        {
            break;
        }
        case CANSUB_ERROR_FRAME_TYPE_BIT_ERROR:
        {
            lines << CANsub::tr("The CANsub device reported a bit error: A transmitted bit was read back with the "
                                "opposite level.");
            break;
        }
        case CANSUB_ERROR_FRAME_TYPE_ACK_ERROR:
        {
            lines << CANsub::tr("The CANsub device reported an ACK error: No node acknowledged the frame.");
            break;
        }
        case CANSUB_ERROR_FRAME_TYPE_FORM_ERROR:
        {
            lines << CANsub::tr("The CANsub device reported a form error: A fixed-form field was invalid.");
            break;
        }
        case CANSUB_ERROR_FRAME_TYPE_STUFF_ERROR:
        {
            lines << CANsub::tr("The CANsub device reported a stuff error: The bit stuffing was invalid.");
            break;
        }
        case CANSUB_ERROR_FRAME_TYPE_CRC_ERROR:
        {
            lines << CANsub::tr("The CANsub device reported a CRC error: The CRC of a frame was invalid.");
            break;
        }
        case CANSUB_ERROR_FRAME_TYPE_OTHER_ERROR:
        {
            lines << CANsub::tr("The CANsub device reported another error.");
            break;
        }
        default:
        {
            lines << CANsub::tr("The CANsub device reported the unknown error type %1.").arg(QString::number(type));
            break;
        }
        }
    }

    return lines.join(QLatin1Char('\n'));
}

} // namespace CANsubQt
