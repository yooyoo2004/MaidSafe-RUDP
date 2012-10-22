/*******************************************************************************
 *  Copyright 2012 MaidSafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of MaidSafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of MaidSafe.net. *
 ******************************************************************************/

#include "maidsafe/rudp/connection_manager.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "maidsafe/common/log.h"

#include "maidsafe/rudp/connection.h"
#include "maidsafe/rudp/transport.h"
#include "maidsafe/rudp/core/multiplexer.h"
#include "maidsafe/rudp/core/socket.h"
#include "maidsafe/rudp/packets/handshake_packet.h"
#include "maidsafe/rudp/parameters.h"
#include "maidsafe/rudp/utils.h"

namespace asio = boost::asio;
namespace ip = asio::ip;
namespace bptime = boost::posix_time;


namespace maidsafe {

namespace rudp {

namespace detail {

namespace {

typedef boost::asio::ip::udp::endpoint Endpoint;

bool IsNormal(std::shared_ptr<Connection> connection) {
  return connection->state() == Connection::State::kPermanent ||
         connection->state() == Connection::State::kUnvalidated ||
         connection->state() == Connection::State::kBootstrapping;
}

}  // unnamed namespace


ConnectionManager::ConnectionManager(std::shared_ptr<Transport> transport,
                                     const boost::asio::io_service::strand& strand,
                                     MultiplexerPtr multiplexer,
                                     const NodeId& this_node_id,
                                     std::shared_ptr<asymm::PublicKey> this_public_key)
    : connections_(),
      mutex_(),
      transport_(transport),
      strand_(strand),
      multiplexer_(multiplexer),
      kThisNodeId_(this_node_id),
      this_public_key_(this_public_key),
      sockets_() {
  multiplexer_->dispatcher_.SetConnectionManager(this);
}

ConnectionManager::~ConnectionManager() {
  Close();
}

void ConnectionManager::Close() {
  multiplexer_->dispatcher_.SetConnectionManager(nullptr);
  boost::mutex::scoped_lock lock(mutex_);
  for (auto it = connections_.begin(); it != connections_.end(); ++it)
    strand_.post(std::bind(&Connection::Close, *it));
}

void ConnectionManager::Connect(const NodeId& peer_id,
                                const Endpoint& peer_endpoint,
                                const std::string& validation_data,
                                const bptime::time_duration& connect_attempt_timeout,
                                const bptime::time_duration& lifespan,
                                const std::function<void()>& failure_functor) {
  if (std::shared_ptr<Transport> transport = transport_.lock()) {
    ConnectionPtr connection(std::make_shared<Connection>(transport, strand_, multiplexer_));
    connection->StartConnecting(peer_id, peer_endpoint, validation_data, connect_attempt_timeout,
                                lifespan, failure_functor);
  }
}

bool ConnectionManager::AddConnection(ConnectionPtr connection) {
  assert(connection->state() != Connection::State::kPending);
  std::pair<ConnectionGroup::iterator, bool> result;
  boost::mutex::scoped_lock lock(mutex_);
  if (IsNormal(connection))
    result = connections_.insert(connection);
  else
    return false;
  assert(result.second);
  return result.second;
}

bool ConnectionManager::CloseConnection(const NodeId& peer_id) {
  boost::mutex::scoped_lock lock(mutex_);
  auto itr(FindConnection(peer_id));
  if (itr == connections_.end()) {
    LOG(kWarning) << "Not currently connected to " << DebugId(peer_id);
    return false;
  }

  ConnectionPtr connection(*itr);
  lock.unlock();
  strand_.dispatch([=] { connection->Close(); });  // NOLINT (Fraser)
  return true;
}

void ConnectionManager::RemoveConnection(ConnectionPtr connection) {
  boost::mutex::scoped_lock lock(mutex_);
  assert(IsNormal(connection));
  connections_.erase(connection);
}


ConnectionManager::ConnectionPtr ConnectionManager::GetConnection(const NodeId& peer_id) {
  boost::mutex::scoped_lock lock(mutex_);
  auto itr(FindConnection(peer_id));
  if (itr == connections_.end()) {
    LOG(kInfo) << "Not currently connected to " << DebugId(peer_id);
    return ConnectionPtr();
  }
  return *itr;
}

void ConnectionManager::Ping(const NodeId& peer_id,
                             const Endpoint& peer_endpoint,
                             const std::function<void(int)> &ping_functor) {  // NOLINT (Fraser)
  if (std::shared_ptr<Transport> transport = transport_.lock()) {
    assert(ping_functor);
    ConnectionPtr connection(std::make_shared<Connection>(transport, strand_, multiplexer_));
    connection->Ping(peer_id, peer_endpoint, ping_functor);
  }
}

bool ConnectionManager::Send(const NodeId& peer_id,
                             const std::string& message,
                             const std::function<void(int)>& message_sent_functor) {  // NOLINT (Fraser)
  boost::mutex::scoped_lock lock(mutex_);
  auto itr(FindConnection(peer_id));
  if (itr == connections_.end()) {
    LOG(kWarning) << "Not currently connected to " << DebugId(peer_id);
    return false;
  }

  ConnectionPtr connection(*itr);
  lock.unlock();
  strand_.dispatch([=] { connection->StartSending(message, message_sent_functor); });  // NOLINT (Fraser)
  return true;
}

Socket* ConnectionManager::GetSocket(const asio::const_buffer& data, const Endpoint& endpoint) {
  if (sockets_.empty())
    return nullptr;

  uint32_t socket_id(0);
  if (!Packet::DecodeDestinationSocketId(&socket_id, data)) {
    LOG(kError) << "Received a non-RUDP packet from " << endpoint;
    return nullptr;
  }

  SocketMap::const_iterator socket_iter(sockets_.end());
  if (socket_id == 0) {
    HandshakePacket handshake_packet;
    if (!handshake_packet.Decode(data)) {
      LOG(kVerbose) << "Failed to decode handshake packet from " << endpoint;
      return nullptr;
    }
    if (handshake_packet.ConnectionReason() == Session::kNormal) {
      // This is a handshake packet on a newly-added socket
      LOG(kVerbose) << "This is a handshake packet on a newly-added socket from " << endpoint;
      socket_iter = std::find_if(
          sockets_.begin(),
          sockets_.end(),
          [endpoint](const SocketMap::value_type& socket_pair) {
            return socket_pair.second->PeerEndpoint() == endpoint;
          });
      // If the socket wasn't found, this could be a connect attempt from a peer using symmetric
      // NAT, so the peer's port may be different to what this node was told to expect.
      if (socket_iter == sockets_.end()) {
        socket_iter = std::find_if(
            sockets_.begin(),
            sockets_.end(),
            [endpoint](const SocketMap::value_type& socket_pair) {
              return socket_pair.second->PeerEndpoint().address() == endpoint.address() &&
                     !OnPrivateNetwork(socket_pair.second->PeerEndpoint()) &&
                     !socket_pair.second->IsConnected();
            });
        if (socket_iter != sockets_.end()) {
          LOG(kVerbose) << "\t\t\tUpdating peer's endpoint from "
                        << socket_iter->second->PeerEndpoint() << " to " << endpoint;
          socket_iter->second->UpdatePeerEndpoint(endpoint);
          LOG(kVerbose) << "\t\t\tPeer's endpoint now: "
                        << socket_iter->second->PeerEndpoint() << "  and guessed port = "
                        << socket_iter->second->PeerGuessedPort();
        }
      }
    } else {  // Session::mode_ != kNormal
      socket_iter = std::find_if(
          sockets_.begin(),
          sockets_.end(),
          [endpoint](const SocketMap::value_type& socket_pair) {
            return socket_pair.second->PeerEndpoint() == endpoint;
          });
      if (socket_iter == sockets_.end()) {
        // This is a handshake packet from a peer trying to ping this node or join the network
        HandlePingFrom(handshake_packet, endpoint);
        return nullptr;
      } else {
        if (sockets_.size() == 1U) {
          // This is a handshake packet from a peer replying to this node's join attempt,
          // or from a peer starting a zero state network with this node
          LOG(kVerbose) << "This is a handshake packet from " << endpoint
                        << " which is replying to a join request, or starting a new network";
        } else {
          LOG(kVerbose) << "This is a handshake packet from " << endpoint
                        << " which is replying to a ping request";
        }
      }
    }
  } else {
    // This packet is intended for a specific connection.
    socket_iter = sockets_.find(socket_id);
  }

  if (socket_iter != sockets_.end()) {
    return socket_iter->second;
  } else {
    const unsigned char* p = asio::buffer_cast<const unsigned char*>(data);
    LOG(kInfo) << "Received a packet \"0x" << std::hex << static_cast<int>(*p) << std::dec
                << "\" for unknown connection " << socket_id << " from " << endpoint;
    return nullptr;
  }
}

void ConnectionManager::HandlePingFrom(const HandshakePacket& handshake_packet,
                                       const Endpoint& endpoint) {
  LOG(kVerbose) << "This is a handshake packet from " << endpoint
                << " which is trying to ping this node or join the network";
  if (handshake_packet.node_id() == kThisNodeId_) {
    LOG(kWarning) << DebugId(kThisNodeId_) << " is handshaking with another local transport.";
    return;
  }
  if (IsValid(endpoint)) {
    // Check if this joining node is already connected
    ConnectionPtr joining_connection;
    bool bootstrap_and_drop(handshake_packet.ConnectionReason() == Session::kBootstrapAndDrop);
    {
      boost::mutex::scoped_lock lock(mutex_);
      auto itr(FindConnection(handshake_packet.node_id()));
      if (itr != connections_.end() && !bootstrap_and_drop)
        joining_connection = *itr;
    }
    if (joining_connection) {
      LOG(kWarning) << DebugId(kThisNodeId_) << " received another bootstrap connection request "
                    << "from currently connected peer " << DebugId(handshake_packet.node_id())
                    << " - " << endpoint << " - closing connection.";
      joining_connection->Close();
    } else {
      // Joining node is not already connected - start new bootstrap or temporary connection
      Connect(handshake_packet.node_id(), endpoint, "", Parameters::bootstrap_connect_timeout,
              bootstrap_and_drop ? bptime::time_duration() :
                                   Parameters::bootstrap_connection_lifespan);
    }
    return;
  }
}

bool ConnectionManager::MakeConnectionPermanent(const NodeId& peer_id,
                                                bool validated,
                                                Endpoint& peer_endpoint) {
  peer_endpoint = Endpoint();
  boost::mutex::scoped_lock lock(mutex_);
  auto itr(FindConnection(peer_id));
  if (itr == connections_.end()) {
    LOG(kWarning) << "Not currently connected to " << DebugId(peer_id);
    return false;
  }
  (*itr)->MakePermanent(validated);
  // TODO(Fraser#5#): 2012-09-11 - Handle passing back peer_endpoint iff it's direct-connected.
  if (!OnPrivateNetwork((*itr)->Socket().PeerEndpoint()))
    peer_endpoint = (*itr)->Socket().PeerEndpoint();
  return true;
}

Endpoint ConnectionManager::ThisEndpoint(const NodeId& peer_id) {
  boost::mutex::scoped_lock lock(mutex_);
  auto itr(FindConnection(peer_id));
  if (itr == connections_.end())
    return Endpoint();
  return (*itr)->Socket().ThisEndpoint();
}

void ConnectionManager::SetBestGuessExternalEndpoint(const Endpoint& external_endpoint) {
  multiplexer_->best_guess_external_endpoint_ = external_endpoint;
}

Endpoint ConnectionManager::RemoteNatDetectionEndpoint(const NodeId& peer_id) {
  boost::mutex::scoped_lock lock(mutex_);
  auto itr(FindConnection(peer_id));
  if (itr == connections_.end())
    return Endpoint();
  return (*itr)->Socket().RemoteNatDetectionEndpoint();
}


uint32_t ConnectionManager::AddSocket(Socket* socket) {
  // Generate a new unique id for the socket.
  uint32_t id = 0;
  while (id == 0 || sockets_.find(id) != sockets_.end())
    id = RandomUint32();

  sockets_[id] = socket;
  return id;
}

void ConnectionManager::RemoveSocket(uint32_t id) {
  if (id)
    sockets_.erase(id);
}

size_t ConnectionManager::NormalConnectionsCount() const {
  boost::mutex::scoped_lock lock(mutex_);
  return connections_.size();
}

ConnectionManager::ConnectionGroup::iterator ConnectionManager::FindConnection(
    const NodeId& peer_id) const {
  assert(!mutex_.try_lock());
  return std::find_if(connections_.begin(),
                      connections_.end(),
                      [&peer_id](const ConnectionPtr& connection) {
                        return connection->Socket().PeerNodeId() == peer_id;
                      });
}

NodeId ConnectionManager::node_id() const {
  return kThisNodeId_;
}

std::shared_ptr<asymm::PublicKey> ConnectionManager::public_key() const {
  return this_public_key_;
}

std::string ConnectionManager::DebugString() {
  std::string s;
  boost::mutex::scoped_lock lock(mutex_);
  for (auto c : connections_) {
    s += "\t\tPeer " + c->PeerDebugId();
    s += std::string("  ") + boost::lexical_cast<std::string>(c->state());
    s += std::string("   Expires in ") + bptime::to_simple_string(c->ExpiresFromNow()) + "\n";
  }
  return s;
}

}  // namespace detail

}  // namespace rudp

}  // namespace maidsafe
