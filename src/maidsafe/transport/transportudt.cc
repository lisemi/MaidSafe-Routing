﻿/* Copyright (c) 2009 maidsafe.net limited
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

#include "maidsafe/transport/transportudt.h"
#include <boost/cstdint.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/progress.hpp>
#include <boost/scoped_array.hpp>
#include <google/protobuf/descriptor.h>
#include <algorithm>
#include <exception>
#include "maidsafe/base/utils.h"
#include "maidsafe/base/log.h"
#include "maidsafe/base/online.h"
#include "maidsafe/base/routingtable.h"
#include "maidsafe/protobuf/transport_message.pb.h"

namespace transport {

TransportUDT::TransportUDT() : Transport(),
                               ports_to_stop_(),
                               ports_to_stop_mutex_() {
  UDT::startup();
}

TransportUDT::~TransportUDT() {
  if (!stop_all_)
    StopAllListening();
}

void TransportUDT::CleanUp() {
  UDT::cleanup();
}

Port TransportUDT::StartListening(const IP &ip, const Port &port) {
  Port try_port = port;
  struct addrinfo hints, *addrinfo_result;
  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = AI_PASSIVE;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  std::string service = boost::lexical_cast<std::string>(try_port);
  const char *address = NULL;
  if (!ip.empty())
    address = ip.c_str();

  if (0 != getaddrinfo(address, service.c_str(), &hints, &addrinfo_result)) {
    DLOG(ERROR) << "Incorrect listening address. " << ip << ":" << port <<
        std::endl;
    freeaddrinfo(addrinfo_result);
    return kInvalidAddress;
  }

  UdtSocketId listening_socket = UDT::socket(addrinfo_result->ai_family,
                                             addrinfo_result->ai_socktype,
                                             addrinfo_result->ai_protocol);

  if (UDT::ERROR == UDT::bind(listening_socket, addrinfo_result->ai_addr,
      addrinfo_result->ai_addrlen)) {
    DLOG(WARNING) << "UDT bind error: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
    freeaddrinfo(addrinfo_result);
    UDT::close(listening_socket);
    return kBindError;
  }
  freeaddrinfo(addrinfo_result);
  // Modify the port to reflect the port UDT has chosen
  struct sockaddr_in name;
  int name_size;
  UDT::getsockname(listening_socket, reinterpret_cast<sockaddr*>(&name),
                   &name_size);
  Port listening_port = ntohs(name.sin_port);

  if (UDT::ERROR == UDT::listen(listening_socket, 1024)) {
    DLOG(ERROR) << "Failed to start listening port "<< port << ": " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
    UDT::close(listening_socket);
    return kListenError;
  }

  try {
    boost::thread(&TransportUDT::AcceptConnection, this, listening_socket);
  }
  catch(const boost::thread_resource_error&) {
    UDT::close(listening_socket);
    return kThreadResourceError;
  }
  stop_all_ = false;
  listening_ports_.push_back(listening_port);
  return listening_port;
}

bool TransportUDT::StopListening(const Port &port) {
  boost::mutex::scoped_lock lock(ports_to_stop_mutex_);
  ports_to_stop_.push_back(port);
  return true;
}

bool TransportUDT::StopAllListening() {
  if (stop_all_)
    return true;
  // iterate through vector
  stop_all_ = true;
//    while (!listening_ports_.empty()) {
//     boost::this_thread::sleep(boost::posix_time::milliseconds(10));
//    }
//
      return true;
}

void TransportUDT::Send(const TransportMessage &transport_message,
                        const IP &remote_ip,
                        const Port &remote_port,
                        const int &response_timeout) {
  SocketId udt_socket_id = NULL;
  if (Connect(remote_ip, remote_port, udt_socket_id) == kSucess) {
  boost::thread(&TransportUDT::SendData, this, transport_message, udt_socket_id,
                response_timeout, response_timeout);
  } else {
        signal_send_(udt_socket_id, kSendUdtFailure);
  }

  return;
}

void TransportUDT::SendResponse(const TransportMessage &transport_message,
                                const SocketId &socket_id) {
  boost::thread(&TransportUDT::SendData, this, transport_message, socket_id,
                kDefaultSendTimeout, 0);
}

ManagedEndpointId TransportUDT::AddManagedEndpoint(
    const IP &remote_ip,
    const Port &remote_port,
    const IP &rendezvous_ip,
    const Port &rendezvous_port,
    const boost::uint16_t &frequency,
    const boost::uint16_t &retry_count,
    const boost::uint16_t &retry_frequency) {
  // Connect a socket
  SocketId udt_socket_id = NULL;
  if (Connect(remote_ip, remote_port, udt_socket_id))
    return  kConnectError;
  // add socket to vector of managed connections
  ManagedEndpointIds_.push_back(udt_socket_id);

// TODO FINISHME

}

void TransportUDT::AcceptConnection(const UdtSocketId &udt_socket_id) {
  sockaddr_storage clientaddr;
  int addrlen = sizeof(clientaddr);
  UdtSocketId receiver_socket_id;
  struct sockaddr_in name;
  int name_size;
  UDT::getsockname(udt_socket_id, reinterpret_cast<sockaddr*>(&name),
                   &name_size);
  Port this_port = ntohs(name.sin_port);
 // FIXME - get port
  while (true) {
    {

      boost::mutex::scoped_lock lock(ports_to_stop_mutex_);
      if (!listening_ports_.empty()) {
        std::vector<Port>::iterator it;
        it = find(listening_ports_.begin(), listening_ports_.end(), this_port);
        if ((*it != this_port)  || (stop_all_)) {
          UDT::close(udt_socket_id);
          return;
        }
      }
    }
// //     if (stop_all_) {
//       LOG(INFO) << "trying to stop " << std::endl;
//       for (std::vector<Port>::iterator it = listening_ports_.begin();
//             it != listening_ports_.end(); ++it) {
//         if ((*it) == receive_port) {
//           listening_ports_.erase(it);
//            UDT::close(receiver_socket_id);
//           break;
//         }
//       }
//     } // FIXME This would leave unsent/received data !!
    if (UDT::INVALID_SOCK == (receiver_socket_id = UDT::accept(udt_socket_id,
        reinterpret_cast<sockaddr*>(&clientaddr), &addrlen))) {
      LOG(ERROR) << "UDT::accept error: " <<
          UDT::getlasterror().getErrorMessage() << std::endl;
      return;
    }
//   struct sockaddr peer_address;
   // if (kSuccess == UDT:: (receiver_socket_id, &peer_address)) {
      boost::thread(&TransportUDT::ReceiveData, this, receiver_socket_id, -1);
   // } else {
    //  LOG(INFO) << "Problem passing socket off to handler, (closing socket)"
     //           << std::endl;
     // UDT::close(receiver_socket_id);
   // }

  }
}

TransportCondition TransportUDT::SendData(
    const TransportMessage &transport_message,
    const UdtSocketId &udt_socket_id,
    const int &send_timeout,
    const int &receive_timeout) {
  // Set timeout
  if (send_timeout > 0) {
    UDT::setsockopt(udt_socket_id, 0, UDT_SNDTIMEO, &send_timeout,
                    sizeof(send_timeout));
  }

  // Send the message size
  TransportCondition result = SendDataSize(transport_message, udt_socket_id);
  if (result != kSuccess)
    return result;

  // Send the message
  boost::shared_ptr<UdtStats> udt_stats(new UdtStats(udt_socket_id,
                                                     UdtStats::kSend));
  result = SendDataContent(transport_message, udt_socket_id);
  if (result != kSuccess)
    return result;
  signal_send_(udt_socket_id, kSuccess);

  // Get stats
  if (UDT::ERROR == UDT::perfmon(udt_socket_id,
                                 &udt_stats->performance_monitor_)) {
    DLOG(ERROR) << "UDT perfmon error: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
  } else {
    signal_stats_(udt_stats);
  }
  if (receive_timeout > 0) {
    boost::thread(&TransportUDT::ReceiveData, this, udt_socket_id,
                  receive_timeout);
  } else {
    UDT::close(udt_socket_id);
  }
  return kSuccess;
}

TransportCondition TransportUDT::SendDataSize(
    const TransportMessage &transport_message,
    const UdtSocketId &udt_socket_id) {
//  std::string data_size_as_string =
//      boost::lexical_cast<std::string>(data.size());
  DataSize data_size = static_cast<DataSize>(transport_message.ByteSize());
  if (data_size != transport_message.ByteSize()) {
    LOG(INFO) << "TransportUDT::SendDataSize: data > max buffer size." <<
        std::endl;
    signal_send_(udt_socket_id, kSendUdtFailure);
    UDT::close(udt_socket_id);
    return kSendUdtFailure;
  }
  DataSize data_buffer_size = sizeof(DataSize);

  int sent_count;
  if (UDT::ERROR == (sent_count = UDT::send(udt_socket_id,
      reinterpret_cast<char*>(&data_size), data_buffer_size, 0))) {
    LOG(ERROR) << "Cannot send data size: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
    signal_send_(udt_socket_id, kSendUdtFailure);
    UDT::close(udt_socket_id);
    return kSendUdtFailure;
  } else if (sent_count != data_buffer_size) {
    LOG(INFO) << "Sending socket " << udt_socket_id << " timed out" <<
        std::endl;
    signal_send_(udt_socket_id, kSendTimeout);
    UDT::close(udt_socket_id);
    return kSendTimeout;
  }
  return kSuccess;
}

TransportCondition TransportUDT::SendDataContent(
    const TransportMessage &transport_message,
    const UdtSocketId &udt_socket_id) {
  DataSize data_size = static_cast<DataSize>(transport_message.ByteSize());
  boost::shared_array<char> serialised_message(new char[data_size]);
  // Check for valid message
  if (!transport_message.SerializeToArray(serialised_message.get(),
                                          data_size)) {
    DLOG(ERROR) << "TransportUDT::SendDataContent: failed to serialise." <<
        std::endl;
    signal_send_(udt_socket_id, kInvalidData);
    UDT::close(udt_socket_id);
    return kInvalidData;
  }
  DataSize sent_total = 0;
  int sent_size = 0;
  while (sent_total < data_size) {
    if (UDT::ERROR == (sent_size = UDT::send(udt_socket_id,
        serialised_message.get() + sent_total, data_size - sent_total, 0))) {
      LOG(ERROR) << "Send: " << UDT::getlasterror().getErrorMessage() <<
          std::endl;
      signal_send_(udt_socket_id, kSendUdtFailure);
      UDT::close(udt_socket_id);
      return kSendUdtFailure;
    } else if (sent_size == 0) {
      LOG(INFO) << "Sending socket " << udt_socket_id << " timed out" <<
          std::endl;
      signal_send_(udt_socket_id, kSendTimeout);
      UDT::close(udt_socket_id);
      return kSendTimeout;
    }
    sent_total += sent_size;
  }
  return kSuccess;
}

void TransportUDT::ReceiveData(const UdtSocketId &udt_socket_id,
                               const int &receive_timeout) {

                                 boost::progress_timer t;
  // Set timeout
  if (receive_timeout > 0) {
    UDT::setsockopt(udt_socket_id, 0, UDT_RCVTIMEO, &receive_timeout,
                    sizeof(receive_timeout));
  }

  // Get the incoming message size
  DataSize data_size = ReceiveDataSize(udt_socket_id);
  if (data_size == 0)
    return;

  // Get message
  boost::shared_ptr<UdtStats> udt_stats(new UdtStats(udt_socket_id,
                                                     UdtStats::kReceive));
  TransportMessage transport_message;
  if (!ReceiveDataContent(udt_socket_id, data_size, &transport_message))
    return;

  // Get stats
  float rtt;
  if (UDT::ERROR == UDT::perfmon(udt_socket_id,
                                 &udt_stats->performance_monitor_)) {
    DLOG(ERROR) << "UDT perfmon error: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
  } else {
    signal_stats_(udt_stats);
    rtt = udt_stats->performance_monitor_.msRTT;
  }

  // Handle message
  HandleTransportMessage(transport_message, udt_socket_id, rtt);
}

DataSize TransportUDT::ReceiveDataSize(const UdtSocketId &udt_socket_id) {
  DataSize data_buffer_size = sizeof(DataSize);
  DataSize data_size;
  int received_count;
  UDT::getlasterror().clear();
  if (UDT::ERROR == (received_count = UDT::recv(udt_socket_id,
      reinterpret_cast<char*>(&data_size), data_buffer_size, 0))) {
    LOG(ERROR) << "Cannot get data size: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
    signal_receive_(udt_socket_id, kReceiveUdtFailure);
    UDT::close(udt_socket_id);
    return 0;
  } else if (received_count == 0) {
    LOG(INFO) << "Receiving socket " << udt_socket_id << " timed out" <<
        std::endl;
    signal_receive_(udt_socket_id, kReceiveTimeout);
    UDT::close(udt_socket_id);
    return 0;
  }
//  try {
//    data_size_as_string.resize(received_count);
//    data_size =
//        boost::lexical_cast<DataSize>(data_size_as_string);
//  }
//  catch(const std::exception &e) {
//    LOG(ERROR) << "Exception getting data size: " << e.what() << std::endl;
//    signal_receive_(udt_socket_id, kReceiveParseFailure);
//    UDT::close(udt_socket_id);
//    return 0;
//  }
  if (data_size < 1) {
    LOG(ERROR) << "Data size is " << data_size << std::endl;
    signal_receive_(udt_socket_id, kReceiveSizeFailure);
    UDT::close(udt_socket_id);
    return 0;
  }
  return data_size;
}

bool TransportUDT::ReceiveDataContent(
    const UdtSocketId &udt_socket_id,
    const DataSize &data_size,
    TransportMessage *transport_message) {
  boost::shared_array<char> serialised_message(new char[data_size]);
  DataSize received_total = 0;
  int received_size = 0;
  while (received_total < data_size) {
    if (UDT::ERROR == (received_size = UDT::recv(udt_socket_id,
        serialised_message.get() + received_total, data_size - received_total,
        0))) {
      LOG(ERROR) << "Recv: " << UDT::getlasterror().getErrorMessage() <<
          std::endl;
      signal_receive_(udt_socket_id, kReceiveUdtFailure);
      UDT::close(udt_socket_id);
      return false;
    } else if (received_size == 0) {
      LOG(INFO) << "Receiving socket " << udt_socket_id << " timed out" <<
          std::endl;
      signal_receive_(udt_socket_id, kReceiveTimeout);
      UDT::close(udt_socket_id);
      return false;
    }
    received_total += received_size;
  }
  return transport_message->ParseFromArray(serialised_message.get(), data_size);
}

bool TransportUDT::HandleTransportMessage(
    const TransportMessage &transport_message,
    const UdtSocketId &udt_socket_id,
    const float &rtt) {
  bool is_request(transport_message.type() == TransportMessage::kRequest);
  // message data should contain exactly one optional field
  const google::protobuf::Message::Reflection *reflection =
      transport_message.data().GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> field_descriptors;
  reflection->ListFields(transport_message.data(), &field_descriptors);
  if (field_descriptors.size() != 1U) {
    LOG(INFO) << "Bad data - doesn't contain exactly one field." << std::endl;
    if (!is_request)
      signal_receive_(udt_socket_id, kReceiveParseFailure);
    UDT::close(udt_socket_id);
    return false;
  }
  switch (field_descriptors.at(0)->number()) {
    case TransportMessage::Data::kRawMessageFieldNumber:
      signal_message_received_(transport_message.data().raw_message(),
                               udt_socket_id, rtt);
      break;
    case TransportMessage::Data::kRpcMessageFieldNumber:
      if (is_request) {
        signal_rpc_request_received_(transport_message.data().rpc_message(),
                                     udt_socket_id, rtt);
        // Leave socket open to send response on.
      } else {
        signal_rpc_response_received_(transport_message.data().rpc_message(),
                                      udt_socket_id, rtt);
        UDT::close(udt_socket_id);
      }
      break;
    case TransportMessage::Data::kHolePunchingMessageFieldNumber:
      // HandleRendezvousMessage(transport_message.data().hole_punching_message());
      UDT::close(udt_socket_id);
      break;
    case TransportMessage::Data::kPingFieldNumber:
      UDT::close(udt_socket_id);
      break;
    case TransportMessage::Data::kProxyPingFieldNumber:
      UDT::close(udt_socket_id);
      break;
    case TransportMessage::Data::kAcceptConnectFieldNumber:
      UDT::close(udt_socket_id);
      break;
    default:
      LOG(INFO) << "Unrecognised data type in TransportMessage." << std::endl;
      UDT::close(udt_socket_id);
      return false;
  }
  return true;
}

TransportCondition TransportUDT::Connect(const IP &remote_ip,
                                         const Port &remote_port,
                                         SocketId * udt_socket_id) {
  if (!udt_socket_id) {
    struct addrinfo hints, *peer;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string peer_port = boost::lexical_cast<std::string>(remote_port);
    if (0 != getaddrinfo(remote_ip.c_str(), peer_port.c_str(), &hints, &peer)) {
      DLOG(ERROR) << "Incorrect peer address. " << remote_ip << ":" <<
          remote_port << std::endl;
      freeaddrinfo(peer);
      return kInvalidAddress;
    }
    udt_socket_id =
        UDT::socket(peer->ai_family, peer->ai_socktype, peer->ai_protocol);
  }

  // Windows UDP problems fix
#ifdef WIN32
  int mtu(1052);
  UDT::setsockopt(udt_socket_id, 0, UDT_MSS, &mtu, sizeof(mtu));
#endif

  if (UDT::ERROR == UDT::connect(udt_socket_id, peer->ai_addr,
      peer->ai_addrlen)) {
    DLOG(ERROR) << "Connect: " << UDT::getlasterror().getErrorMessage() <<
        std::endl;
    UDT::close(udt_socket_id);
    freeaddrinfo(peer);
    return kConnectError;
  }
  freeaddrinfo(peer);
  return kSuccess;
}


/*
void TransportUDT::AsyncReceiveData(const UdtSocketId &udt_socket_id,
                                    const int &timeout) {
 DLOG(INFO) << "running receive data loop!" << std::endl;
 AddUdtSocketId(udt_socket_id);

  std::vector<UdtSocketId> sockets_ready_to_receive;
  if (UDT::ERROR ==
      GetAndRefreshSocketStates(&sockets_ready_to_receive, NULL)) {
    UDT::close(udt_socket_id);
    return;
  }

 DLOG(INFO) << sockets_ready_to_receive.size() <<
      " receiving sockets available." << std::endl;
  std::vector<UdtSocketId>::iterator it =
      std::find(sockets_ready_to_receive.begin(),
                sockets_ready_to_receive.end(), udt_socket_id);
  if (it == sockets_ready_to_receive.end()) {
   DLOG(INFO) << "Receiving socket unavailable." << std::endl;
    UDT::close(udt_socket_id);
    return;
  }

  // Get the incoming message size
  std::string data_size_as_string(sizeof(DataSize), 0);
  DataSize data_size;
  int received_count;
  UDT::getlasterror().clear();
  if (UDT::ERROR == (received_count = UDT::recv(udt_socket_id,
      &data_size_as_string.at(0), sizeof(DataSize), 0))) {
   DLOG(INFO) << "Cannot get data size: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
    UDT::close(udt_socket_id);
    return;
  }
  try {
    data_size_as_string.resize(received_count);
    data_size =
        boost::lexical_cast<DataSize>(data_size_as_string);
  }
  catch(const std::exception &e) {
   DLOG(INFO) << "Cannot get data size: " << e.what() << std::endl;
    UDT::close(udt_socket_id);
    return;
  }
  if (data_size < 1) {
   DLOG(INFO) << "Data size is " << data_size << std::endl;
    UDT::close(udt_socket_id);
    return;
  }
 DLOG(INFO) << "OK we have the data size " << data_size <<
      " now read it from the socket." << std::endl;

  // Get message
  std::string data(data_size, 0);

  UDT::setsockopt(udt_socket_id, 0, UDT_RCVTIMEO, &timeout, sizeof(timeout));
  DataSize received_total = 0;
  int received_size = 0;
  while (received_total < data_size) {
    if (UDT::ERROR == (received_size = UDT::recv(udt_socket_id,
        &data.at(0) + received_total, data_size - received_total, 0))) {
     DLOG(INFO) << "Recv: " << UDT::getlasterror().getErrorMessage() <<
          std::endl;
      UDT::close(udt_socket_id);
      return;
    }
    received_total += received_size;
    boost::this_thread::sleep(boost::posix_time::milliseconds(10));
  }
 DLOG(INFO) << "SUCCESS we have read " << received_total << " bytes of data." <<
      std::endl;
  float rtt;
  UDT::TRACEINFO performance_monitor;
  if (UDT::ERROR == UDT::perfmon(udt_socket_id, &performance_monitor)) {
    DLOG(ERROR) << "UDT perfmon error: " <<
        UDT::getlasterror().getErrorMessage() << std::endl;
  } else {
    float rtt = performance_monitor.msRTT;
    float bandwidth = performance_monitor.mbpsBandwidth;
    float receive_rate = performance_monitor.mbpsRecvRate;
    float send_rate = performance_monitor.mbpsSendRate;
   DLOG(INFO) << "looked for " << data_size << " got " << received_total <<
        std::endl;
   DLOG(INFO) <<"RTT = : " << rtt << "msecs " << std::endl;
   DLOG(INFO) <<"B/W used = : " << bandwidth << " Mb/s " << std::endl;
   DLOG(INFO) <<"RcvRate = : " << receive_rate << " Mb/s " << std::endl;
   DLOG(INFO) <<"SndRate = : " << send_rate << " Mb/s " << std::endl;
  }

  ParseTransportMessage(data, udt_socket_id, rtt);
}*/

