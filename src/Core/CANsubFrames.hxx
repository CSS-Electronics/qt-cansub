#ifndef CANSUB_QT_CANSUB_FRAMES_HXX
#define CANSUB_QT_CANSUB_FRAMES_HXX

#include <optional>
#include <QString>
#include <QtSerialBus/QCanBusFrame>

#include "cansub/cansub_ffi.h"

namespace CANsubQt
{

/**
 * Convert a frame received from the CANsub to a QCanBusFrame.
 *
 * Error frames carry the QCanBusFrame errors of their CANsub error type, and the raw type as their only payload byte,
 * see describeErrorFrame(). Transmit acknowledgements are marked as local echo. Remote request frames get as many zero
 * bytes of payload as their DLC gives, as Qt uses the payload size as the requested length.
 *
 * @return The frame, or nothing for an error frame without an error (CANSUB_ERROR_FRAME_TYPE_NO_ERROR).
 */
std::optional<QCanBusFrame> fromCansubFrame(cansub_frame const &frame);

/**
 * Convert a QCanBusFrame to a frame to send.
 *
 * Checks everything that makes a frame unsendable, with a specific reason for each, ending with
 * cansub_frame_validate().
 *
 * @param[out] reason Set on failure to a translated description of why the frame cannot be sent.
 * @return The frame, or nothing if it cannot be sent.
 */
std::optional<cansub_frame> toCansubFrame(QCanBusFrame const &frame, QString &reason);

/**
 * Translated description of an error frame from fromCansubFrame(), one sentence per line, for
 * QCanBusDevice::interpretErrorFrame().
 *
 * @return The description, empty for a frame that is no error frame.
 */
QString describeErrorFrame(QCanBusFrame const &frame);

} // namespace CANsubQt

#endif // CANSUB_QT_CANSUB_FRAMES_HXX
