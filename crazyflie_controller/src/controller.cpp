#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <std_srvs/Empty.h>
#include <geometry_msgs/Twist.h>


#include "pid.hpp"

double get(
    const ros::NodeHandle& n,
    const std::string& name) {
    double value;
    n.getParam(name, value);
    return value;
}

class Controller
{
public:

    Controller(
        const std::string& worldFrame,
        const std::string& frame,
        const ros::NodeHandle& n)
        : m_worldFrame(worldFrame)
        , m_frame(frame)
        , m_pubNav()
        , m_listener()
        , m_pidX(
            get(n, "PIDs/X/kp"),
            get(n, "PIDs/X/kd"),
            get(n, "PIDs/X/ki"),
            get(n, "PIDs/X/minOutput"),
            get(n, "PIDs/X/maxOutput"),
            get(n, "PIDs/X/integratorMin"),
            get(n, "PIDs/X/integratorMax"),
            "x")
        , m_pidY(
            get(n, "PIDs/Y/kp"),
            get(n, "PIDs/Y/kd"),
            get(n, "PIDs/Y/ki"),
            get(n, "PIDs/Y/minOutput"),
            get(n, "PIDs/Y/maxOutput"),
            get(n, "PIDs/Y/integratorMin"),
            get(n, "PIDs/Y/integratorMax"),
            "y")
        , m_pidZ(
            get(n, "PIDs/Z/kp"),
            get(n, "PIDs/Z/kd"),
            get(n, "PIDs/Z/ki"),
            get(n, "PIDs/Z/minOutput"),
            get(n, "PIDs/Z/maxOutput"),
            get(n, "PIDs/Z/integratorMin"),
            get(n, "PIDs/Z/integratorMax"),
            "z")
        , m_pidYaw(
            get(n, "PIDs/Yaw/kp"),
            get(n, "PIDs/Yaw/kd"),
            get(n, "PIDs/Yaw/ki"),
            get(n, "PIDs/Yaw/minOutput"),
            get(n, "PIDs/Yaw/maxOutput"),
            get(n, "PIDs/Yaw/integratorMin"),
            get(n, "PIDs/Yaw/integratorMax"),
            "yaw")
        , m_state(Idle)
        , m_goal()
        , m_subscribeGoal()
        , m_serviceTakeoff()
        , m_serviceLand()
        , m_thrust(0)
        , m_startZ(0)
	, m_last_thrust(0)
    {
        ros::NodeHandle nh;
        m_listener.waitForTransform(m_worldFrame, m_frame, ros::Time(0), ros::Duration(10.0)); 
        m_pubNav = nh.advertise<geometry_msgs::Twist>("cmd_vel", 1);
        m_subscribeGoal = nh.subscribe("goal", 1, &Controller::goalChanged, this);
        m_serviceTakeoff = nh.advertiseService("takeoff", &Controller::takeoff, this);
        m_serviceLand = nh.advertiseService("land", &Controller::land, this);
    }

    void run(double frequency)
    {
        ros::NodeHandle node;
        ros::Timer timer = node.createTimer(ros::Duration(1.0/frequency), &Controller::iteration, this);
        ros::spin();
    }

private:
    void goalChanged(
        const geometry_msgs::PoseStamped::ConstPtr& msg)
    {	
        m_goal = *msg;
	m_last_marker = ros::Time::now();
	
    }

    bool takeoff(
        std_srvs::Empty::Request& req,
        std_srvs::Empty::Response& res)
    {
	pidReset();
	m_thrust = 25000;
        ROS_INFO("Takeoff requested!");
        m_state = TakingOff;
        tf::StampedTransform transform;
        m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);
        m_startZ = transform.getOrigin().z();

