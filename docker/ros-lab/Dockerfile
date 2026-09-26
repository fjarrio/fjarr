# The lab's ROS 2 end: the base image plus Cyclone DDS, so both RMW implementations docs/27 makes
# claims about can be measured rather than assumed. Nothing here ships — on a robot ROS 2 is the
# customer's software and the agent neither contains nor knows about it (docs/27#ros2).
FROM ros:jazzy-ros-base

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*
