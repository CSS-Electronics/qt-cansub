#ifndef CANSUB_QT_DEFERREDMDNSHOSTRESOLVER_HXX
#define CANSUB_QT_DEFERREDMDNSHOSTRESOLVER_HXX

#include <memory>
#include <QMutex>

#include "qmdnsengine/abstractserver.h"
#include "qmdnsengine/cache.h"
#include "qmdnsengine/resolver.h"

#include "HostResolver.hxx"

/**
 * Production HostResolver: A thin adapter that owns a QMdnsEngine::Resolver and forwards its resolved() signal.
 * Lives in the plugin as it owns the shared server/cache.
 * Similar to the MdnsHostResolver, but allows the actions to be deferred to avoid deadlocks when threading is involved.
 */
class DeferredMdnsHostResolver : public HostResolver
{
    Q_OBJECT;

public:
    /* Must be constructed in the thread that will own the proxy (the caller of createResolver) */
    DeferredMdnsHostResolver(QMdnsEngine::AbstractServer* server, QByteArray const& name,
                             QMdnsEngine::Cache* cache, QObject* parent);
    ~DeferredMdnsHostResolver() override;

private:
    /* Inner shared state */
    struct State
    {
        QMutex mutex;
        bool proxyDead = false;
        QMdnsEngine::Resolver* inner = nullptr; /* parented to the server, nulled by its destroyed() hook */
    };
    std::shared_ptr<State> state;
};

#endif // CANSUB_QT_DEFERREDMDNSHOSTRESOLVER_HXX
