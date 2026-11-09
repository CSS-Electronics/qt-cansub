#ifndef CANSUB_QT_CANSUB_CONNECTION_HXX
#define CANSUB_QT_CANSUB_CONNECTION_HXX

#include <functional>
#include <list>
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "cansub/cansub_ffi.h"

class QTcpSocket;

namespace CANsubQt
{

/**
 * Holds one successful cansub_library_init() for its lifetime.
 *
 * The API conventions in cansub_ffi.h require every caller to hold its own initialization while validating options or
 * creating contexts, and until all of its contexts are destroyed.
 */
class LibraryReference
{
public:
    LibraryReference();
    ~LibraryReference();

    LibraryReference(LibraryReference const &) = delete;
    LibraryReference &operator=(LibraryReference const &) = delete;

    /** Result of cansub_library_init(). */
    cansub_result result() const { return initResult; }

    /** True if the library is initialized and this object balances it on destruction. */
    bool isInitialized() const { return initResult == CANSUB_RESULT_OK; }

private:
    cansub_result initResult;
};

/**
 * Translated description of a library result code, for error messages shown to the user.
 *
 * Covers the CONNECTION_* codes in particular, e.g. "The TLS handshake with the device failed". Unknown codes give a
 * generic text with the code name from cansub_result_name().
 */
QString resultDescription(cansub_result result);

/**
 * The glue between one CANsub library context (cansub_ctx) and Qt sockets.
 *
 * Owns the context, a library initialization and up to two raw TCP sockets, one per stream (the library does the TLS).
 * Drives the context with the ready flags as described in cansub_ffi.h ("Driving a context"), bounds every wait with
 * the timeouts of "Timeouts" in cansub_ffi.h, and enables TCP keep-alive on both sockets.
 *
 * A context cannot be reopened once closed, so the device creates a new connection for every open().
 *
 * Lives in the thread it was created in, which must run an event loop. Not thread-safe.
 *
 * Re-entrancy: The context is only driven from a queued call, never directly from within a socket signal or a public
 * method, so the signals are only emitted from the event loop. A slot may call any public method, and may delete the
 * connection directly.
 */
class Connection : public QObject
{
    Q_OBJECT

public:
    /** Options of the connection. Strings are ASCII. */
    struct Options
    {
        /** Host as given by the user. Passed to the library for the TLS server name and the HTTP Host header. */
        QByteArray host;
        /** Resolved IPv4 address the sockets connect to. Not checked by validate(). */
        QHostAddress address;
        /** TCP port of the device. */
        quint16 port = 443;
        /** CAN channel on the device, the first is 1. */
        quint32 channel = 0;
        /** Device ID (8 hex digits) to verify the device certificate against, or empty. */
        QByteArray deviceId;
        /** Only verify the root certificate. Ignored if deviceId is set. */
        bool acceptAnyDevice = false;
        /** Client certificate for mutual TLS exactly as passed to the library (PEM including the NUL terminator, or
         * DER), or empty. */
        QByteArray clientCertificate;
        /** Unencrypted client key for mutual TLS, in the same form as clientCertificate, or empty. */
        QByteArray clientKey;
    };

    /** Response to a request. */
    struct Response
    {
        /**
         * Outcome of the request:
         * - 0 or positive: A response arrived. Check httpStatus.
         * - CONNECTION_*: The request failed, see cansub_response_recv().
         * - CANSUB_RESULT_CLOSED: The connection closed before the request could be queued in the library.
         * - CANSUB_RESULT_INVALID_ARG: The library rejected the request, e.g. an invalid path.
         * - CANSUB_RESULT_ERROR: An internal library error ended the connection.
         */
        cansub_result result = CANSUB_RESULT_OK;
        /** HTTP status code, 0 if the request failed. */
        quint32 httpStatus = 0;
        /** Response body. Error bodies are plain text or empty, whatever the content type. */
        QByteArray body;
    };

    using ResponseHandler = std::function<void(Response const &response)>;

    /**
     * Validate options with cansub_context_options_validate(), without creating a context. The address is not checked.
     *
     * Holds its own library initialization for the call.
     *
     * @param options Options to validate.
     * @param[out] errorMessage If not NULL, set on failure to a translated description of every reported fault, one
     *             sentence each. Library initialization and memory failures are described too.
     * @return True if the options are valid.
     */
    static bool validate(Options const &options, QString *errorMessage);

