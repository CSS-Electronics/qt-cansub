#include <optional>
#include <vector>
#include <cstring>
#include <cstddef>
#include <QCanBusFrame>
#include <QDebug>
#include <QtEndian>
#include <boost/crc.hpp>
#include <boost/endian.hpp>

#include "CANsubProtocol.hxx"

#include "CANsubHelper.hxx"

static constexpr uint8_t HDLC_BOUNDARY_BYTE = 0x7E;
static constexpr uint8_t HDLC_ESCAPE_BYTE = 0x7D;
static constexpr uint8_t HDLC_ESCAPE_XOR_PATTERN = 0x20;

static constexpr uint8_t PROTOCOL_DLC_MASK = 0x0F;
static constexpr uint8_t PROTOCOL_FLAGS1_TX_MASK = 0x10;
static constexpr uint8_t PROTOCOL_FLAGS1_ESI_MASK = 0x20;
static constexpr uint16_t PROTOCOL_ID_MASK_STANDARD = 0x7FF;
static constexpr uint32_t PROTOCOL_ID_MASK_EXTENDED = 0x1FFFFFFF;
static constexpr uint32_t PROTOCOL_ID_MASK_EXTENDED_BIT = 0x80000000;

static constexpr uint8_t PROTOCOL_FLAGS1_RTR_MASK = 0xC0;
static constexpr uint8_t PROTOCOL_FLAGS1_FD_VALUE = 0x80;
static constexpr uint8_t PROTOCOL_FLAGS1_RTR_VALUE = 0x40;
static constexpr uint8_t PROTOCOL_FLAGS1_BRS_VALUE = 0x40;

static constexpr uint32_t MSG_ENCODE_ERR_HEADER_SIZE = (6 + 1);
static constexpr uint32_t MSG_ENCODE_STD_HEADER_SIZE = (6 + 3);
static constexpr uint32_t MSG_ENCODE_EXT_HEADER_SIZE = (6 + 5);
static constexpr uint32_t MSG_ENCODE_HEADER_MIN_SIZE = std::min({MSG_ENCODE_ERR_HEADER_SIZE, MSG_ENCODE_STD_HEADER_SIZE,
                                                                 MSG_ENCODE_EXT_HEADER_SIZE});

static constexpr uint64_t MSG_ENCODE_TIME_MIN_S = 1735689600;
/* Unused for now, but kept for reference */
// static constexpr uint64_t MSG_ENCODE_TIME_MAX_S = MSG_ENCODE_TIME_MIN_S + (0xFFFFFFFFFFFF / 1000000);

typedef boost::crc_optimal<32, 0x04C11DB7, 0xFFFFFFFF, 0xFFFFFFFF, true, true> crc_32_type;

static void decodeMessage(const std::byte*& current, const std::byte* end, QVector<QCanBusFrame>& results);
static void decodeMessages(const std::byte* data, std::size_t length, QVector<QCanBusFrame>& results);

