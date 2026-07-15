#include "Certificate.hxx"

#include <QSslCertificate>
#include <QtGlobal>

/* The embedded certificate resource is compiled into this (Core) library. Because Core is consumed as an OBJECT
 * library rather than being part of the final target directly, the resource's auto-registration initializer can be
 * discarded by the linker unless something references it. Q_INIT_RESOURCE forces that reference.
 *
 * Q_INIT_RESOURCE must be invoked from a function at global namespace scope as it declares an extern symbol that lives
 * in the global namespace, so calling it from inside a namespace (including an anonymous one) would reference the
 * wrong symbol. Hence, this small file-local free function. */
static void cansubInitCoreResources()
{
    Q_INIT_RESOURCE(resources);
}

QSslConfiguration &coreSslConfiguration()
{
    static QSslConfiguration config = []
    {
        cansubInitCoreResources();

        QSslConfiguration c = QSslConfiguration::defaultConfiguration();

        auto cert = QSslCertificate::fromPath(QStringLiteral(":/cansub_root_cert.crt"));
        if (cert.empty())
        {
            qCritical() << "Failed to load embedded certificate";
        }
        c.setCaCertificates(cert);

        return c;
    }();

    return config;
}
