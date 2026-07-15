#ifndef CANSUB_QT_MDNSHOSTRESOLVER_HXX
#define CANSUB_QT_MDNSHOSTRESOLVER_HXX

#include "qmdnsengine/abstractserver.h"
#include "qmdnsengine/cache.h"
#include "qmdnsengine/resolver.h"

#include "HostResolver.hxx"

/**
 * Production HostResolver: A thin adapter that owns a QMdnsEngine::Resolver and forwards its resolved() signal.
 * Lives in the plugin as it owns the shared server/cache.
 */
class MdnsHostResolver : public HostResolver
{
    Q_OBJECT

public:
    MdnsHostResolver(QMdnsEngine::AbstractServer* server, QByteArray const& name, QMdnsEngine::Cache* cache, QObject* parent);

private:
    QMdnsEngine::Resolver inner;
};

#endif // CANSUB_QT_MDNSHOSTRESOLVER_HXX
