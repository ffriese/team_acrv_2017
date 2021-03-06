#include <eigen_conversions/eigen_msg.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/TransformStamped.h>
#include <pcl_conversions/pcl_conversions.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>
#include <tf2_ros/transform_broadcaster.h>

#include <visualization_msgs/MarkerArray.h>
#include <string>

#include <apc_grasping/pcl_filters.h>

#include <apc_msgs/DetectGraspCandidates.h>

#include <pcl/common/geometry.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>

#include <pcl/ModelCoefficients.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/extract_indices.h>

ros::Publisher grasp_pub, marker_array_pub, marker_pub, boundaries_pub,
               downsampled_pub, input_pub, grip_pose_pub;
ros::NodeHandle *nh_;
tf::TransformListener *tf_listener;


typedef pcl::PointXYZ InputPointType;
typedef pcl::PointNormal PointTypeNormal;
typedef pcl::PointXYZ PointTypeXYZ;
typedef pcl::PointCloud<InputPointType> InputPointCloud;
typedef pcl::PointCloud<PointTypeXYZ> PointCloudXYZ;
typedef pcl::PointCloud<PointTypeNormal> PointCloudNormal;

boost::shared_ptr<pcl::visualization::PCLVisualizer> visualizer;

float distance_to_closest_point(int K, pcl::PointXYZ searchPoint,
                                pcl::PointCloud<pcl::PointXYZ>::Ptr cloud) {
        pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;

        kdtree.setInputCloud(cloud);

        // K nearest neighbor search
        std::vector<int> pointIdxNKNSearch(K);
        std::vector<float> pointNKNSquaredDistance(K);

        if (kdtree.nearestKSearch(searchPoint, K, pointIdxNKNSearch,
                                  pointNKNSquaredDistance) > 0) {
                // for (size_t i = 0; i < pointIdxNKNSearch.size(); ++i)
                // std::cout << "    " << cloud->points[pointIdxNKNSearch[i]].x << " "
                //           << cloud->points[pointIdxNKNSearch[i]].y << " "
                //           << cloud->points[pointIdxNKNSearch[i]].z
                //           << " (squared distance: " << pointNKNSquaredDistance[i] <<
                //           ")"
                //           << std::endl;

                return sqrt(pointNKNSquaredDistance[0]);

        } else {
                return 0.0;
        }
}

std::vector<std::pair<geometry_msgs::Pose, double> >
sort_grasp_vectors(std::vector<geometry_msgs::Pose> &grasps,
                   std::vector<double> utilities) {
        // initialize original index locations
        std::vector<std::pair<geometry_msgs::Pose, double> > grasp_score_pairs;
        for (unsigned int i = 0; i < grasps.size(); i++) {
                std::pair<geometry_msgs::Pose, double> pair(grasps[i], utilities[i]);
                grasp_score_pairs.push_back(pair);
        }

        std::sort(
                grasp_score_pairs.begin(), grasp_score_pairs.end(),
                boost::bind(&std::pair<geometry_msgs::Pose, double>::second, _1) >
                boost::bind(&std::pair<geometry_msgs::Pose, double>::second, _2));

        return grasp_score_pairs;
}

