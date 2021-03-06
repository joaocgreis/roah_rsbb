/*
 * Copyright 2014 Instituto de Sistemas e Robotica, Instituto Superior Tecnico
 *
 * This file is part of RoAH RSBB.
 *
 * RoAH RSBB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RoAH RSBB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with RoAH RSBB.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CORE_ZONE_EXEC_H__
#define __CORE_ZONE_EXEC_H__

#include "core_includes.h"

#include "core_shared_state.h"
#include "core_zone_base.h"



class ExecutingBenchmark
  : boost::noncopyable
{
  protected:
    CoreSharedState& ss_;

    Publisher timeout_pub_;

    Event const& event_;

    DisplayText display_log_;
    DisplayText display_online_data_;

    roah_rsbb_msgs::BenchmarkState::State state_;
    enum { PHASE_PRE, PHASE_EXEC, PHASE_POST } phase_;
    bool stoped_due_to_timeout_;
    TimeControl time_;
    Time last_stop_time_;
    string state_desc_;
    Time state_time_;

    string manual_operation_;

    RsbbLog log_;

    vector<ScoringItem> scoring_;

    void
    set_state (Time const& now,
               roah_rsbb_msgs::BenchmarkState::State const& state,
               string const& desc)
    {
      state_ = state;
      state_desc_ = desc;
      state_time_ = now;

      log_.set_state (now, state, desc);
    }

    virtual void
    phase_exec_2 (Time const& now) {}

    void
    phase_exec (string const& desc)
    {
      Time now = Time::now();

      if (phase_ == PHASE_PRE) {
        time_.start_reset (now);
      }
      else {
        time_.resume_hot (now);
      }
      phase_ = PHASE_EXEC;
      stoped_due_to_timeout_ = false;
      set_state (now, roah_rsbb_msgs::BenchmarkState_State_PREPARE, desc);

      phase_exec_2 (now);
    }

    virtual void
    phase_post_2 (Time const& now) {}

    void
    phase_post (string const& desc)
    {
      Time now = Time::now();

      cout << "AT PHASE POST --- REASON: " << desc << endl;

      phase_ = PHASE_POST;
      last_stop_time_ = now;
      set_state (now, roah_rsbb_msgs::BenchmarkState_State_STOP, desc);

      time_.stop_pause (now);

      phase_post_2 (now);
    }

  private:
    boost::function<void() > end_;

    void
    timeout_2 ()
    {
      if (phase_ != PHASE_EXEC) {
        return;
      }

      stoped_due_to_timeout_ = true;
      phase_post ("Stopped due to timeout!");

      timeout_pub_.publish (std_msgs::Empty());
    }

  public:
    ExecutingBenchmark (CoreSharedState& ss,
                        Event const& event,
                        boost::function<void() > end)
      : ss_ (ss)
      , timeout_pub_ (ss_.nh.advertise<std_msgs::Empty> ("/timeout", 1, false))
      , event_ (event)
      , display_log_()
      , display_online_data_()
      , phase_ (PHASE_PRE)
      , stoped_due_to_timeout_ (false)
      , time_ (ss, event_.benchmark.timeout, boost::bind (&ExecutingBenchmark::timeout_2, this))
      , manual_operation_ ("")
      , log_ (event.team, event.round, event.run, ss.run_uuid, display_log_)
      , scoring_ (event.benchmark.scoring)
      , end_ (end)
    {
      Time now = Time::now();

      set_state (now, roah_rsbb_msgs::BenchmarkState_State_STOP, "All OK for start");
    }

    virtual ~ExecutingBenchmark()
    {
      log_.end();
    }

    void
    terminate_benchmark()
    {
      time_.stop_pause (Time());
      stop_communication();
      end_();
    }

    void
    set_score (roah_rsbb::Score const& score)
    {
      Time now = Time::now();

      for (ScoringItem& i : scoring_) {
        if ( (score.group == i.group) && (score.desc == i.desc)) {
          i.current_value = score.value;
          log_.log_score ("/rsbb_log/score", now, score);
          return;
        }
      }
      ROS_ERROR_STREAM ("Did not find group " << score.group << " desc " << score.desc);
    }

    virtual void
    manual_operation_complete()
    {
      ROS_WARN_STREAM ("Ignored unexpected manual operation command");
    }

    virtual void
    omf_complete()
    {
      ROS_WARN_STREAM ("Ignored unexpected omf_complete command");
    }

    virtual void
    omf_damaged (uint8_t damaged)
    {
      ROS_WARN_STREAM ("Ignored unexpected omf_damaged command");
    }

    virtual void
    omf_button (uint8_t button)
    {
      ROS_WARN_STREAM ("Ignored unexpected omf_button command");
    }

    void
    start()
    {
      switch (state_) {
        case roah_rsbb_msgs::BenchmarkState_State_STOP:
          phase_exec ("Robot preparing for task");
          return;
        case roah_rsbb_msgs::BenchmarkState_State_PREPARE:
        case roah_rsbb_msgs::BenchmarkState_State_GOAL_TX:
        case roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT:
          return;
      }
    }

    void
    stop()
    {
      switch (state_) {
        case roah_rsbb_msgs::BenchmarkState_State_STOP:
          terminate_benchmark();
          return;
        case roah_rsbb_msgs::BenchmarkState_State_PREPARE:
        case roah_rsbb_msgs::BenchmarkState_State_GOAL_TX:
        case roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT:
          phase_post ("Benchmark Stopped by referee");
          return;
      }
    }

    virtual void
    fill_2 (Time const& now,
            roah_rsbb::ZoneState& zone) = 0;

    void
    fill (Time const& now,
          roah_rsbb::ZoneState& zone)
    {
      switch (phase_) {
        case PHASE_PRE:
          zone.timer = event_.benchmark.timeout;
          break;
        case PHASE_EXEC:
          zone.timer = time_.get_until_timeout (now);
          break;
        case PHASE_POST:
          zone.timer = last_stop_time_ + Duration (param_direct<double> ("~after_stop_duration", 120.0)) - now;
          break;
      }

      zone.state = state_desc_;

      zone.manual_operation = manual_operation_;

      zone.start_enabled = state_ == roah_rsbb_msgs::BenchmarkState_State_STOP;
      zone.stop_enabled = ! zone.start_enabled;

      const size_t log_size = param_direct<int> ("~display_log_size", 3000);
      zone.log = display_log_.last (log_size);
      zone.online_data = display_online_data_.last (log_size);

      for (ScoringItem const& i : scoring_) {
        if (zone.scoring.empty() || (zone.scoring.back().group_name != i.group)) {
          zone.scoring.push_back (roah_rsbb::ZoneScoreGroup());
          zone.scoring.back().group_name = i.group;
        }
        switch (i.type) {
          case ScoringItem::SCORING_BOOL:
            zone.scoring.back().types.push_back (roah_rsbb::ZoneScoreGroup::SCORING_BOOL);
            break;
          case ScoringItem::SCORING_UINT:
            zone.scoring.back().types.push_back (roah_rsbb::ZoneScoreGroup::SCORING_UINT);
            break;
          default:
            ROS_FATAL_STREAM ("type in ScoringItem error");
            abort_rsbb();
        }
        zone.scoring.back().descriptions.push_back (i.desc);
        zone.scoring.back().current_values.push_back (i.current_value);
      }

      fill_2 (now, zone);
    }

    roah_rsbb_msgs::BenchmarkState::State
    state()
    {
      return state_;
    }

    virtual void
    stop_communication() = 0;
};



class ExecutingSingleRobotBenchmark
  : public ExecutingBenchmark
{
  protected:
    string robot_name_;

    unique_ptr<roah_rsbb::RosPrivateChannel> private_channel_;

    roah_rsbb_msgs::Time ack_;
    Duration last_skew_;
    Time last_beacon_;

    Timer state_timer_;

    uint32_t messages_saved_;

    ReceiverRepeated rcv_notifications_;
    ReceiverRepeated rcv_activation_event_;
    ReceiverRepeated rcv_visitor_;
    ReceiverRepeated rcv_final_command_;

    virtual void
    receive_robot_state_2 (Time const& now,
                           roah_rsbb_msgs::RobotState const& msg) {}

    virtual void
    fill_benchmark_state_2 (roah_rsbb_msgs::BenchmarkState& msg) {}

  private:
    void
    transmit_state (const TimerEvent& = TimerEvent())
    {
      ROS_DEBUG ("Transmitting benchmark state");

      roah_rsbb_msgs::BenchmarkState msg;
      msg.set_benchmark_type (event_.benchmark_code);
      msg.set_benchmark_state (state_);
      (* (msg.mutable_acknowledgement())) = ack_;
      fill_benchmark_state_2 (msg);
      private_channel_->send (msg);
    }

    void
    receive_benchmark_state (boost::asio::ip::udp::endpoint endpoint,
                             uint16_t comp_id,
                             uint16_t msg_type,
                             std::shared_ptr<const roah_rsbb_msgs::BenchmarkState> msg)
    {
      ROS_ERROR_STREAM ("Detected another RSBB transmitting in the private channel for team " << event_.team << ": " << endpoint.address().to_string()
                        << ":" << endpoint.port()
                        << ", COMP_ID " << comp_id
                        << ", MSG_TYPE " << msg_type << endl);
    }

    void
    receive_robot_state (boost::asio::ip::udp::endpoint endpoint,
                         uint16_t comp_id,
                         uint16_t msg_type,
                         std::shared_ptr<const roah_rsbb_msgs::RobotState> msg)
    {
      Time now = last_beacon_ = Time::now();
      Time msg_time (msg->time().sec(), msg->time().nsec());
      Duration last_skew_ = msg_time - now;

      ROS_DEBUG_STREAM ("Received RobotState from " << endpoint.address().to_string()
                        << ":" << endpoint.port()
                        << ", COMP_ID " << comp_id
                        << ", MSG_TYPE " << msg_type
                        << ", time: " << msg->time().sec() << "." << msg->time().nsec()
                        << ", skew: " << last_skew_);

      ss_.active_robots.add (event_.team, robot_name_, last_skew_, now);

      messages_saved_ = msg->messages_saved();
      /* if ( (messages_saved_ == 0) */
      /*      && (param_direct<bool> ("~check_messages_saved", true)) */
      /*      && ( (state_ == roah_rsbb_msgs::BenchmarkState_State_GOAL_TX) */
      /*           || (state_ == roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT)) */
      /*      && ( (now - state_time_).toSec() > param_direct<double> ("~check_messages_saved_timeout", 5.0))) { */
      /*   phase_post ("STOPPED BENCHMARK: Messages saved information received from robot is still 0!"); */
      /* } */

      ack_ = msg->time();

      rcv_notifications_.receive (now, msg->notifications());
      rcv_activation_event_.receive (now, msg->activation_event());
      rcv_visitor_.receive (now, msg->visitor());
      rcv_final_command_.receive (now, msg->final_command());

      receive_robot_state_2 (now, *msg);
    }

  public:
    ExecutingSingleRobotBenchmark (CoreSharedState& ss,
                                   Event const& event,
                                   boost::function<void() > end,
                                   string const& robot_name)
      : ExecutingBenchmark (ss, event, end)
      , robot_name_ (robot_name)
      , private_channel_ (new roah_rsbb::RosPrivateChannel (param_direct<string> ("~rsbb_host", "10.255.255.255"),
                          ss_.private_port(),
                          event_.password,
                          param_direct<string> ("~rsbb_cypher", "aes-128-cbc")))
      , state_timer_ (ss_.nh.createTimer (Duration (0.2), &ExecutingSingleRobotBenchmark::transmit_state, this))
      , messages_saved_ (0)
      , rcv_notifications_ (log_, "/notification", display_online_data_)
      , rcv_activation_event_ (log_, "/command", display_online_data_)
      , rcv_visitor_ (log_, "/visitor", display_online_data_)
      , rcv_final_command_ (log_, "/command", display_online_data_)
    {
      ack_.set_sec (0);
      ack_.set_nsec (0);
      private_channel_->set_benchmark_state_callback (&ExecutingSingleRobotBenchmark::receive_benchmark_state, this);
      private_channel_->set_robot_state_callback (&ExecutingSingleRobotBenchmark::receive_robot_state, this);
      ss_.benchmarking_robots[event_.team] = make_pair (robot_name_, private_channel_->port());
    }

    void
    stop_communication()
    {
      state_timer_.stop();
      private_channel_->signal_benchmark_state_received().disconnect_all_slots();
      private_channel_->signal_robot_state_received().disconnect_all_slots();
      ss_.benchmarking_robots.erase (event_.team);
    }
};



