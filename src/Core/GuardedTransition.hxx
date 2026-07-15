#ifndef CANSUB_QT_GUARDEDTRANSITION_HXX
#define CANSUB_QT_GUARDEDTRANSITION_HXX

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtStateMachine/QStateMachine>
#include <QtStateMachine/QState>
#include <QtStateMachine/QFinalState>
#include <QtStateMachine/QSignalTransition>
#else
#include <QStateMachine>
#include <QState>
#include <QFinalState>
#include <QSignalTransition>
#endif

/**
 * Helper for QStateMachine to trigger a transition on a signal if a condition is fulfilled
 */
class GuardedTransition : public QSignalTransition
{
    Q_OBJECT;

public:
    GuardedTransition(QState* sourceState, std::function<bool(QList<QVariant> args)> callback);
    GuardedTransition(
        QObject const* sender,
        const char* signal,
        QState* sourceState,
        std::function<bool(QList<QVariant> args)> callback
        );

protected:
    bool eventTest(QEvent* event) override;

private:
    std::function<bool(QList<QVariant> args)> callback;
};

#endif // CANSUB_QT_GUARDEDTRANSITION_HXX
