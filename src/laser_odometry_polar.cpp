#include <laser_odometry_polar/laser_odometry_polar.h>

#include <laser_odometry_core/laser_odometry_utils.h>

#include <pluginlib/class_list_macros.h>

namespace laser_odometry {

// convert from cm to m
constexpr double M_TO_CM = 100.0;
constexpr double CM_TO_M = 1/M_TO_CM;
constexpr double READING_ERROR = 99999;

OdomType LaserOdometryPolar::odomType() const noexcept
{
  return OdomType::Odom2D;
}

bool LaserOdometryPolar::configureImpl()
{
  params_ptr_ = std::make_shared<Parameters>(private_nh_);
  params_ptr_->fromParamServer();


  kf_dist_linear_x_  = params_ptr_->kf_dist_linear_x;
  kf_dist_linear_y_  = params_ptr_->kf_dist_linear_y;
  kf_dist_angular_   = params_ptr_->kf_dist_angular;

  ROS_INFO_STREAM("LaserOdometryPolar parameters:\n" << *params_ptr_);

  return true;
}

bool LaserOdometryPolar::processImpl(const sensor_msgs::LaserScanConstPtr& laser_msg,
                                     const Transform& prediction)
{
  current_scan_ = std::make_shared<PMScan>(laser_msg->ranges.size());

  convert(laser_msg, current_scan_);

  current_scan_->rx =  prediction.translation()(1) * M_TO_CM; // y
  current_scan_->ry = -prediction.translation()(0) * M_TO_CM; // -x
  current_scan_->th = utils::getYaw(prediction.linear());

  prev_scan_->rx = 0;
  prev_scan_->ry = 0;
  prev_scan_->th = 0;

  try
  {
    polar_matcher_.pm_psm(prev_scan_.get(), current_scan_.get());
  }
  catch(const int err)
  {
    ROS_WARN("Error %i in polar scan matching.", err);
    prev_scan_ = current_scan_;
    return false;
  }

  increment_.translation()(0) =  current_scan_->ry * CM_TO_M;
  increment_.translation()(1) = -current_scan_->rx * CM_TO_M;
  increment_.linear() = utils::matrixYaw(current_scan_->th);

  return true;
}

void LaserOdometryPolar::convert(const sensor_msgs::LaserScanConstPtr& scan_msg,
                                 std::shared_ptr<PMScan>& psm_scan)
{
  psm_scan->rx = 0;
  psm_scan->ry = 0;
  psm_scan->th = 0;

  for (int i = 0; i < scan_msg->ranges.size(); ++i)
  {
    if (scan_msg->ranges[i] == 0)
    {
      psm_scan->r[i] = READING_ERROR;
    }
    else
    {
      psm_scan->r[i] = scan_msg->ranges[i] * M_TO_CM;
      psm_scan->x[i] = psm_scan->r[i] * polar_matcher_.pm_co[i];
      psm_scan->y[i] = psm_scan->r[i] * polar_matcher_.pm_si[i];
    }
    psm_scan->bad[i] = false;
  }

  polar_matcher_.pm_median_filter  (psm_scan.get());
  polar_matcher_.pm_find_far_points(psm_scan.get());
  polar_matcher_.pm_segment_scan   (psm_scan.get());
}

bool LaserOdometryPolar::initialize(const sensor_msgs::LaserScanConstPtr& scan_msg)
{
  polar_matcher_.PM_L_POINTS         = scan_msg->ranges.size();

  polar_matcher_.PM_FOV              = (scan_msg->angle_max - scan_msg->angle_min) * PM_R2D;
  polar_matcher_.PM_MAX_RANGE        = scan_msg->range_max * M_TO_CM;

  polar_matcher_.PM_TIME_DELAY       = 0.00;

  polar_matcher_.PM_MIN_VALID_POINTS = params_ptr_->min_valid_points;
  polar_matcher_.PM_SEARCH_WINDOW    = params_ptr_->search_window;
  polar_matcher_.PM_MAX_ERROR        = params_ptr_->max_error * M_TO_CM;

  polar_matcher_.PM_MAX_ITER         = params_ptr_->max_iterations;
  polar_matcher_.PM_MAX_ITER_ICP     = params_ptr_->max_iterations;
  polar_matcher_.PM_STOP_COND        = params_ptr_->stop_condition * M_TO_CM;
  polar_matcher_.PM_STOP_COND_ICP    = params_ptr_->stop_condition * M_TO_CM;


  ROS_DEBUG_STREAM("Set PM_L_POINTS: "         << polar_matcher_.PM_L_POINTS);
  ROS_DEBUG_STREAM("Set PM_FOV: "              << polar_matcher_.PM_FOV);
  ROS_DEBUG_STREAM("Set PM_MAX_RANGE: "        << polar_matcher_.PM_MAX_RANGE);
  ROS_DEBUG_STREAM("Set PM_TIME_DELAY: "       << polar_matcher_.PM_TIME_DELAY);
  ROS_DEBUG_STREAM("Set PM_MIN_VALID_POINTS: " << polar_matcher_.PM_MIN_VALID_POINTS);
  ROS_DEBUG_STREAM("Set PM_SEARCH_WINDOW: "    << polar_matcher_.PM_SEARCH_WINDOW);
  ROS_DEBUG_STREAM("Set PM_MAX_ERROR: "        << polar_matcher_.PM_MAX_ERROR);
  ROS_DEBUG_STREAM("Set PM_MAX_ITER: "         << polar_matcher_.PM_MAX_ITER);
  ROS_DEBUG_STREAM("Set PM_MAX_ITER_ICP: "     << polar_matcher_.PM_MAX_ITER_ICP);
  ROS_DEBUG_STREAM("Set PM_STOP_COND: "        << polar_matcher_.PM_STOP_COND);
  ROS_DEBUG_STREAM("Set PM_STOP_COND_ICP: "    << polar_matcher_.PM_STOP_COND_ICP);

  polar_matcher_.pm_init();

  prev_scan_ = std::make_shared<PMScan>(scan_msg->ranges.size());
  convert(scan_msg, prev_scan_);

  return true;
}

bool LaserOdometryPolar::isKeyFrame(const Transform& increment)
{
  if (std::abs(utils::getYaw(increment.rotation())) > kf_dist_angular_)
  {
    ROS_WARN_STREAM("Yaw too big. Max allowed: " << kf_dist_angular_ << " actual: " << std::abs(utils::getYaw(increment.rotation())));
    return false;
  }

  if (std::fabs(static_cast<float>(increment.translation()(0))) > kf_dist_linear_x_)
  {
    ROS_WARN_STREAM("X-dist too big. Max allowed: " << kf_dist_linear_x_ << " actual: " << std::fabs(static_cast<float>(increment.translation()(0))));
    return false;
  }

  if (std::fabs(static_cast<float>(increment.translation()(1))) > kf_dist_linear_y_)
  {
      ROS_WARN_STREAM("Y-dist too big. Max allowed: " << kf_dist_linear_y_ << " actual: " << std::fabs(static_cast<float>(increment.translation()(1))));
    return false;
  }

  return true;
}

void LaserOdometryPolar::isKeyFrame()
{
  prev_scan_.reset();
  prev_scan_ = current_scan_;
}

void LaserOdometryPolar::isNotKeyFrame()
{
  prev_scan_.reset();
  prev_scan_ = current_scan_;
}



} /* namespace laser_odometry */

PLUGINLIB_EXPORT_CLASS(laser_odometry::LaserOdometryPolar, laser_odometry::LaserOdometryBase);
