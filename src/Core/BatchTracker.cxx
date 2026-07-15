#include "BatchTracker.hxx"

BatchTracker::BatchTracker(int count, QObject* parent) :
    QObject(parent), pending(count), failed(false)
{

}

void BatchTracker::requestFinished(bool success)
{
    if (failed)
    {
        /* Already failed on another request, ignore subsequent successes */
        return;
    }

    if (!success)
    {
        failed = true;
        emit finishedWithErrors();
        return;
    }

    pending--;
    if (pending == 0)
    {
        emit finishedOk();
    }
}

void BatchTracker::requestFinishedOk()
{
    requestFinished(true);
}

void BatchTracker::requestFinishedError()
{
    requestFinished(false);
}
