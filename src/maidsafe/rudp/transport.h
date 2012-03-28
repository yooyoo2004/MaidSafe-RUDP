/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Author: Christopher M. Kohlhoff (chris at kohlhoff dot com)

#ifndef MAIDSAFE_TRANSPORT_MC_TRANSPORT_H_
#define MAIDSAFE_TRANSPORT_MC_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "boost/asio/io_service.hpp"
#include "boost/asio/strand.hpp"
#include "maidsafe/transport/transport.h"
#include "maidsafe/transport/contact.h"
#include "maidsafe/transport/rudp_parameters.h"
#include "maidsafe/transport/message_handler.h"

#if MAIDSAFE_TRANSPORT_VERSION != 300
#  error This API is not compatible with the installed library.\
    Please update the maidsafe-transport library.
#endif

namespace maidsafe {

namespace transport {

class RudpAcceptor;
class RudpConnection;
class RudpMultiplexer;
class RudpSocket;

typedef std::function<void(const TransportCondition&)> ConnectFunctor;

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#endif
class McTransport : public std::enable_shared_from_this<McTransport> {
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
 public:
  explicit McTransport(boost::asio::io_service &asio_service);  // NOLINT
  virtual ~McTransport();

  virtual TransportCondition StartListening(const Endpoint &endpoint);
  virtual TransportCondition Bootstrap(const std::vector<Contact> &candidates);

  virtual void StopListening();
  // This timeout defines the max allowed duration for the receiver to respond
  // to a request. If the receiver is expected to respond slowly (e.g. because
  // of a large request msg to be processed), a long duration shall be given for
  // this timeout. If no response is expected, kImmediateTimeout shall be given.
  virtual void Send(const std::string &data,
                    const Endpoint &endpoint,
                    const Timeout &timeout);
  void Connect(const Endpoint &endpoint,
               const Timeout &timeout,
               ConnectFunctor callback);
  static DataSize kMaxTransportMessageSize() { return 67108864; }



  void RendezvousConnect(const Endpoint &peer_endpoint,
                         const std::string &this_node_id);
  // Returns kSuccess if the connection existed and was closed.  Returns
  // kInvalidMcConnection if the connection didn't exist.  If this causes the
  // size of connected_endpoints_ to dop to 0, this transport will remove
  // itself from ManagedConnections which will cause it to be destroyed.
  int CloseConnection(const Endpoint &peer_endpoint);
  int Send(const Endpoint &peer_endpoint, const std::string &message) const;
  Endpoint this_endpoint() const;
  Endpoint Bootstrap(const std::vector<Endpoint> &bootstrap_endpoints);
  std::vector<Endpoint> connected_endpoints() const;
  size_t connected_endpoints_size() const;
  static uint32_t kMaxConnections() { return 50; }

 private:
  McTransport(const McTransport&);
  McTransport &operator=(const McTransport&);

  typedef std::shared_ptr<RudpMultiplexer> MultiplexerPtr;
  typedef std::shared_ptr<RudpAcceptor> AcceptorPtr;
  typedef std::shared_ptr<RudpConnection> ConnectionPtr;
  typedef std::set<ConnectionPtr> ConnectionSet;

  static void CloseAcceptor(AcceptorPtr acceptor);
  static void CloseMultiplexer(MultiplexerPtr multiplexer);

  void StartDispatch();
  void HandleDispatch(MultiplexerPtr multiplexer,
                      const boost::system::error_code &ec);

  void StartAccept();
  void HandleAccept(AcceptorPtr acceptor,
                    ConnectionPtr connection,
                    const boost::system::error_code &ec);

  void DoSend(const std::string &data,
              const Endpoint &endpoint,
              const Timeout &timeout);
  void DoConnect(const Endpoint &endpoint,
                 const Timeout &timeout,
                 ConnectFunctor callback);
  friend class RudpConnection;
  void InsertConnection(ConnectionPtr connection);
  void DoInsertConnection(ConnectionPtr connection);
  void RemoveConnection(ConnectionPtr connection);
  void DoRemoveConnection(ConnectionPtr connection);

  boost::asio::io_service::strand strand_;
  MultiplexerPtr multiplexer_;
  AcceptorPtr acceptor_;

  // Because the connections can be in an idle initial state with no pending
  // async operations (after calling PrepareSend()), they are kept alive with
  // a shared_ptr in this map, as well as in the async operation handlers.
  ConnectionSet connections_;
};

typedef std::shared_ptr<RudpTransport> RudpTransportPtr;

}  // namespace transport

}  // namespace maidsafe

#endif  // MAIDSAFE_TRANSPORT_MC_TRANSPORT_H_