/*******************************************************************************
 *  Copyright 2012 maidsafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of maidsafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of maidsafe.net. *
 ******************************************************************************/

#include <vector>
#include <thread>

#include "boost/thread/future.hpp"

#include "maidsafe/routing/routing_api.h"
#include "maidsafe/routing/routing_api_impl.h"
#include "maidsafe/routing/rpcs.h"
#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/routing_pb.h"
#include "maidsafe/routing/tests/routing_network.h"

namespace args = std::placeholders;

namespace maidsafe {

namespace routing {

namespace test {


class FindNode : public GenericNode {
 public:
  explicit FindNode(bool client_mode = false)
      : GenericNode(client_mode),
        messages_() {
    LOG(kVerbose) << "RoutingNode constructor";
  }

  virtual ~FindNode() {}

  void RudpSend(const Endpoint &peer_endpoint,
                const std::string &message,
                rudp::MessageSentFunctor message_sent_functor) {
    routing_->impl_->rudp_.Send(peer_endpoint, message, message_sent_functor);
  }

  void PrintRoutingTable() {
    LOG(kInfo) << " PrintRoutingTable() ";
    for (auto node_info : routing_->impl_->routing_table_.routing_table_nodes_) {
      LOG(kInfo) << "Port: " << node_info.endpoint.port();
    }
  }

  bool RoutingTableHasEndpoint(const Endpoint &endpoint) {
    return (std::find_if(routing_->impl_->routing_table_.routing_table_nodes_.begin(),
                         routing_->impl_->routing_table_.routing_table_nodes_.end(),
                 [&endpoint](const NodeInfo &node_info) {
                   return (endpoint == node_info.endpoint); }) !=
            routing_->impl_->routing_table_.routing_table_nodes_.end());
  }

 protected:
  std::vector<std::pair<int32_t, std::string> > messages_;
};

template <typename NodeType>
class FindNodeNetwork : public GenericNetwork<NodeType> {
 public:
  FindNodeNetwork(void) : GenericNetwork<NodeType>() {}

 protected:
  testing::AssertionResult Find(std::shared_ptr<NodeType> source,
                                std::shared_ptr<NodeType> destination) {
    std::string find_node_rpc(rpcs::FindNodes(destination->Id(), source->Id(),
        true, source->endpoint()).SerializeAsString());
    boost::promise<bool> message_sent_promise;
    auto message_sent_future = message_sent_promise.get_future();
    uint8_t attempts(0);
    rudp::MessageSentFunctor message_sent_functor = [&] (bool message_sent) {
        if (message_sent) {
          message_sent_promise.set_value(true);
        } else if (attempts < 3) {
          source->RudpSend(
              this->nodes_[1]->endpoint(),
              find_node_rpc,
              message_sent_functor);
        } else {
          message_sent_promise.set_value(false);
        }
      };
    source->PrintRoutingTable();
    source->RudpSend(this->nodes_[1]->endpoint(), find_node_rpc, message_sent_functor);
    if(!message_sent_future.timed_wait(boost::posix_time::seconds(10))) {
      return testing::AssertionFailure() << "Unable to send FindValue rpc to bootstrap endpoint - "
                                         << destination->endpoint().port();
    }
    return testing::AssertionSuccess();
  }

  void PrintAllRoutingTables() {
    for (size_t index = 0; index < this->nodes_.size(); ++index) {
      LOG(kInfo) << "Routing table of node # " << index;
      this->nodes_[index]->PrintRoutingTable();
    }
  }
};


TYPED_TEST_CASE_P(FindNodeNetwork);

TYPED_TEST_P(FindNodeNetwork, FUNC_FindNodes) {
  this->SetUpNetwork(6);
  LOG(kInfo) << "source: " << this->nodes_[3]->endpoint().port()
             << " destination: " << this->nodes_[2]->endpoint().port();
  this->PrintAllRoutingTables();
  EXPECT_TRUE(this->Find(this->nodes_[3], this->nodes_[2]));
  std::this_thread::sleep_for(std::chrono::seconds(5));
  EXPECT_TRUE(this->nodes_[3]->RoutingTableHasEndpoint(this->nodes_[2]->endpoint()));
}

REGISTER_TYPED_TEST_CASE_P(FindNodeNetwork, FUNC_FindNodes);
INSTANTIATE_TYPED_TEST_CASE_P(MAIDSAFE, FindNodeNetwork, FindNode);

}  // namespace test

}  // namespace routing

}  // namespace maidsafe
