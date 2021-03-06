/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ListenSocket.h"
#include <fcntl.h>
#include "ConnectionOrientedSocket.h"
#include "DataSocket.h"
#include "ListenSocketConsumer.h"
#include "mozilla/RefPtr.h"
#include "nsXULAppAPI.h"
#include "UnixSocketConnector.h"

namespace mozilla {
namespace ipc {

//
// ListenSocketIO
//

class ListenSocketIO final
  : public UnixSocketWatcher
  , public SocketIOBase
{
public:
  class ListenTask;

  ListenSocketIO(nsIThread* aConsumerThread,
                 MessageLoop* aIOLoop,
                 ListenSocket* aListenSocket,
                 UnixSocketConnector* aConnector);
  ~ListenSocketIO();

  UnixSocketConnector* GetConnector() const;

  // Task callback methods
  //

  /**
   * Run bind/listen to prepare for further runs of accept()
   */
  void Listen(ConnectionOrientedSocketIO* aCOSocketIO);

  // I/O callback methods
  //

  void OnConnected() override;
  void OnError(const char* aFunction, int aErrno) override;
  void OnListening() override;
  void OnSocketCanAcceptWithoutBlocking() override;

  // Methods for |SocketIOBase|
  //

  SocketBase* GetSocketBase() override;

  bool IsShutdownOnConsumerThread() const override;
  bool IsShutdownOnIOThread() const override;

  void ShutdownOnConsumerThread() override;
  void ShutdownOnIOThread() override;

private:
  void FireSocketError();

  /**
   * Consumer pointer. Non-thread safe RefPtr, so should only be manipulated
   * directly from consumer thread. All non-consumer-thread accesses should
   * happen with mIO as container.
   */
  RefPtr<ListenSocket> mListenSocket;

  /**
   * Connector object used to create the connection we are currently using.
   */
  nsAutoPtr<UnixSocketConnector> mConnector;

  /**
   * If true, do not requeue whatever task we're running
   */
  bool mShuttingDownOnIOThread;

  /**
   * Number of valid bytes in |mAddress|
   */
  socklen_t mAddressLength;

  /**
   * Address structure of the socket currently in use
   */
  struct sockaddr_storage mAddress;

  ConnectionOrientedSocketIO* mCOSocketIO;
};

ListenSocketIO::ListenSocketIO(nsIThread* aConsumerThread,
                               MessageLoop* aIOLoop,
                               ListenSocket* aListenSocket,
                               UnixSocketConnector* aConnector)
  : UnixSocketWatcher(aIOLoop)
  , SocketIOBase(aConsumerThread)
  , mListenSocket(aListenSocket)
  , mConnector(aConnector)
  , mShuttingDownOnIOThread(false)
  , mAddressLength(0)
  , mCOSocketIO(nullptr)
{
  MOZ_ASSERT(mListenSocket);
  MOZ_ASSERT(mConnector);
}

ListenSocketIO::~ListenSocketIO()
{
  MOZ_ASSERT(IsConsumerThread());
  MOZ_ASSERT(IsShutdownOnConsumerThread());
}

UnixSocketConnector*
ListenSocketIO::GetConnector() const
{
  return mConnector;
}

void
ListenSocketIO::Listen(ConnectionOrientedSocketIO* aCOSocketIO)
{
  MOZ_ASSERT(MessageLoopForIO::current() == GetIOLoop());
  MOZ_ASSERT(mConnector);
  MOZ_ASSERT(aCOSocketIO);

  struct sockaddr* address = reinterpret_cast<struct sockaddr*>(&mAddress);
  mAddressLength = sizeof(mAddress);

  if (!IsOpen()) {
    int fd;
    nsresult rv = mConnector->CreateListenSocket(address, &mAddressLength,
                                                 fd);
    if (NS_FAILED(rv)) {
      FireSocketError();
      return;
    }
    SetFd(fd);
  }

  mCOSocketIO = aCOSocketIO;

  // calls OnListening on success, or OnError otherwise
  nsresult rv = UnixSocketWatcher::Listen(address, mAddressLength);
  NS_WARN_IF(NS_FAILED(rv));
}

void
ListenSocketIO::OnConnected()
{
  MOZ_ASSERT(MessageLoopForIO::current() == GetIOLoop());

  NS_NOTREACHED("Invalid call to |ListenSocketIO::OnConnected|");
}

void
ListenSocketIO::OnListening()
{
  MOZ_ASSERT(MessageLoopForIO::current() == GetIOLoop());
  MOZ_ASSERT(GetConnectionStatus() == SOCKET_IS_LISTENING);

  AddWatchers(READ_WATCHER, true);

  /* We signal a successful 'connection' to a local address for listening. */
  GetConsumerThread()->Dispatch(
    new SocketIOEventRunnable(this, SocketIOEventRunnable::CONNECT_SUCCESS),
    NS_DISPATCH_NORMAL);
}

void
ListenSocketIO::OnError(const char* aFunction, int aErrno)
{
  MOZ_ASSERT(MessageLoopForIO::current() == GetIOLoop());

  UnixFdWatcher::OnError(aFunction, aErrno);
  FireSocketError();
}

void
ListenSocketIO::FireSocketError()
{
  MOZ_ASSERT(MessageLoopForIO::current() == GetIOLoop());

  // Clean up watchers, statuses, fds
  Close();

  // Tell the consumer thread we've errored
  GetConsumerThread()->Dispatch(
    new SocketIOEventRunnable(this, SocketIOEventRunnable::CONNECT_ERROR),
    NS_DISPATCH_NORMAL);
}

void
ListenSocketIO::OnSocketCanAcceptWithoutBlocking()
{
  MOZ_ASSERT(MessageLoopForIO::current() == GetIOLoop());
  MOZ_ASSERT(GetConnectionStatus() == SOCKET_IS_LISTENING);
  MOZ_ASSERT(mCOSocketIO);

  RemoveWatchers(READ_WATCHER|WRITE_WATCHER);

  struct sockaddr_storage storage;
  socklen_t addressLength = sizeof(storage);

  int fd;
  nsresult rv = mConnector->AcceptStreamSocket(
    GetFd(),
    reinterpret_cast<struct sockaddr*>(&storage), &addressLength,
    fd);
  if (NS_FAILED(rv)) {
    FireSocketError();
    return;
  }

  mCOSocketIO->Accept(fd,
                      reinterpret_cast<struct sockaddr*>(&storage),
                      addressLength);
}

// |SocketIOBase|

SocketBase*
ListenSocketIO::GetSocketBase()
{
  return mListenSocket.get();
}

bool
ListenSocketIO::IsShutdownOnConsumerThread() const
{
  MOZ_ASSERT(IsConsumerThread());

  return mListenSocket == nullptr;
}

bool
ListenSocketIO::IsShutdownOnIOThread() const
{
  return mShuttingDownOnIOThread;
}

void
ListenSocketIO::ShutdownOnConsumerThread()
{
  MOZ_ASSERT(IsConsumerThread());
  MOZ_ASSERT(!IsShutdownOnConsumerThread());

  mListenSocket = nullptr;
}

void
ListenSocketIO::ShutdownOnIOThread()
{
  MOZ_ASSERT(!IsConsumerThread());
  MOZ_ASSERT(!mShuttingDownOnIOThread);

  Close(); // will also remove fd from I/O loop
  mShuttingDownOnIOThread = true;
}

//
// Socket tasks
//

class ListenSocketIO::ListenTask final
  : public SocketIOTask<ListenSocketIO>
{
public:
  ListenTask(ListenSocketIO* aIO, ConnectionOrientedSocketIO* aCOSocketIO)
  : SocketIOTask<ListenSocketIO>(aIO)
  , mCOSocketIO(aCOSocketIO)
  {
    MOZ_ASSERT(mCOSocketIO);
  }

  void Run() override
  {
    MOZ_ASSERT(!GetIO()->IsConsumerThread());

    if (!IsCanceled()) {
      GetIO()->Listen(mCOSocketIO);
    }
  }

private:
  ConnectionOrientedSocketIO* mCOSocketIO;
};

//
// UnixSocketConsumer
//

ListenSocket::ListenSocket(ListenSocketConsumer* aConsumer, int aIndex)
  : mIO(nullptr)
  , mConsumer(aConsumer)
  , mIndex(aIndex)
{
  MOZ_ASSERT(mConsumer);
}

ListenSocket::~ListenSocket()
{
  MOZ_ASSERT(!mIO);
}

nsresult
ListenSocket::Listen(UnixSocketConnector* aConnector,
                     nsIThread* aConsumerThread,
                     MessageLoop* aIOLoop,
                     ConnectionOrientedSocket* aCOSocket)
{
  MOZ_ASSERT(!mIO);

  mIO = new ListenSocketIO(aConsumerThread, aIOLoop, this, aConnector);

  // Prepared I/O object, now start listening.
  nsresult rv = Listen(aCOSocket);
  if (NS_FAILED(rv)) {
    delete mIO;
    mIO = nullptr;
    return rv;
  }

  return NS_OK;
}

nsresult
ListenSocket::Listen(UnixSocketConnector* aConnector,
                     ConnectionOrientedSocket* aCOSocket)
{
  nsIThread* consumerThread = nullptr;
  nsresult rv = NS_GetCurrentThread(&consumerThread);
  if (NS_FAILED(rv)) {
    return rv;
  }

  return Listen(aConnector, consumerThread, XRE_GetIOMessageLoop(), aCOSocket);
}

nsresult
ListenSocket::Listen(ConnectionOrientedSocket* aCOSocket)
{
  MOZ_ASSERT(aCOSocket);
  MOZ_ASSERT(mIO);

  // We first prepare the connection-oriented socket with a
  // socket connector and a socket I/O class.

  nsAutoPtr<UnixSocketConnector> connector;
  nsresult rv = mIO->GetConnector()->Duplicate(*connector.StartAssignment());
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoPtr<ConnectionOrientedSocketIO> io;
  rv = aCOSocket->PrepareAccept(connector,
                                mIO->GetConsumerThread(), mIO->GetIOLoop(),
                                *io.StartAssignment());
  if (NS_FAILED(rv)) {
    return rv;
  }
  connector.forget(); // now owned by |io|

  // Then we start listening for connection requests.

  SetConnectionStatus(SOCKET_LISTENING);

  mIO->GetIOLoop()->PostTask(
    FROM_HERE, new ListenSocketIO::ListenTask(mIO, io.forget()));

  return NS_OK;
}

// |SocketBase|

void
ListenSocket::Close()
{
  if (!mIO) {
    return;
  }

  MOZ_ASSERT(mIO->IsConsumerThread());

  // From this point on, we consider mIO as being deleted. We sever
  // the relationship here so any future calls to listen or connect
  // will create a new implementation.
  mIO->ShutdownOnConsumerThread();
  mIO->GetIOLoop()->PostTask(FROM_HERE, new SocketIOShutdownTask(mIO));
  mIO = nullptr;

  NotifyDisconnect();
}

void
ListenSocket::OnConnectSuccess()
{
  mConsumer->OnConnectSuccess(mIndex);
}

void
ListenSocket::OnConnectError()
{
  mConsumer->OnConnectError(mIndex);
}

void
ListenSocket::OnDisconnect()
{
  mConsumer->OnDisconnect(mIndex);
}

} // namespace ipc
} // namespace mozilla
