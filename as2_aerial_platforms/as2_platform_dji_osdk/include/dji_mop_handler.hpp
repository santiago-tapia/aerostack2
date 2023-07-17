#ifndef DJI_MOP_HANDLER_HPP_
#define DJI_MOP_HANDLER_HPP_

#include <mutex>  // std::mutex
#include <queue>  // std::queue
#include <rclcpp/rclcpp.hpp>
#include <thread>
#include "as2_core/names/topics.hpp"
#include "as2_core/node.hpp"
#include "dji_vehicle.hpp"
#include "std_msgs/msg/string.hpp"

#include "dji_linux_helpers.hpp"
#include "dji_vehicle.hpp"
#include "osdk_platform.h"
#include "osdkhal_linux.h"

#define CHANNEL_ID 49152
#define RELIABLE_RECV_ONCE_BUFFER_SIZE (1024)
#define RELIABLE_SEND_ONCE_BUFFER_SIZE (1024)
#define SEND_MAX_RETRIES 3
#define MSG_DELIMITER '\r'
#define READ_RATE 50
#define WRITE_RATE 500
#define RECONNECTION_RATE 5000

class DJIMopHandler {
  DJI::OSDK::Vehicle* vehicle_ptr_;
  as2::Node* node_ptr_;
  DJI::OSDK::MopPipeline* pipeline_ = NULL;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr uplink_pub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr downlink_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr keep_alive_sub_;

 public:
  DJIMopHandler(DJI::OSDK::Vehicle* vehicle, as2::Node* node)
      : vehicle_ptr_(vehicle), node_ptr_(node) {
    uplink_pub_ = node_ptr_->create_publisher<std_msgs::msg::String>(
        "/uplink", as2_names::topics::global::qos);

    downlink_sub_ = node_ptr_->create_subscription<std_msgs::msg::String>(
        "/downlink", as2_names::topics::global::qos,
        std::bind(&DJIMopHandler::downlinkCB, this, std::placeholders::_1));

    keep_alive_sub_ = node_ptr_->create_subscription<std_msgs::msg::String>(
        "/keep_alive", rclcpp::QoS(1),
        std::bind(&DJIMopHandler::keepAliveCB, this, std::placeholders::_1));

    static auto timer_ = node_ptr_->create_timer(
        std::chrono::milliseconds(RECONNECTION_RATE), [this]() {
          // Check if thread is already running to launch a new mopServer
          if (mop_communication_th_.get_id() == std::thread::id()) {
            RCLCPP_INFO(node_ptr_->get_logger(), "CREATING NEW MOP CHANNEL");
            mop_communication_th_ = std::thread(
                &DJIMopHandler::mopCommunicationFnc, this, CHANNEL_ID);
          }
          if (mop_send_th_.get_id() == std::thread::id()) {
            RCLCPP_INFO(node_ptr_->get_logger(), "NEW SEND THREAD");

            mop_send_th_ =
                std::thread(&DJIMopHandler::mopSendFnc, this, CHANNEL_ID);
          }

          // If connection closed, wait to join thread before launching a new
          // one
          if (closed_) {
            mop_communication_th_.join();
            mop_send_th_.join();
            closed_ = false;
          }
        });
  };

  ~DJIMopHandler() {
    mop_communication_th_.join();
    mop_send_th_.join();
    OsdkOsal_Free(recvBuf_);
    OsdkOsal_Free(sendBuf_);
    pipeline_->~MopPipeline();
    vehicle_ptr_->mopServer->~MopServer();
  };

  void downlinkCB(const std_msgs::msg::String::SharedPtr msg);
  void keepAliveCB(const std_msgs::msg::String::SharedPtr msg);
  void mopCommunicationFnc(int id);
  void mopSendFnc(int id);

 private:
  bool getReady();
  bool send();
  void parseData(MopPipeline::DataPackType data);
  std::string bytesToString(const uint8_t* data, size_t len);
  std::tuple<std::vector<std::string>, std::string> checkString(
      const std::string& input, char delimiter);
  void publishUplink(const MopPipeline::DataPackType* dataPack);
  void close();

 private:
  std::queue<std::string> msg_queue_;
  std::mutex queue_mtx_;
  std::atomic<bool> connected_ = false;  // when read msg from downlink
  std::atomic<bool> closed_ = false;     // when open new MOP Server
  std::string status_ = "{}\r";
  std::string missed_msg_ = "";
  std::thread mop_communication_th_;
  std::thread mop_send_th_;
  uint8_t* recvBuf_;
  uint8_t* sendBuf_;
  MopPipeline::DataPackType readPack_;
  MopPipeline::DataPackType writePack_;
};
#endif  // DJI_MOP_HANDLER_HPP_