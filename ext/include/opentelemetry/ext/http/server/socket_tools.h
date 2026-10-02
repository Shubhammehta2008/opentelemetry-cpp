// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

#  include <winsock2.h>
#  include <ws2tcpip.h>  // inet_pton

#  undef min
#  undef max
#  pragma comment(lib, "ws2_32.lib")

#else

#  include <unistd.h>

#  ifdef __linux__
#    include <sys/epoll.h>
#  endif

#  ifdef __APPLE__
#    include "TargetConditionals.h"
#    include <sys/event.h>
#    include <sys/time.h>
#    include <sys/types.h>
#  endif

// Common POSIX headers for Linux and Mac OS X
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>

#endif

#if defined(HAVE_CONSOLE_LOG) && !defined(LOG_DEBUG)
#  include <cstdio>
#  ifndef LOG_DEBUG
#    define LOG_DEBUG(fmt_, ...) std::printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_TRACE(fmt_, ...) std::printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_INFO(fmt_, ...) std::printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_WARN(fmt_, ...) std::printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_ERROR(fmt_, ...) std::printf(" " fmt_ "\n", ##__VA_ARGS__)
#  endif
#endif

#ifndef LOG_DEBUG
#  define LOG_DEBUG(fmt_, ...)
#  define LOG_TRACE(fmt_, ...)
#  define LOG_INFO(fmt_, ...)
#  define LOG_WARN(fmt_, ...)
#  define LOG_ERROR(fmt_, ...)
#endif

namespace common
{

/// <summary>
/// A simple thread, derived class overloads onThread() method.
/// </summary>
struct Thread
{
  std::thread m_thread;
  std::atomic<bool> m_terminate{false};

  Thread()                          = default;
  Thread(const Thread &)            = delete;
  Thread(Thread &&)                 = delete;
  Thread &operator=(const Thread &) = delete;
  Thread &operator=(Thread &&)      = delete;

  void startThread()
  {
    m_terminate = false;
    m_thread    = std::thread([&]() { this->onThread(); });
  }

  void joinThread()
  {
    m_terminate = true;
    if (m_thread.joinable())
    {
      m_thread.join();
    }
  }

  bool shouldTerminate() const { return m_terminate; }

  virtual void onThread() = 0;

  virtual ~Thread() noexcept = default;
};

}  // namespace common

namespace SocketTools
{

#ifdef _WIN32
struct WsaInitializer
{
  WsaInitializer()
  {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
  }

  ~WsaInitializer() { WSACleanup(); }
};

static WsaInitializer g_wsaInitializer;
#endif

/// <summary>
/// Encapsulation of sockaddr_storage for safe alignment and protocol independence
/// </summary>
struct SocketAddr
{
  static u_long const Loopback = 0x7F000001;

  sockaddr_storage m_data{};
  socklen_t m_len{sizeof(sockaddr_storage)};

  SocketAddr()
  {
    std::memset(&m_data, 0, sizeof(m_data));
    m_data.ss_family = AF_UNSPEC;
  }

  SocketAddr(u_long addr, uint16_t port)
  {
    std::memset(&m_data, 0, sizeof(m_data));
    sockaddr_in inet4{};
    inet4.sin_family      = AF_INET;
    inet4.sin_port        = htons(port);
    inet4.sin_addr.s_addr = htonl(addr);

    std::memcpy(&m_data, &inet4, sizeof(inet4));
    m_len = sizeof(sockaddr_in);
  }