    /**
     * Create a connection with a new library context. Nothing is connected yet.
     *
     * @return The connection, or nullptr with errorMessage set (if not NULL) to a translated reason, as for validate().
     */
    static Connection *create(Options const &options, QObject *parent, QString *errorMessage);

    /**
     * Destroys the context immediately: No more data is exchanged, open sockets are aborted and no signal is emitted.
     * Handlers of outstanding requests are not called. Use close() for an orderly close.
     */
    ~Connection() override;

    /**
     * Queue a REST request on the control stream. The control transport is opened as needed.
     *
     * The handler is called exactly once, never from within request(), unless the connection is destroyed first.
     * Handlers are called in request order, and every handler is called before closed() is emitted.
     *
     * A "409 Conflict" is retried internally a few times after a short, growing and partly random delay, as the device
     * rejects overlapping requests to the same endpoint (also from other channels or clients). The handler only sees
     * the final response.
     *
     * Requests are accepted until close() is called. Later requests fail with CANSUB_RESULT_CLOSED.
     *
     * @param method HTTP method, e.g. "GET".
     * @param path Absolute path, e.g. "/api/version".
     * @param body Request body. If not empty, it is sent with the content type "application/json".
     * @param handler Called with the response.
     */
    void request(QByteArray const &method, QByteArray const &path, QByteArray const &body, ResponseHandler handler);

    /**
     * Start opening the data stream (the WebSocket of the channel) with cansub_open().
     *
     * Check the remote API version and configure the device before calling this. opened() is emitted once the
     * WebSocket is up, or closed() if it fails or times out.
     *
     * @return False if the library refused to open, e.g. the connection is already closing.
     */
    bool open();

    /**
     * Start an orderly close with cansub_close(). Does nothing if already closing.
     *
     * Escalates by itself: cansub_close_force() if the device does not confirm the close in time (it only confirms
     * once its queued CAN frames are on the bus), and ending the transports if the abort does not complete either.
     * closed() is always emitted afterwards, never from within close().
     */
    void close();

    /**
     * Queue a CAN frame for transmission with cansub_frame_send().
     *
     * @return The library result: CANSUB_RESULT_OK if queued. CANSUB_RESULT_AGAIN if the library has no space, wait
     *         for readyToSend(). CANSUB_RESULT_INVALID_ARG for a malformed frame (see cansub_frame_validate()).
     *         CANSUB_RESULT_CLOSED if the connection is closing.
     */
    cansub_result sendFrame(cansub_frame const &frame);

    /** True while cansub_frame_send() accepts a frame (CANSUB_READY_CAN_TX). */
    bool canSend() const;

    /** Current state of the context, see cansub_state_code. */
    cansub_state state() const;

signals:
    /** The WebSocket is up (CANSUB_STATE_OPEN). CAN frames can flow. */
    void opened();

    /** Frames were received, in bus order. Includes transmit acknowledgements and error frames. */
    void framesReceived(QVector<cansub_frame> const &frames);

    /** cansub_frame_send() accepts frames again after it returned CANSUB_RESULT_AGAIN. */
    void readyToSend();

    /**
     * The connection has ended (CANSUB_STATE_CLOSED, or the context was abandoned after the close escalation).
     * Emitted exactly once. The connection does nothing afterwards and may be deleted.
     *
     * @param error Recorded error of the data stream from cansub_error(). CANSUB_RESULT_OK for a clean close.
     * @param message Translated description for the user. Empty for a clean close.
     */
    void closed(cansub_result error, QString const &message);

private:
    explicit Connection(Options const &options, QObject *parent);

    /* Where a request is. Queued and RetryWait requests are only known to this class, Sent ones to the library. */
    enum class RequestPhase
    {
        Queued,
        Sent,
        RetryWait,
        Done,
    };

    /* One call of request(). Kept in request order until its handler has been scheduled. */
    struct RequestEntry
    {
        quint64 id = 0;
        QByteArray method;
        QByteArray path;
        QByteArray body;
        ResponseHandler handler;
        RequestPhase phase = RequestPhase::Queued;
        /* Tag of the current attempt in the library, unique per attempt. */
        quint64 tag = 0;
        int attempts = 0;
        /* Final response once Done, or the last "409 Conflict" while waiting for a retry. */
        Response response;
    };

    /* Where the transport of a stream is. */
    enum class TransportPhase
    {
        /* No transport and no socket. TRANSPORT_OPEN starts one. */
        Idle,
        /* The socket connects. TRANSPORT_OPEN is not answered yet. */
        Connecting,
        /* cansub_data_connected() succeeded. Ends with exactly one cansub_data_eof(). */
        Connected,
    };

