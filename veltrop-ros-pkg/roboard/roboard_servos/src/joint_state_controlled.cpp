//#include <fcntl.h>
#include <sys/io.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <math.h>

#include <pthread.h>
#include <ros/ros.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Int16.h>
#include <veltrobot_msgs/CapturePose.h>
#include <sensor_msgs/JointState.h>
#include <roboard.h>
#include "servo.h"

namespace roboard_servos
{

class JointStateControlled
{
public:
  JointStateControlled()
    : np_("~")
    , do_mix_(false)
    , roboio_ok_(false)
    , running_(false)
  {
    pthread_mutex_init (&playframe_mutex_, NULL);
    pthread_mutex_init (&mixframe_mutex_, NULL);
           
    // Get servo configuration as it relates to the robot description                         
    servos_.openURDFparam();
    if (servos_.getUsedChannels() == 0)
      ROS_ERROR("Robot Description contains no servo channels");   
      
    // Setup an initial pose
    memset(mixframe_, 0, sizeof(long) * 32);
    memset(commandframe_, 0, sizeof(long) * 32);
    memset(playframe_, 0, sizeof(unsigned long) * 32);
    ServoLibrary::iterator j = servos_.begin();
    for (; j != servos_.end(); ++j)
    {
      Servo& servo = j->second;
      if (servo.channel_ >= 1)
        playframe_[servo.channel_ - 1] = (int)servo.trim_pwm_ +
                                         ((int)servo.max_pwm_ + (int)servo.min_pwm_)
                                         / (int)2;
    }
    
    np_.param<int>("servo_fps", servo_fps_, 100); 
    np_.param<bool>("force_mix", force_mix_, false);     
    np_.param<bool>("reset_after_mix", reset_after_mix_, false);     
    
    // Prepare our subscription callbacks
    update_trim_sub_ = n_.subscribe("/trim_updated", 10, 
                                    &JointStateControlled::updateTrimCB, this);   
    joint_state_sub_ = n_.subscribe("/joint_states", 10, 
                                    &JointStateControlled::jointStateCB, this);  
		balance_joint_state_sub_ = n_.subscribe("/balancing_joint_states", 1, 
                                    &JointStateControlled::balancingJointStateCB, this); 
    receive_servo_command_sub_ = n_.subscribe("/servo_command", 1,
                                           &JointStateControlled::receiveServoCommandCB, this);
    
    capture_pose_srv_ = n_.advertiseService("capture_pose", &JointStateControlled::capturePoseCB, this);
  }
  
  void spin() 
  {
    running_ = true;
    initRCservo();
    if (roboio_ok_)
      rcservo_EnterPlayMode(); 
      
    static pthread_t controlThread_;
    static pthread_attr_t controlThreadAttr_;

    int rv;
    if ((rv = pthread_create(&controlThread_, &controlThreadAttr_, playActionThread, this)) != 0)
    {
      ROS_FATAL("Unable to create control thread: rv = %d", rv);
      ROS_BREAK();
    }
    
    ros::spin();

    running_ = false;
    pthread_join(controlThread_, NULL);//(void **)&rv);
    rcservo_Close();
  }
  
private:
  ros::NodeHandle n_;
  ros::NodeHandle np_;
  ros::Subscriber update_trim_sub_;
  ros::Subscriber joint_state_sub_;
  ros::Subscriber balance_joint_state_sub_;
  ros::Subscriber receive_servo_command_sub_;
  ros::ServiceServer capture_pose_srv_;
  ServoLibrary    servos_;
  bool            do_mix_;
  bool            roboio_ok_;
  int             servo_fps_;
  unsigned long   playframe_[32];
  long            mixframe_[32]; 
  long            commandframe_[32];
  bool            running_;
  bool            force_mix_;
  bool            reset_after_mix_;
  pthread_mutex_t playframe_mutex_;
  pthread_mutex_t mixframe_mutex_;
  
  static void* playActionThread(void *ptr)
  {
    JointStateControlled* that = (JointStateControlled*)ptr;

    if (!that->roboio_ok_)
      return NULL;
    
    ros::Rate loop_rate(that->servo_fps_); 
    while (that->running_)
    {
      if (that->do_mix_ || that->force_mix_) 
      {
        pthread_mutex_lock(&that->mixframe_mutex_);
        pthread_mutex_lock(&that->playframe_mutex_);
        rcservo_PlayActionMix(that->mixframe_);
        pthread_mutex_unlock(&that->playframe_mutex_);
        if (that->reset_after_mix_)
          memset(that->mixframe_, 0, sizeof(long) * 32);
        that->do_mix_ = false;
        pthread_mutex_unlock(&that->mixframe_mutex_);
      }
      else
      {
        pthread_mutex_lock(&that->playframe_mutex_);
        rcservo_PlayAction();
        pthread_mutex_unlock(&that->playframe_mutex_);
      }
      loop_rate.sleep();
    }
    
    return NULL;
  }
  
  void initRCservo()
  {
    ServoLibrary::iterator i = servos_.begin();
    for(; i != servos_.end(); ++i)
    {
      //std::string& joint_name = i->first;
      Servo& servo = i->second;
      if (servo.channel_ != -1)
        rcservo_SetServo(servo.channel_, servo.type_);
    }
            
    if (rcservo_Initialize(servos_.getUsedChannels()) == true)
    {
      rcservo_EnableMPOS();
      rcservo_SetFPS(servo_fps_); 
      roboio_ok_ = true;
    } 
    else
      ROS_ERROR("RoBoIO: %s", roboio_GetErrMsg());
  }
  
