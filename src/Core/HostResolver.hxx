#ifndef CANSUB_QT_HOSTRESOLVER_HXX
#define CANSUB_QT_HOSTRESOLVER_HXX

#include <QHostAddress>
#include <QObject>

/**
 * Interface to decouple the plugin core logic from the plugin itself.
 */
class HostResolver : public QObject
{
    Q_OBJECT;

public:
    explicit HostResolver(QObject* parent = nullptr);

    ~HostResolver() override = default;

signals :
    /**
     * Emitted once for each resolved address
     * @param address Resolved address
     */
    void resolved(QHostAddress const& address);
};

#endif // CANSUB_QT_HOSTRESOLVER_HXX
