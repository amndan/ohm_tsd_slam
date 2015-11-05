/*
 * slam_node.cpp
 *
 *  Created on: Apr 28, 2014
 *      Author: phil
 */

#include "SlamNode.h"

#include <ros/ros.h>
#include <signal.h>

#include "obcore/base/Logger.h"

ohm_tsd_slam::SlamNode* instance;

void mySigintHandler(int sig) {
	printf("\n***got sigint\n");
	delete instance;
	ros::shutdown();
}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "slam_node", ros::init_options::NoSigintHandler);

  signal(SIGINT, mySigintHandler);

  LOGMSG_CONF("slamlog.log", obvious::Logger::file_off|obvious::Logger::screen_off, DBG_DEBUG, DBG_ERROR);

  instance = new ohm_tsd_slam::SlamNode();

  instance->start();

  return 0;
}
