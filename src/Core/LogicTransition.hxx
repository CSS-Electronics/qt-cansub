#ifndef CANSUB_QT_LOGICTRANSITION_HXX
#define CANSUB_QT_LOGICTRANSITION_HXX

#include <QAbstractTransition>

/**
 * Helper for QStateMachine to manually trigger a state transition on a condition from an external source
 */
class LogicTransition : public QAbstractTransition
{
public:
    explicit LogicTransition(QState* targetState);

    void trigger();

protected:
    bool eventTest(QEvent* e) override;

    void onTransition(QEvent* event) override;

private:
    bool eventTriggered;
};

#endif //CANSUB_QT_LOGICTRANSITION_HXX
