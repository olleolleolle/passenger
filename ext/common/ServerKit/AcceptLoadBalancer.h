/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2014-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_SERVER_KIT_ACCEPT_LOAD_BALANCER_H_
#define _PASSENGER_SERVER_KIT_ACCEPT_LOAD_BALANCER_H_

#include <boost/bind.hpp>
#include <boost/cstdint.hpp>
#include <boost/config.hpp>
#include <oxt/thread.hpp>
#include <oxt/macros.hpp>
#include <vector>
#include <limits>
#include <cassert>
#include <cerrno>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include <Constants.h>
#include <Logging.h>
#include <Utils/IOUtils.h>

namespace Passenger {
namespace ServerKit {

using namespace std;
using namespace boost;


/**
 * Listens for client connections and load balances them to multiple
 * Server objects in a round-robin manner.
 *
 * Normally, the Server class listens for client connections directly.
 * But this is inefficient in multithreaded situations where you are
 * running one Server and event loop per CPU core, that all happen to
 * listen on the same server socket. This is because every time a client
 * connects, all threads wake up, but only one thread will succeed in
 * accept()ing the client.
 *
 * Furthermore, it can also be very easy for threads to become
 * unbalanced. If a burst of clients connect to the server socket,
 * then it is very likely that a single Server accepts all of
 * those clients. This can result in situations where, for example,
 * thread 1 has 40 clients and thread 2 has only 3.
 *
 * The AcceptLoadBalancer solves this problem by being the sole entity
 * that listens on the server socket. All client sockets that it
 * accepts are distributed to all registered Server objects, in a
 * round-robin manner.
 *
 * Inside the "PassengerAgent server", we activate AcceptLoadBalancer
 * only if `server_threads > 1`, which is often the case because
 * `server_threads` defaults to the number of CPU cores.
 */
template<typename Server>
class AcceptLoadBalancer {
private:
	static const unsigned int ACCEPT_BURST_COUNT = 16;

	int endpoints[SERVER_KIT_MAX_SERVER_ENDPOINTS];
	struct pollfd pollers[1 + SERVER_KIT_MAX_SERVER_ENDPOINTS];
	int newClients[ACCEPT_BURST_COUNT];

	unsigned int nEndpoints;
	boost::uint8_t newClientCount;
	boost::uint8_t nextServer;
	bool accept4Available;
	bool quit;

	int exitPipe[2];
	oxt::thread *thread;

	#if __cplusplus >= 199711L && !defined(BOOST_NO_STATIC_ASSERT)
		static_assert(std::numeric_limits<typeof(nEndpoints)>::max()
			>= SERVER_KIT_MAX_SERVER_ENDPOINTS,
			"nEndpoints's type is too small to accomodate for SERVER_KIT_MAX_SERVER_ENDPOINTS");
		static_assert(std::numeric_limits<typeof(newClientCount)>::max()
			>= ACCEPT_BURST_COUNT,
			"newClientCount's type is too small to accomodate for ACCEPT_BURST_COUNT");
	#endif

	void pollAllEndpoints() {
		pollers[0].fd = exitPipe[0];
		pollers[0].events = POLLIN;
		for (unsigned int i = 0; i < nEndpoints; i++) {
			pollers[i + 1].fd = endpoints[i];
			pollers[i + 1].events = POLLIN;
		}
		if (poll(pollers, nEndpoints + 1, -1) == -1) {
			int e = errno;
			throw SystemException("poll() failed", e);
		}
	}

	bool acceptNewClients(int endpoint) {
		bool error = false;
		int fd, errcode = 0;

		while (newClientCount < ACCEPT_BURST_COUNT) {
			fd = acceptNonBlockingSocket(endpoint);
			if (fd == -1) {
				error = true;
				errcode = errno;
				break;
			}

			P_TRACE(2, "Accepted client file descriptor: " << fd);
			newClients[newClientCount] = fd;
			newClientCount++;
		}

		if (error && errcode != EAGAIN && errcode != EWOULDBLOCK) {
			P_ERROR("Cannot accept client: " << getErrorDesc(errcode) <<
				" (errno=" << errcode << "). " <<
				"Stop accepting clients for 3 seconds.");
			pollers[0].fd = exitPipe[0];
			pollers[0].events = POLLIN;
			if (poll(pollers, 1, 3) == 1) {
				quit = true;
			} else {
				P_NOTICE("Resuming accepting new clients");
			}
			return false;
		} else {
			return true;
		}
	}