    /* The transport of one stream. */
    struct Transport
    {
        QTcpSocket *socket = nullptr;
        TransportPhase phase = TransportPhase::Idle;
        /* Connecting: The TCP connection is up. */
        bool tcpConnected = false;
        /* Connected: The remote closed the connection. The received bytes stay readable in the socket until sunk. */
        bool remoteClosed = false;
        /* End the transport without draining at the next pump (socket error or timeout). */
        bool abortRequested = false;
    };

    /* Escalation of a close, see "Timeouts" in cansub_ffi.h. */
    enum class CloseStage
    {
        None,
        Graceful,
        Forced,
        EndingTransports,
    };

    void schedulePump();
    void pump();
    bool pumpPass();
    bool pumpPendingCalls();
    bool pumpTransportAborts();
    bool pumpTransport(cansub_stream stream);
    bool pumpDrain(cansub_stream stream);
    bool pumpSink(cansub_stream stream);
    bool pumpFrames();
    bool pumpResponses();
    bool pumpRequests();
    bool pumpRemoteClosedTransports();
    void pumpState();
    void updateRequestTimers();

    void startTransport(cansub_stream stream);
    void endTransport(cansub_stream stream);
    void closeTransportGracefully(cansub_stream stream);
    void retireTransport(cansub_stream stream, bool graceful);

    void onSocketConnected(cansub_stream stream, QTcpSocket *socket);
    void onSocketDisconnected(cansub_stream stream, QTcpSocket *socket);
    void onSocketError(cansub_stream stream, QTcpSocket *socket);
    void onDataOpenTimeout();
    void onControlOpenTimeout();
    void onRequestTimeout();
    void onCloseTimeout();
    void onRequestRetry(quint64 id);

    bool submitRequest(RequestEntry &entry);
    void handleResponse(quint64 tag, quint32 httpStatus, cansub_result result, QByteArray const &body);
    /* Keeps entry.response, the last "409 Conflict", as the final response */
    void completeRequest(RequestEntry &entry);
    void completeRequest(RequestEntry &entry, Response const &response);
    void completeRequest(RequestEntry &entry, cansub_result result);
    int outstandingRequests() const;

    bool isFatal(cansub_result result, char const *call);
    void finishClosed();
    void finish(cansub_result sentFailure, cansub_result queuedFailure, cansub_result error, QString const &message);
    void stopTimers();
    void emitEvents();

    Options options;
    /* Declared before the context, so the initialization is released after the context is destroyed. */
    LibraryReference library;
    cansub_ctx *context = nullptr;

    Transport transports[2];
    /* Moves the bytes between the sockets and the library, allocated once. */
    QByteArray ioBuffer;

    std::list<RequestEntry> requests;
    quint64 nextRequestId = 1;
    quint64 nextTag = 1;
    /* Tag reported by cansub_request_in_flight() at the last check, 0 for none. */
    quint64 inFlightTag = 0;

    /* Collected by the pump, emitted at its end. */
    QVector<cansub_frame> receivedFrames;

    QTimer retryTimer;
    QTimer dataOpenTimer;
    QTimer controlOpenTimer;
    QTimer requestTimer;
    QTimer closeTimer;

    CloseStage closeStage = CloseStage::None;
    cansub_state lastState = CANSUB_STATE_INITIALIZED;
    cansub_result fatalResult = CANSUB_RESULT_OK;
    char const *fatalCall = nullptr;
    /* Why the glue ended the data transport, used if the library records CANSUB_RESULT_CONNECTION_LOST. */
    QString dataEndReason;
    /* Arguments of closed(), set by finish(). */
    cansub_result closeError = CANSUB_RESULT_OK;
    QString closeMessage;

    bool pumpScheduled = false;
    bool openRequested = false;
    bool openPending = false;
    bool closeRequested = false;
    bool closePending = false;
    bool forcePending = false;
    bool endingTransports = false;
    /* CANSUB_STATE_OPEN was seen, also if it was left again within the same pump. */
    bool reachedOpen = false;
    bool openedEmitted = false;
    bool sendBlocked = false;
    bool readyToSendPending = false;
    bool fatal = false;
    bool finished = false;
    bool closedEmitted = false;
};

} // namespace CANsubQt

#endif // CANSUB_QT_CANSUB_CONNECTION_HXX