geometry_msgs::PoseArray
compute_grasp_utility(geometry_msgs::PoseArray grasp_pose_pool,
                      std::vector<double> curvature,
                      pcl::PointCloud<pcl::PointXYZ>::Ptr boundaryCloud,
                      std::vector<double> *utility, double curvature_threshold,
                      double curvature_weight, double boundary_threshold,
                      double boundary_weight) {
        geometry_msgs::PoseArray selected_grasps;

        std::vector<float> distance_to_boundary;
        std::vector<float> normalised_curvature;

        for (unsigned int i = 0; i < grasp_pose_pool.poses.size(); i++) {
                pcl::PointXYZ grasp_point(grasp_pose_pool.poses[i].position.x,
                                          grasp_pose_pool.poses[i].position.y,
                                          grasp_pose_pool.poses[i].position.z);

                float min_distance =
                        distance_to_closest_point(1, grasp_point, boundaryCloud);

                // Find max distance in order to normalise min_distance
                // pcl::getMinMax3D<pcl::PointXYZ>(*boundaryCloud, min_pt, max_pt);
                Eigen::Vector4f point_4f(grasp_point.x, grasp_point.y, grasp_point.z, 0.0);
                Eigen::Vector4f max_pt_4f;

                pcl::getMaxDistance(*boundaryCloud, point_4f, max_pt_4f);
                pcl::PointXYZ max_pt(max_pt_4f[0], max_pt_4f[1], max_pt_4f[2]);

                float max_distance =
                        pcl::geometry::distance<pcl::PointXYZ>(grasp_point, max_pt);

                float norm_min_distance = min_distance / max_distance;

                // compute normalised curvature using min and max values
                double maxCurvature = *std::max_element(curvature.begin(), curvature.end());
                double minCurvature = *std::min_element(curvature.begin(), curvature.end());
                normalised_curvature.push_back((curvature[i] - minCurvature) /
                                               (maxCurvature - minCurvature));

                if (min_distance > boundary_threshold) {
                        // ROS_INFO_STREAM("[CARTESIAN GRASP] Min Distance: " << min_distance << " Norm Min Distance: "
                        //                                  << norm_min_distance
                        //                                  << " Max Distance: " << max_distance
                        //                                  << " Normalised Curvature: "
                        //                                  << (1 - normalised_curvature[i]));
                        // ROS_INFO_STREAM("Found Grasp Pose at: " <<
                        // grasp_pose_pool.poses[i]);
                        // ROS_INFO_STREAM("Curvature: " << curvature[i]);
                        // ROS_INFO_STREAM("Distance from boundary: " << min_distance);

                        selected_grasps.poses.push_back(grasp_pose_pool.poses[i]);

                        distance_to_boundary.push_back(norm_min_distance);

                        utility->push_back((1 - normalised_curvature[i]) * curvature_weight +
                                           norm_min_distance * boundary_weight);
                }
        }

        return selected_grasps;
}

geometry_msgs::Pose translate_pose(geometry_msgs::Pose input_pose,
                                   Eigen::Vector3d offset) {
        Eigen::Affine3d input_pose_eigen, input_pose_eigen_translated;
        geometry_msgs::Pose output_pose;
        tf::poseMsgToEigen(input_pose, input_pose_eigen);

        Eigen::Translation3d translation(offset);

        input_pose_eigen_translated = input_pose_eigen * translation;

        tf::poseEigenToMsg(input_pose_eigen_translated, output_pose);

        return output_pose;
}

void publishGraspMarkers(geometry_msgs::PoseArray grasp_poses,
                         std::vector<double> grasp_utility,
                         double marker_scale) {
        visualization_msgs::MarkerArray grasp_array_delete, grasp_array;

        for (unsigned int i = 0; i < 5000; i++) {
                visualization_msgs::Marker marker;
                marker.header.frame_id = grasp_poses.header.frame_id;
                marker.header.stamp = ros::Time::now();
                marker.ns = "grasp_poses";
                marker.id = i;
                // marker.type = visualization_msgs::Marker::ARROW;
                marker.action = visualization_msgs::Marker::DELETE;

                grasp_array_delete.markers.push_back(marker);
        }

        marker_array_pub.publish(grasp_array_delete);

        double max = grasp_utility[0];
        double min = grasp_utility[grasp_utility.size() - 1];

        for (unsigned int i = 0; i < grasp_poses.poses.size(); i++) {
                visualization_msgs::Marker marker;
                marker.header.frame_id = grasp_poses.header.frame_id;
                marker.header.stamp = ros::Time::now();
                marker.ns = "grasp_poses";
                marker.id = i;
                marker.type = visualization_msgs::Marker::ARROW;
                marker.action = visualization_msgs::Marker::ADD;

                // marker.scale.x = grasp_utility[i] * marker_scale;
                marker.scale.x = marker_scale / 4.0;
                marker.scale.y = 0.0035;
                marker.scale.z = 0.0035;
                marker.color.a = 1.0;
                marker.color.r = 1.0;
                marker.color.g = 0.0;
                marker.color.b = 0.0;
                marker.pose = translate_pose(grasp_poses.poses[i],
                                             Eigen::Vector3d(-marker.scale.x, 0, 0));
                grasp_array.markers.push_back(marker);
        }

        marker_array_pub.publish(grasp_array);
}

