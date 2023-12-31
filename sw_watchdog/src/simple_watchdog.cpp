// Copyright (c) 2020 Mapless AI, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <atomic>
#include <iostream>
#include <map>

#include "rclcpp/rclcpp.hpp"
#include "rcutils/cmdline_parser.h"
#include "rclcpp_components/register_node_macro.hpp"

#include "rclcpp_lifecycle/lifecycle_node.hpp"

#include "rcutils/logging_macros.h"

#include "sw_watchdog_msgs/msg/heartbeat.hpp"
#include "sw_watchdog_msgs/msg/status.hpp"
#include "sw_watchdog/visibility_control.h"
#include "message_filters/subscriber.h"
#include "message_filters/cache.h"

using namespace std::chrono_literals;

constexpr char OPTION_AUTO_START[] = "--activate";
constexpr char OPTION_PUB_STATUS[] = "--publish";
constexpr char DEFAULT_TOPIC_NAME[] = "heartbeat";

namespace {

void print_usage()
{
    std::cout <<
        "Usage: simple_watchdog lease [" << OPTION_AUTO_START << "] [-h]\n\n"
        "required arguments:\n"
        "\tlease: Lease in positive integer milliseconds granted to the watched entity.\n"
        "optional arguments:\n"
        "\t" << OPTION_AUTO_START << ": Start the watchdog on creation.  Defaults to false.\n"
        "\t" << OPTION_PUB_STATUS << ": Publish lease expiration of the watched entity.  "
        "Defaults to false.\n"
        "\t-h : Print this help message." <<
        std::endl;
}

} // anonymous ns

namespace sw_watchdog
{

/// SimpleWatchdog inheriting from rclcpp_lifecycle::LifecycleNode
/**
 * Internally relies on the QoS liveliness policy provided by rmw implementation (e.g., DDS).
 * The lease passed to this watchdog has to be > the period of the heartbeat signal to account
 * for network transmission times.
 */
class SimpleWatchdog : public rclcpp_lifecycle::LifecycleNode
{
public:
    SW_WATCHDOG_PUBLIC
    explicit SimpleWatchdog(const rclcpp::NodeOptions& options)
        : rclcpp_lifecycle::LifecycleNode("simple_watchdog", options),
          autostart_(false), enable_pub_(false), topic_name_(DEFAULT_TOPIC_NAME), qos_profile_(10)
    {
        // Parse node arguments
        const std::vector<std::string>& args = this->get_node_options().arguments();
        std::vector<char *> cargs;
        cargs.reserve(args.size());
        for(size_t i = 0; i < args.size(); ++i)
            cargs.push_back(const_cast<char*>(args[i].c_str()));

        if(args.size() < 2 || rcutils_cli_option_exist(&cargs[0], &cargs[0] + cargs.size(), "-h")) {
            print_usage();
            std::exit(0);
        }

        // Configuration of the Cache
        heartbeat_cache_sub.subscribe((rclcpp::Node*)this, topic_name_);
        heartbeat_cache.setCacheSize(25);
        heartbeat_cache.connectInput(heartbeat_cache_sub);
        //message_filters::Cache<sw_watchdog_msgs::msg::Heartbeat> heartbeat_cache(heartbeat_cache_sub, 100);

        // Lease duration must be >= heartbeat's lease duration
        lease_duration_ = std::chrono::milliseconds(std::stoul(args[1]));

        if(rcutils_cli_option_exist(&cargs[0], &cargs[0] + cargs.size(), OPTION_AUTO_START))
            autostart_ = true;
        if(rcutils_cli_option_exist(&cargs[0], &cargs[0] + cargs.size(), OPTION_PUB_STATUS))
            enable_pub_ = true;

        if(autostart_) {
            configure();
            activate();
        }
    }

    void cache_callback(const sw_watchdog_msgs::msg::Heartbeat message)
    {   
        //RCLCPP_INFO(get_logger(), "CacheCallback triggert");
        RCLCPP_INFO(get_logger(), "Put message with ID %d in cache", message.checkpoint_id);
    }
    
