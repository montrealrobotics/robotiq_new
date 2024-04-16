// Copyright (c) 2016, Toyota Research Institute. All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.

// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <robotiq_2f_gripper_control/robotiq_2f_gripper_api.h>
#include <robotiq_2f_gripper_control/robotiq_2f_gripper_tcp_client.h>
#include <robotiq_2f_gripper_control/robotiq_2f_gripper_hw_interface.h>
#include <controller_manager/controller_manager.h>
#include <ros/ros.h>

// Used to convert seconds elapsed to nanoseconds
static const double BILLION = 1000000000.0;

class GenericHWLoop
{
public:
    GenericHWLoop(
            const ros::NodeHandle& nh,
            boost::shared_ptr<robotiq_2f_gripper_control::Robotiq2FGripperHWInterface> hardware_interface)
        : nh_(nh), name_("generic_hw_control_loop"), hardware_interface_(hardware_interface)
    {
        ROS_DEBUG("creating loop");

        //! Create the controller manager
        controller_manager_.reset(new controller_manager::ControllerManager(hardware_interface_.get(), nh_));
        ROS_DEBUG("created controller manager");

        //! Load rosparams
        ros::NodeHandle rpsnh(nh, name_);

        loop_hz_ = rpsnh.param<double>("loop_hz", 30);
        cycle_time_error_threshold_ = rpsnh.param<double>("cycle_time_error_threshold", 0.1);

        //! Get current time for use with first update
        clock_gettime(CLOCK_MONOTONIC, &last_time_);

        //! Start timer
        desired_update_freq_ = ros::Duration(1 / loop_hz_);
        non_realtime_loop_ = nh_.createTimer(desired_update_freq_, &GenericHWLoop::update, this);
        ROS_DEBUG("created timer");
    }

    /** \brief Timer event
     *         Note: we do not use the TimerEvent time difference because it does NOT guarantee that
     * the time source is
     *         strictly linearly increasing
     */
    void update(const ros::TimerEvent& e)
    {
        //! Get change in time
        clock_gettime(CLOCK_MONOTONIC, &current_time_);
        elapsed_time_ = ros::Duration(current_time_.tv_sec - last_time_.tv_sec
                                      + (current_time_.tv_nsec - last_time_.tv_nsec) / BILLION);
        last_time_ = current_time_;
        ROS_DEBUG_STREAM_THROTTLE_NAMED(1, "GenericHWLoop","Sampled update loop with elapsed time " << elapsed_time_.toSec());

        //! Error check cycle time
        const double cycle_time_error = (elapsed_time_ - desired_update_freq_).toSec();
        if (cycle_time_error > cycle_time_error_threshold_)
        {
            ROS_WARN_STREAM_NAMED(name_, "Cycle time exceeded error threshold by: "
                                  << cycle_time_error << ", cycle time: " << elapsed_time_
                                  << ", threshold: " << cycle_time_error_threshold_);
        }

        //! Input
        hardware_interface_->read(elapsed_time_);

        //! Control
        controller_manager_->update(ros::Time::now(), elapsed_time_);

        //! Output
        hardware_interface_->write(elapsed_time_);
    }

protected:
    // Startup and shutdown of the internal node inside a roscpp program
    ros::NodeHandle nh_;

    // Name of this class
    std::string name_;

    // Settings
    ros::Duration desired_update_freq_;
    double cycle_time_error_threshold_;

    // Timing
    ros::Timer non_realtime_loop_;
    ros::Duration elapsed_time_;
    double loop_hz_;
    struct timespec last_time_{};
    struct timespec current_time_{};

    /** \brief ROS Controller Manager and Runner
     *
     * This class advertises a ROS interface for loading, unloading, starting, and
     * stopping ros_control-based controllers. It also serializes execution of all
     * running controllers in \ref update.
     */
    boost::shared_ptr<controller_manager::ControllerManager> controller_manager_;

    /** \brief Abstract Hardware Interface for your robot */
    boost::shared_ptr<robotiq_2f_gripper_control::Robotiq2FGripperHWInterface> hardware_interface_;
};

int main(int argc, char** argv)
{

    using robotiq_2f_gripper_control::Robotiq2FGripperTcpClient;
    ros::init(argc, argv, "robotiq_2f_gripper_node");
    ros::NodeHandle nh;
    ros::NodeHandle pnh("~");

    // NOTE: We run the ROS loop in a separate thread as external calls such
    // as service callbacks to load controllers can block the (main) control loop
    ros::AsyncSpinner spinner(2);
    spinner.start();

    // Parameter names
    std::string ipaddr;
    int port;
    bool activate;

    pnh.param<std::string>("ipaddr", ipaddr, "192.168.1.102");
    pnh.param<int>("port", port, 63352);
    pnh.param<bool>("activate", activate, true);

    // Create the hw client layer
    boost::shared_ptr<robotiq_2f_gripper_control::Robotiq2FGripperTcpClient> tcp_client
            (new robotiq_2f_gripper_control::Robotiq2FGripperTcpClient());
    tcp_client->init(pnh);
    bool started = tcp_client->connectToServer(ipaddr, port);
    if (!started)
    {
        throw std::runtime_error("Could not connect to gripper.");
    }

    ROS_INFO("Tcp client started");

    // Create the hw api layer
    boost::shared_ptr<robotiq_2f_gripper_control::Robotiq2FGripperAPI> hw_api
            (new robotiq_2f_gripper_control::Robotiq2FGripperAPI(tcp_client));

    // Create the hardware interface layer
    boost::shared_ptr<robotiq_2f_gripper_control::Robotiq2FGripperHWInterface> hw_interface
            (new robotiq_2f_gripper_control::Robotiq2FGripperHWInterface(pnh, hw_api));

    ROS_INFO("Created hw interface");

    int sid;
    robotiq::MotionStatus motionState = robotiq::MOTION_STARTED;
    robotiq::InitializationMode initMode = robotiq::INIT_ACTIVATION;

    hw_api->getSid(&sid);
    ROS_INFO("Gripper SID received");
    if (activate)
    {
        hw_api->setMotionState(motionState);
        hw_api->setInitialization(initMode);
    }

    // Register interfaces
    hardware_interface::JointStateInterface joint_state_interface;
    hardware_interface::PositionJointInterface position_joint_interface;
    hw_interface->configure(joint_state_interface, position_joint_interface);
    hw_interface->registerInterface(&joint_state_interface);
    hw_interface->registerInterface(&position_joint_interface);

    ROS_DEBUG("registered control interfaces");

    // Start the control loop
    GenericHWLoop control_loop(pnh, hw_interface);
    ROS_INFO("started");

    // Wait until shutdown signal received
    ros::waitForShutdown();

    return 0;
}