  SocketAddr(char const *addr)
  {
    std::memset(&m_data, 0, sizeof(m_data));
    m_data.ss_family = AF_UNSPEC;

    if (addr == nullptr)
    {
      LOG_WARN("SocketAddr: cannot parse a null address");
      return;
    }

    sockaddr_in parsed{};
    parsed.sin_family = AF_INET;

    char const *colon          = std::strchr(addr, ':');
    char const *hostEnd        = colon ? colon : addr + std::strlen(addr);
    ptrdiff_t const hostLength = hostEnd - addr;

    bool ok = (hostLength >= 1 && hostLength <= 15);
    if (ok)
    {
      char host[16];
      std::memcpy(host, addr, static_cast<size_t>(hostLength));
      host[hostLength] = '\0';
      ok               = (::inet_pton(AF_INET, host, &parsed.sin_addr) == 1);
    }

    if (ok && colon)
    {
      char const *p = colon + 1;
      uint16_t port = 0;
      if (*p == '\0')
      {
        ok = false;
      }
      while (ok && *p != '\0')
      {
        if (*p < '0' || *p > '9')
        {
          ok = false;
          break;
        }
        const uint16_t digit = static_cast<uint16_t>(*p - '0');
        if (port > (65535u - digit) / 10u)
        {
          ok = false;
          break;
        }
        port = port * 10u + digit;
        ++p;
      }
      if (ok)
      {
        parsed.sin_port = htons(port);
      }
    }

    if (ok)
    {
      std::memcpy(&m_data, &parsed, sizeof(parsed));
      m_len = sizeof(sockaddr_in);
    }
    else
    {
      LOG_WARN("SocketAddr: cannot parse address");
    }
  }

  operator sockaddr *() { return reinterpret_cast<sockaddr *>(&m_data); }

  operator const sockaddr *() const { return reinterpret_cast<const sockaddr *>(&m_data); }

  socklen_t length() const { return m_len; }

  int port() const
  {
    switch (m_data.ss_family)
    {
      case AF_INET: {
        sockaddr_in inet4{};
        std::memcpy(&inet4, &m_data, sizeof(inet4));
        return ntohs(inet4.sin_port);
      }
      case AF_INET6: {
        sockaddr_in6 inet6{};
        std::memcpy(&inet6, &m_data, sizeof(inet6));
        return ntohs(inet6.sin6_port);
      }
      default:
        return -1;
    }
  }

  std::string toString() const
  {
    std::ostringstream os;

    switch (m_data.ss_family)
    {
      case AF_INET: {
        sockaddr_in inet4{};
        std::memcpy(&inet4, &m_data, sizeof(inet4));
        u_long addr = ntohl(inet4.sin_addr.s_addr);
        os << (addr >> 24) << '.' << ((addr >> 16) & 255) << '.' << ((addr >> 8) & 255) << '.'
           << (addr & 255);
        os << ':' << ntohs(inet4.sin_port);
        break;
      }
      default:
        os << "[?AF?" << m_data.ss_family << ']';
    }
    return os.str();
  }
};

/// <summary>
/// Encapsulation of a socket (non-exclusive ownership)
/// </summary>
struct Socket
{
#ifdef _WIN32
  using Type                = SOCKET;
  static Type const Invalid = INVALID_SOCKET;
#else
  using Type                = int;
  static Type const Invalid = -1;
#endif

  Type m_sock;

  Socket(Type sock = Invalid) : m_sock(sock) {}

  Socket(int af, int type, int proto) : m_sock(::socket(af, type, proto)) {}

  operator Socket::Type() const { return m_sock; }

  bool operator==(Socket const &other) const { return (m_sock == other.m_sock); }

  bool operator!=(Socket const &other) const { return (m_sock != other.m_sock); }

  bool operator<(Socket const &other) const { return (m_sock < other.m_sock); }

  bool invalid() const { return (m_sock == Invalid); }

  void setNonBlocking()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    u_long value = 1;
    ::ioctlsocket(m_sock, FIONBIO, &value);
#else
    int flags = ::fcntl(m_sock, F_GETFL, 0);
    ::fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
#endif
  }

  bool setReuseAddr()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    BOOL value = TRUE;
#else
    int value = 1;
#endif
    return (::setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&value),
                         sizeof(value)) == 0);
  }

  bool setNoDelay()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    BOOL value = TRUE;
#else
    int value = 1;
#endif
    return (::setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&value),
                         sizeof(value)) == 0);
  }

  bool connect(SocketAddr const &addr)
  {
    assert(m_sock != Invalid);
    return (::connect(m_sock, addr, addr.length()) == 0);
  }

  void close()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    ::closesocket(m_sock);
#else
    ::close(m_sock);
#endif
    m_sock = Invalid;
  }

  int recv(void *buffer, unsigned size)
  {
    assert(m_sock != Invalid);
    int flags = 0;
    return static_cast<int>(::recv(m_sock, reinterpret_cast<char *>(buffer), size, flags));
  }

  int send(void const *buffer, unsigned size)
  {
    assert(m_sock != Invalid);
    return static_cast<int>(::send(m_sock, reinterpret_cast<char const *>(buffer), size, 0));
  }

  bool bind(SocketAddr const &addr)
  {
    assert(m_sock != Invalid);
    return (::bind(m_sock, addr, addr.length()) == 0);
  }

  bool getsockname(SocketAddr &addr) const
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    int addrlen = sizeof(addr.m_data);
#else
    socklen_t addrlen = sizeof(addr.m_data);
