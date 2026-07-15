#include "NetworkManagerWithPeerInfo.hxx"

#include <QSslConfiguration>
#include <QSslSocket>

NetworkManagerWithPeerInfo::NetworkManagerWithPeerInfo(QObject* parent) :
    QNetworkAccessManager(parent)
{
    //
}

void NetworkManagerWithPeerInfo::setTlsPolicy(TlsPolicy const& policy)
{
    this->policy = policy;
}

QNetworkReply* NetworkManagerWithPeerInfo::createRequest(Operation op, const QNetworkRequest& originalRequest, QIODevice* outgoingData)
{
    QNetworkRequest request = originalRequest;

    if (policy.disableValidation)
    {
        /* Validation disabled. Do not verify the remote identity at all. */
        QSslConfiguration sslConfig = request.sslConfiguration();
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(sslConfig);
    }
    else
    {
        /* Verify the certificate against the resolved verification hostname rather than the connection host (which
         * is the resolved IP). */
        request.setPeerVerifyName(policy.verificationHostname);
    }

    /* Forward to base class */
    return QNetworkAccessManager::createRequest(op, request, outgoingData);
}