        return true;
    }

    bool land(
        std_srvs::Empty::Request& req,
        std_srvs::Empty::Response& res)
    {
        ROS_WARN("Landing requested!");
        m_state = Landing;

        return true;
    }

    void getTransform(
        const std::string& sourceFrame,
        const std::string& targetFrame,
        tf::StampedTransform& result)
    {
        m_listener.lookupTransform(sourceFrame, targetFrame, ros::Time(0), result);
    }

    void pidReset()
    {
        m_pidX.reset();
        m_pidY.reset();
        m_pidZ.reset();
        m_pidYaw.reset();
    }

    void iteration(const ros::TimerEvent& e)
    {
        float dt = e.current_real.toSec() - e.last_real.toSec();

        switch(m_state)
        {
        case TakingOff:
            {	
                tf::StampedTransform transform;
                m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);
		
                if (transform.getOrigin().z() > m_startZ + 0.05 /*|| m_thrust > 50000*/)
                {
                    pidReset();
                    //m_pidZ.setIntegral(m_thrust / m_pidZ.ki());
                    m_state = Automatic;
		    ROS_WARN("Entering Automatic Mode");
                    m_thrust = 0;
                }
                else
                {
                    m_thrust += 10000 * dt;
                    geometry_msgs::Twist msg;
                    msg.linear.z = m_thrust;
                    m_pubNav.publish(msg);
                }

            }
            break;
        case Landing:
            {
		m_target_height = 0.05;
		tf::StampedTransform transform;
                m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);
		
		if(transform.getOrigin().z() <= 0.35) {
		     ros::Time m_start_time = ros::Time::now();
		     ros::Duration timeout(3.0);
		     while(ros::Time::now() - m_start_time < timeout) {
			geometry_msgs::Twist msg;
			msg.linear.x = 0;
			msg.linear.y = 0;
			msg.linear.z = m_last_thrust - 2000;
			m_pubNav.publish(msg);
			m_last_thrust = msg.linear.z;
			ros::Duration(0.1).sleep();
		     }
			m_state = Idle;
		        geometry_msgs::Twist msg;
                        m_pubNav.publish(msg); 
		}
				
            }
            // intentional fall-thru
        case Automatic:
            {	
		
		// Safety Landing if sight of marker is lost longer than 1 sec
		ros::Duration timeout(1.0);
		if (ros::Time::now() - m_last_marker > timeout) {
			m_state = SafetyLanding;
			m_safety_start_time = ros::Time::now();
			ROS_WARN("Safety Landing initialized");
			break;
		}

                tf::StampedTransform transform;
                m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);

                geometry_msgs::PoseStamped targetWorld;
                targetWorld.header.stamp = transform.stamp_;
                targetWorld.header.frame_id = m_worldFrame;
                targetWorld.pose = m_goal.pose;
		
		if(m_state != Landing) m_target_height = 0.7;

                tfScalar roll, pitch, yaw;
                tf::Matrix3x3(
                    tf::Quaternion(
                        targetWorld.pose.orientation.x,
                        targetWorld.pose.orientation.y,
                        targetWorld.pose.orientation.z,
                        targetWorld.pose.orientation.w
                    )).getRPY(roll, pitch, yaw);

                geometry_msgs::Twist msg;
		if (targetWorld.pose.position.x > 0.2 && targetWorld.pose.position.x < -0.2)
			msg.linear.x = m_pidX.updateWithoutI(0.0, targetWorld.pose.position.x);
		else
			msg.linear.x = m_pidX.update(0.0, targetWorld.pose.position.x);

		if (targetWorld.pose.position.y > 0.2 && targetWorld.pose.position.y < -0.2)
			msg.linear.y = m_pidY.updateWithoutI(0.0, targetWorld.pose.position.y);
		else
			msg.linear.y = m_pidY.update(0.0, targetWorld.pose.position.y);			
			
                msg.linear.z = 39000 + m_pidZ.update(m_target_height, targetWorld.pose.position.z);
                msg.angular.z = m_pidYaw.update(0, yaw);
                m_pubNav.publish(msg);
		m_last_thrust = msg.linear.z;
            }
            break;
        case Idle:
            {	
                geometry_msgs::Twist msg;
                m_pubNav.publish(msg);
            }
            break;
	case SafetyLanding:
	    {
		ros::Duration timeout(3.0);
		if (ros::Time::now() - m_safety_start_time < timeout && m_last_thrust > 0.0) {
			geometry_msgs::Twist msg;
			msg.linear.x = 0;
			msg.linear.y = 0;
			msg.linear.z = m_last_thrust - 10000*dt;
			m_pubNav.publish(msg);
			m_last_thrust = msg.linear.z;
		}
		else m_state = Idle;	
	    }
	    break;
        }
    }

private:

    enum State
    {
        Idle = 0,
        Automatic = 1,
        TakingOff = 2,
        Landing = 3,
	SafetyLanding = 4,
    };

private:
    std::string m_worldFrame;
    std::string m_frame;
    ros::Publisher m_pubNav;
    tf::TransformListener m_listener;
    PID m_pidX;
    PID m_pidY;
    PID m_pidZ;
    PID m_pidYaw;
    State m_state;
    geometry_msgs::PoseStamped m_goal;
    ros::Subscriber m_subscribeGoal;
    ros::ServiceServer m_serviceTakeoff;
    ros::ServiceServer m_serviceLand;
    float m_thrust;
    float m_startZ;
    float m_last_thrust;
    ros::Time m_last_marker;	
    float m_target_height;
    ros::Time m_safety_start_time;
};

int main(int argc, char **argv)
{
  ros::init(argc, argv, "controller");

  // Read parameters
  ros::NodeHandle n("~");
  std::string worldFrame;
  n.param<std::string>("worldFrame", worldFrame, "/world");
  std::string frame;
  n.getParam("frame", frame);
  double frequency;
  n.param("frequency", frequency, 50.0);

  Controller controller(worldFrame, frame, n);
  controller.run(frequency);

  return 0;
}