#endif
    if (::getsockname(m_sock, addr, &addrlen) == 0)
    {
      addr.m_len = static_cast<socklen_t>(addrlen);
      return true;
    }
    return false;
  }

  bool listen(int backlog)
  {
    assert(m_sock != Invalid);
    return (::listen(m_sock, backlog) == 0);
  }

  bool accept(Socket &csock, SocketAddr &caddr)
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    int addrlen = sizeof(caddr.m_data);
#else
    socklen_t addrlen = sizeof(caddr.m_data);
#endif
    csock = ::accept(m_sock, caddr, &addrlen);
    if (!csock.invalid())
    {
      caddr.m_len = static_cast<socklen_t>(addrlen);
      return true;
    }
    return false;
  }

  bool shutdown(int how)
  {
    assert(m_sock != Invalid);
    return (::shutdown(m_sock, how) == 0);
  }

  int error() const
  {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
  }

  enum
  {
#ifdef _WIN32
    ErrorWouldBlock = WSAEWOULDBLOCK
#else
    ErrorWouldBlock = EWOULDBLOCK
#endif
  };

  enum
  {
#ifdef _WIN32
    ShutdownReceive = SD_RECEIVE,
    ShutdownSend    = SD_SEND,
    ShutdownBoth    = SD_BOTH
#else
    ShutdownReceive = SHUT_RD,
    ShutdownSend    = SHUT_WR,
    ShutdownBoth    = SHUT_RDWR
#endif
  };
};

/// <summary>
/// Socket Data
/// </summary>
struct SocketData
{
  Socket socket;
  int flags{0};

  bool operator==(const Socket &s) const { return (socket == s); }
};

/// <summary>
/// Socket Reactor
/// </summary>
struct Reactor : protected common::Thread
{
  class SocketCallback
  {
  public:
    SocketCallback() = default;

    SocketCallback(const SocketCallback &)            = delete;
    SocketCallback(SocketCallback &&)                 = delete;
    SocketCallback &operator=(const SocketCallback &) = delete;
    SocketCallback &operator=(SocketCallback &&)      = delete;

    virtual ~SocketCallback()                    = default;
    virtual void onSocketReadable(Socket sock)   = 0;
    virtual void onSocketWritable(Socket sock)   = 0;
    virtual void onSocketAcceptable(Socket sock) = 0;
    virtual void onSocketClosed(Socket sock)     = 0;
  };

  enum State : std::uint8_t
  {
    Readable   = 1,
    Writable   = 2,
    Acceptable = 4,
    Closed     = 8
  };

  SocketCallback &m_callback;
  std::vector<SocketData> m_sockets;

#ifdef _WIN32
  std::vector<WSAEVENT> m_events{};
#endif

#ifdef __linux__
  int m_epollFd{-1};
#endif

#ifdef TARGET_OS_MAC
#  define KQUEUE_SIZE 32
  int kq{-1};
  struct kevent m_events[KQUEUE_SIZE];
#endif

public:
  Reactor(SocketCallback &callback)
      : m_callback(callback)
#ifdef __linux__
        ,
        m_epollFd{
#  ifdef ANDROID
            ::epoll_create(1)
#  else
            ::epoll_create1(0)
#  endif
        }
#endif
  {
#ifdef TARGET_OS_MAC
    bzero(&m_events[0], sizeof(m_events));
    kq = kqueue();
#endif
  }

  Reactor(const Reactor &)            = delete;
  Reactor(Reactor &&)                 = delete;
  Reactor &operator=(const Reactor &) = delete;
  Reactor &operator=(Reactor &&)      = delete;

  ~Reactor() override
  {
#ifdef __linux__
    if (m_epollFd != -1)
    {
      ::close(m_epollFd);
    }
#endif
#ifdef TARGET_OS_MAC
    if (kq != -1)
    {
      ::close(kq);
    }
#endif
  }

