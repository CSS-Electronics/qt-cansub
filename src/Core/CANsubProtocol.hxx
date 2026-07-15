#ifndef CANSUB_QT_CANSUBPROTOCOL_HXX
#define CANSUB_QT_CANSUBPROTOCOL_HXX

#include <QCanBusFrame>
#include <QVector>
#include "FrameBuffer.hxx"

/**
 * Decode raw binary CANsub protocol data into QCanBusFrames
 * @param dataBuffer Buffer with data to decode from
 * @param results Storage for successfull decoded CAN frames
 */
void CANsubProtocolDecode(FrameBuffer& dataBuffer, QVector<QCanBusFrame>& results);

/**
 * Encode QCanBusFrame instances to the CANsub format
 * @param dataBuffer Storage for the encoded CANsub websocket binary payload
 * @param frame QCanBusFrame to encode
 * @note There is only a single CAN frame variant of this function, since the QCanBusDevice interface does not expose
 *       a function which takes multiple frames in a single call.
 */
void CANsubProtocolEncode(QByteArray& dataBuffer, QCanBusFrame const& frame);

#endif // CANSUB_QT_CANSUBPROTOCOL_HXX
