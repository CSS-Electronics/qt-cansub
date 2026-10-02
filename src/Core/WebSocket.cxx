#include "WebSocket.hxx"

#include <cstring>
#include <sstream>

#include <QCryptographicHash>
#include <QDebug>
#include <QRandomGenerator>
#include <boost/beast/http.hpp>
#include <boost/endian.hpp>

WebSocket::WebSocket(QObject* parent) : QObject(parent), tlsSocket(this), packetAssemblyBuffer(), sslConfiguration(QSslConfiguration::defaultConfiguration())
{
    /* Connect the wrapped sockets signals to the private slots */
    connect(&tlsSocket, &QSslSocket::encrypted, this, &WebSocket::onConnected);
    connect(&tlsSocket, &QSslSocket::disconnected, this, &WebSocket::onDisconnected);
    connect(&tlsSocket, &QSslSocket::readyRead, this, &WebSocket::onReadyRead);
    connect(&tlsSocket, &QSslSocket::errorOccurred, this, &WebSocket::onError);

    /* While perhaps not entirely correct from a semantics point of view, we forward the encrypted bytes emitted signal
     * as out bytes emitted signal. */
    connect(&tlsSocket, &QSslSocket::encryptedBytesWritten, this, &WebSocket::bytesWritten);

    /* Directly forward the TLS error signal */
    connect(&tlsSocket, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors), this, &WebSocket::sslErrors);
}

void WebSocket::abort()
{
    tlsSocket.abort();

    if (socketState != QAbstractSocket::UnconnectedState)
    {
        socketState = QAbstractSocket::UnconnectedState;
        emit disconnected();
    }
}

quint64 WebSocket::bytesToWrite() const
{
    return tlsSocket.encryptedBytesToWrite() + tlsSocket.bytesToWrite();
}

void WebSocket::close()
{
    if (socketState == QAbstractSocket::ConnectedState)
    {
        /* Enqueue a close frame */
        std::array<uint8_t, 6> closeFrame = { 0x88, 0x80, 0x00, 0x00, 0x00, 0x00};

        QRandomGenerator::system()->generate(closeFrame.begin() + 2, closeFrame.end());

        tlsSocket.write(reinterpret_cast<char const*>(closeFrame.data()), closeFrame.size());

        socketState = QAbstractSocket::ClosingState;
    }
    else if (socketState == QAbstractSocket::ClosingState)
    {
        /* Do nothing, we are in the process of closing */
    }
    else
    {
        /* Force-close now */
        abort();
    }
}

QString WebSocket::errorString() const
{
    return errorMessage.isEmpty() ? tlsSocket.errorString() : errorMessage;
}

void WebSocket::failConnection(QAbstractSocket::SocketError socketError, QString const& message)
{
    errorMessage = message;
    tlsSocket.abort();
    socketState = QAbstractSocket::UnconnectedState;
    emit error(socketError);
}

void WebSocket::flush()
{
    tlsSocket.flush();
}

void WebSocket::ignoreSslErrors()
{
    tlsSocket.ignoreSslErrors();
}

void WebSocket::ignoreSslErrors(QList<QSslError> const& errorsToIgnore)
{
    tlsSocket.ignoreSslErrors(errorsToIgnore);
}

bool WebSocket::isValid() const
{
    return socketState == QAbstractSocket::ConnectedState && tlsSocket.isValid();
}

void WebSocket::open(QUrl const &host)
{
    if (socketState != QAbstractSocket::UnconnectedState)
    {
        qWarning() << "WebSocket already open";
        return;
    }

    /* Create and configure new TLS socket and required auxiliary resources */
    tlsSocket.setSslConfiguration(sslConfiguration);
    tlsSocket.setPeerVerifyName(peerVerifyName);
    url = host;
    socketState = QAbstractSocket::ConnectingState;

    /* Perform TLS handshake */
    if (url.path().isEmpty())
    {
        url.setPath("/");
    }
    if (url.port() < 0)
    {
        url.setPort(443);
    }

    tlsSocket.connectToHostEncrypted(url.host(), url.port());
}