  void addSocket(const Socket &socket, int flags)
  {
    if (flags == 0)
    {
      removeSocket(socket);
      return;
    }

    auto it = std::find(m_sockets.begin(), m_sockets.end(), socket);
    if (it == m_sockets.end())
    {
      LOG_TRACE("Reactor: Adding socket 0x%x with flags 0x%x", static_cast<int>(socket), flags);
#ifdef _WIN32
      m_events.push_back(::WSACreateEvent());
#endif
#ifdef __linux__
      epoll_event event = {};
      event.data.fd     = socket;
      event.events      = 0;
      ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, socket, &event);
#endif
#ifdef TARGET_OS_MAC
      struct kevent event;
      bzero(&event, sizeof(event));
      event.ident = socket.m_sock;
      EV_SET(&event, event.ident, EVFILT_READ, EV_ADD, 0, 0, NULL);
      kevent(kq, &event, 1, NULL, 0, NULL);
      EV_SET(&event, event.ident, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
      kevent(kq, &event, 1, NULL, 0, NULL);
#endif
      SocketData sd;
      sd.socket = socket;
      sd.flags  = 0;
      m_sockets.push_back(sd);
      it = m_sockets.end() - 1;
    }
    else
    {
      LOG_TRACE("Reactor: Updating socket 0x%x with flags 0x%x", static_cast<int>(socket), flags);
    }

    if (it->flags != flags)
    {
      it->flags = flags;
#ifdef _WIN32
      long lNetworkEvents = 0;
      if (it->flags & Readable)   lNetworkEvents |= FD_READ;
      if (it->flags & Writable)   lNetworkEvents |= FD_WRITE;
      if (it->flags & Acceptable) lNetworkEvents |= FD_ACCEPT;
      if (it->flags & Closed)     lNetworkEvents |= FD_CLOSE;

      auto eventIt = m_events.begin() + std::distance(m_sockets.begin(), it);
      ::WSAEventSelect(socket, *eventIt, lNetworkEvents);
#endif
#ifdef __linux__
      int events = 0;
      if (it->flags & Readable)   events |= EPOLLIN;
      if (it->flags & Writable)   events |= EPOLLOUT;
      if (it->flags & Acceptable) events |= EPOLLIN;

      epoll_event event = {};
      event.data.fd     = socket;
      event.events      = events;
      ::epoll_ctl(m_epollFd, EPOLL_CTL_MOD, socket, &event);
#endif
    }
  }

  void removeSocket(const Socket &socket)
  {
    LOG_TRACE("Reactor: Removing socket 0x%x", static_cast<int>(socket));
    auto it = std::find(m_sockets.begin(), m_sockets.end(), socket);
    if (it != m_sockets.end())
    {
#ifdef _WIN32
      auto eventIt = m_events.begin() + std::distance(m_sockets.begin(), it);
      ::WSAEventSelect(it->socket, *eventIt, 0);
      ::WSACloseEvent(*eventIt);
      m_events.erase(eventIt);
#endif
#ifdef __linux__
      ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, socket, nullptr);
#endif
#ifdef TARGET_OS_MAC
      struct kevent event;
      bzero(&event, sizeof(event));
      event.ident = socket;
      EV_SET(&event, socket, EVFILT_READ, EV_DELETE, 0, 0, NULL);
      kevent(kq, &event, 1, NULL, 0, NULL);
      EV_SET(&event, socket, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
      kevent(kq, &event, 1, NULL, 0, NULL);
#endif
      m_sockets.erase(it);
    }
  }

  void start()
  {
    LOG_INFO("Reactor: Starting...");
    startThread();
  }