bool TransportUDT::CheckSocketSend(const UdtSocketId &udt_socket_id) {
  return CheckSocket(udt_socket_id, true);
}

bool TransportUDT::CheckSocketReceive(const UdtSocketId &udt_socket_id) {
  return CheckSocket(udt_socket_id, false);
}

bool TransportUDT::CheckSocket(const UdtSocketId &udt_socket_id, bool send) {
  std::vector<UdtSocketId> socket_to_check(1, udt_socket_id);
  std::vector<UdtSocketId> sockets_ready;
  int result;
  if (send) {
    result = UDT::selectEx(socket_to_check, NULL, &sockets_ready, NULL, 1000);
  } else {
    result = UDT::selectEx(socket_to_check, &sockets_ready, NULL, NULL, 1000);
  }
  if (result == UDT::ERROR) {
    LOG(ERROR) << "Error checking socket." <<
          UDT::getlasterror().getErrorMessage() << std::endl;
    UDT::close(udt_socket_id);
    return false;
  }
  std::string message = (send ? " to send." : " to receive.");
  if (sockets_ready.empty()) {
    //LOG(INFO) << "Cannot use socket " << udt_socket_id << message <<
    //    "  Closing it!" << std::endl;
    //UDT::close(udt_socket_id);
    return false;
  } else {
    return true;
  }
}

}  // namespace transport