void graspPoseFromPointNormal(const pcl::PointNormal &pointNormal,
                              geometry_msgs::Pose *grasp_pose) {

        Eigen::Vector3f normal_vector(pointNormal.normal_x, pointNormal.normal_y,
                                      pointNormal.normal_z);
        Eigen::Quaternion<float> q = Eigen::Quaternion<float>::FromTwoVectors(
                Eigen::Vector3f::UnitX(), normal_vector);

        grasp_pose->position.y = pointNormal.y;
        grasp_pose->position.x = pointNormal.x;
        grasp_pose->position.z = pointNormal.z;
        grasp_pose->orientation.x = q.x();
        grasp_pose->orientation.y = q.y();
        grasp_pose->orientation.z = q.z();
        grasp_pose->orientation.w = q.w();
}

void computePointNormalCloud(
        pcl::PointCloud<pcl::PointXYZ>::Ptr objectCloud_XYZ,
        pcl::PointCloud<pcl::PointXYZ>::Ptr objectCloudDownSampled_XYZ,
        PointCloudNormal::Ptr objectCloudPointNormal, double suction_cup_radius) {

        ROS_INFO("[CARTESIAN GRASP] Computing normals on down sampled cloud");

        Eigen::Vector4f centroid;
        pcl::compute3DCentroid(*objectCloud_XYZ, centroid);

        Eigen::Vector3f view_point(centroid[0], centroid[1], centroid[2]);
        view_point.normalize();

        pcl::PointCloud<pcl::Normal>::Ptr cloudNormals(
                new pcl::PointCloud<pcl::Normal>);
        pcl_filters::compute_normals_down_sampled(
                objectCloud_XYZ, objectCloudDownSampled_XYZ, cloudNormals, view_point,
                suction_cup_radius);

        pcl::concatenateFields(*objectCloudDownSampled_XYZ, *cloudNormals,
                               *objectCloudPointNormal);
}

