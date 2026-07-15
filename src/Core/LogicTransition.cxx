#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtStateMachine/QStateMachine>
#else
#include <QStateMachine>
#endif

#include "LogicTransition.hxx"

LogicTransition::LogicTransition(QState* targetState) : eventTriggered(false)
{
    setTargetState(targetState);
}

void LogicTransition::trigger()
{
    /* NOTE: This is never reset, because this is intended to be used as a one-shot trigger. */
    eventTriggered = true;

    /* Post a dummy event to the machine to force an evaluation of transitions */
    if (QStateMachine* m = machine())
    {
        m->postEvent(new QEvent(QEvent::None));
    }
}

bool LogicTransition::eventTest(QEvent* e)
{
    Q_UNUSED(e);

    return eventTriggered;
}

void LogicTransition::onTransition(QEvent* event)
{
    Q_UNUSED(event);
}