void CANsubProtocolDecode(FrameBuffer& dataBuffer, QVector<QCanBusFrame>& results)
{
    /* Reusable de-stuffing scratchpad, allocated once per thread and kept across calls */
    thread_local std::vector<std::byte> destuffed;

    /* Extract as many frames as possible (minimum possible frame size is two boundary bytes) */
    while (dataBuffer.available() >= 2)
    {
        std::byte const* const base = dataBuffer.peek();
        std::size_t const available = dataBuffer.available();

        /* Find the opening boundary byte */
        void const* const startPtr = std::memchr(base, HDLC_BOUNDARY_BYTE, available);
        if (startPtr == nullptr)
        {
            /* No frame start anywhere in the remainder of the buffer */
            dataBuffer.consume(available);
            break;
        }
        std::size_t const startIdx = static_cast<std::byte const*>(startPtr) - base;

        /* Find the closing boundary byte after the opening one */
        std::byte const* const framePayloadStart = base + startIdx + 1;
        std::size_t const remaining = available - (startIdx + 1);
        void const* const endPtr = std::memchr(framePayloadStart, HDLC_BOUNDARY_BYTE, remaining);
        if (endPtr == nullptr)
        {
            /* Incomplete frame. Drop all data up to the first boundary byte, but keep the partial data of the current
             * frame in the buffer. */
            dataBuffer.consume(startIdx);
            break;
        }
        auto const* const frameEnd = static_cast<std::byte const*>(endPtr);

        /* De-stuff the data into the scratch buffer */
        destuffed.clear();
        bool destuffWithNoErrors = true;

        for (std::byte const* p = framePayloadStart; p != frameEnd; ++p)
        {
            auto current = std::to_integer<uint8_t>(*p);

            if (current == HDLC_ESCAPE_BYTE)
            {
                if (p + 1 == frameEnd)
                {
                    /* Error: The byte should have been escaped, but there is no next byte */
                    qWarning() << "Invalid framing detected";
                    destuffWithNoErrors = false;
                    break;
                }
                current = std::to_integer<uint8_t>(*(++p)) ^ HDLC_ESCAPE_XOR_PATTERN;
            }
            destuffed.push_back(std::byte{current});
        }

        /* Consume through the closing boundary */
        dataBuffer.consume(static_cast<std::size_t>(frameEnd - base) + 1);

        if (!destuffWithNoErrors)
        {
            continue;
        }

        /* Check CRC32 */
        if (destuffed.size() < 4)
        {
            qWarning() << "Frame too short";
            continue;
        }

        std::size_t const bodyLength = destuffed.size() - 4;

        /* Copy out the last 4 bytes */
        boost::endian::big_uint32_buf_t expectedCrc;
        std::memcpy(&expectedCrc, destuffed.data() + bodyLength, sizeof(expectedCrc));

        /* Calculate CRC over the body */
        crc_32_type crc_32;
        crc_32.process_bytes(destuffed.data(), bodyLength);
        uint32_t const calculatedCrc = crc_32.checksum();

        if (expectedCrc.value() != calculatedCrc)
        {
            qWarning() << "CRC mismatch" << QString::asprintf("%08X - %08X", expectedCrc.value(), calculatedCrc);
            continue;
        }

        /* Decode frames */
        decodeMessages(destuffed.data(), bodyLength, results);
    }

    dataBuffer.recycle();
}

static void decodeMessages(std::byte const* data, std::size_t length, QVector<QCanBusFrame>& results)
{
    std::byte const* current = data;
    std::byte const* const end = data + length;

    while (current < end)
    {
        std::byte const* const before = current;
        decodeMessage(current, end, results);

        if (current == before)
        {
            qWarning() << "No decode progress though there should be enough data";
            break;
        }
    }
}

