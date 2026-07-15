#ifndef CANSUB_QT_CERTIFICATE_HXX
#define CANSUB_QT_CERTIFICATE_HXX

#include <QSslConfiguration>

/**
 * Access the shared SSL configuration used for all communication with the CANsub remote (both the REST API and the
 * WebSocket). The configuration trusts only the embedded CANsub root certificate.
 *
 * The configuration is backed by an object of static storage duration, so the returned reference stays valid for the
 * entire lifetime of the program.
 */
QSslConfiguration& coreSslConfiguration();

#endif //CANSUB_QT_CERTIFICATE_HXX
