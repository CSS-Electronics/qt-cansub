#ifndef CANSUB_QT_BATCHTRACKER_HXX
#define CANSUB_QT_BATCHTRACKER_HXX

#include <QObject>

/**
 * Helper class used to track n signals.
 *
 * Instantiate with the expected number of events and wire the slots to the signal sources. Provides a single signal
 * which can be used for both success/error and dedicated signals for each case.
 */
class BatchTracker : public QObject
{
    Q_OBJECT;

public:
    explicit BatchTracker(int count, QObject* parent);

public slots:
    void requestFinished(bool success);
    void requestFinishedOk();
    void requestFinishedError();

signals:
    void finishedOk();
    void finishedWithErrors();

private:
    int pending;
    bool failed;
};

#endif //CANSUB_QT_BATCHTRACKER_HXX