bool align_pca(
    pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud,
    pcl::PointCloud<pcl::PointXYZ>::Ptr output_cloud,
    bool visualize_pca) {

    ROS_INFO("[CARTESIAN GRASP] Starting the align principle axis function ...");

    // // Declare local temporary variables
    pcl::PointCloud<pcl::PointXYZ>::Ptr local_cloud(
            new pcl::PointCloud<pcl::PointXYZ>);

    // Calculate the centroid
    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*input_cloud,centroid);
    // Subtract the centroid from the point cloud
    pcl::demeanPointCloud<pcl::PointXYZ>(*input_cloud, centroid,
                                         *local_cloud);

    ROS_INFO("[CARTESIAN GRASP] Computing the PCA");
    /// Compute PCA for the input cloud
    pcl::PCA<pcl::PointXYZ> pca;
    pca.setInputCloud(local_cloud);
    //Eigen::Vector3f eigenvalues_float = pca.getEigenValues();
    //Eigen::Vector3d eigenvalues = eigenvalues_float.cast<double>();
    Eigen::Matrix3f eigenvectors_float = pca.getEigenVectors();
    Eigen::Matrix<double, 3, 3> eigenvectors =
        eigenvectors_float.cast<double>();

    // Ensure z-axis direction satisfies right-hand rule
    eigenvectors.col(2) = eigenvectors.col(0).cross(eigenvectors.col(1));

    //Normalising these modifies the scaling of my transformed object
    //eigenvalues = eigenvalues.normalized();
    eigenvectors = eigenvectors.normalized();

    Eigen::Vector3d axis_0 = eigenvectors.col(0);
    Eigen::Vector3d axis_1 = eigenvectors.col(1);
    Eigen::Vector3d axis_2 = eigenvectors.col(2);

    ROS_INFO("[CARTESIAN GRASP] AXIS 0: %f %f %f\n", axis_0[0], axis_0[1], axis_0[2]);
    ROS_INFO("[CARTESIAN GRASP] AXIS 1: %f %f %f\n", axis_1[0], axis_1[1], axis_1[2]);
    ROS_INFO("[CARTESIAN GRASP] AXIS 2: %f %f %f\n", axis_2[0], axis_2[1], axis_2[2]);

    pcl::PointXYZ axis_0_start;
    pcl::PointXYZ axis_0_end;

    axis_0_start.x = centroid[0];
    axis_0_start.y = centroid[1];
    axis_0_start.z = 0.0;
    axis_0_end.x = centroid[0] + axis_0[0];
    axis_0_end.y = centroid[1] + axis_0[1];
    axis_0_end.z = 0.0;

    ROS_INFO("[CARTESIAN GRASP] Principal Axis START: %f %f %f\n", axis_0_start.x, axis_0_start.y, axis_0_start.z);
    ROS_INFO("[CARTESIAN GRASP] Principal Axis END: %f %f %f\n", axis_0_end.x, axis_0_end.y, axis_0_end.z);

    //Get orientation of the prinicpal axis
    Eigen::Quaternion<double> n = Eigen::Quaternion<double>::FromTwoVectors(centroid.block(0,0,3,1).cast<double>(),axis_0);
    ROS_INFO_STREAM("[CARTESIAN GRASP] Quaternion: " << n.x() << " " << n.y() << " " << n.z() << " " << n.w());

    static tf2_ros::TransformBroadcaster static_broadcaster;
    geometry_msgs::TransformStamped pca_transformStamped;
    geometry_msgs::TransformStamped grip_transformStamped;
    tf::Quaternion q;
    tf::Quaternion new_q;
    tf::quaternionEigenToTF(n,q);

    pca_transformStamped.header.stamp = ros::Time::now();
    pca_transformStamped.header.frame_id = "realsense_wrist_rgb_optical_frame";
    pca_transformStamped.child_frame_id = "PCA";
    pca_transformStamped.transform.translation.x = centroid[0];
    pca_transformStamped.transform.translation.y = centroid[1];
    pca_transformStamped.transform.translation.z = centroid[2];
    pca_transformStamped.transform.rotation.x = q.x();
    pca_transformStamped.transform.rotation.y = q.y();
    pca_transformStamped.transform.rotation.z = q.z();
    pca_transformStamped.transform.rotation.w = q.w();

    static_broadcaster.sendTransform(pca_transformStamped);

    double roll, pitch, yaw;
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
    new_q = tf::createQuaternionFromRPY((roll + 1.57),pitch,yaw);

    grip_transformStamped.header.stamp = ros::Time::now();
    grip_transformStamped.header.frame_id = "realsense_wrist_rgb_optical_frame";
    grip_transformStamped.child_frame_id = "GRIP POSE";
    grip_transformStamped.transform.translation.x = centroid[0];
    grip_transformStamped.transform.translation.y = centroid[1];
    grip_transformStamped.transform.translation.z = centroid[2];
    grip_transformStamped.transform.rotation.x = new_q.x();
    grip_transformStamped.transform.rotation.y = new_q.y();
    grip_transformStamped.transform.rotation.z = new_q.z();
    grip_transformStamped.transform.rotation.w = new_q.w();

    static_broadcaster.sendTransform(grip_transformStamped);


    // geometry_msgs::Pose grip_pose;
    //
    // grip_pose.position.y = centroid[0];
    // grip_pose.position.x = centroid[1];
    // grip_pose.position.z = centroid[2];
    // grip_pose.orientation.x = n.x();
    // grip_pose.orientation.y = n.y();
    // grip_pose.orientation.z = n.z();
    // grip_pose.orientation.w = n.w();

    // ROS_INFO("[CARTESIAN] Publishing Grip Pose");
    // // grip_pose.header.frame_id = input_frame;
    // // grip_pose.header.stamp = ros::Time::now();
    // grip_pose_pub.publish(grip_pose);

    // // Align the principal axes to the objects frame
    // Eigen::Matrix<double, 4, 4> transformation(Eigen::Matrix<double, 4, 4>::Identity());
    // transformation.template block<3, 3>(0, 0) = eigenvectors.transpose();
    // transformation.template block<3, 1>(0, 3) = -1.f * (transformation.template block<3,3>(0,0) * centroid.template block<3,1>(0,0));
    //
    // pcl::transformPointCloud(*local_cloud, *output_cloud, transformation);
    //
    // if (visualize_pca) {
    //     ROS_INFO("[CARTESIAN GRASP] Showing PCA Results ...");
    //     // Visualisation
    //     pcl::visualization::PCLVisualizer visu1("PCA Alignment");
    //     pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZ>
    //         input_colour(local_cloud, 0.0, 255.0, 0.0);
    //     //pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZ>
    //     //    output_colour(output_cloud, 255.0, 0.0, 0.0);
    //     visu1.addPointCloud(local_cloud, input_colour, "original");
    //     //visu1.addPointCloud(output_cloud, output_colour, "aligned");
    //     visu1.addLine<pcl::PointXYZ>(
    //          axis_0_start, (axis_0_end), 255.0, 0.0, 0.0, "original_principal_axis_0");
    //     visu1.addCoordinateSystem(0.1);
    //     visu1.spin();
    // }

    return true;
}

