/*
 * Copyright (C) 2014 Love Park Robotics, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distribted on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <o3d3xx.h>
#include <opencv2/opencv.hpp>
#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <o3d3xx/Config.h>
#include <o3d3xx/Dump.h>
#include <o3d3xx/GetVersion.h>
#include <o3d3xx/Rm.h>

class O3D3xxNode
{
public:
  O3D3xxNode()
    : timeout_millis_(500),
      publish_viz_images_(false),
      spinner_(new ros::AsyncSpinner(1))
  {
    std::string camera_ip;
    int xmlrpc_port;
    std::string password;

    ros::NodeHandle nh("~");
    nh.param("ip", camera_ip, o3d3xx::DEFAULT_IP);
    nh.param("xmlrpc_port", xmlrpc_port, (int) o3d3xx::DEFAULT_XMLRPC_PORT);
    nh.param("password", password, o3d3xx::DEFAULT_PASSWORD);
    nh.param("timeout_millis", this->timeout_millis_, 500);
    nh.param("publish_viz_images", this->publish_viz_images_, false);

    this->frame_id_ = ros::this_node::getName() + "_link";

    //-----------------------------------------
    // Instantiate the camera and frame-grabber
    //-----------------------------------------
    this->cam_ =
      std::make_shared<o3d3xx::Camera>(camera_ip, xmlrpc_port, password);

    this->fg_ =
      std::make_shared<o3d3xx::FrameGrabber>(this->cam_);

    //----------------------
    // Published topics
    //----------------------
    this->cloud_pub_ =
      nh.advertise<pcl::PointCloud<o3d3xx::PointT> >("/cloud", 1);

    image_transport::ImageTransport it(nh);
    this->depth_pub_ = it.advertise("/depth", 1);
    this->depth_viz_pub_ = it.advertise("/depth_viz", 1);
    this->amplitude_pub_ = it.advertise("/amplitude", 1);
    this->conf_pub_ = it.advertise("/confidence", 1);
    this->good_bad_pub_ = it.advertise("/good_bad_pixels", 1);
    this->hist_pub_ = it.advertise("/hist", 1);

    //----------------------
    // Advertised services
    //----------------------
    this->version_srv_ =
      nh.advertiseService<o3d3xx::GetVersion::Request,
			  o3d3xx::GetVersion::Response>
      ("/GetVersion", std::bind(&O3D3xxNode::GetVersion, this,
				std::placeholders::_1,
				std::placeholders::_2));

    this->dump_srv_ =
      nh.advertiseService<o3d3xx::Dump::Request, o3d3xx::Dump::Response>
      ("/Dump", std::bind(&O3D3xxNode::Dump, this,
			  std::placeholders::_1,
			  std::placeholders::_2));

    this->config_srv_ =
      nh.advertiseService<o3d3xx::Config::Request, o3d3xx::Config::Response>
      ("/Config", std::bind(&O3D3xxNode::Config, this,
			    std::placeholders::_1,
			    std::placeholders::_2));

    this->rm_srv_ =
      nh.advertiseService<o3d3xx::Rm::Request, o3d3xx::Rm::Response>
      ("/Rm", std::bind(&O3D3xxNode::Rm, this,
			std::placeholders::_1,
			std::placeholders::_2));
  }

  /**
   * Main publishing loop
   */
  void Run()
  {
    std::unique_lock<std::mutex> fg_lock(this->fg_mutex_, std::defer_lock);
    this->spinner_->start();

    o3d3xx::ImageBuffer::Ptr buff =
      std::make_shared<o3d3xx::ImageBuffer>();

    pcl::PointCloud<o3d3xx::PointT>::Ptr
      cloud(new pcl::PointCloud<o3d3xx::PointT>());

    cv::Mat confidence_img;
    cv::Mat depth_img;
    cv::Mat depth_viz_img;
    cv::Mat hist_img;
    double min, max;

    while (ros::ok())
      {
	fg_lock.lock();
	if (! this->fg_->WaitForFrame(buff.get(), this->timeout_millis_))
	  {
	    fg_lock.unlock();
	    ROS_WARN("Timeout waiting for camera!");
	    continue;
	  }
	fg_lock.unlock();

	// boost::shared_ptr vs std::shared_ptr forces us to make this copy :(
	pcl::copyPointCloud(*(buff->Cloud().get()), *cloud);
	cloud->header.frame_id = this->frame_id_;
	this->cloud_pub_.publish(cloud);

	depth_img = buff->DepthImage();
	sensor_msgs::ImagePtr depth =
	  cv_bridge::CvImage(std_msgs::Header(),
			     "mono16", depth_img).toImageMsg();
	depth->header.frame_id = this->frame_id_;
	this->depth_pub_.publish(depth);

	sensor_msgs::ImagePtr amplitude =
	  cv_bridge::CvImage(std_msgs::Header(),
			     "mono16", buff->AmplitudeImage()).toImageMsg();
	amplitude->header.frame_id = this->frame_id_;
	this->amplitude_pub_.publish(amplitude);

	confidence_img = buff->ConfidenceImage();
	sensor_msgs::ImagePtr confidence =
	  cv_bridge::CvImage(std_msgs::Header(),
			     "mono8", confidence_img).toImageMsg();
	confidence->header.frame_id = this->frame_id_;
	this->conf_pub_.publish(confidence);

	if (this->publish_viz_images_)
	  {
	    // depth image with better colormap
	    cv::minMaxIdx(depth_img, &min, &max);
	    cv::convertScaleAbs(depth_img, depth_viz_img, 255 / max);
	    cv::applyColorMap(depth_viz_img, depth_viz_img, cv::COLORMAP_JET);
	    sensor_msgs::ImagePtr depth_viz =
	      cv_bridge::CvImage(std_msgs::Header(),
				 "bgr8", depth_viz_img).toImageMsg();
	    depth_viz->header.frame_id = this->frame_id_;
	    this->depth_viz_pub_.publish(depth_viz);

	    // show good vs bad pixels as binary image
	    cv::Mat good_bad_map = cv::Mat::ones(confidence_img.rows,
						 confidence_img.cols,
						 CV_8UC1);
	    cv::bitwise_and(confidence_img, good_bad_map,
			    good_bad_map);
	    good_bad_map *= 255;
	    sensor_msgs::ImagePtr good_bad =
	      cv_bridge::CvImage(std_msgs::Header(),
				 "mono8", good_bad_map).toImageMsg();
	    good_bad->header.frame_id = this->frame_id_;
	    this->good_bad_pub_.publish(good_bad);

	    // histogram of amplitude image
	    hist_img = o3d3xx::hist1(buff->AmplitudeImage());
	    cv::minMaxIdx(hist_img, &min, &max);
	    cv::convertScaleAbs(hist_img, hist_img, 255 / max);
	    sensor_msgs::ImagePtr hist =
	      cv_bridge::CvImage(std_msgs::Header(),
				 "bgr8", hist_img).toImageMsg();
	    hist->header.frame_id = this->frame_id_;
	    this->hist_pub_.publish(hist);
	  }
      }
  }

  /**
   * Implements the `GetVersion' service.
   *
   * The `GetVersion' service will return the version string of the underlying
   * libo3d3xx library.
   */
  bool GetVersion(o3d3xx::GetVersion::Request &req,
		  o3d3xx::GetVersion::Response &res)
  {
    int major, minor, patch;
    o3d3xx::version(&major, &minor, &patch);

    std::ostringstream ss;
    ss << O3D3XX_LIBRARY_NAME
       << ": " << major << "." << minor << "." << patch;

    res.version = ss.str();
    return true;
  }

  /**
   * Implements the `Dump' service.
   *
   * The `Dump' service will dump the current camera configuration to a JSON
   * string. This JSON string is suitable for editing and using to reconfigure
   * the camera via the `Config' service.
   */
  bool Dump(o3d3xx::Dump::Request &req,
	    o3d3xx::Dump::Response &res)
  {
    std::lock_guard<std::mutex> lock(this->fg_mutex_);
    res.status = 0;

    try
      {
	res.config = this->cam_->ToJSON();
      }
    catch (const o3d3xx::error_t& ex)
      {
	res.status = ex.code();
      }

    this->fg_.reset(new o3d3xx::FrameGrabber(this->cam_));
    return true;
  }

  /**
   * Implements the `Config' service.
   *
   * The `Config' service will read the input JSON configuration data and
   * mutate the camera's settings to match that of the configuration
   * described by the JSON file. Syntactically, the JSON should look like the
   * JSON that is produced by `Dump'. However, you need not specify every
   * parameter. You can specify only the parameters you wish to change with the
   * only caveat being that you need to specify the parameter as fully
   * qualified from the top-level root of the JSON tree.
   */
  bool Config(o3d3xx::Config::Request &req,
	      o3d3xx::Config::Response &res)
  {
    std::lock_guard<std::mutex> lock(this->fg_mutex_);
    res.status = 0;
    res.msg = "OK";

    try
      {
	this->cam_->FromJSON(req.json);
      }
    catch (const o3d3xx::error_t& ex)
      {
	res.status = ex.code();
	res.msg = ex.what();
      }
    catch (const std::exception& std_ex)
      {
	res.status = -1;
	res.msg = std_ex.what();
      }

    this->fg_.reset(new o3d3xx::FrameGrabber(this->cam_));
    return true;
  }

  /**
   * Implements the `Rm' service.
   *
   * The `Rm' service is used to remove an application from the camera. This
   * service restricts removing the current active application.
   */
  bool Rm(o3d3xx::Rm::Request &req,
	  o3d3xx::Rm::Response &res)
  {
    std::lock_guard<std::mutex> lock(this->fg_mutex_);
    res.status = 0;
    res.msg = "OK";

    try
      {
	if (req.index > 0)
	  {
	    this->cam_->RequestSession();
	    this->cam_->SetOperatingMode(o3d3xx::Camera::operating_mode::EDIT);
	    o3d3xx::DeviceConfig::Ptr dev = this->cam_->GetDeviceConfig();

	    if (dev->ActiveApplication() != req.index)
	      {
		this->cam_->DeleteApplication(req.index);
	      }
	    else
	      {
		res.status = -1;
		res.msg = std::string("Cannot delete active application!");
	      }
	  }
      }
    catch (const o3d3xx::error_t& ex)
      {
	res.status = ex.code();
	res.msg = ex.what();
      }
    catch (const std::exception& std_ex)
      {
	res.status = -1;
	res.msg = std_ex.what();
      }

    this->cam_->CancelSession(); // <-- OK to do this here
    this->fg_.reset(new o3d3xx::FrameGrabber(this->cam_));
    return true;
  }


private:
  int timeout_millis_;
  bool publish_viz_images_;
  std::unique_ptr<ros::AsyncSpinner> spinner_;
  o3d3xx::Camera::Ptr cam_;
  o3d3xx::FrameGrabber::Ptr fg_;
  std::mutex fg_mutex_;

  std::string frame_id_;
  ros::Publisher cloud_pub_;
  image_transport::Publisher depth_pub_;
  image_transport::Publisher depth_viz_pub_;
  image_transport::Publisher amplitude_pub_;
  image_transport::Publisher conf_pub_;
  image_transport::Publisher good_bad_pub_;
  image_transport::Publisher hist_pub_;

  ros::ServiceServer version_srv_;
  ros::ServiceServer dump_srv_;
  ros::ServiceServer config_srv_;
  ros::ServiceServer rm_srv_;

}; // end: class O3D3xxNode

int main(int argc, char **argv)
{
  o3d3xx::Logging::Init();
  ros::init(argc, argv, "o3d3xx");
  O3D3xxNode().Run();
  return 0;
}
