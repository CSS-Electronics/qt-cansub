#ifndef CANSUB_QT_RESOLVERFACTORY_HXX
#define CANSUB_QT_RESOLVERFACTORY_HXX

#include <QByteArray>

#include "HostResolver.hxx"

/**
 * Abstract factory for HostResolver instances.
 *
 * CANsub depends on a resolver to speed up connections. With this, a plugin can provide a cached resolver.
 */
struct ResolverFactory
{
    virtual ~ResolverFactory() = default;

    /**
     * Create a resolver instance
     * @param name Name of the resolver
     * @param parent Parent if in the same thread, cross-related lifetimes if in different threads
     * @return Result or nullptr on error
     */
    virtual HostResolver* createResolver(QByteArray const& name, QObject* parent) const = 0;
};

#endif // CANSUB_QT_RESOLVERFACTORY_HXX
