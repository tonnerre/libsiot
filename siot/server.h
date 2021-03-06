/*-
 * Copyright (c) 2012 Caoimhe Chaos <caoimhechaos@protonmail.com>,
 *                    Ancient Solutions. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions  of source code must retain  the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions  in   binary  form  must   reproduce  the  above
 *    copyright  notice, this  list  of conditions  and the  following
 *    disclaimer in the  documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS  SOFTWARE IS  PROVIDED BY  ANCIENT SOLUTIONS  AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO,  THE IMPLIED WARRANTIES OF  MERCHANTABILITY AND FITNESS
 * FOR A  PARTICULAR PURPOSE  ARE DISCLAIMED.  IN  NO EVENT  SHALL THE
 * FOUNDATION  OR CONTRIBUTORS  BE  LIABLE FOR  ANY DIRECT,  INDIRECT,
 * INCIDENTAL,   SPECIAL,    EXEMPLARY,   OR   CONSEQUENTIAL   DAMAGES
 * (INCLUDING, BUT NOT LIMITED  TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE,  DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT  LIABILITY,  OR  TORT  (INCLUDING NEGLIGENCE  OR  OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef INCLUDED_SIOT_SERVER_H
#define INCLUDED_SIOT_SERVER_H 1

#include <google/protobuf/stubs/common.h>
#include <thread++/mutex.h>
#include <thread++/threadpool.h>
#include <toolbox/scopedptr.h>
#include <condition_variable>
#include <siot/connection.h>
#include <siot/ssl.h>
#include <string>
#include <map>

namespace toolbox
{
namespace siot
{
using google::protobuf::Closure;
using ssl::ServerSSLContext;
using threadpp::ReadWriteMutex;

// Exception for errors which occurr during setup of the server.
class ServerSetupException : public std::exception
{
public:
	// Creates a new exception with the given error message.
	ServerSetupException(const std::string& errmsg) noexcept;

	// Retrieves the error message from the exception.
	virtual const char* what() const noexcept;

private:
	const std::string errmsg_;
};

// Exception for errors which occurr during client connections. These
// simply mean the client connection has failed or is having problems.
class ClientConnectionException : public std::exception
{
public:
	// Creates a new exception with the given error message.
	ClientConnectionException(const std::string& identifier,
			const std::string& errmsg) noexcept;

	// Retrieves the error message from the exception.
	virtual const char* what() const noexcept;

	// Retrieves the short identifier of the message.
	virtual string identifier() const noexcept;

private:
	const std::string identifier_;
	const std::string errmsg_;
};

// Prototype of a class to notify when new connections have been established.
// The class should take ownership of the connection object.
class ConnectionCallback
{
public:
	virtual ~ConnectionCallback();

	// This allows the service to wrap the connection object into
	// something it can use better, e.g. the LineBufferDecorator. This
	// method should return a connection handle of the type which the
	// service ultimately intends to use. The default is to just return
	// the origial object, which works just as well.
	//
	// Unlike ConnectionEstablished(), this method is called in the same
	// thread which accepts new connections and will thus block. If you
	// watn to do some extensive processing or anything like it, you
	// should run that in ConnectionEstablished(), which is called in
	// a thread of its own.
	virtual Connection* AddDecorators(Connection* in);

	// This method is invoked when a new connection "conn" has been made
	// to the server. The class should take ownership of the connection
	// object.
	virtual void ConnectionEstablished(Connection* conn) = 0;

	// Report an error which occurred trying to establish the socket.
	// By default, they're ignored.
	virtual void ConnectionFailed(std::string msg);

	// This indicates that data is available for reading on the given
	// connection.
	virtual void DataReady(Connection* conn) = 0;

	// This is invoked to indicate a connection has been terminated and
	// is about to be removed. The default is to ignore it.
	virtual void ConnectionTerminated(Connection* conn);

	// This is to report connection errors. The default is to ignore them.
	virtual void Error(Connection* conn);
};

// The server class implements a server which accepts new connections and
// spawns off handlers for them.
class Server
{
public:
	// Create a new server and bind it to the address specified in
	// "addr". The callback "connected" is invoked with the Connection
	// structure when a new connection has been established.
	// "num_threads" sets the number of parallel processing threads in
	// the pool for answering requests to the clients.
	Server(std::string addr, ConnectionCallback* connected = 0,
			uint32_t num_threads = 16);

	// Stop listening and shut down the server. This will wait until
	// all outstanding connections are terminated.
	virtual ~Server();

	// Set the maximum number of parallel connections to "maxconn". The
	// default is SOMAXCONN.
	Server* SetMaxConnections(int maxconn);

	// Set the callback to be invoked when a new connection was
	// established.
	Server* SetConnectionCallback(ConnectionCallback* connected);

	// Configures the server to provide SSL sessions to the clients,
	// rather than regular TCP sessions, with the parameters outlined in
	// the "context". This should be called before
	// Listen(). This will take ownership of "context".
	Server* SetServerSSLContext(const ServerSSLContext* context);

	// Marks the given connection as to be shut down when the next thread
	// becomes free. This is useful for shutting down connections from
	// handlers, which would otherwise block because the handlers are
	// already holding a lock on the connection pool.
	void DeferShutdown(Connection* conn);

	// Removes the given connection from the pool of connections which
	// are watched (i.e. we stop monitoring events and so forth).
	void DequeueConnection(Connection* conn);

	// Sets the maximum number of seconds a connection may be idle before
	// it is automatically terminated. Setting this to 0 or a negative
	// value (the default) means they're never terminated.
	void SetMaxIdle(int max_idle);

	// Start listening on the given address. This call will block, so you
	// may want to start it in a separate thread.
	void Listen();

	// Instruct the listener to stop listening after the next round. A
	// round ends either after a timeout (see SetMaxIdle) or when a new
	// event (connection, data, disconnection) is received.
	void Shutdown();

private:
	ScopedPtr<ConnectionCallback> connected_;
	const ServerSSLContext* ssl_context_;
	threadpp::ThreadPool executor_;
	int maxconn_;
	uint32_t num_threads_;
	int max_idle_;
	bool running_;

#ifdef _POSIX_SOURCE
	struct addrinfo *info_;
	int serverfd_;
	int epollfd_;

	std::map<int, Connection*> connections_;
	ScopedPtr<ReadWriteMutex> connections_lock_;
	std::condition_variable connections_updated_;
	void LockCallAndUnlock(Closure* c, Connection* conn);

	void ListenPoll();
#ifdef HAVE_SELECT
	void ListenSelect();
#endif /* HAVE_SELECT */

#ifdef HAVE_KQUEUE
	void ListenKQueue();
#endif /* HAVE_KQUEUE */

#ifdef HAVE_EPOLL_CREATE
	void ListenEpoll();
	void ReapConnectionsEpoll();
#endif /* HAVE_EPOLL_CREATE */
#endif /* _POSIX_SOURCE */
};
}  // namespace siot
}  // namespace toolbox

#endif /* INCLUDED_SIOT_SERVER_H */
