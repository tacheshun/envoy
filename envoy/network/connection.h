#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/common/pure.h"
#include "envoy/common/scope_tracker.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/network/address.h"
#include "envoy/network/filter.h"
#include "envoy/network/listen_socket.h"
#include "envoy/network/socket.h"
#include "envoy/ssl/connection.h"
#include "envoy/stream_info/stream_info.h"

namespace Envoy {
namespace Event {
class Dispatcher;
}

namespace Network {

/**
 * Events that occur on a connection.
 */
enum class ConnectionEvent {
  RemoteClose,
  LocalClose,
  Connected,
};

/**
 * Connections have both a read and write buffer.
 */
enum class ConnectionBufferType { Read, Write };

/**
 * Network level callbacks that happen on a connection.
 */
class ConnectionCallbacks {
public:
  virtual ~ConnectionCallbacks() = default;

  /**
   * Callback for connection events.
   * @param events supplies the ConnectionEvent that occurred.
   */
  virtual void onEvent(ConnectionEvent event) PURE;

  /**
   * Called when the write buffer for a connection goes over its high watermark.
   */
  virtual void onAboveWriteBufferHighWatermark() PURE;

  /**
   * Called when the write buffer for a connection goes from over its high
   * watermark to under its low watermark.
   */
  virtual void onBelowWriteBufferLowWatermark() PURE;
};

/**
 * Type of connection close to perform.
 */
enum class ConnectionCloseType {
  FlushWrite, // Flush pending write data before raising ConnectionEvent::LocalClose
  NoFlush,    // Do not flush any pending data and immediately raise ConnectionEvent::LocalClose
  FlushWriteAndDelay // Flush pending write data and delay raising a ConnectionEvent::LocalClose
                     // until the delayed_close_timeout expires
};

/**
 * An abstract raw connection. Free the connection or call close() to disconnect.
 */
class Connection : public Event::DeferredDeletable,
                   public FilterManager,
                   public ScopeTrackedObject {
public:
  enum class State { Open, Closing, Closed };

  /**
   * Callback function for when bytes have been sent by a connection.
   * @param bytes_sent supplies the number of bytes written to the connection.
   * @return indicates if callback should be called in the future. If true is returned
   * the callback will be called again in the future. If false is returned, the callback
   * will be removed from callback list.
   */
  using BytesSentCb = std::function<bool(uint64_t bytes_sent)>;

  struct ConnectionStats {
    Stats::Counter& read_total_;
    Stats::Gauge& read_current_;
    Stats::Counter& write_total_;
    Stats::Gauge& write_current_;
    // Counter* as this is an optional counter. Bind errors will not be tracked if this is nullptr.
    Stats::Counter* bind_errors_;
    // Optional counter. Delayed close timeouts will not be tracked if this is nullptr.
    Stats::Counter* delayed_close_timeouts_;
  };

  ~Connection() override = default;

  /**
   * Register callbacks that fire when connection events occur.
   */
  virtual void addConnectionCallbacks(ConnectionCallbacks& cb) PURE;

  /**
   * Unregister callbacks which previously fired when connection events occur.
   */
  virtual void removeConnectionCallbacks(ConnectionCallbacks& cb) PURE;

  /**
   * Register for callback every time bytes are written to the underlying TransportSocket.
   */
  virtual void addBytesSentCallback(BytesSentCb cb) PURE;

  /**
   * Enable half-close semantics on this connection. Reading a remote half-close
   * will not fully close the connection. This is off by default.
   * @param enabled Whether to set half-close semantics as enabled or disabled.
   */
  virtual void enableHalfClose(bool enabled) PURE;

  /**
   * @return true if half-close semantics are enabled, false otherwise.
   */
  virtual bool isHalfCloseEnabled() PURE;

  /**
   * Close the connection.
   */
  virtual void close(ConnectionCloseType type) PURE;

  /**
   * @return Event::Dispatcher& the dispatcher backing this connection.
   */
  virtual Event::Dispatcher& dispatcher() PURE;

  /**
   * @return uint64_t the unique local ID of this connection.
   */
  virtual uint64_t id() const PURE;

  /**
   * @param vector of bytes to which the connection should append hash key data. Any data already in
   * the key vector must not be modified.
   */
  virtual void hashKey(std::vector<uint8_t>& hash) const PURE;

  /**
   * @return std::string the next protocol to use as selected by network level negotiation. (E.g.,
   *         ALPN). If network level negotiation is not supported by the connection or no protocol
   *         has been negotiated the empty string is returned.
   */
  virtual std::string nextProtocol() const PURE;

  /**
   * Enable/Disable TCP NO_DELAY on the connection.
   */
  virtual void noDelay(bool enable) PURE;

  /**
   * Disable socket reads on the connection, applying external back pressure. When reads are
   * enabled again if there is data still in the input buffer it will be re-dispatched through
   * the filter chain.
   * @param disable supplies TRUE is reads should be disabled, FALSE if they should be enabled.
   *
   * Note that this function reference counts calls. For example
   * readDisable(true);  // Disables data
   * readDisable(true);  // Notes the connection is blocked by two sources
   * readDisable(false);  // Notes the connection is blocked by one source
   * readDisable(false);  // Marks the connection as unblocked, so resumes reading.
   */
  virtual void readDisable(bool disable) PURE;

  /**
   * Set if Envoy should detect TCP connection close when readDisable(true) is called.
   * By default, this is true on newly created connections.
   *
   * @param should_detect supplies if disconnects should be detected when the connection has been
   * read disabled
   */
  virtual void detectEarlyCloseWhenReadDisabled(bool should_detect) PURE;

  /**
   * @return bool whether reading is enabled on the connection.
   */
  virtual bool readEnabled() const PURE;

  /**
   * @return the address provider backing this connection.
   */
  virtual const SocketAddressProvider& addressProvider() const PURE;
  virtual SocketAddressProviderSharedPtr addressProviderSharedPtr() const PURE;

  /**
   * Credentials of the peer of a socket as decided by SO_PEERCRED.
   */
  struct UnixDomainSocketPeerCredentials {
    /**
     * The process id of the peer.
     */
    int32_t pid;
    /**
     * The user id of the peer.
     */
    uint32_t uid;
    /**
     * The group id of the peer.
     */
    uint32_t gid;
  };

  /**
   * @return The unix socket peer credentials of the remote client. Note that this is only
   * supported for unix socket connections.
   */
  virtual absl::optional<UnixDomainSocketPeerCredentials> unixSocketPeerCredentials() const PURE;

  /**
   * Set the stats to update for various connection state changes. Note that for performance reasons
   * these stats are eventually consistent and may not always accurately represent the connection
   * state at any given point in time.
   */
  virtual void setConnectionStats(const ConnectionStats& stats) PURE;

  /**
   * @return the const SSL connection data if this is an SSL connection, or nullptr if it is not.
   */
  // TODO(snowp): Remove this in favor of StreamInfo::downstreamSslConnection.
  virtual Ssl::ConnectionInfoConstSharedPtr ssl() const PURE;

  /**
   * @return requested server name (e.g. SNI in TLS), if any.
   */
  virtual absl::string_view requestedServerName() const PURE;

  /**
   * @return State the current state of the connection.
   */
  virtual State state() const PURE;

  /**
   * @return true if the connection has not completed connecting, false if the connection is
   * established.
   */
  virtual bool connecting() const PURE;

  /**
   * Write data to the connection. Will iterate through downstream filters with the buffer if any
   * are installed.
   * @param data Supplies the data to write to the connection.
   * @param end_stream If true, this indicates that this is the last write to the connection. If
   *        end_stream is true, the connection is half-closed. This may only be set to true if
   *        enableHalfClose(true) has been set on this connection.
   */
  virtual void write(Buffer::Instance& data, bool end_stream) PURE;

  /**
   * Set a soft limit on the size of buffers for the connection.
   * For the read buffer, this limits the bytes read prior to flushing to further stages in the
   * processing pipeline.
   * For the write buffer, it sets watermarks. When enough data is buffered it triggers a call to
   * onAboveWriteBufferHighWatermark, which allows subscribers to enforce flow control by disabling
   * reads on the socket funneling data to the write buffer. When enough data is drained from the
   * write buffer, onBelowWriteBufferHighWatermark is called which similarly allows subscribers
   * resuming reading.
   */
  virtual void setBufferLimits(uint32_t limit) PURE;

  /**
   * Get the value set with setBufferLimits.
   */
  virtual uint32_t bufferLimit() const PURE;

  /**
   * @return boolean telling if the connection is currently above the high watermark.
   */
  virtual bool aboveHighWatermark() const PURE;

  /**
   * Get the socket options set on this connection.
   */
  virtual const ConnectionSocket::OptionsSharedPtr& socketOptions() const PURE;

  /**
   * The StreamInfo object associated with this connection. This is typically
   * used for logging purposes. Individual filters may add specific information
   * via the FilterState object within the StreamInfo object. The StreamInfo
   * object in this context is one per connection i.e. different than the one in
   * the http ConnectionManager implementation which is one per request.
   *
   * @return StreamInfo object associated with this connection.
   */
  virtual StreamInfo::StreamInfo& streamInfo() PURE;
  virtual const StreamInfo::StreamInfo& streamInfo() const PURE;

  /**
   * Set the timeout for delayed connection close()s.
   * This can only be called prior to issuing a close() on the connection.
   * @param timeout The timeout value in milliseconds
   */
  virtual void setDelayedCloseTimeout(std::chrono::milliseconds timeout) PURE;

  /**
   * @return std::string the failure reason of the underlying transport socket, if no failure
   *         occurred an empty string is returned.
   */
  virtual absl::string_view transportFailureReason() const PURE;

  /**
   * Instructs the connection to start using secure transport.
   * Note: Not all underlying transport sockets support such operation.
   * @return boolean telling if underlying transport socket was able to
             start secure transport.
   */
  virtual bool startSecureTransport() PURE;

  /**
   *  @return absl::optional<std::chrono::milliseconds> An optional of the most recent round-trip
   *  time of the connection. If the platform does not support this, then an empty optional is
   *  returned.
   */
  virtual absl::optional<std::chrono::milliseconds> lastRoundTripTime() const PURE;
};

using ConnectionPtr = std::unique_ptr<Connection>;

/**
 * Connections servicing inbound connects.
 */
class ServerConnection : public virtual Connection {
public:
  /**
   * Set the amount of time allowed for the transport socket to report that a connection is
   * established. The provided timeout is relative to the current time. If this method is called
   * after a connection has already been established, it is a no-op.
   */
  virtual void setTransportSocketConnectTimeout(std::chrono::milliseconds timeout) PURE;
};

using ServerConnectionPtr = std::unique_ptr<ServerConnection>;

/**
 * Connections capable of outbound connects.
 */
class ClientConnection : public virtual Connection {
public:
  /**
   * Connect to a remote host. Errors or connection events are reported via the event callback
   * registered via addConnectionCallbacks().
   */
  virtual void connect() PURE;
};

using ClientConnectionPtr = std::unique_ptr<ClientConnection>;

} // namespace Network
} // namespace Envoy