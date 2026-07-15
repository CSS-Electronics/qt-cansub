#include <QDebug>

#include "CANsubHelper.hxx"

size_t CANsubHelper::dlc2nob(uint8_t const dlc)
{
    static constexpr size_t lookupTable[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 12, 16, 20, 24, 32, 48, 64,
    };

    return lookupTable[dlc & 0xF];
}

uint8_t CANsubHelper::nob2dlc(size_t const nob)
{
    uint8_t result = 0;

    if (nob <= 8)
    {
        result = nob;
    }
    else if (nob <= 12)
    {
        result = 9;
    }
    else if (nob <= 16)
    {
        result = 10;
    }
    else if (nob <= 20)
    {
        result = 11;
    }
    else if (nob <= 24)
    {
        result = 12;
    }
    else if (nob <= 32)
    {
        result = 13;
    }
    else if (nob <= 48)
    {
        result = 14;
    }
    else if (nob <= 64)
    {
        result = 15;
    }
    else
    {
        qWarning() << "Invalid length detected when encoding to DLC, clamping";
        result = 15;
    }

    return result;
}