class ExecutingSimpleBenchmark
  : public ExecutingSingleRobotBenchmark
{
    void
    receive_robot_state_2 (Time const& now,
                           roah_rsbb_msgs::RobotState const& msg)
    {
      switch (state_) {
        case roah_rsbb_msgs::BenchmarkState_State_STOP:
          break;
        case roah_rsbb_msgs::BenchmarkState_State_PREPARE:
          if (msg.robot_state() == roah_rsbb_msgs::RobotState_State_WAITING_GOAL) {
            set_state (now, roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT, "Robot finished preparation, executing (no explicit goal)");
          }
          break;
        case roah_rsbb_msgs::BenchmarkState_State_GOAL_TX:
          ROS_FATAL_STREAM ("Internal error, state should never be BenchmarkState_State_GOAL_TX for this benchmark");
          terminate_benchmark();
          return;
        case roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT:
          switch (msg.robot_state()) {
            case roah_rsbb_msgs::RobotState_State_STOP:
            case roah_rsbb_msgs::RobotState_State_PREPARING:
              phase_exec ("Received wrong state from robot, retrying from prepare");
              break;
            case roah_rsbb_msgs::RobotState_State_WAITING_GOAL:
            case roah_rsbb_msgs::RobotState_State_EXECUTING:
              // Keep
              break;
            case roah_rsbb_msgs::RobotState_State_RESULT_TX:
              phase_post ("Benchmark completed by the robot");
              break;
          }
          break;
      }

      if (event_.benchmark_code == "HCFGAC") {
        if (msg.has_devices_switch_1()
            && (msg.devices_switch_1() != ss_.last_devices_state->switch_1)) {
          roah_devices::Bool b;
          b.request.data = msg.devices_switch_1();
          call_service ("/devices/switch_1/set", b);
          log_.log_uint8 ("/rsbb_log/devices/switch_1", now, b.request.data ? 1 : 0);
        }
        if (msg.has_devices_switch_2()
            && (msg.devices_switch_2() != ss_.last_devices_state->switch_2)) {
          roah_devices::Bool b;
          b.request.data = msg.devices_switch_2();
          call_service ("/devices/switch_2/set", b);
          log_.log_uint8 ("/rsbb_log/devices/switch_2", now, b.request.data ? 1 : 0);
        }
        if (msg.has_devices_switch_3()
            && (msg.devices_switch_3() != ss_.last_devices_state->switch_3)) {
          roah_devices::Bool b;
          b.request.data = msg.devices_switch_3();
          call_service ("/devices/switch_3/set", b);
          log_.log_uint8 ("/rsbb_log/devices/switch_3", now, b.request.data ? 1 : 0);
        }
        if (msg.has_devices_blinds()
            && (msg.devices_blinds() != ss_.last_devices_state->blinds)) {
          roah_devices::Percentage p;
          p.request.data = msg.devices_blinds();
          call_service ("/devices/blinds/set", p);
          log_.log_uint8 ("/rsbb_log/devices/blinds", now, p.request.data);
        }
        if (msg.has_devices_dimmer()
            && (msg.devices_dimmer() != ss_.last_devices_state->dimmer)) {
          roah_devices::Percentage p;
          p.request.data = msg.devices_dimmer();
          call_service ("/devices/dimmer/set", p);
          log_.log_uint8 ("/rsbb_log/devices/dimmer", now, p.request.data);
        }

        if (msg.has_tablet_display_map()
            && (ss_.tablet_display_map != msg.tablet_display_map())) {
          ss_.tablet_display_map = msg.tablet_display_map();
          log_.log_uint8 ("/rsbb_log/tablet/display_map", now, ss_.tablet_display_map ? 1 : 0);
        }
      }
    }

  public:
    ExecutingSimpleBenchmark (CoreSharedState& ss,
                              Event const& event,
                              boost::function<void() > end,
                              string const& robot_name)
      : ExecutingSingleRobotBenchmark (ss, event, end, robot_name)
    {
    }

    void
    fill_2 (Time const& now,
            roah_rsbb::ZoneState& zone)
    {
      add_to_sting (zone.state) << "Messages saved: " << messages_saved_;

      if (last_skew_ > Duration (0.5)) {
        zone.state += "\nWARNING: Last clock skew above threshold: " + to_string (last_skew_.toSec());
      }
      if ( (now - last_beacon_) > Duration (5)) {
        zone.state += "\nWARNING: Last robot transmission received " + to_string ( (now - last_beacon_).toSec()) + " seconds ago";
      }
    }
};