  void stop()
  {
    LOG_INFO("Reactor: Stopping...");
    joinThread();
#ifdef _WIN32
    for (auto &hEvent : m_events)
    {
      ::WSACloseEvent(hEvent);
    }
#else
    for (auto &sd : m_sockets)
    {
#  ifdef __linux__
      ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, sd.socket, nullptr);
#  endif
#  ifdef TARGET_OS_MAC
      struct kevent event;
      bzero(&event, sizeof(event));
      event.ident = sd.socket;
      EV_SET(&event, sd.socket, EVFILT_READ, EV_DELETE, 0, 0, NULL);
      kevent(kq, &event, 1, NULL, 0, NULL);
      EV_SET(&event, sd.socket, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
      kevent(kq, &event, 1, NULL, 0, NULL);
#  endif
    }
#endif
    m_sockets.clear();
  }

protected:
  void onThread() override
  {
    LOG_INFO("Reactor: Thread started");
    while (!shouldTerminate())
    {
#ifdef _WIN32
      if (m_events.empty())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }

      DWORD dwResult = ::WSAWaitForMultipleEvents(static_cast<DWORD>(m_events.size()),
                                                  m_events.data(), FALSE, 500, FALSE);
      if (dwResult == WSA_WAIT_TIMEOUT)
      {
        continue;
      }

      if (dwResult >= WSA_WAIT_EVENT_0 && dwResult < WSA_WAIT_EVENT_0 + m_events.size())
      {
        int index     = dwResult - WSA_WAIT_EVENT_0;
        Socket socket = m_sockets[index].socket;
        int flags     = m_sockets[index].flags;

        WSANETWORKEVENTS ne;
        ::WSAEnumNetworkEvents(socket, m_events[index], &ne);

        if ((flags & Readable) && (ne.lNetworkEvents & FD_READ))
          m_callback.onSocketReadable(socket);
        if ((flags & Writable) && (ne.lNetworkEvents & FD_WRITE))
          m_callback.onSocketWritable(socket);
        if ((flags & Acceptable) && (ne.lNetworkEvents & FD_ACCEPT))
          m_callback.onSocketAcceptable(socket);
        if ((flags & Closed) && (ne.lNetworkEvents & FD_CLOSE))
          m_callback.onSocketClosed(socket);
      }
#endif

#ifdef __linux__
      epoll_event events[4];
      int result = ::epoll_wait(m_epollFd, events, sizeof(events) / sizeof(events[0]), 500);
      if (result <= 0)
      {
        continue;
      }

      for (int i = 0; i < result; i++)
      {
        auto it = std::find(m_sockets.begin(), m_sockets.end(), events[i].data.fd);
        if (it == m_sockets.end())
        {
          continue;
        }
        Socket socket = it->socket;
        int flags     = it->flags;

        if ((flags & Readable) && (events[i].events & EPOLLIN))
          m_callback.onSocketReadable(socket);
        if ((flags & Writable) && (events[i].events & EPOLLOUT))
          m_callback.onSocketWritable(socket);
        if ((flags & Acceptable) && (events[i].events & EPOLLIN))
          m_callback.onSocketAcceptable(socket);
        if ((flags & Closed) && (events[i].events & (EPOLLHUP | EPOLLERR)))
          m_callback.onSocketClosed(socket);
      }
#endif

#if defined(TARGET_OS_MAC)
      unsigned waitms = 500;
      struct timespec timeout;
      timeout.tv_sec  = waitms / 1000;
      timeout.tv_nsec = (waitms % 1000) * 1000 * 1000;

      int nev = kevent(kq, NULL, 0, m_events, KQUEUE_SIZE, &timeout);
      for (int i = 0; i < nev; i++)
      {
        struct kevent &event = m_events[i];
        int fd               = static_cast<int>(event.ident);
        auto it              = std::find(m_sockets.begin(), m_sockets.end(), fd);
        if (it == m_sockets.end())
        {
          continue;
        }
        Socket socket = it->socket;
        int flags     = it->flags;

        if (event.filter == EVFILT_READ)
        {
          if (flags & Acceptable) m_callback.onSocketAcceptable(socket);
          if (flags & Readable)   m_callback.onSocketReadable(socket);
          continue;
        }

        if (event.filter == EVFILT_WRITE)
        {
          if (flags & Writable) m_callback.onSocketWritable(socket);
          continue;
        }

        if ((event.flags & EV_EOF) || (event.flags & EV_ERROR))
        {
          m_callback.onSocketClosed(socket);
          it->flags = Closed;
          struct kevent kevt;
          EV_SET(&kevt, event.ident, EVFILT_READ, EV_DELETE, 0, 0, NULL);
          kevent(kq, &kevt, 1, NULL, 0, NULL);
          EV_SET(&kevt, event.ident, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
          kevent(kq, &kevt, 1, NULL, 0, NULL);
        }
      }
#endif
    }
    LOG_TRACE("Reactor: Thread done");
  }
};

}  // namespace SocketTools
