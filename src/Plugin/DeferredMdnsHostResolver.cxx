#include "DeferredMdnsHostResolver.hxx"

DeferredMdnsHostResolver::DeferredMdnsHostResolver(QMdnsEngine::AbstractServer* server, QByteArray const& name, QMdnsEngine::Cache* cache, QObject* parent) : HostResolver(parent), state(std::make_shared<State>())
{
    /* Create a local copy of the shared pointer to use in the queud lambda */
    auto const s = state;
    auto *const self = this;

    /* Queue the resolver. The context is set to the server, so if the server dies first the lambda is simply dropped.
     * Assumes the consumer has a timeout path. */
    QMetaObject::invokeMethod(server, [s, self, server, name, cache]()
    {
        /* Executes in the server's thread */
        QMutexLocker const lock(&s->mutex);
        if (s->proxyDead)
        {
            return;
        }

        /* Ensure we have any new interface discovered here */
        server->forceRefresh();

        /* Parent the inner resolver to the server. If the server is destroyed while the proxy still holds a pending
         * resolver, the inner instance (and its retry timer, which dereferences the server) must die with it. */
        s->inner = new QMdnsEngine::Resolver(server, name, cache, server);

        /* Track the inner resolver's destruction (parent-driven or via the proxy's deleteLater), so the proxy
         * destructor never touches a pointer the server already deleted. The context-less connection lives exactly
         * as long as the inner resolver and runs in the thread destroying it. */
        connect(s->inner, &QObject::destroyed, [s]()
        {
            QMutexLocker const innerLock(&s->mutex);
            s->inner = nullptr;
        });

        /* Signal-to-signal forward. Emission happens on the server's thread, consumer's connection to the proxy is
         * auto → queued into its own thread, same delivery semantics as the direct MdnsHostResolver. */
        connect(s->inner, &QMdnsEngine::Resolver::resolved, self, &HostResolver::resolved);
    }, Qt::QueuedConnection);
}

DeferredMdnsHostResolver::~DeferredMdnsHostResolver()
{
    QMutexLocker const lock(&state->mutex);
    state->proxyDead = true;
    if (state->inner)
    {
        state->inner->deleteLater();
        state->inner = nullptr;
    }
}