    bool check_messages_in_cache(sw_watchdog_msgs::msg::Heartbeat* lost_message){
        auto messages_in_cache = heartbeat_cache.getInterval(heartbeat_cache.getOldestTime(), heartbeat_cache.getLatestTime());
        std::map<int, std::vector<sw_watchdog_msgs::msg::Heartbeat>> node_id_list;
        for (std::shared_ptr<const sw_watchdog_msgs::msg::Heartbeat> message : messages_in_cache){
            if (node_id_list.count(message->checkpoint_id)==0){
                std::vector<sw_watchdog_msgs::msg::Heartbeat> msg_vec;
                msg_vec.push_back(*message);
                node_id_list.insert({message->checkpoint_id, msg_vec});
            }
            else{
                node_id_list.at(message->checkpoint_id).push_back(*message);
            }
        }
        std::map<int, uint64_t> node_avg_time_inervall_map;
        for (std::pair<const int, std::vector<sw_watchdog_msgs::msg::Heartbeat>> node_and_msgs_in_cache : node_id_list){
            std::vector<uint64_t> intervall_between_msgs;
            size_t i = 1;
            while(i<node_and_msgs_in_cache.second.size()){
                intervall_between_msgs.push_back(
                    // Check if uint64_t is large enough
                    (node_and_msgs_in_cache.second[i].header.stamp.sec-node_and_msgs_in_cache.second[i].header.stamp.sec)*1000000000 +
                    (node_and_msgs_in_cache.second[i-1].header.stamp.nanosec-node_and_msgs_in_cache.second[i-1].header.stamp.nanosec)
                    );
                i++;
            }
            // Check if a special treatment is required for the case that only one message for a id is in cache
            uint64_t avg_intervall_between_msgs = std::accumulate(intervall_between_msgs.begin(), intervall_between_msgs.end(), 0.0) / intervall_between_msgs.size();
            node_avg_time_inervall_map.insert({node_and_msgs_in_cache.first, avg_intervall_between_msgs});
        }
        std::pair<const int, uint64_t> result = std::pair<const int, uint64_t>(-1, 10000000000);
        rclcpp::Time now = this->get_clock()->now();
        for (std::pair<const int, uint64_t> node_intervall_pair : node_avg_time_inervall_map){
            uint64_t diff_last_msg_to_now =
                (now.seconds() - node_id_list.at(node_intervall_pair.first).back().header.stamp.sec)*1000000000 +
                (now.seconds() - node_id_list.at(node_intervall_pair.first).back().header.stamp.sec);
            if(diff_last_msg_to_now < result.second){     
                // Warning here that the parameter is not used   
                std::pair<const int, uint64_t> result = std::pair<const int, uint64_t>(node_intervall_pair.first, diff_last_msg_to_now);
            }
        }
        *lost_message = node_id_list.at(result.first).back();
        return false;
    }

    /// Publish lease expiry of the watched entity
    void publish_failure(sw_watchdog_msgs::msg::Heartbeat lost_message)
    {
        auto msg = std::make_unique<sw_watchdog_msgs::msg::Status>();
        rclcpp::Time now = this->get_clock()->now();
        msg->header.stamp = now;
        msg->missed_number = lost_message.checkpoint_id;
        RCLCPP_INFO(get_logger(),
                        "Publishing failure message. Faulty node was with ID %u at [%f] seconds",
                        msg->missed_number, now.seconds());
        // Print the current state for demo purposes 
        /*
        if (!failure_pub_->is_activated()) {
            RCLCPP_INFO(get_logger(),
                        "Lifecycle publisher is currently inactive. Messages are not published.");
        } else {
            RCLCPP_INFO(get_logger(),
                        "Publishing failure message. Faulty node was with ID %u at [%f] seconds",
                        msg->missed_number, now.seconds());
        }*/

        // Only if the publisher is in an active state, the message transfer is
        // enabled and the message actually published.
        failure_pub_->publish(std::move(msg));
    }

    /// Transition callback for state configuring
    rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn on_configure(
        const rclcpp_lifecycle::State &)
    {
        // Initialize and configure node
        qos_profile_
            .liveliness(RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC)
            .liveliness_lease_duration(lease_duration_);

        heartbeat_sub_options_.event_callbacks.liveliness_callback =
            [this](rclcpp::QOSLivelinessChangedInfo &event) -> void {
                printf("Reader Liveliness changed event: \n");
                printf("  alive_count: %d\n", event.alive_count);
                printf("  not_alive_count: %d\n", event.not_alive_count);
                printf("  alive_count_change: %d\n", event.alive_count_change);
                printf("  not_alive_count_change: %d\n", event.not_alive_count_change);
                if(event.alive_count_change <= 0) {
                    sw_watchdog_msgs::msg::Heartbeat lost_message;
                    // Check which message got lost in the cache
                    if(!check_messages_in_cache(&lost_message)){
                        publish_failure(lost_message);
                    }
                }
            };

        if(enable_pub_)
            failure_pub_ = create_publisher<sw_watchdog_msgs::msg::Status>("failure", 1); /* QoS history_depth */

        RCUTILS_LOG_INFO_NAMED(get_name(), "on_configure() is called.");
        return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
    }