static void decodeMessage(std::byte const*& current, std::byte const* end, QVector<QCanBusFrame>& results)
{
    auto const remaining = [&](std::byte const* p) { return static_cast<std::size_t>(end - p); };

    /* Work on a local cursor and only commit it once a full message has been decoded */
    std::byte const* cursor = current;

    if (remaining(cursor) < MSG_ENCODE_HEADER_MIN_SIZE)
    {
        /* Not enough data for any frame type */
        return;
    }

    QCanBusFrame frame;

    /* Timestamp (us) */
    boost::endian::big_uint48_buf_t timestampUsRaw;
    std::memcpy(timestampUsRaw.data(), cursor, sizeof(timestampUsRaw));
    cursor += sizeof(timestampUsRaw);

    uint64_t const timestampUs = timestampUsRaw.value() + MSG_ENCODE_TIME_MIN_S * 1000000;
    int64_t const s = static_cast<int64_t>(timestampUs) / 1000000;
    int64_t const us = static_cast<int64_t>(timestampUs) % 1000000;
    frame.setTimeStamp(QCanBusFrame::TimeStamp(s, us));

    auto const flags_1 = std::to_integer<uint8_t>(*cursor++);

    /* Detect frame type */
    if ((flags_1 & 0xE0) == 0x20)
    {
        /* Error frame */
        frame.setFrameType(QCanBusFrame::ErrorFrame);
        uint8_t const errorType = flags_1 & 0x1F;

        /* Best-effort mapping of the error types to Qt's error types */
        switch (errorType)
        {
            case 0:
            {
                frame.setError(QCanBusFrame::TransceiverError);
                break;
            }
            case 1:
            {
                frame.setError(QCanBusFrame::MissingAcknowledgmentError);
                break;
            }
            case 2:
            case 3:
            case 4:
            {
                frame.setError(QCanBusFrame::ProtocolViolationError);
                break;
            }
            default:
            {
                frame.setError(QCanBusFrame::UnknownError);
                break;
            }
        }
    }
    else
    {
        if ((flags_1 & PROTOCOL_FLAGS1_RTR_MASK) == PROTOCOL_FLAGS1_RTR_VALUE)
        {
            frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
        }
        else
        {
            frame.setFrameType(QCanBusFrame::DataFrame);
        }

        frame.setFlexibleDataRateFormat(flags_1 & PROTOCOL_FLAGS1_FD_VALUE);
        if (frame.hasFlexibleDataRateFormat())
        {
            frame.setBitrateSwitch(flags_1 & PROTOCOL_FLAGS1_BRS_VALUE);
            frame.setErrorStateIndicator(flags_1 & PROTOCOL_FLAGS1_ESI_MASK);
        }

        frame.setLocalEcho(flags_1 & PROTOCOL_FLAGS1_TX_MASK);
        uint8_t const dlc = flags_1 & PROTOCOL_DLC_MASK;

        if (remaining(cursor) < 1)
        {
            /* Not enough bytes for the second flag byte */
            return;
        }
        auto const flags_2 = std::to_integer<uint8_t>(*cursor++);

        frame.setExtendedFrameFormat(flags_2 & 0x80);
        uint frameId = 0;
        if (frame.hasExtendedFrameFormat())
        {
            boost::endian::big_uint24_buf_t helper;
            if (remaining(cursor) < sizeof(helper))
            {
                /* Not enough bytes */
                return;
            }

            std::memcpy(helper.data(), cursor, sizeof(helper));
            cursor += sizeof(helper);
            frameId = ((flags_2 & 0x1F) << 24 | helper.value());
        }
        else
        {
            if (remaining(cursor) < 1)
            {
                /* Not enough bytes for the second id byte */
                return;
            }
            uint8_t const idByte2 = std::to_integer<uint8_t>(*cursor++);
            frameId = ((flags_2 & 0x07) << 8 | idByte2);
        }
        frame.setFrameId(frameId);

        QByteArray frameData;
        size_t const numberOfBytes = std::clamp(CANsubHelper::dlc2nob(dlc), static_cast<size_t>(0), static_cast<size_t>(frame.hasFlexibleDataRateFormat() ? 64 : 8));
        if (frame.frameType() != QCanBusFrame::RemoteRequestFrame)
        {
            if (remaining(cursor) < numberOfBytes)
            {
                /* Not enough bytes to extract the full payload */
                return;
            }

            frameData = QByteArray(reinterpret_cast<char const*>(cursor), static_cast<int>(numberOfBytes));
            cursor += numberOfBytes;
        }
        else
        {
            frameData.resize(static_cast<int>(numberOfBytes));
        }
        frame.setPayload(frameData);
    }

    results.push_back(frame);
    current = cursor; /* commit the consumed bytes */
}

