#include "MetaTypes.hxx"

#include <QHostAddress>
#include <QMetaType>
#include <QVector>
#include <QtGlobal>
#include <QtSerialBus/QCanBusFrame>

#include <qmdnsengine/message.h>

#include "CANsub.hxx"

namespace CANsubQt
{

static bool isMetaTypeRegistered(char const *const name)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QMetaType::fromName(name).isValid();
#else
    int const typeId = QMetaType::type(name);
    return (typeId != QMetaType::UnknownType) && QMetaType::isRegistered(typeId);
#endif
}

void registerMetaTypes()
{
    if (!isMetaTypeRegistered("QCanBusFrame"))
    {
        qRegisterMetaType<QCanBusFrame>("QCanBusFrame");
    }
    if (!isMetaTypeRegistered("QVector<QCanBusFrame>"))
    {
        qRegisterMetaType<QVector<QCanBusFrame>>("QVector<QCanBusFrame>");
    }
    if (!isMetaTypeRegistered("CANsubQt::BitTiming"))
    {
        qRegisterMetaType<BitTiming>();
    }
    if (!isMetaTypeRegistered("QHostAddress"))
    {
        qRegisterMetaType<QHostAddress>("QHostAddress");
    }

    /* The qmdnsengine signals name the type unqualified as "Message" in their signature, which Qt 5 uses to look up
     * the type of a queued argument. Register both names. */
    if (!isMetaTypeRegistered("QMdnsEngine::Message"))
    {
        qRegisterMetaType<QMdnsEngine::Message>("QMdnsEngine::Message");
    }
    if (!isMetaTypeRegistered("Message"))
    {
        qRegisterMetaType<QMdnsEngine::Message>("Message");
    }
}

} // namespace CANsubQt
