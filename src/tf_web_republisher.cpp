/*********************************************************************
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.

 *  Author: Julius Kammerl (jkammerl@willowgarage.com)
 *
 */

#include <ros/ros.h>
#include <tf2_ros/transform_listener.h>

#include <boost/thread/mutex.hpp>
#include "boost/thread.hpp"

#include <tf/tfMessage.h>
#include <tf/transform_datatypes.h>
#include <geometry_msgs/TransformStamped.h>

#include <actionlib/server/simple_action_server.h>
#include <tf2_web_republisher/TFSubscriptionAction.h>

#include "tf_pair.h"

class TFRepublisher
{
protected:

  typedef actionlib::ActionServer<tf2_web_republisher::TFSubscriptionAction> TFTransformServer;
  typedef TFTransformServer::GoalHandle GoalHandle;

  ros::NodeHandle nh_;

  TFTransformServer as_;
  struct GoalInfo
  {
    GoalHandle handle;
    std::vector<TFPair> tf_subscriptions_;
    unsigned int client_ID_;
  };

  std::list<boost::shared_ptr<GoalInfo> > active_goals_;
  boost::mutex mutex_;

  ros::Timer republish_timer_;

  tf2::Buffer tf_buffer_;
  tf2::TransformListener tf_listener_;

  unsigned int client_ID_count_;

public:

  TFRepublisher(const std::string& name) :
      nh_(), as_(ros::NodeHandle(),
                 name,
                 boost::bind(&TFRepublisher::goalCB, this, _1),
                 boost::bind(&TFRepublisher::cancelCB, this, _1),
                 false),
      tf_listener_(tf_buffer_), client_ID_count_(0)
  {
    // start action server
    as_.start();

    // read update rate from parameter server
    double rate;
    nh_.param<double>("rate", rate, 10.0);
    republish_timer_ = nh_.createTimer(ros::Duration(1.0 / rate), boost::bind(&TFRepublisher::processActiveGoals, this));
  }

  ~TFRepublisher() {}

  void cancelCB(GoalHandle& gh)
  {
    boost::mutex::scoped_lock l(mutex_);

    ROS_DEBUG("GoalHandle canceled");

    // search for goal handle and remove it from active_goals_ list
    for(std::list<boost::shared_ptr<GoalInfo> >::iterator it = active_goals_.begin(); it != active_goals_.end();)
    {
      GoalInfo& info = **it;
      if(info.handle == gh)
      {
        it = active_goals_.erase(it);
        info.handle.setCanceled();

        return;
      }
      else
        ++it;
    }
  }

  void goalCB(GoalHandle& gh)
  {
    ROS_DEBUG("GoalHandle request received");

    // accept new goals
    gh.setAccepted();

    // get goal from handle
    const tf2_web_republisher::TFSubscriptionGoal::ConstPtr& goal = gh.getGoal();

    // generate goal_info struct
    boost::shared_ptr<GoalInfo> goal_info = boost::make_shared<GoalInfo>();
    goal_info->handle = gh;
    goal_info->client_ID_ = client_ID_count_++;

    std::size_t request_size_ = goal->source_frames.size();
    goal_info->tf_subscriptions_.resize(request_size_);

    for (std::size_t i=0; i<request_size_; ++i )
    {

      TFPair& tf_pair = goal_info->tf_subscriptions_[i];

      tf_pair.setSourceFrame(goal->source_frames[i]);
      tf_pair.setTargetFrame(goal->target_frame);
      tf_pair.setAngularThres(goal->angular_thres);
      tf_pair.setTransThres(goal->trans_thres);
    }

    {
      boost::mutex::scoped_lock l(mutex_);
      // add new goal to list of active goals/clients
      active_goals_.push_back(goal_info);
    }
  }

  void processActiveGoals()
  {
    for (std::list<boost::shared_ptr<GoalInfo> >::iterator it = active_goals_.begin(); it != active_goals_.end(); ++it)
    {
      tf2_web_republisher::TFSubscriptionFeedback feedback;

      GoalInfo& info = **it;

      {
        boost::mutex::scoped_lock lock (mutex_);

        // iterate over tf_subscription map
        std::vector<TFPair>::iterator it ;
        std::vector<TFPair>::const_iterator end = info.tf_subscriptions_.end();

        for (it=info.tf_subscriptions_.begin(); it!=end; ++it)
        {
          geometry_msgs::TransformStamped transform;

          try
          {
            // lookup transformation for tf_pair
            transform = tf_buffer_.lookupTransform(it->getTargetFrame(),
                                                   it->getSourceFrame(),
                                                   ros::Time(0));

            // update tf_pair with transformtion
            it->updateTransform(transform);
          }
          catch (tf2::TransformException ex)
          {
            ROS_ERROR("%s", ex.what());
          }

          // check angular and translational thresholds
          if (it->updateNeeded())
          {
            transform.header.stamp = ros::Time::now();
            transform.header.frame_id = it->getTargetFrame();
            transform.child_frame_id = it->getSourceFrame();

            // notify tf_subscription that a network transmission has been triggered
            it->transmissionTriggered();

            // add transform to the feedback
            feedback.transforms.push_back(transform);
          }
        }
      }

      if (feedback.transforms.size() > 0)
      {
        // publish feedback
        info.handle.publishFeedback(feedback);
        ROS_DEBUG("Client %d: TF feedback published:", info.client_ID_);
      } else
      {
        ROS_DEBUG("Client %d: No TF frame update needed:", info.client_ID_);
      }

    }
  }

};

int main(int argc, char **argv)
{
ros::init(argc, argv, "tf2_web_republisher");

TFRepublisher tf2_web_republisher(ros::this_node::getName());
ros::spin();

return 0;
}
