#include "MdnsHostResolver.hxx"

MdnsHostResolver::MdnsHostResolver(QMdnsEngine::AbstractServer* server, QByteArray const& name, QMdnsEngine::Cache* cache, QObject* parent)
    : HostResolver(parent), inner(server, name, cache, nullptr)
{
    connect(&inner, &QMdnsEngine::Resolver::resolved, this, &HostResolver::resolved);
}