class ExecutingExternallyControlledBenchmark
  : public ExecutingSingleRobotBenchmark
{
    bool waiting_for_omf_complete_;
    rockin_benchmarking::RefBoxState::_state_type refbox_state_;
    string annoying_refbox_payload_;
    rockin_benchmarking::ClientState::_state_type client_state_;
    string annoying_client_payload_;

    Publisher client_state_pub_;
    Publisher refbox_state_pub_;
    Subscriber bmbox_state_sub_;
    rockin_benchmarking::BmBoxState::ConstPtr last_bmbox_state_;
    Timer annoying_timer_;

    vector<bool> goal_initial_state_;
    vector<uint32_t> goal_switches_;

    Time last_exec_start_;
    Duration exec_duration_;
    set<uint32_t> on_switches_;
    vector<uint32_t> changed_switches_;
    uint32_t damaged_switches_;

    Duration total_timeout_;
    bool last_timeout_;

    vector<vector<double> > fbm2_locations_;
    int location_idx_;
    double fbm2_penalty_time_;
    double fbm2_timeout_time_;
    int fbm2_num_points_;
    vector<double> fbm2_starting_pose_;


    void
    set_client_state (Time const& now,
                      rockin_benchmarking::ClientState::_state_type client_state,
                      string const& payload = "")
    {
      if (client_state != client_state_) {
        // ROS_INFO("-------------------Setting Client state to: %d", client_state);
        client_state_ = client_state;
        annoying_client_payload_ = payload;
        rockin_benchmarking::ClientState msg;
        msg.state = client_state;
        msg.payload = payload;
        client_state_pub_.publish (msg);
        log_.log_uint8 ("/rsbb_log/client_state", now, client_state);
        log_.log_string ("/rsbb_log/client_state_payload", now, payload);
      }
    }

    void
    set_refbox_state (Time const& now,
                      rockin_benchmarking::RefBoxState::_state_type refbox_state,
                      string const& payload = "")
    {
      if (refbox_state != refbox_state_) {
        // ROS_INFO("-------------------Setting RefBox state to: %d-------------------", refbox_state);
        refbox_state_ = refbox_state;
        annoying_refbox_payload_ = payload;
        rockin_benchmarking::RefBoxState msg;
        msg.state = refbox_state;
        msg.payload = payload;
        refbox_state_pub_.publish (msg);
        log_.log_uint8 ("/rsbb_log/refbox_state", now, refbox_state);
        log_.log_string ("/rsbb_log/refbox_state_payload", now, payload);
      }
    }

    void
    annoying_timer (const TimerEvent& = TimerEvent())
    {
      if (client_state_ != rockin_benchmarking::ClientState::START) {
        rockin_benchmarking::ClientState msg;
        msg.state = client_state_;
        msg.payload = annoying_client_payload_;
        client_state_pub_.publish (msg);
      }
      if (refbox_state_ != rockin_benchmarking::RefBoxState::START) {
        rockin_benchmarking::RefBoxState msg;
        msg.state = refbox_state_;
        msg.payload = annoying_refbox_payload_;
        refbox_state_pub_.publish (msg);
      }
    }

    void
    check_bmbox_transition()
    {
      Time now = Time::now();

      switch (state_) {
        case roah_rsbb_msgs::BenchmarkState_State_STOP:
          if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_SCORE) {
            cout << "AT STOP" << endl;
          }
          break;
        case roah_rsbb_msgs::BenchmarkState_State_PREPARE:
          if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_SCORE) {
            if (event_.benchmark_code == "HNF") {
              if (location_idx_ >= fbm2_num_points_) {
                set_client_state (now, rockin_benchmarking::ClientState::END);
                set_refbox_state (now, rockin_benchmarking::RefBoxState::RECEIVED_SCORE);
                phase_post ("Benchmark complete! Received score from BmBox: " + last_bmbox_state_->payload);
              }
            }
          }
          if ( (refbox_state_ == rockin_benchmarking::RefBoxState::READY)
               && (client_state_ == rockin_benchmarking::ClientState::WAITING_GOAL)) {
            //ROS_INFO("-------------------BmBox: at_prepare");
            if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::WAITING_MANUAL_OPERATION) {
              set_refbox_state (now, rockin_benchmarking::RefBoxState::EXECUTING_MANUAL_OPERATION);
              manual_operation_ = last_bmbox_state_->payload;
              // Stop main timer
              time_.stop_pause (now);
            }
          }
          if ( ( (refbox_state_ == rockin_benchmarking::RefBoxState::READY)
                 || (refbox_state_ == rockin_benchmarking::RefBoxState::EXECUTING_GOAL))
               && (client_state_ == rockin_benchmarking::ClientState::WAITING_GOAL)) {
            //ROS_INFO("-------------------BmBox: PRE ---- transmitting goal");
            if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_GOAL) {
              //ROS_INFO("-------------------BmBox: transmitting goal");
              last_exec_start_ = now;
              exec_duration_ = Duration();

              // Resume main timer
              time_.resume (now);

              YAML::Node node = YAML::Load (last_bmbox_state_->payload);
              goal_initial_state_.clear();
              for (auto const& i : node[0]["initial_state"]) {
                goal_initial_state_.push_back (i.as<int>() ? true : false);
              }
              for (size_t i = 0; i < goal_initial_state_.size(); ++i) {
                if (goal_initial_state_[i]) {
                  on_switches_.insert (i + 1);
                }
              }
              goal_switches_.clear();
              for (auto const& i : node[0]["switches"]) {
                goal_switches_.push_back (i.as<uint32_t>() + param_direct<int> ("~switch_ids_bmbox_to_right", 1));
              }

              log_.log_string ("/rsbb_log/bmbox/goal", now, last_bmbox_state_->payload);
              set_state (now, roah_rsbb_msgs::BenchmarkState_State_GOAL_TX, "Robot finished preparation, received goal from BmBox, starting execution");
            }
            else if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::WAITING_RESULT) {
              last_exec_start_ = now;
              exec_duration_ = Duration();

              // Resume main timer
              time_.resume (now);

              set_state (now, roah_rsbb_msgs::BenchmarkState_State_GOAL_TX, "Robot finished preparation, no goal from BmBox, starting execution");
              set_refbox_state (now, rockin_benchmarking::RefBoxState::EXECUTING_GOAL);
              set_client_state (now, rockin_benchmarking::ClientState::EXECUTING_GOAL);
              // No check_bmbox_transition here
            }/*  else if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::READY) { */
            /*   cout << "\n\nMAIS UM PRINT BONITO\n\n"; */
            /* } */
          }
          break;
        case roah_rsbb_msgs::BenchmarkState_State_GOAL_TX:
          if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_SCORE) {
            cout << "AT GOAL TX" << endl;
          }
          if ( (refbox_state_ == rockin_benchmarking::RefBoxState::EXECUTING_GOAL)
               && (client_state_ == rockin_benchmarking::ClientState::EXECUTING_GOAL)
               && (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::WAITING_RESULT)) {
            //ROS_INFO("-------------------BmBox: goal tx");
            set_state (now, roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT, "Robot received goal, waiting for result");
          }
          break;
        case roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT:
          if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_SCORE) {
            cout << "AT WAITING RESULT" << endl;
          }
          if ( (refbox_state_ == rockin_benchmarking::RefBoxState::READY)
               && (client_state_ == rockin_benchmarking::ClientState::COMPLETED_GOAL)) {
            //ROS_INFO("-------------------BmBox: waiting result");
            if (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_SCORE) {
              log_.log_string ("/rsbb_log/bmbox/score", now, last_bmbox_state_->payload);
              set_refbox_state (now, rockin_benchmarking::RefBoxState::RECEIVED_SCORE);
              phase_post ("Benchmark complete! Received score from BmBox: " + last_bmbox_state_->payload);
            }
            else if ( (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::WAITING_MANUAL_OPERATION)
                      || (last_bmbox_state_->state == rockin_benchmarking::BmBoxState::TRANSMITTING_GOAL)) {
              phase_exec ("Robot preparing for new goal!");
            }
          }
          break;
      }
    }

    void
    receive_robot_state_2 (Time const& now,
                           roah_rsbb_msgs::RobotState const& msg)
    {
      switch (state_) {
        case roah_rsbb_msgs::BenchmarkState_State_STOP:
          break;
        case roah_rsbb_msgs::BenchmarkState_State_PREPARE:
          /* if (event_.benchmark_code == "HNF") { */
          /*   set_state (now, roah_rsbb_msgs::BenchmarkState_State_GOAL_TX, "Robot is waiting for goal."); */
          /*   set_refbox_state (now, rockin_benchmarking::RefBoxState::EXECUTING_GOAL); */
          /*   set_client_state (now, rockin_benchmarking::ClientState::WAITING_GOAL); */
          /* } */
          if (client_state_ != rockin_benchmarking::ClientState::WAITING_GOAL) {
            if (msg.robot_state() == roah_rsbb_msgs::RobotState_State_WAITING_GOAL) {
              set_refbox_state (now, rockin_benchmarking::RefBoxState::READY);
              set_client_state (now, rockin_benchmarking::ClientState::WAITING_GOAL);
              check_bmbox_transition();
              set_state (now, state_, "Robot is waiting for goal.");
              //ROS_INFO("-------------------ROBOT: at_prepare");
            }
          }
          break;
        case roah_rsbb_msgs::BenchmarkState_State_GOAL_TX:
          if (client_state_ == rockin_benchmarking::ClientState::WAITING_GOAL) {
            if (msg.robot_state() == roah_rsbb_msgs::RobotState_State_EXECUTING) {
              set_refbox_state (now, rockin_benchmarking::RefBoxState::EXECUTING_GOAL);
              set_client_state (now, rockin_benchmarking::ClientState::EXECUTING_GOAL);
              check_bmbox_transition();
              set_state (now, state_, "Robot is executing.");
              //ROS_INFO("-------------------ROBOT: at_goal_tx");
            }
          }
          break;
        case roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT:
          if (client_state_ == rockin_benchmarking::ClientState::EXECUTING_GOAL) {
            if (msg.robot_state() == roah_rsbb_msgs::RobotState_State_RESULT_TX) {
              //ROS_INFO("-------------------ROBOT: at_waiting_result");
              if (exec_duration_.isZero()) {
                exec_duration_ = now - last_exec_start_;
                if (event_.benchmark_code == "HOMF") {
                  set_state (now, state_, "Robot finished executing. Waiting for switches input from referee.");

                  //ROS_INFO("-------------------ROBOT: at_waiting_Result_2");
                  // Time for the referee to press OMF Complete should be discarded
                  time_.stop_pause (now);
                }
              }

              if (event_.benchmark_code == "HOPF") {
                YAML::Node node;
                // node["object_class"] = msg.has_object_class() ? msg.object_class() : "";
                // node["object_name"] = msg.has_object_class() ? msg.object_name() : "";
                // node["object_pose"]["x"] = msg.has_object_class() ? msg.object_pose_x() : 0;
                // node["object_pose"]["y"] = msg.has_object_class() ? msg.object_pose_y() : 0;
                // node["object_pose"]["theta"] = msg.has_object_class() ? msg.object_pose_theta() : 0;
                node["item_class"] = msg.has_object_class() ? msg.object_class() : "";
                node["item_instance"] = msg.has_object_class() ? msg.object_name() : "";
                node["x"] = msg.has_object_class() ? msg.object_pose_x() : 0;
                node["y"] = msg.has_object_class() ? msg.object_pose_y() : 0;
                node["theta"] = msg.has_object_class() ? msg.object_pose_theta() : 0;
                node["execution_time"] = exec_duration_.toSec();
                string result = YAML::Dump (node);

                log_.log_string ("/rsbb_log/opf_result", now, result);

                set_refbox_state (now, rockin_benchmarking::RefBoxState::READY);
                set_client_state (now, rockin_benchmarking::ClientState::COMPLETED_GOAL, result);
                check_bmbox_transition();
              }
              else if (event_.benchmark_code == "HOMF") {
                waiting_for_omf_complete_ = true;
              }
              else if (event_.benchmark_code == "HNF") {
                if (location_idx_ < fbm2_num_points_) {
                  location_idx_++;
                  if (location_idx_ == fbm2_num_points_) {
                    set_refbox_state (now, rockin_benchmarking::RefBoxState::RECEIVED_SCORE);
                    phase_post ("Benchmark complete! Received score from BmBox: " + last_bmbox_state_->payload);
                  }
                }

                //cout << "\n\nCENAS COISAS ACONTECERAM\n\n" << flush;
                set_refbox_state (now, rockin_benchmarking::RefBoxState::READY);
                set_client_state (now, rockin_benchmarking::ClientState::COMPLETED_GOAL, "");
                check_bmbox_transition();
              }
            }
          }
          break;
      }
    }

    void
    fill_benchmark_state_2 (roah_rsbb_msgs::BenchmarkState& msg)
    {
      if (state_ == roah_rsbb_msgs::BenchmarkState_State_GOAL_TX) {
        /* for (auto const& i : goal_initial_state_) { */
        /*   msg.add_initial_state (i); */
        /* } */
        /* for (auto const& i : goal_switches_) { */
        /*   msg.add_switches (i); */
        /* } */

        msg.set_target_pose_x (fbm2_locations_[location_idx_][0]);
        msg.set_target_pose_y (fbm2_locations_[location_idx_][1]);
        msg.set_target_pose_theta (fbm2_locations_[location_idx_][2]);

        ROS_INFO ("Publishing Goal!!");
        ROS_INFO ("Goal: %f, %f, %f", fbm2_locations_[location_idx_][0], fbm2_locations_[location_idx_][1], fbm2_locations_[location_idx_][2]);
      }
    }

    void
    bmbox_state_callback (rockin_benchmarking::BmBoxState::ConstPtr const& msg)
    {
      Time now = Time::now();

      if (msg->state == last_bmbox_state_->state) {
        return;
      }
      last_bmbox_state_ = msg;

      if (phase_ != PHASE_EXEC) {
        return;
      }

      check_bmbox_transition();
    }

    void
    phase_exec_2 (Time const& now)
    {
      waiting_for_omf_complete_ = false;
      /* goal_initial_state_.clear(); */
      /* goal_switches_.clear(); */
      /* on_switches_.clear(); */
      /* changed_switches_.clear(); */
      /* damaged_switches_ = 0; */

      // OPF: Timeout should also happen for each object.
      // Therefore, timeout refers to each object and a total_timeout
      // is added for the whole benchmark.
      total_timeout_ -= time_.get_elapsed (now);
      if (event_.benchmark.timeout < total_timeout_) {
        time_.start_reset (now, event_.benchmark.timeout);
        last_timeout_ = false;
      }
      else {
        time_.start_reset (now, total_timeout_);
        last_timeout_ = true;
      }
    }

    void
    phase_post_2 (Time const& now)
    {
      if (refbox_state_ != rockin_benchmarking::RefBoxState::RECEIVED_SCORE) {
        if (stoped_due_to_timeout_
            && (! last_timeout_)) {
          // Partial timeout
          //ROS_INFO("TIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUTTIMEOUT");

          /* if (location_idx_ >= fbm2_num_points_) { */
          /*   set_refbox_state (now, rockin_benchmarking::RefBoxState::RECEIVED_SCORE); */
          /*   set_client_state (now, rockin_benchmarking::ClientState::END); */
          /*   phase_post ("Benchmark complete! Received score from BmBox: " + last_bmbox_state_->payload); */
          /*   return; */
          /* } */
          // If there is a partial timeout, update the FBM2 location index accordingly
          if (event_.benchmark_code == "HNF") {
            if (location_idx_ < fbm2_num_points_) {
              location_idx_++;
              set_client_state (now, rockin_benchmarking::ClientState::COMPLETED_GOAL, "reason: timeout");
              //cout << "\n\nINCREMENTING LOCATION IDX DUE TO TIMEOUT!!!!\n\n";
              if (location_idx_ == fbm2_num_points_) {
                //set_refbox_state (now, rockin_benchmarking::RefBoxState::RECEIVED_SCORE);
                set_client_state (now, rockin_benchmarking::ClientState::COMPLETED_GOAL, "reason: timeout");
                //phase_post ("Benchmark complete! Received score from BmBox: " + last_bmbox_state_->payload);
              }
            }
            set_state (now, roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT, "Robot received goal, waiting for result");
          }
          else {
            set_refbox_state (now, rockin_benchmarking::RefBoxState::END, "reason: timeout");
            set_client_state (now, rockin_benchmarking::ClientState::END);
          }

          /* ros::Duration(1).sleep(); */
          /* set_client_state (now, rockin_benchmarking::ClientState::READY); */
          phase_exec ("Robot timedout a goal, trying the next one...");
        }
        else {
          // Global timeout
          //ROS_INFO("GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT || GOBAL TIMEOUT");
          set_state (now, roah_rsbb_msgs::BenchmarkState_State_STOP, "Global timeout.");
          set_refbox_state (now, rockin_benchmarking::RefBoxState::END, "reason: stop");
          set_client_state (now, rockin_benchmarking::ClientState::END);
        }
      }
    }

  private:
    string
    bmbox_prefix (Event const& event)
    {
      if (event.benchmark_code == "HOPF") {
        return "/fbm1h/";
      }
      else if (event.benchmark_code == "HNF") {
        return "/fbm2h/";
      }

      ROS_FATAL_STREAM ("Cannot execute benchmark of type " << event.benchmark_code << " with ExecutingExternallyControlledBenchmark");
      terminate_benchmark();
      return "/";
    }

  public:
    ExecutingExternallyControlledBenchmark (CoreSharedState& ss,
                                            Event const& event,
                                            boost::function<void() > end,
                                            string const& robot_name)
      : ExecutingSingleRobotBenchmark (ss, event, end, robot_name)
      , waiting_for_omf_complete_ (false)
      , refbox_state_ (rockin_benchmarking::RefBoxState::START)
      , client_state_ (rockin_benchmarking::ClientState::START)
      , client_state_pub_ (ss_.nh.advertise<rockin_benchmarking::ClientState> (bmbox_prefix (event) + "client_state", 1, true))
      , refbox_state_pub_ (ss_.nh.advertise<rockin_benchmarking::RefBoxState> (bmbox_prefix (event) + "refbox_state", 1, true))
      , bmbox_state_sub_ (ss_.nh.subscribe (bmbox_prefix (event) + "bmbox_state", 1, &ExecutingExternallyControlledBenchmark::bmbox_state_callback, this))
      , last_bmbox_state_ (boost::make_shared<rockin_benchmarking::BmBoxState>())
      , annoying_timer_ (ss_.nh.createTimer (Duration (0.2), &ExecutingExternallyControlledBenchmark::annoying_timer, this))
      , total_timeout_ (event.benchmark.total_timeout)
      , location_idx_ (0)
    {
      Time now = Time::now();

      std::vector< std::vector<double> > temp;

      string fbm2_locations;
      ss_.nh.getParam ("/roah_rsbb_core/fbm2_locations_file", fbm2_locations);
      YAML::Node fbm2_config = YAML::LoadFile (fbm2_locations);

      if (!fbm2_config["goal"]["starting_pose"]) {
        ROS_FATAL_STREAM ("FBM2H file is missing a \"starting_pose\" entry!");
        abort_rsbb();
      }
      else {
        fbm2_starting_pose_ = fbm2_config["goal"]["starting_pose"].as<std::vector<double>>();
      }

      if (!fbm2_config["goal"]["penalty_time"]) {
        ROS_FATAL_STREAM ("FBM2H file is missing a \"penalty_time\" entry!");
        abort_rsbb();
      }
      else {
        fbm2_penalty_time_ = fbm2_config["goal"]["penalty_time"].as<double>();
      }

      if (!fbm2_config["goal"]["timeout_time"]) {
        ROS_FATAL_STREAM ("FBM2H file is missing a \"timeout_time\" entry!");
        abort_rsbb();
      }
      else {
        fbm2_timeout_time_ = fbm2_config["goal"]["timeout_time"].as<double>();
      }

      if (!fbm2_config["goal"]["waypoints"]) {
        ROS_FATAL_STREAM ("FBM2H file is missing a \"timeout_time\" entry!");
        abort_rsbb();
      }
      else {
        for (YAML::const_iterator it = fbm2_config["goal"]["waypoints"].begin(); it != fbm2_config["goal"]["waypoints"].end(); ++it) {
          std::vector<double> wp = (*it).as<std::vector<double>>();
          temp.push_back (wp);
        }
      }

      fbm2_locations_ = temp;
      fbm2_num_points_ = fbm2_locations_.size();

      ostringstream pl;

      pl << "RefBox - FBM2 Config:" << endl;
      pl << "Penalty Time: " << fbm2_penalty_time_ << endl;
      pl << "Timeout Time: " << fbm2_timeout_time_ << endl;

      pl << "Starting Pose: [ ";
      for (auto i = fbm2_starting_pose_.begin(); i != fbm2_starting_pose_.end(); ++i) {
        pl << *i << ' ';
      }
      pl << "]" << endl;

      pl << "Waypoints: " << endl;
      for (uint i = 0; i < fbm2_locations_.size(); i++) {
        pl << "\tWP #" << i << ": [ " << fbm2_locations_[i][0] << ' ' << fbm2_locations_[i][1] << ' ' << fbm2_locations_[i][2] << " ]" << endl;
      }
      pl << endl;

      log_.log_string ("/rsbb_log/waypoints_loading", now, pl.str());
    }

    void
    manual_operation_complete()
    {
      manual_operation_.clear();

      if ( (state_ == roah_rsbb_msgs::BenchmarkState_State_PREPARE)
           && (refbox_state_ == rockin_benchmarking::RefBoxState::EXECUTING_MANUAL_OPERATION)
           && (client_state_ == rockin_benchmarking::ClientState::WAITING_GOAL)) {
        Time now = Time::now();

        set_refbox_state (now, rockin_benchmarking::RefBoxState::EXECUTING_GOAL);
        check_bmbox_transition();
      }
    }

    void
    omf_complete()
    {
      if (waiting_for_omf_complete_) {
        Time now = Time::now();

        waiting_for_omf_complete_ = false;

        if (exec_duration_.isZero()) {
          exec_duration_ = now - last_exec_start_;
        }

        YAML::Node node;
        node["switches"] = YAML::Node (YAML::NodeType::Sequence);
        for (auto const& i : changed_switches_) {
          node["switches"].push_back (i - param_direct<int> ("~switch_ids_bmbox_to_right", 1));
        }
        node["execution_time"] = exec_duration_.toSec();
        node["damaged_switches"] = damaged_switches_;

        log_.log_string ("/rsbb_log/omf_complete", now, last_bmbox_state_->payload);

        set_refbox_state (now, rockin_benchmarking::RefBoxState::READY);
        set_client_state (now, rockin_benchmarking::ClientState::COMPLETED_GOAL, YAML::Dump (node));
        check_bmbox_transition();

        goal_initial_state_.clear();
        goal_switches_.clear();
        on_switches_.clear();
        changed_switches_.clear();
        damaged_switches_ = 0;

        // Time for the referee to press OMF Complete should be discarded
        time_.resume (now);
      }
    }

    void
    omf_damaged (uint8_t damaged)
    {
      damaged_switches_ = damaged;
      log_.log_uint8 ("/rsbb_log/omf_damaged", Time::now(), damaged);
    }

    void
    omf_button (uint8_t button)
    {
      changed_switches_.push_back (button);
      if (on_switches_.count (button)) {
        on_switches_.erase (button);
      }
      else {
        on_switches_.insert (button);
      }
      log_.log_uint8 ("/rsbb_log/omf_button", Time::now(), button);
    }

    void
    fill_2 (Time const& now,
            roah_rsbb::ZoneState& zone)
    {
      add_to_sting (zone.state) << "Messages saved: " << messages_saved_;
      if (phase_ == PHASE_EXEC) {
        // add_to_sting (zone.state) << "total_timeout_: " << to_string(total_timeout_.toSec());
        add_to_sting (zone.state) << "Benchmark timeout: " << to_qstring (time_.get_until_timeout_for_timeout (now, total_timeout_)).toStdString();
      }

      if (bmbox_state_sub_.getNumPublishers() == 0) {
        add_to_sting (zone.state) << "NOT CONNECTED TO BmBox!!!";
      }
      else if (phase_ == PHASE_POST) {
        add_to_sting (zone.state) << "You may need to restart BmBox if you are to press start again";
      }

      if ( (event_.benchmark_code == "HNF")
           && (! (goal_initial_state_.empty()))
           && (phase_ == PHASE_EXEC)) {
        zone.omf = true;
        for (auto const& i : on_switches_) {
          zone.omf_switches.push_back (i);
        }
        zone.omf_damaged = damaged_switches_;
        zone.omf_complete = waiting_for_omf_complete_;
      }
      else {
        zone.omf = false;
      }
    }
};