bool graspPosesFromSurface(
        pcl::PointCloud<pcl::PointXYZ>::Ptr objectCloud_XYZ,
        pcl::PointCloud<pcl::PointXYZ>::Ptr objectCloudDownSampled_XYZ,
        pcl::PointCloud<pcl::PointXYZ>::Ptr boundaryCloudPCL,
        geometry_msgs::PoseArray *selected_grasp_poses,
        std::vector<double> *grasp_utility, std::string input_frame) {

        geometry_msgs::PoseArray grasp_poses;
        PointCloudNormal::Ptr objectCloudPointNormal(new PointCloudNormal);
        pcl::PointCloud<pcl::Boundary>::Ptr boundaries(
                new pcl::PointCloud<pcl::Boundary>);
        PointCloudNormal::Ptr graspPointNormalsDownsampled;

        double suction_cup_radius, grasp_sample_radius;
        double boundary_detector_radius, boundary_detector_angle, boundary_threshold,
               curvature_threshold, curvature_weight, boundary_weight;

        int boundary_detector_k;
        // Grab parameters from rosparam server
        nh_->param("suction_cup_radius", suction_cup_radius, 25.0 / 1000.0);
        nh_->param("grasp_sample_radius", grasp_sample_radius, 25.0 / 1000.0);

        nh_->param("curvature_weight", curvature_weight, 0.5);
        nh_->param("boundary_weight", boundary_weight, 0.5);

        nh_->param("curvature_threshold", curvature_threshold, 0.1);
        nh_->param("boundary_threshold", boundary_threshold, 0.01);
        nh_->param("boundary_detector_k", boundary_detector_k, 0);
        nh_->param("boundary_detector_radius", boundary_detector_radius, 0.03);
        nh_->param("boundary_detector_angle", boundary_detector_angle, M_PI / 2.0);

        computePointNormalCloud(objectCloud_XYZ, objectCloudDownSampled_XYZ,
                                objectCloudPointNormal, suction_cup_radius);

        ROS_INFO("[CARTESIAN GRASP] Estimating Object Boundary");
        boundaries = pcl_filters::boundaryEstimation(
                objectCloudPointNormal, boundary_detector_k, boundary_detector_radius,
                boundary_detector_angle);

        ROS_INFO("[CARTESIAN GRASP] Downsampling Grasp Normal Cloud");
        graspPointNormalsDownsampled = pcl_filters::downSample<pcl::PointNormal>(
                objectCloudPointNormal, grasp_sample_radius);

        ROS_INFO("[CARTESIAN GRASP] Constructing Grasp Poses from cloud");
        if (graspPointNormalsDownsampled->size() > 0) {
                std::vector<double> curvatures;

                grasp_poses.header.frame_id = input_frame;
                grasp_poses.header.stamp = ros::Time::now();

                for (unsigned int i = 0; i < graspPointNormalsDownsampled->width; i++) {

                        // construct grasp poses from point normals
                        geometry_msgs::Pose grasp_pose;
                        graspPoseFromPointNormal(graspPointNormalsDownsampled->points[i],
                                                 &grasp_pose);

                        grasp_poses.poses.push_back(grasp_pose);
                        curvatures.push_back(graspPointNormalsDownsampled->points[i].curvature);
                }

                // create point cloud of boundary points
                ROS_INFO("[CARTESIAN GRASP] Creating Boundary Cloud for visualisation");
                for (unsigned int i = 0; i < objectCloudPointNormal->size(); i++) {
                        if (boundaries->points[i].boundary_point == 1) {
                                pcl::PointXYZ point;
                                point.x = objectCloudPointNormal->points[i].x;
                                point.y = objectCloudPointNormal->points[i].y;
                                point.z = objectCloudPointNormal->points[i].z;
                                boundaryCloudPCL->push_back(point);
                        }
                }

                // compute a grasp utility
                ROS_INFO("[CARTESIAN GRASP] Computing Grasp utilities on grasp candidats");
                *selected_grasp_poses = compute_grasp_utility(
                        grasp_poses, curvatures, boundaryCloudPCL, grasp_utility,
                        curvature_threshold, curvature_weight, boundary_threshold,
                        boundary_weight);

        } else {
                ROS_INFO("[CARTESIAN GRASP] Not enough points in down sampled object");
                return false;
        }

        return true;
}

