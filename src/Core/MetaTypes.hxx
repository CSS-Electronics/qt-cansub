#ifndef CANSUB_QT_META_TYPES_HXX
#define CANSUB_QT_META_TYPES_HXX

namespace CANsubQt
{

/**
 * Register the metatypes the binding passes through queued signals, possibly across threads, unless they are already
 * registered:
 * - QCanBusFrame
 * - QVector<QCanBusFrame>
 * - CANsubQt::BitTiming
 * - QHostAddress
 * - QMdnsEngine::Message, also under its unqualified name "Message"
 *
 * Called by the plugin when it is loaded. Safe to call more than once.
 */
void registerMetaTypes();

} // namespace CANsubQt

#endif // CANSUB_QT_META_TYPES_HXX