	void distributeNewClients() {
		unsigned int i;

		for (i = 0; i < newClientCount; i++) {
			ServerKit::Context *ctx = servers[nextServer]->getContext();
			P_TRACE(2, "Feeding client to server thread " << nextServer <<
				": file descriptor " << newClients[i]);
			ctx->libev->runLater(boost::bind(feedNewClient, servers[nextServer],
				newClients[i]));
			nextServer = (nextServer + 1) % servers.size();
		}

		newClientCount = 0;
	}

	static void feedNewClient(Server *server, int fd) {
		server->feedNewClients(&fd, 1);
	}

	int acceptNonBlockingSocket(int serverFd) {
		union {
			struct sockaddr_in inaddr;
			struct sockaddr_un unaddr;
		} u;
		socklen_t addrlen = sizeof(u);

		if (accept4Available) {
			int fd = callAccept4(serverFd,
				(struct sockaddr *) &u,
				&addrlen,
				O_NONBLOCK);
			// FreeBSD returns EINVAL if accept4() is called with invalid flags.
			if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
				accept4Available = false;
				return acceptNonBlockingSocket(serverFd);
			} else {
				return fd;
			}
		} else {
			int fd = syscalls::accept(serverFd,
				(struct sockaddr *) &u,
				&addrlen);
			FdGuard guard(fd);
			if (fd == -1) {
				return -1;
			} else {
				try {
					setNonBlocking(fd);
				} catch (const SystemException &e) {
					P_DEBUG("Unable to set non-blocking flag on accepted client socket: " <<
						e.what() << " (errno=" << e.code() << ")");
					errno = e.code();
					return -1;
				}
				guard.clear();
				return fd;
			}
		}
	}

	void mainLoop() {
		while (!quit) {
			pollAllEndpoints();
			if (OXT_UNLIKELY(pollers[0].revents & POLLIN)) {
				// Exit pipe signaled.
				quit = true;
				break;
			}

			unsigned int i = 0;
			newClientCount = 0;

			while (newClientCount < ACCEPT_BURST_COUNT && i < nEndpoints) {
				if (pollers[i + 1].revents & POLLIN) {
					if (!acceptNewClients(endpoints[i])) {
						break;
					}
				}
				i++;
			}

			distributeNewClients();
		}
	}

public:
	vector<Server *> servers;

	AcceptLoadBalancer()
		: nEndpoints(0),
		  newClientCount(0),
		  nextServer(0),
		  accept4Available(true),
		  quit(false),
		  thread(NULL)
	{
		if (pipe(exitPipe) == -1) {
			int e = errno;
			throw SystemException("Cannot create pipe", e);
		}
		FdGuard guard1(exitPipe[0]);
		FdGuard guard2(exitPipe[1]);
		setNonBlocking(exitPipe[0]);
		setNonBlocking(exitPipe[1]);
		guard1.clear();
		guard2.clear();
	}

	~AcceptLoadBalancer() {
		shutdown();
		close(exitPipe[0]);
		close(exitPipe[1]);
	}

	void listen(int fd) {
		#ifdef EOPNOTSUPP
			#define EXTENSION_EOPNOTSUPP EOPNOTSUPP
		#else
			#define EXTENSION_EOPNOTSUPP ENOTSUP
		#endif

		assert(nEndpoints < SERVER_KIT_MAX_SERVER_ENDPOINTS);
		setNonBlocking(fd);
		int flag = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) == -1
		 && errno != ENOPROTOOPT
		 && errno != ENOTSUP
		 && errno != EXTENSION_EOPNOTSUPP)
		{
			int e = errno;
			P_WARN("Cannot disable Nagle's algorithm on a TCP socket: " <<
				strerror(e) << " (errno=" << e << ")");
		}
		endpoints[nEndpoints] = fd;
		nEndpoints++;

		#undef EXTENSION_EOPNOTSUPP
	}

	void start() {
		thread = new oxt::thread(boost::bind(&AcceptLoadBalancer<Server>::mainLoop, this),
			"Load balancer");
	}

	void shutdown() {
		if (thread != NULL) {
			if (write(exitPipe[1], "x", 1) == -1) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					int e = errno;
					P_WARN("Cannot write to the load balancer's exit pipe: " <<
						strerror(e) << " (errno=" << e << ")");
				}
			}
			thread->join();
			delete thread;
			thread = NULL;
		}
	}
};


} // namespace ServerKit
} // namespace Passenger

#endif /* _PASSENGER_SERVER_KIT_ACCEPT_LOAD_BALANCER_H_ */