//PUBLISH SOME CLOUDS

void publishClouds(
        pcl::PointCloud<pcl::PointXYZ>::Ptr boundaryCloudPCL,
        pcl::PointCloud<pcl::PointXYZ>::Ptr objectCloudDownSampled_XYZ,
        std::string input_frame) {

        // Publish object clouds and grasp markers
        ROS_INFO("Publishing Boundary and Object Clouds");
        sensor_msgs::PointCloud2 boundaryCloud, objectDownSampled;
        pcl::toROSMsg(*boundaryCloudPCL, boundaryCloud);
        pcl::toROSMsg(*objectCloudDownSampled_XYZ, objectDownSampled);

        boundaryCloud.header.frame_id = input_frame;
        boundaryCloud.header.stamp = ros::Time::now();
        boundaries_pub.publish(boundaryCloud);

        objectDownSampled.header.frame_id = input_frame;
        objectDownSampled.header.stamp = ros::Time::now();
        downsampled_pub.publish(objectDownSampled);
}

// GRASP CANDIDATE DETECTION SERVICE

bool grasp_candidate_detection(apc_msgs::DetectGraspCandidates::Request &req,
                               apc_msgs::DetectGraspCandidates::Response &res) {
        ROS_INFO("[CARTESIAN GRASP] Got point cloud, finding surface properties");

        //CREATE SOME OBJECTS

        InputPointCloud::Ptr objectCloud(new InputPointCloud);
        pcl::PointCloud<pcl::PointXYZL>::Ptr objectCloudlabelled(
                new pcl::PointCloud<pcl::PointXYZL>);
        InputPointCloud::Ptr objectCloudDownSampled(new InputPointCloud);
        pcl::PointCloud<pcl::Normal>::Ptr objectCloudNormal(
            new pcl::PointCloud<pcl::Normal>);
        PointCloudNormal::Ptr objectCloudPointNormal(
                new PointCloudNormal);
        pcl::PointCloud<pcl::PointXYZ>::Ptr boundaryCloudPCL(
                new pcl::PointCloud<pcl::PointXYZ>);

        pcl::PointCloud<pcl::PointXYZ>::Ptr pca_output(
                new pcl::PointCloud<pcl::PointXYZ>);

        geometry_msgs::PoseArray selected_grasp_poses;
        std::vector<double> grasp_utility;

        double utility_scale_to_marker_length, down_sample_radius;

        nh_->param("utility_scale_to_marker_length", utility_scale_to_marker_length,
                   0.1);
        nh_->param("down_sample_radius", down_sample_radius, 0.01);

        //INPUT OUR POINT CLOUD

        std::string input_frame = "realsense_wrist_rgb_optical_frame";
        pcl::fromROSMsg(req.cloud, *objectCloud); // Put our point cloud into the ObjectCloudlabelled object
        // pcl::copyPointCloud(*objectCloudlabelled, *objectCloud); //Copy this to our objectCloud object

        // //Downsample object cloud
        // objectCloudDownSampled =
        //         pcl_filters::downSample<InputPointType>(objectCloud, down_sample_radius);

        // Filter downsampled point cloud
        std::vector<int> indices;
        pcl::removeNaNFromPointCloud(*objectCloud, *objectCloud,
                                     indices);

        // Check downsampled cloud has points
        if (objectCloud->width > 0) {
          ROS_INFO("[CARTESIAN GRASP] Point cloud successfully downsampled!");
          // ensure point cloud is in XYZ format
          PointCloudXYZ::Ptr objectCloudDownSampled_XYZ(new PointCloudXYZ);
          PointCloudXYZ::Ptr objectCloud_XYZ(new PointCloudXYZ);

          pcl::copyPointCloud(*objectCloudDownSampled, *objectCloudDownSampled_XYZ);
          pcl::copyPointCloud(*objectCloud, *objectCloud_XYZ);

          ROS_INFO("[CARTESIAN GRASP] Attempting to start the ALIGN PCA function");

          align_pca(objectCloud_XYZ, pca_output, true);

          // ROS_INFO("[CARTESIAN GRASP] Detecting Grasp Candidates from Surface");
          // graspPosesFromSurface(objectCloud_XYZ, objectCloudDownSampled_XYZ,
          //                       boundaryCloudPCL, &selected_grasp_poses,
          //                       &grasp_utility, input_frame);
          //
          // if(selected_grasp_poses.poses.size() > 0){
          //         selected_grasp_poses.header.frame_id = input_frame;
          //         selected_grasp_poses.header.stamp = ros::Time::now();
          //
          //
          //         publishClouds(boundaryCloudPCL, objectCloudDownSampled_XYZ, input_frame);
          //         grasp_pub.publish(selected_grasp_poses);
          //
          //         std::vector<std::pair<geometry_msgs::Pose, double> > sorted_grasp_pairs =
          //                 sort_grasp_vectors(selected_grasp_poses.poses, grasp_utility);
          //
          //         res.grasp_candidates.grasp_poses.header.frame_id = input_frame;
          //         res.grasp_candidates.grasp_poses.header.stamp = ros::Time::now();
          //
          //         for (unsigned int i = 0; i < sorted_grasp_pairs.size(); i++) {
          //                 res.grasp_candidates.grasp_poses.poses.push_back(
          //                         sorted_grasp_pairs[i].first);
          //                 res.grasp_candidates.grasp_utilities.push_back(
          //                         sorted_grasp_pairs[i].second);
          //         }
          //
          //         publishGraspMarkers(res.grasp_candidates.grasp_poses, res.grasp_candidates.grasp_utilities,
          //                     utility_scale_to_marker_length);
          // }else{
          //         ROS_INFO("[CARTESIAN GRASP] No grasp candidates found");
          //         return false;
          // }


        } else {
          ROS_INFO("[CARTESIAN GRASPING] Not enough points in object");
          return true;
        }

        // ROS_INFO_STREAM("[CARTESIAN GRASPING] Finished! Found "
        //                 << res.grasp_candidates.grasp_poses.poses.size()
        //                 << " Candidate Grasps");
        return true;
}

