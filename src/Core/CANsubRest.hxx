#ifndef CANSUB_QT_CANSUB_REST_HXX
#define CANSUB_QT_CANSUB_REST_HXX

#include <optional>
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QtGlobal>

/**
 * Wire format of the REST API of the device (API version 05.00): Paths, request bodies and response parsers.
 *
 * Only the format: Which requests to send, what a status code means for the connect and the messages for the user are
 * decided by the device (CANsub). See the cansub-docs `openapi.json` for the paths and schemas. Bodies are compact JSON.
 * The parsers accept any input and return nothing if it is invalid. Parse only the bodies of successful responses, as
 * error bodies are plain text whatever the content type.
 */
namespace CANsubQt::Rest
{

/** GET: The remote API version, a quoted JSON string such as "05.00", see cansub_api_version_check(). */
inline constexpr char const versionPath[] = "/api/version";

/** GET: The PTP status of the device clock, see parsePtpState(). */
inline constexpr char const ptpPath[] = "/api/time/ptp";

/** GET, PUT: The device time, see parseTime() and timeBody(). */
inline constexpr char const timePath[] = "/api/time";

/** PUT: The PHY configuration of a channel, see phyBody(). */
QByteArray phyPath(quint32 channel);

/** GET: The filter IDs of a channel, see parseFilterIds(). POST: Add a filter, see acceptAllFilterBody(). */
QByteArray filterPath(quint32 channel);

/** DELETE: One filter of a channel. */
QByteArray filterPath(quint32 channel, quint32 filterId);

/** Bit timing of one CAN phase, as the REST API takes it. seg1 excludes the sync segment. */
struct Timing
{
    quint32 brp = 0;
    quint32 seg1 = 0;
    quint32 seg2 = 0;
    quint32 sjw = 0;
};

/** PHY configuration of a channel. */
struct PhyConfig
{
    bool listenOnly = false;
    bool autoReset = true;
    bool errorFrames = false;
    bool txAckFrames = false;
    /** Nominal bit timing */
    Timing timing;
    /** Data phase bit timing, only used by frames with bit rate switch */
    Timing timingData;
};

/**
 * Body of PUT /api/can/{channel}/phy: listen_only, auto_reset, error_frames, tx_ack_frames, timing and timing_data,
 * each timing with brp, seg1, seg2 and sjw.
 */
QByteArray phyBody(PhyConfig const &config);

/**
 * Body of PUT /api/time, with rate_ppb 0.
 *
 * @param time Time to set. Converted to UTC and written as "YYYY-MM-DDTHH:MM:SS.sssZ", as the device requires the
 *             milliseconds.
 */
QByteArray timeBody(QDateTime const &time);

/**
 * Body of POST /api/can/{channel}/filter for a mask filter that accepts every ID of one format: ID 0 with mask 0.
 *
 * @param extended True for extended (29-bit) IDs, false for standard (11-bit) IDs.
 */
QByteArray acceptAllFilterBody(bool extended);

/**
 * Parse the body of GET /api/time.
 *
 * @return The "utc" field as a valid QDateTime in UTC, or nothing if the body or the time is invalid. A time without a
 *         zone designator is read as UTC.
 */
std::optional<QDateTime> parseTime(QByteArray const &body);

/**
 * Parse the body of GET /api/time/ptp.
 *
 * @return The "state" field, e.g. "tracking", or nothing if the body is no JSON object with a string "state".
 */
std::optional<QString> parsePtpState(QByteArray const &body);

/**
 * Parse the body of GET /api/can/{channel}/filter.
 *
 * @return The filter IDs, or nothing if the body is not a JSON array of whole numbers from 0 to 2^32 - 1.
 */
std::optional<QList<quint32>> parseFilterIds(QByteArray const &body);

} // namespace CANsubQt::Rest

#endif // CANSUB_QT_CANSUB_REST_HXX
