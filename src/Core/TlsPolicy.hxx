#ifndef CANSUB_QT_TLSPOLICY_HXX
#define CANSUB_QT_TLSPOLICY_HXX

#include <QString>

/**
 * Settings structure for TLS verification
 */
struct TlsPolicy
{
    /* Hostname the remote certificate must be valid for. When connecting by hostname this is the hostname itself,
     * when connecting by IP it is the user-supplied expected hostname. Empty when validation is disabled. */
    QString verificationHostname;

    /* When true the remote identity is not verified at all. The connection stays encrypted but unauthenticated. */
    bool disableValidation = false;
};

#endif // CANSUB_QT_TLSPOLICY_HXX
