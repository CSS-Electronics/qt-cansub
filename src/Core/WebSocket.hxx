#ifndef CANSUB_QT_WEBSOCKET_HXX
#define CANSUB_QT_WEBSOCKET_HXX

#include <array>
#include <cstdint>
#include <optional>

#include <QObject>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/http.hpp>

class WebSocket : public QObject
{
    Q_OBJECT

public:
    explicit WebSocket(QObject* parent = nullptr);

    void setSslConfiguration(QSslConfiguration const& config);
    void setPeerVerifyName(QString const& name);
    void open(QUrl const &host);
    void flush();
    [[nodiscard]] quint64 bytesToWrite() const;

    void abort();
    void close();
    [[nodiscard]] QAbstractSocket::SocketState state() const;
    [[nodiscard]] bool isValid() const;

    void ignoreSslErrors();
    void ignoreSslErrors(QList<QSslError> const& errorsToIgnore);
    [[nodiscard]] QString errorString() const;
    qint64 sendBinaryMessage(QByteArray const& data);

signals:
    void connected();
    /* Do *not* call open synchronously from this signal handler */
    void disconnected();
    void sslErrors(QList<QSslError> errors);
    void binaryMessageReceived(QByteArray const &data);
    void bytesWritten(quint64 bytes);
    /* Do *not* call open synchronously from this signal handler */
    void error(QAbstractSocket::SocketError socketError);

private:
    QSslSocket tlsSocket;
    QAbstractSocket::SocketState socketState = QAbstractSocket::UnconnectedState;
    QString errorMessage;

    /* For the connection */
    QUrl url;
    QByteArray handshakeKey;
    std::optional<boost::beast::http::response_parser<boost::beast::http::empty_body>> responseParser;
    QSslConfiguration sslConfiguration;
    QString peerVerifyName;

    boost::beast::flat_static_buffer<100 * 1024> bufferRx;
    std::array<uint32_t, (UINT16_MAX + 8 + sizeof(uint32_t) - 1) / sizeof(uint32_t)> packetAssemblyBuffer;

private slots:
    void failConnection(QAbstractSocket::SocketError socketError, QString const &message);
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError socketError);
    void onReadyRead();
};

#endif // CANSUB_QT_WEBSOCKET_HXX