void WebSocket::onConnected()
{
    /* Manually perform the HTTP upgrade call. Report state back in connect/error signals. */
    QByteArray resource = url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority | QUrl::RemoveFragment);
    if (resource.isEmpty())
    {
        resource = "/";
    }

    quint64 nonce[2];
    QRandomGenerator::system()->fillRange(nonce);
    handshakeKey = QByteArray(reinterpret_cast<char const*>(nonce), sizeof(nonce)).toBase64();
    bufferRx.clear();
    responseParser.emplace();

    /* Generate and encode request */
    boost::beast::http::request<boost::beast::http::empty_body> request {boost::beast::http::verb::get, resource.toStdString(), 11};

    std::stringstream hostStream;
    hostStream << url.host().toStdString();
    if (url.port() != 443)
    {
        hostStream << ":" << url.port();
    }
    request.set(boost::beast::http::field::host, hostStream.str());
    request.set(boost::beast::http::field::upgrade, "websocket");
    request.set(boost::beast::http::field::connection, "Upgrade");
    request.set(boost::beast::http::field::sec_websocket_key, handshakeKey.toStdString());
    request.set(boost::beast::http::field::sec_websocket_version, "13");

    std::ostringstream out;
    out << request;
    auto const outStr = out.str();
    tlsSocket.write(outStr.c_str(), outStr.size());
}

void WebSocket::onDisconnected()
{
    socketState = QAbstractSocket::UnconnectedState;
    emit disconnected();
}

void WebSocket::onError(QAbstractSocket::SocketError socketError)
{
    errorMessage.clear();
    tlsSocket.abort();
    socketState = QAbstractSocket::UnconnectedState;
    emit error(socketError);
}

void WebSocket::onReadyRead()
{
    qint64 bytesReadFromSocket = 0;

    do
    {
        /* Read as many bytes as possible from the stream */
        auto const availableBufferSpace = bufferRx.max_size() - bufferRx.size();
        auto const toRequest = std::min(availableBufferSpace, static_cast<std::size_t>(tlsSocket.bytesAvailable()));

        auto bufferData = bufferRx.prepare(toRequest);
        bytesReadFromSocket = tlsSocket.read(reinterpret_cast<char *>(bufferData.data()), static_cast<qint64>(bufferData.size()));
        if (bytesReadFromSocket < 0)
        {
            /* Error */
            failConnection(QAbstractSocket::UnknownSocketError, "TLS socket error");
            return;
        }
        bufferRx.commit(bytesReadFromSocket);

        if (socketState == QAbstractSocket::ConnectingState)
        {
            /* We expect this to be part of our HTTP response */
            boost::beast::error_code ec;
            std::size_t const parsed = responseParser->put(bufferRx.cdata(), ec);
            bufferRx.consume(parsed);

            if (ec == boost::beast::http::error::need_more)
            {
                /* Not enough data yet, wait (unless our buffer is too small, which should never happen) */
                if ((bufferRx.max_size() - bufferRx.size()) == 0)
                {
                    failConnection(QAbstractSocket::UnknownSocketError, "HTTP buffer error");
                    return;
                }
                continue;
            }
            if (ec || !responseParser->is_header_done())
            {
                /* An error has occurred or the header is not done, but we have gotten all the data we expect (per above) */
                failConnection(QAbstractSocket::UnknownSocketError, "HTTP response error");
                return;
            }

            /* Calculate the expected WebSocket response */
            QByteArray const expectedAccept = QCryptographicHash::hash(handshakeKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();

            auto const &response = responseParser->get();
            if (response.result() != boost::beast::http::status::switching_protocols
                || !boost::beast::iequals(response[boost::beast::http::field::upgrade], "websocket")
                || response[boost::beast::http::field::sec_websocket_accept] != expectedAccept.toStdString())
            {
                failConnection(QAbstractSocket::UnknownSocketError, "Invalid WebSocket upgrade response");
                return;
            }

            /* Switch to low-latency */
            tlsSocket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
            tlsSocket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);

            /* We are now connected. Emit the connected call and soft-emit the data signal if there was
             * any pending data in the stream. */
            socketState = QAbstractSocket::ConnectedState;
            emit connected();
        }

        /* Read remaining data as WebSocket frames */
        while ((bufferRx.size() >= 2) && ((socketState == QAbstractSocket::ConnectedState) || (socketState == QAbstractSocket::ClosingState)))
        {
            auto buffer = bufferRx.cdata();
            auto const *const base = reinterpret_cast<uint8_t const*>(buffer.data());
            uint64_t requiredFrameSize = 0;
            uint64_t frameSize = 0;

            /* Sanity check */
            if (base[1] & 0x80)
            {
                /* This frame is masked, however only traffic from client to the server should be masked. */
                failConnection(QAbstractSocket::UnknownSocketError, "Received masked WebSocket frame from server");
                return;
            }

            /* Decode the frame length, and ensure we have all the bytes available */
            if ((base[1] & 0x7F) < 126)
            {
                /* Short frame */
                requiredFrameSize = 2;
                frameSize = base[1] & 0x7F;
            }
            else if((base[1] & 0x7F) == 126)
            {
                if (buffer.size() < 4)
                {
                    /* Not enough data */
                    break;
                }

                requiredFrameSize = 2 + 2;
                frameSize = boost::endian::load_big_u16(base + 2);
            }
            else
            {
                if (buffer.size() < 10)
                {
                    /* Not enough data */
                    break;
                }

                requiredFrameSize = 2 + 8;
                frameSize = boost::endian::load_big_u64(base + 2);
            }

            /* NOTE: We have the first check to guard against wrapping counters in case requiredFrameSize would make the
             *       variable wrap-around. */
            if ((frameSize > bufferRx.capacity()) || ((requiredFrameSize + frameSize) > bufferRx.capacity()))
            {
                failConnection(QAbstractSocket::UnknownSocketError, "Too large a WebSocket frame received");
                return;
            }

            if (buffer.size() < (requiredFrameSize + frameSize))
            {
                /* Not enough data yet */
                break;
            }

            /* There is never a mask on the data from the server */
            auto const* const data = base + requiredFrameSize;

            uint8_t const frameType = base[0] & 0x0F;
            if (frameType == 0x08)
            {
                /* WebSocket close frame */
                if (socketState == QAbstractSocket::ClosingState)
                {
                    /* This is a response to our close */
                    socketState = QAbstractSocket::UnconnectedState;
                    tlsSocket.close();
                }
                else
                {
                    /* Emit a close to echo this, then disconnect the socket. */
                    close();
                    tlsSocket.disconnectFromHost();
                }
            }
            else if (frameType == 0x02)
            {
                /* Binary frame */
                emit binaryMessageReceived(QByteArray(reinterpret_cast<char const*>(data), static_cast<int>(frameSize)));
            }
            else
            {
                qWarning() << "Unsupported WebSocket frame type" << frameType;
            }

            bufferRx.consume(requiredFrameSize + frameSize);
        }
    } while ((bytesReadFromSocket > 0) && (socketState != QAbstractSocket::UnconnectedState));
}