    /// Transition callback for state activating
    rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn on_activate(
        const rclcpp_lifecycle::State &)
    {
        if(!heartbeat_sub_) {
            heartbeat_sub_ = create_subscription<sw_watchdog_msgs::msg::Heartbeat>(
                topic_name_,
                qos_profile_,
                [this](const typename sw_watchdog_msgs::msg::Heartbeat::SharedPtr msg) -> void {
                    RCLCPP_INFO(get_logger(), "Watchdog raised, heartbeat sent at %d seconds", msg->header.stamp.sec);
                },
                heartbeat_sub_options_);
        }

        heartbeat_cache.registerCallback(&SimpleWatchdog::cache_callback, this); // Uncertain if even required
        //heartbeat_cache.registerCallback(std::bind(&SimpleWatchdog::cache_callback, this, sw_watchdog_msgs::msg::Heartbeat));

        // Starting from this point, all messages are sent to the network.
        if (enable_pub_)
            failure_pub_->on_activate();

        // Starting from this point, all messages are sent to the network.
        RCUTILS_LOG_INFO_NAMED(get_name(), "on_activate() is called.");
        return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
    }

    /// Transition callback for state deactivating
    rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn on_deactivate(
        const rclcpp_lifecycle::State &)
    {
        heartbeat_sub_.reset(); // XXX there does not seem to be a 'deactivate' for subscribers.
        heartbeat_sub_ = nullptr;

        // Starting from this point, all messages are no longer sent to the network.
        if(enable_pub_)
            failure_pub_->on_deactivate();

        RCUTILS_LOG_INFO_NAMED(get_name(), "on_deactivate() is called.");

        return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
    }

    /// Transition callback for state cleaningup
    rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn on_cleanup(
        const rclcpp_lifecycle::State &)
    {
        failure_pub_.reset();
        RCUTILS_LOG_INFO_NAMED(get_name(), "on cleanup is called.");

        return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
    }

    /// Transition callback for state shutting down
    rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn on_shutdown(
        const rclcpp_lifecycle::State &state)
    {
        heartbeat_sub_.reset();
        heartbeat_sub_ = nullptr;
        failure_pub_.reset();

        RCUTILS_LOG_INFO_NAMED(get_name(), "on shutdown is called from state %s.", state.label().c_str());

        return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
    }

private:
    /// The lease duration granted to the remote (heartbeat) publisher
    std::chrono::milliseconds lease_duration_;
    rclcpp::Subscription<sw_watchdog_msgs::msg::Heartbeat>::SharedPtr heartbeat_sub_ = nullptr;
    // A seperate Message Filters Subscription is requiered for the Cache
    message_filters::Subscriber<sw_watchdog_msgs::msg::Heartbeat> heartbeat_cache_sub;
    message_filters::Cache<sw_watchdog_msgs::msg::Heartbeat> heartbeat_cache; 
    /// Publish lease expiry for the watched entity
    // By default, a lifecycle publisher is inactive by creation and has to be activated to publish.
    std::shared_ptr<rclcpp_lifecycle::LifecyclePublisher<sw_watchdog_msgs::msg::Status>> failure_pub_ = nullptr;
    /// Whether to enable the watchdog on startup. Otherwise, lifecycle transitions have to be raised.
    bool autostart_;
    /// Whether a lease expiry should be published
    bool enable_pub_;
    /// Topic name for heartbeat signal by the watched entity
    const std::string topic_name_;
    rclcpp::QoS qos_profile_;
    rclcpp::SubscriptionOptions heartbeat_sub_options_;
};

} // namespace sw_watchdog

RCLCPP_COMPONENTS_REGISTER_NODE(sw_watchdog::SimpleWatchdog)
