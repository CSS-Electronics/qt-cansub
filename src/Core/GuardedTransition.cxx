#include "GuardedTransition.hxx"

GuardedTransition::GuardedTransition(QState* sourceState, std::function<bool(QList<QVariant> args)> callback) :
    QSignalTransition(sourceState), callback(callback)
{
    //
}

GuardedTransition::GuardedTransition(QObject const* sender, const char* signal, QState* sourceState,
                                     std::function<bool(QList<QVariant> args)> callback) :
    QSignalTransition(sender, signal, sourceState), callback(callback)
{
    //
}

bool GuardedTransition::eventTest(QEvent* event)
{
    if (!QSignalTransition::eventTest(event))
    {
        return false;
    }
    auto* se = dynamic_cast<QStateMachine::SignalEvent*>(event);
    if (!se)
    {
        return false;
    }

    return callback(se->arguments());
}
