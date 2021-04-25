/*
 * @Description: IMU-lidar fusion for localization workflow
 * @Author: Ge Yao
 * @Date: 2020-11-12 15:14:07
 */

#include "lidar_localization/global_defination/global_defination.h"

#include "lidar_localization/filtering/kitti_filtering_flow.hpp"

#include "lidar_localization/tools/file_manager.hpp"

#include "glog/logging.h"
#include <ostream>


namespace lidar_localization {

KITTIFilteringFlow::KITTIFilteringFlow(
    ros::NodeHandle& nh
) {
    // subscriber:
    // a. IMU raw measurement:
    // imu_raw_sub_ptr_ = std::make_shared<IMUSubscriber>(nh, "/kitti/oxts/imu/extract", 1000000); //CF
    imu_raw_sub_ptr_ = std::make_shared<IMUSubscriber>(nh, "/imu/data", 1000000); // CF
    // b. undistorted Velodyne measurement: 
    cloud_sub_ptr_ = std::make_shared<CloudSubscriber>(nh, "/synced_cloud", 100000);
    // c. IMU synced measurement:
    imu_synced_sub_ptr_ = std::make_shared<IMUSubscriber>(nh, "/synced_imu", 100000); 
    // d. synced GNSS-odo measurement:
    // pos_vel_sub_ptr_ = std::make_shared<PosVelSubscriber>(nh, "/synced_pos_vel", 100000);//CF
    // e. lidar pose in map frame:
    gnss_sub_ptr_ = std::make_shared<OdometrySubscriber>(nh, "/synced_gnss", 100000);
    // f. lidar to imu tf:
    // lidar_to_imu_ptr_ = std::make_shared<TFListener>(nh, "/imu_link", "/velo_link"); //CF
    
    // publisher:
    // a. global point cloud map:
    global_map_pub_ptr_ = std::make_shared<CloudPublisher>(nh, "/global_map", "/map", 100);
    // b. local point cloud map:
    local_map_pub_ptr_ = std::make_shared<CloudPublisher>(nh, "/local_map", "/map", 100);
    // c. current scan:
    current_scan_pub_ptr_ = std::make_shared<CloudPublisher>(nh, "/current_scan", "/map", 100);
    // d. estimated lidar pose in map frame:
    laser_odom_pub_ptr_ = std::make_shared<OdometryPublisher>(nh, "/laser_localization", "/map", "/lidar", 100);
    // e. fused pose in map frame:
    fused_odom_pub_ptr_ = std::make_shared<OdometryPublisher>(nh, "/fused_localization", "/map", "/lidar", 100);

    // f. tf:
    laser_tf_pub_ptr_ = std::make_shared<TFBroadCaster>("/map", "/vehicle_link");

    filtering_ptr_ = std::make_shared<KITTIFiltering>();
}

bool KITTIFilteringFlow::Run() {

//     if ( !InitCalibration() ) {
//         return false; //CF
//     }

    // if new global map is available, publish it:
    PublishGlobalMap();
    // if new local map is available, publish it:
    PublishLocalMap();

    ReadData();

    while( HasData() ) {
        if ( !HasInited() ) {
            if ( ValidLidarData() ) {
                InitLocalization();
            }
        } else {
            // TODO: handle timestamp chaos in an more elegant way
            if (  HasLidarData() && ValidLidarData() ) {
                if ( HasIMUData() ) {
                    while (
                        HasIMUData() && ValidIMUData() && 
                        current_imu_raw_data_.time < current_cloud_data_.time
                    ) {
                        UpdateLocalization();
                    }

                    if (
                        current_imu_raw_data_.time >= current_cloud_data_.time
                    ) {
                        imu_raw_data_buff_.push_back(current_imu_raw_data_);
                    }
                }

                CorrectLocalization();
            }

            // if ( HasIMUData() && ValidIMUData() ) {
            //     UpdateLocalization(); //CF
            // }

            if ( current_imu_raw_data_.time <= current_cloud_data_.time ) {
                if (HasIMUData() && ValidIMUData() ) {
                    UpdateLocalization();
                }
            } else {
                break;
            }

        }

    }

    return true;
}

bool KITTIFilteringFlow::SaveOdometry(void) {
    if ( 0 == trajectory.N ) {
        return false;
    }

    // init output files:
    std::ofstream fused_odom_ofs;
    std::ofstream laser_odom_ofs;
    std::ofstream ref_odom_ofs;
    // std::ofstream fused_vel_ofs;//CF
    // std::ofstream ref_vel_ofs;//CF
    if (
        !FileManager::CreateFile(fused_odom_ofs, WORK_SPACE_PATH + "/slam_data/trajectory/fused.txt") ||
        !FileManager::CreateFile(laser_odom_ofs, WORK_SPACE_PATH + "/slam_data/trajectory/laser.txt") ||
        !FileManager::CreateFile(ref_odom_ofs, WORK_SPACE_PATH + "/slam_data/trajectory/ground_truth.txt")
        // !FileManager::CreateFile(fused_vel_ofs, WORK_SPACE_PATH + "/slam_data/trajectory/vel_fused.txt")|| //CF
        // !FileManager::CreateFile(ref_vel_ofs, WORK_SPACE_PATH + "/slam_data/trajectory/vel_ground_truth.txt")//CF
    ) {
        return false;
    }

    // write outputs:
    for (size_t i = 0; i < trajectory.N; ++i) {
        // sync ref pose with gnss measurement:
        while (
            !gnss_data_buff_.empty() && 
            (gnss_data_buff_.front().time - trajectory.time_.at(i) <= -0.05)
        ) {
            gnss_data_buff_.pop_front();
        }
    
        if ( gnss_data_buff_.empty() ) {
            break;
        }
        
        current_gnss_data_ = gnss_data_buff_.front();

        const Eigen::Vector3f &position_ref = current_gnss_data_.pose.block<3, 1>(0, 3);
        const Eigen::Vector3f &position_lidar = trajectory.lidar_.at(i).block<3, 1>(0, 3);

        if ( (position_ref - position_lidar).norm() > 3.0 ) {
            continue;
        }

        SavePose(trajectory.fused_.at(i), fused_odom_ofs);
        SavePose(trajectory.lidar_.at(i), laser_odom_ofs);
        SavePose(current_gnss_data_.pose, ref_odom_ofs);

        // const Eigen::Matrix3f &Cnb = current_gnss_data_.pose.block<3, 3>(0, 0); //CF

        // SaveVel(velocity_n.time_.at(i), Cnb.transpose()*velocity_n.fused_.at(i), fused_vel_ofs); //CF
        // SaveVel(velocity_n.time_.at(i), current_gnss_data_.vel, ref_vel_ofs); //CF
    }

    return true;
}

bool KITTIFilteringFlow::ReadData() {
    //
    // pipe raw IMU measurements into buffer:
    // 
    imu_raw_sub_ptr_->ParseData(imu_raw_data_buff_);
    while (
        HasInited() && 
        HasIMUData() && 
        imu_raw_data_buff_.front().time < filtering_ptr_->GetTime()
    ) {
        imu_raw_data_buff_.pop_front();
    }

    //
    // pipe synced lidar-GNSS-IMU measurements into buffer:
    // 
    cloud_sub_ptr_->ParseData(cloud_data_buff_);
    imu_synced_sub_ptr_->ParseData(imu_synced_data_buff_);
    // pos_vel_sub_ptr_->ParseData(pos_vel_data_buff_); //CF
    gnss_sub_ptr_->ParseData(gnss_data_buff_);

    return true;
}

bool KITTIFilteringFlow::HasInited(void) {
    return filtering_ptr_->HasInited();
}

bool KITTIFilteringFlow::HasData() {
    if ( !HasInited() ) {
        if ( !HasLidarData() ) {
            return false;
        }
    } else {
        if ( !HasIMUData() && !HasLidarData() ) {
            return false;
        }
    }

    return true;
}

bool KITTIFilteringFlow::ValidIMUData() {
    current_imu_raw_data_ = imu_raw_data_buff_.front();

    imu_raw_data_buff_.pop_front();

    return true;
}

bool KITTIFilteringFlow::ValidLidarData() {
    current_cloud_data_ = cloud_data_buff_.front();
    current_imu_synced_data_ = imu_synced_data_buff_.front();
    // current_pos_vel_data_ = pos_vel_data_buff_.front(); //CF

    double diff_imu_time = current_cloud_data_.time - current_imu_synced_data_.time;
    // double diff_pos_vel_time = current_cloud_data_.time - current_pos_vel_data_.time; //CF

    // if ( diff_imu_time < -0.05 || diff_pos_vel_time < -0.05 ) {//CF
    if ( diff_imu_time < -0.05 ) {
        cloud_data_buff_.pop_front();
        return false;
    }

    if (diff_imu_time > 0.05) {
        imu_synced_data_buff_.pop_front();
        return false;
    }

    // if (diff_pos_vel_time > 0.05) {
    //     pos_vel_data_buff_.pop_front(); //CF
    //     return false;
    // }

    cloud_data_buff_.pop_front();
    imu_synced_data_buff_.pop_front();
    // pos_vel_data_buff_.pop_front(); //CF

    return true;
}

bool KITTIFilteringFlow::InitCalibration() {
    // lookup imu pose in lidar frame:
    static bool calibration_received = false;

    if (!calibration_received) {
        if (lidar_to_imu_ptr_->LookupData(lidar_to_imu_)) {
            calibration_received = true;
        }
    }

    return calibration_received;
}

bool KITTIFilteringFlow::InitLocalization(void) {
    // ego vehicle velocity in body frame:
    Eigen::Vector3f init_vel = current_pos_vel_data_.vel;


    // first try to init using scan context query:
    if (
        filtering_ptr_->Init(
            current_cloud_data_,
            init_vel,
            current_imu_synced_data_
        )
    ) {
        // prompt:
        LOG(INFO) << "Scan Context Localization Init Succeeded." << std::endl;
    } 

    return true;
}

bool KITTIFilteringFlow::UpdateLocalization() {
    if ( filtering_ptr_->Update(current_imu_raw_data_) ) {
        PublishFusionOdom();
        return true;
    }

    return false;
}

bool KITTIFilteringFlow::CorrectLocalization() {
    bool is_fusion_succeeded = filtering_ptr_->Correct(
        current_imu_synced_data_, 
        current_cloud_data_, 
        current_pos_vel_data_,
        laser_pose_
    );
    PublishLidarOdom();

    if ( is_fusion_succeeded ) {        
        PublishFusionOdom();
        
        // add to odometry output for evo evaluation:
        UpdateOdometry(current_cloud_data_.time);

        return true;
    }

    return false;
}

bool KITTIFilteringFlow::PublishGlobalMap() {
    if (filtering_ptr_->HasNewGlobalMap() && global_map_pub_ptr_->HasSubscribers()) {
        CloudData::CLOUD_PTR global_map_ptr(new CloudData::CLOUD());
        filtering_ptr_->GetGlobalMap(global_map_ptr);
        global_map_pub_ptr_->Publish(global_map_ptr);

        return true;
    }

    return false;
}

bool KITTIFilteringFlow::PublishLocalMap() {
    if (filtering_ptr_->HasNewLocalMap() && local_map_pub_ptr_->HasSubscribers()) {
        local_map_pub_ptr_->Publish(filtering_ptr_->GetLocalMap());

        return true;
    }
    
    return false;
}

bool KITTIFilteringFlow::PublishLidarOdom() {
    // a. publish lidar odometry
    laser_odom_pub_ptr_->Publish(laser_pose_, current_cloud_data_.time);
    // b. publish current scan:
    current_scan_pub_ptr_->Publish(filtering_ptr_->GetCurrentScan());

    return true;
}

bool KITTIFilteringFlow::PublishFusionOdom() {
    // get odometry from Kalman filter:
    filtering_ptr_->GetOdometry(fused_pose_, fused_vel_);
    // a. publish tf:
    laser_tf_pub_ptr_->SendTransform(fused_pose_, current_imu_raw_data_.time);
    // b. publish fusion odometry:
    fused_odom_pub_ptr_->Publish(fused_pose_, fused_vel_, current_imu_raw_data_.time);

    return true;
}

bool KITTIFilteringFlow::UpdateOdometry(const double &time) {
    trajectory.time_.push_back(time);
    
    trajectory.fused_.push_back(fused_pose_);
    trajectory.lidar_.push_back(laser_pose_);

    ++trajectory.N;


    // velocity_n.time_.push_back(time); //CF
    
    // velocity_n.fused_.push_back(fused_vel_);//CF

    // ++velocity_n.N;//CF

    return true;
}

/**
 * @brief  save pose in KITTI format for evo evaluation
 * @param  pose, input pose
 * @param  ofs, output file stream
 * @return true if success otherwise false
 */
bool KITTIFilteringFlow::SavePose(
    const Eigen::Matrix4f& pose, 
    std::ofstream& ofs
) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            ofs << pose(i, j);
            
            if (i == 2 && j == 3) {
                ofs << std::endl;
            } else {
                ofs << " ";
            }
        }
    }

    return true;
}

/**
 * @brief  save velocity for evo evaluation
 * @param  vel, input velocity
 * @param  ofs, output file stream
 * @return true if success otherwise false
 */
bool KITTIFilteringFlow::SaveVel(
    const double& time, 
    const Eigen::Vector3f& vel, 
    std::ofstream& ofs
) {
    ofs << time;
    ofs << " ";

    for (int i =0; i<3;++i){
        ofs << vel(i);

        if(i == 2){
            ofs << std::endl;
        } else{
            ofs << " ";
        }
    }
    return true;
}

}