qint64 WebSocket::sendBinaryMessage(QByteArray const& data)
{
    if (!isValid())
    {
        return -1;
    }

    /* We support the two compact frames, with the length encoded in the first byte or the first 2 bytes.
     * Anything larger will have to be split. */
    size_t offset = 0;
    bool first = true;

    /* Access helpers */
    auto *const base = reinterpret_cast<uint8_t* const>(packetAssemblyBuffer.data());
    auto *const mask = base + 4;
    auto *const payload = base + 8;
    auto *const payloadWords = packetAssemblyBuffer.data() + 2;

    do
    {
        /* How much data can we put into this frame? */
        auto const frame_size = std::min(static_cast<size_t>(data.size()) - offset, static_cast<size_t>(UINT16_MAX));
        auto const frame_size_words = frame_size / sizeof(uint32_t);
        bool const last = offset + frame_size == static_cast<size_t>(data.size());

        /* Write the correct header */
        auto * header = mask;
        if (frame_size < 126)
        {
            header -= 2;
            header[1] = 0x80 | static_cast<uint8_t>(frame_size);
        }
        else
        {
            header -= 4;
            header[1] = 0x80 | 126;
            boost::endian::store_big_u16(header + 2, static_cast<std::uint16_t>(frame_size));
        }

        header[0] = (first ? 0x02 : 0x00) | (last ? 0x80 : 0x00);

        /* Generate a random masking key */
        quint32 const maskingKey = QRandomGenerator::system()->generate();
        memcpy(mask, &maskingKey, sizeof(maskingKey));

        /* Copy over all data */
        memcpy(payloadWords, data.constData() + offset, frame_size);

        /* Mask data word by word */
        for (size_t i = 0; i < frame_size_words; ++i)
        {
            payloadWords[i] ^= maskingKey;
        }

        /* Handle the (potential) trailing bytes */
        for (size_t i = frame_size_words * sizeof(uint32_t); i < frame_size; ++i)
        {
            payload[i] ^= mask[i & 3];
        }

        /* Enqueue */
        auto const bytesToWriteToSocket = payload + frame_size - header;
        if (tlsSocket.write(reinterpret_cast<char const*>(header), bytesToWriteToSocket) != bytesToWriteToSocket)
        {
            return -1;
        }

        offset += frame_size;
        first = false;
    } while (offset < static_cast<size_t>(data.size()));

    return data.size();
}

void WebSocket::setPeerVerifyName(QString const& name)
{
    peerVerifyName = name;
}

void WebSocket::setSslConfiguration(QSslConfiguration const& config)
{
    /* Cache the configuration here until we actually perform the connect call */
    sslConfiguration = config;
}

QAbstractSocket::SocketState WebSocket::state() const
{
    return socketState;
}
