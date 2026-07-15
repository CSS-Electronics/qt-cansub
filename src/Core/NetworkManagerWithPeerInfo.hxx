#ifndef CANSUB_QT_NETWORKMANAGERWITHPEERINFO_HXX
#define CANSUB_QT_NETWORKMANAGERWITHPEERINFO_HXX

#include <QNetworkAccessManager>

#include "TlsPolicy.hxx"

/**
 * Override of the default QNetworkAccessManager to allow for injecting a target hostname to validate the remote TLS
 * certificate against, even when connecting via IP.
 */
class NetworkManagerWithPeerInfo : public QNetworkAccessManager
{
    Q_OBJECT;
public:
    explicit NetworkManagerWithPeerInfo(QObject* parent = nullptr);

    /**
     * Set the TLS identity-verification policy applied to every request.
     * @param policy The resolved TLS policy to apply to subsequent requests
     */
    void setTlsPolicy(TlsPolicy const& policy);

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& originalRequest, QIODevice* outgoingData) override;

private:
    TlsPolicy policy;
};

#endif //CANSUB_QT_NETWORKMANAGERWITHPEERINFO_HXX