int main(int argc, char **argv) {
        std::string topicObjectCloud, graspPosesTopic, boundariesTopic,
                    marker_array_topic;

        ros::init(argc, argv, "grasp_candidate_node");

        nh_ = new ros::NodeHandle("~");

        tf_listener = new tf::TransformListener();

        nh_->param("object_cloud_topic", topicObjectCloud,
                   std::string("/capsicum/points"));
        nh_->param("grasp_poses_topic", graspPosesTopic, std::string("/grasp_poses"));
        nh_->param("object_boundaries_topic", boundariesTopic,
                   std::string("/object_boundaries"));
        nh_->param("marker_array_topic", marker_array_topic,
                   std::string("/grasp_candidate_markers"));

        //      ros::Subscriber object_cloud_subscriber =
        //      nh_->subscribe(topicObjectCloud,
        //                                                               10,
        //    object_cloud_callback);

        ros::ServiceServer service = nh_->advertiseService(
                "/apc_grasping/cartesian_grasp_candidates", grasp_candidate_detection);

        //      ros::ServiceServer grasp_service =
        //          nh_.advertiseService("/apc_grasping/local_grasp_estimate",
        //          &grasp_estimation);

        grip_pose_pub = nh_->advertise<geometry_msgs::Pose>("/grip_poses", 0);

        grasp_pub = nh_->advertise<geometry_msgs::PoseArray>(graspPosesTopic, 0);

        boundaries_pub = nh_->advertise<sensor_msgs::PointCloud2>(boundariesTopic, 0);

        downsampled_pub =
                nh_->advertise<sensor_msgs::PointCloud2>("/object_downsampled", 0);

        input_pub =
                nh_->advertise<sensor_msgs::PointCloud2>("/grasp_input", 0);

        marker_array_pub =
                nh_->advertise<visualization_msgs::MarkerArray>(marker_array_topic, 0);

        marker_pub =
                nh_->advertise<visualization_msgs::Marker>("/marker_array/delete", 0);

        ros::spin();

        return 0;
}