template <typename T>
static void encodeFrame(T& stackBuffer, QCanBusFrame const& frame)
{
    if (frame.frameType() == QCanBusFrame::ErrorFrame)
    {
        /* Remote can't emit an error frame on the bus, ignored */
        return;
    }

    /* Timestamp. CANsub ignores tx timestamps, so not implemented (0) */
    boost::endian::big_uint48_buf_t timestampUs(0);
    std::copy_n(timestampUs.data(), sizeof(timestampUs), std::back_inserter(stackBuffer));

    /* Encode first flag byte */
    uint8_t flags_1 = 0x00;

    if (frame.frameType() == QCanBusFrame::RemoteRequestFrame)
    {
        flags_1 = PROTOCOL_FLAGS1_RTR_VALUE;
    }
    else if (frame.hasFlexibleDataRateFormat())
    {
        flags_1 |= PROTOCOL_FLAGS1_FD_VALUE;
        if (frame.hasBitrateSwitch())
        {
            flags_1 |= PROTOCOL_FLAGS1_BRS_VALUE;
        }
        if (frame.hasErrorStateIndicator())
        {
            flags_1 |= PROTOCOL_FLAGS1_ESI_MASK;
        }
    }

    if (frame.hasLocalEcho())
    {
        flags_1 |= PROTOCOL_FLAGS1_TX_MASK;
    }
    uint8_t const dlc = CANsubHelper::nob2dlc(frame.payload().size());
    flags_1 |= dlc & PROTOCOL_DLC_MASK;
    stackBuffer.push_back(flags_1);

    if (frame.hasExtendedFrameFormat())
    {
        boost::endian::big_uint32_buf_t flags_2(
            PROTOCOL_ID_MASK_EXTENDED_BIT | (frame.frameId() & PROTOCOL_ID_MASK_EXTENDED));
        std::copy_n(flags_2.data(), sizeof(flags_2), std::back_inserter(stackBuffer));
    }
    else
    {
        boost::endian::big_uint16_buf_t flags_2(frame.frameId() & PROTOCOL_ID_MASK_STANDARD);
        std::copy_n(flags_2.data(), sizeof(flags_2), std::back_inserter(stackBuffer));
    }

    /* Write payload to buffer (if not RTR frame) */
    if (frame.frameType() == QCanBusFrame::DataFrame)
    {
        size_t const encodedSize = CANsubHelper::dlc2nob(dlc);
        size_t const copyCount = std::min(static_cast<size_t>(frame.payload().size()), encodedSize);

        std::copy_n(frame.payload().cbegin(), copyCount, std::back_inserter(stackBuffer));

        /* Zero fill in case there are any remaining bytes */
        for (size_t i = copyCount; i < encodedSize; ++i)
        {
            stackBuffer.push_back(static_cast<char>(0));
        }
    }
}

void CANsubProtocolEncode(QByteArray& dataBuffer, QCanBusFrame const& frame)
{
    /* Simplified encoder */
    dataBuffer.push_back(HDLC_BOUNDARY_BYTE);

    /* Encode the frame into a temporary buffer */
    QByteArray tempBuffer;
    encodeFrame(tempBuffer, frame);

    crc_32_type crc_32;
    crc_32.process_bytes(tempBuffer.cbegin(), tempBuffer.size());

    /* Write into main buffer with HDLC byte stuffing */
    for (auto& current : tempBuffer)
    {
        if ((current == HDLC_BOUNDARY_BYTE) || (current == HDLC_ESCAPE_BYTE))
        {
            dataBuffer.push_back(HDLC_ESCAPE_BYTE);
            dataBuffer.push_back(static_cast<char>(current ^ HDLC_ESCAPE_XOR_PATTERN));
        }
        else
        {
            dataBuffer.push_back(current);
        }
    }

    /* Write updated CRC in big-endian order, matching the decoder and the rest of the protocol */
    {
        boost::endian::big_uint32_buf_t crc(crc_32.checksum());
        for (std::size_t i = 0; i < sizeof(crc); ++i)
        {
            auto current = static_cast<char>(crc.data()[i]);
            if ((current == HDLC_BOUNDARY_BYTE) || (current == HDLC_ESCAPE_BYTE))
            {
                current ^= HDLC_ESCAPE_XOR_PATTERN;
                dataBuffer.push_back(HDLC_ESCAPE_BYTE);
            }
            dataBuffer.push_back(current);
        }
    }

    /* Write frame termination */
    dataBuffer.push_back(HDLC_BOUNDARY_BYTE);
}
