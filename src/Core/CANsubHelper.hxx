#ifndef CANSUB_QT_CANSUB_HELPER_HXX
#define CANSUB_QT_CANSUB_HELPER_HXX

#include <QtSerialBus/QCanBusDevice>

namespace CANsubHelper
{
    /**
     * Translate a DLC code to a number of bytes
     * @param dlc CAN DLC code
     * @return Number of bytes this DLC corresponds to
     */
    size_t dlc2nob(uint8_t dlc);

    /**
     * Convert a number of bytes to a DLC code
     * @param nob Number of bytes
     * @return The next DLC code that is large enough to contain all the bytes
     */
    uint8_t nob2dlc(size_t nob);
};

#endif // CANSUB_QT_CANSUB_HELPER_HXX