class ExecutingAllRobotsBenchmark
  : public ExecutingBenchmark
{
    vector<Event> dummy_events_;
    vector<unique_ptr<ExecutingSimpleBenchmark>> simple_benchmarks_;

    void
    phase_exec_2 (Time const& now)
    {
      for (auto const& i : simple_benchmarks_) {
        i->start();
      }
      set_state (now, roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT, "Preparing and executing");
    }

    void
    phase_post_2 (Time const& now)
    {
      for (auto const& i : simple_benchmarks_) {
        i->stop();
      }
    }

    static void
    end()
    {
      // empty
    }

  public:
    ExecutingAllRobotsBenchmark (CoreSharedState& ss,
                                 Event const& event,
                                 boost::function<void() > end)
      : ExecutingBenchmark (ss, event, end)
    {
      for (roah_rsbb::RobotInfo const& ri : ss_.active_robots.get ()) {
        if (ss_.benchmarking_robots.count (ri.team)) {
          ROS_ERROR_STREAM ("Ignoring robot of team " << ri.team << " because it is already executing a benchmark");
          continue;
        }

        dummy_events_.push_back (event);
        dummy_events_.back().team = ri.team;
        dummy_events_.back().password = ss_.passwords.get (dummy_events_.back().team);

        bool ok = false;
        do {
          try {
            simple_benchmarks_.push_back (unique_ptr<ExecutingSimpleBenchmark> (new ExecutingSimpleBenchmark (ss, dummy_events_.back(), &ExecutingAllRobotsBenchmark::end, ri.robot)));
            ok = true;
          }
          catch (const std::exception& exc) {
            std::cerr << exc.what();
            ROS_ERROR_STREAM ("Failed to create a private channel. Retrying on next port.");
          }
        }
        while (! ok);
      }
    }

    void
    fill_2 (Time const& now,
            roah_rsbb::ZoneState& zone)
    {
      unsigned prep = 0, exec = 0, stopped = 0;
      for (auto const& i : simple_benchmarks_) {
        switch (i->state()) {
          case roah_rsbb_msgs::BenchmarkState_State_STOP:
            ++stopped;
            break;
          case roah_rsbb_msgs::BenchmarkState_State_PREPARE:
          case roah_rsbb_msgs::BenchmarkState_State_GOAL_TX:
            ++prep;
            break;
          case roah_rsbb_msgs::BenchmarkState_State_WAITING_RESULT:
            ++exec;
            break;
        }
      }

      add_to_sting (zone.state) << "Robots preparing: " << prep;
      add_to_sting (zone.state) << "Robots executing: " << exec;
      add_to_sting (zone.state) << "Robots stopped: " << stopped;
    }

    void
    stop_communication()
    {
      for (auto const& i : simple_benchmarks_) {
        i->stop_communication();
      }
    }
};

#endif