  void updateTrimCB(const std_msgs::BoolConstPtr& msg)
  {
    servos_.openURDFparam();
  }
  
  void jointStateCB(const sensor_msgs::JointStateConstPtr& msg)
  {
    if (!msg->name.size())
      return;
      
    executeJointState(msg);
  }
  
  void balancingJointStateCB(const sensor_msgs::JointStateConstPtr& msg)
  {
    if (!msg->name.size())
      return;
      
    pthread_mutex_lock(&mixframe_mutex_);
    
		// todo? velocity? duration?
    
    for (size_t i=0; i < msg->name.size(); i++)
    {
      Servo& servo = servos_[msg->name[i]];
      if (servo.channel_ < 1 || servo.channel_ > 32)
        continue;
      float rot_range = servo.max_rot_ - servo.min_rot_;
      float pwm_range = servo.max_pwm_ - servo.min_pwm_; 
      float pwm_per_rot = pwm_range / rot_range;
      float f_pwm_val = (float)msg->position[i] * pwm_per_rot;
      int i_pwm_val = (int)round(f_pwm_val);    
     
      mixframe_[servo.channel_-1] = i_pwm_val;
    }
    
    do_mix_ = true;
    
    pthread_mutex_unlock(&mixframe_mutex_);   
  }
  
  void receiveServoCommandCB(const std_msgs::Int16ConstPtr& msg)
  {
    long cmd;
    // cmd = msg->data 
    switch (msg->data)
    {
      case 0:
        cmd = RCSERVO_CMD_POWEROFF;
        break;
      case 1:
        cmd = RCSERVO_MIXWIDTH_CMD1;
        break;
      case 2:
        cmd = RCSERVO_MIXWIDTH_CMD2;
        break;
      case 3:
        cmd = RCSERVO_MIXWIDTH_CMD3;
        break;    
      default:
        return;
    }
    
    for (size_t i=0; i < 32; i++)
      commandframe_[i] = cmd;
    
    pthread_mutex_lock(&playframe_mutex_);
    rcservo_PlayActionMix(commandframe_);
    pthread_mutex_unlock(&playframe_mutex_);
  }
  
  bool capturePoseCB(veltrobot_msgs::CapturePose::Request  &req,
                     veltrobot_msgs::CapturePose::Response &res )
  {
    unsigned long width[32];
    pthread_mutex_lock(&playframe_mutex_);
    rcservo_EnterCaptureMode();
    
    rcservo_ReadPositions(servos_.getUsedChannels(), RCSERVO_CMD_POWEROFF, width);
    
    rcservo_EnterPlayMode();
    pthread_mutex_unlock(&playframe_mutex_);
    
    for (size_t i=0; i < req.requestedJointNames.size(); i++)
    {    
      Servo& servo = servos_[req.requestedJointNames[i]];
      if (servo.channel_ < 1 || servo.channel_ > 32)
        continue;
      
      int pwm_center = (servo.max_pwm_ + servo.min_pwm_) / 2;
      int pwm_offset = (int)width[servo.channel_-1] - pwm_center - servo.trim_pwm_;
      
      float rot_range = servo.max_rot_ - servo.min_rot_;
      float pwm_range = servo.max_pwm_ - servo.min_pwm_;      
      float pwm_per_rot = pwm_range / rot_range;
     
      float radian_offset = (float)pwm_offset / pwm_per_rot;
      
      res.jointNames.push_back(req.requestedJointNames[i]);
      res.jointPositions.push_back(radian_offset);
    }
    
    if (res.jointPositions.size() == 0)
      return false;
    
    return true;
  }
  
  void executeJointState(const sensor_msgs::JointStateConstPtr& msg)
  {
    // TODO: something better than this for velocity and duration.
    //       it could be possible to work out a scheme where each servo can get
    //       an individual duration.  but what about the velocity interpretation?
    //       the concept of velocity is completely unaccounted for in the roboio.
    uint32_t duration;
    if (msg->velocity.size())
      duration = msg->velocity[0];
    else
      duration = 300;
    
    for (size_t i=0; i < msg->name.size(); i++)
    {
      Servo& servo = servos_[msg->name[i]];
      if (servo.channel_ < 1 || servo.channel_ > 32)
        continue;
      float rot_range = servo.max_rot_ - servo.min_rot_;
      float pwm_range = servo.max_pwm_ - servo.min_pwm_; 
      int   pwm_center = (servo.max_pwm_ + servo.min_pwm_) / 2;
      float pwm_per_rot = pwm_range / rot_range;
      float f_pwm_val = (float)msg->position[i] * pwm_per_rot;
      unsigned int i_pwm_val = (int)round(f_pwm_val) + (int)servo.trim_pwm_ + (int)pwm_center;    
      playframe_[servo.channel_-1] = i_pwm_val;
    }

    if (roboio_ok_)
    {
      // OPTIONAL?: wait for previous movement to finish:
      //  while (rcservo_PlayAction() != RCSERVO_PLAYEND) { }
      //  or rcservo_MoveTo(playframe_, duration);
      pthread_mutex_lock(&playframe_mutex_);
      rcservo_SetAction(playframe_, duration);
      pthread_mutex_unlock(&playframe_mutex_);
      
    }
  }  
};

}  // namespace roboard_servos

int main(int argc, char** argv)
{
  // Keep the kernel from swapping us out
  if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
    perror("mlockall() failed");
    return -1;
  }
  
  //if (iopl(3) != 0)
  //  perror("iopl() failed");
  
  ros::init(argc, argv, "joint_state_controlled");
  
  roboard_servos::JointStateControlled this_node;
  this_node.spin();
}

