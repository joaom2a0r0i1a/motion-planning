#include "motion_planning_python/NBV/NBVMultiplanner.h"

NBVMultiPlanner::NBVMultiPlanner(const ros::NodeHandle& nh, const ros::NodeHandle& nh_private) : nh_(nh), nh_private_(nh_private), voxblox_server_(nh_, nh_private_) {

    //ns = "uav1";

    /* Parameter loading */
    mrs_lib::ParamLoader param_loader(nh_private_, "NBVMultiPlanner");

    // Namespace
    param_loader.loadParam("uav_namespace", ns);
    param_loader.loadParam("uav_id", uav_id);

    // Frames, Coordinates and Dimensions
    param_loader.loadParam("frame_id", frame_id);
    param_loader.loadParam("body/frame_id", body_frame_id);
    param_loader.loadParam("camera/frame_id", camera_frame_id);
    param_loader.loadParam("center/x", center_x);
    param_loader.loadParam("center/y", center_y);
    param_loader.loadParam("center/z", center_z);
    param_loader.loadParam("dimensions/x", dimensions_x);
    param_loader.loadParam("dimensions/y", dimensions_y);
    param_loader.loadParam("dimensions/z", dimensions_z);

    // Bounded Box
    param_loader.loadParam("bounded_box/min_x", min_x);
    param_loader.loadParam("bounded_box/max_x", max_x);
    param_loader.loadParam("bounded_box/min_y", min_y);
    param_loader.loadParam("bounded_box/max_y", max_y);
    param_loader.loadParam("bounded_box/min_z", min_z);
    param_loader.loadParam("bounded_box/max_z", max_z);
    param_loader.loadParam("bounded_box/planner_range", planner_range);

    // RRT Tree
    param_loader.loadParam("rrt/N_max", N_max);
    param_loader.loadParam("rrt/N_termination", N_termination);
    param_loader.loadParam("rrt/N_yaw_samples", num_yaw_samples);
    param_loader.loadParam("rrt/radius", radius);
    param_loader.loadParam("rrt/step_size", step_size);
    param_loader.loadParam("rrt/tolerance", tolerance);

    // Camera
    param_loader.loadParam("camera/h_fov", horizontal_fov);
    param_loader.loadParam("camera/width", resolution_x);
    param_loader.loadParam("camera/height", resolution_y);
    param_loader.loadParam("camera/min_distance", min_distance);
    param_loader.loadParam("camera/max_distance", max_distance);

    // Planner
    param_loader.loadParam("path/uav_radius", uav_radius);
    param_loader.loadParam("path/lambda", lambda);

    // Timer
    param_loader.loadParam("timer_main/rate", timer_main_rate);

    // Initialize UAV as state IDLE
    //changeState(STATE_IDLE);
    state_ = STATE_IDLE;
    iteration_ = 0;

    // Get vertical FoV and setup camera
    vertical_fov = segment_evaluator.getVerticalFoV(horizontal_fov, resolution_x, resolution_y);
    segment_evaluator.setCameraModelParametersFoV(horizontal_fov, vertical_fov, min_distance, max_distance);

    // Setup Voxblox
    tsdf_map_ = voxblox_server_.getTsdfMapPtr();
    esdf_map_ = voxblox_server_.getEsdfMapPtr();
    segment_evaluator.setTsdfLayer(tsdf_map_->getTsdfLayerPtr());
    segment_evaluator.setEsdfMap(esdf_map_);
            
    // Setup Tf Transformer
    transformer_ = std::make_unique<mrs_lib::Transformer>("NBVMultiPlanner");
    transformer_->setDefaultFrame(frame_id);
    transformer_->setDefaultPrefix(ns);
    transformer_->retryLookupNewest(true);

    set_variables = false;

    // Setup Collision Avoidance
    voxblox_server_.setTraversabilityRadius(uav_radius);
    voxblox_server_.publishTraversable();

    // Get Sampling Radius
    bounded_radius = sqrt(pow(min_x - max_x, 2.0) + pow(min_y - max_y, 2.0) + pow(min_z - max_z, 2.0));
    
    /* Publishers */
    pub_markers = nh_private_.advertise<visualization_msgs::Marker>("visualization_marker_out", 50);
    pub_reference = nh_private_.advertise<mrs_msgs::Reference>("reference_out", 1);
    pub_start = nh_private_.advertise<std_msgs::Bool>("simulation_ready", 1);
    pub_frustum = nh_private_.advertise<visualization_msgs::Marker>("frustum_out", 10);
    pub_voxels = nh_private_.advertise<visualization_msgs::MarkerArray>("unknown_voxels_out", 10);
    pub_initial_reference = nh_private_.advertise<mrs_msgs::ReferenceStamped>("initial_reference_out", 5);
    pub_evade = nh_.advertise<multiagent_collision_check::Segment>("evasion_segment_out", 100);

    /* Subscribers */
    mrs_lib::SubscribeHandlerOptions shopts;
    shopts.nh                 = nh_private_;
    shopts.node_name          = "NBVMultiPlanner";
    shopts.no_message_timeout = mrs_lib::no_timeout;
    shopts.threadsafe         = true;
    shopts.autostart          = true;
    shopts.queue_size         = 10;
    shopts.transport_hints    = ros::TransportHints().tcpNoDelay();

    sub_uav_state = mrs_lib::SubscribeHandler<mrs_msgs::UavState>(shopts, "uav_state_in", &NBVMultiPlanner::callbackUavState, this);
    sub_control_manager_diag = mrs_lib::SubscribeHandler<mrs_msgs::ControlManagerDiagnostics>(shopts, "control_manager_diag_in", &NBVMultiPlanner::callbackControlManagerDiag, this);
    sub_evade = mrs_lib::SubscribeHandler<multiagent_collision_check::Segment>(shopts, "evasion_segment_in", &NBVMultiPlanner::callbackEvade, this);
    //sub_control_manager_diag =  mrs_lib::SubscribeHandler<mrs_msgs::ControlManagerDiagnostics>(shopts, "control_manager_diag_in", ros::Duration(3.0), &NBVMultiPlanner::timeoutControlManagerDiag, this);
    sub_constraints = mrs_lib::SubscribeHandler<mrs_msgs::DynamicsConstraints>(shopts, "constraints_in");

    /* Service Servers */
    ss_start = nh_private_.advertiseService("start_in", &NBVMultiPlanner::callbackStart, this);
    ss_stop = nh_private_.advertiseService("stop_in", &NBVMultiPlanner::callbackStop, this);

    /* Service Clients */
    sc_trajectory_generation = mrs_lib::ServiceClientHandler<mrs_msgs::GetPathSrv>(nh_private_, "trajectory_generation_out");
    sc_trajectory_reference = mrs_lib::ServiceClientHandler<mrs_msgs::TrajectoryReferenceSrv>(nh_private_, "trajectory_reference_out");

    /* Timer */
    timer_main = nh_private_.createTimer(ros::Duration(1.0 / timer_main_rate), &NBVMultiPlanner::timerMain, this);

    is_initialized = true;
}

double NBVMultiPlanner::getMapDistance(const Eigen::Vector3d& position) const {
    if (!voxblox_server_.getEsdfMapPtr()) {
        return 0.0;
    }
    double distance = 0.0;
    if (!voxblox_server_.getEsdfMapPtr()->getDistanceAtPosition(position, &distance)) {
        return 0.0;
    }
    return distance;
}

bool NBVMultiPlanner::isPathCollisionFree(const std::vector<std::shared_ptr<rrt_star::Node>>& path) const {
    for (const std::shared_ptr<rrt_star::Node>& node : path) {
        if (getMapDistance(node->point.head(3)) < uav_radius) {
            return false;
        }
    }
    return true;
}

void NBVMultiPlanner::GetTransformation() {
    // From Body Frame to Camera Frame
    auto Message_C_B = transformer_->getTransform(body_frame_id, camera_frame_id, ros::Time(0));
    if (!Message_C_B) {
        ROS_ERROR_THROTTLE(1.0, "[NBVMultiPlanner]: could not get transform from body frame to the camera frame!");
        return;
    }

    T_C_B_message = Message_C_B.value();
    T_B_C_message = transformer_->inverse(T_C_B_message);

    // Transform into matrix
    tf::transformMsgToKindr(T_C_B_message.transform, &T_C_B);
    tf::transformMsgToKindr(T_B_C_message.transform, &T_B_C);
    segment_evaluator.setCameraExtrinsics(T_C_B);
}

void NBVMultiPlanner::NBV() {
    best_score_ = 0;
    std::shared_ptr<rrt_star::Node> best_node = nullptr;
    //std::shared_ptr<rrt_star::Node> root = std::make_shared<rrt_star::Node>(pose);
    //segment_evaluator.computeGainFromsampledYaw(root, num_yaw_samples, trajectory_point);

    // Multi-UAV remove previous planned agent path
    int k;
    for (k = 0; k < agentsId_.size(); k++) {
        if (agentsId_[k] == uav_id) {
            break;
        }
    }
    if (k < agentsId_.size()) {
        segments_[k]->clear();
    }

    std::shared_ptr<rrt_star::Node> root;
    if (prev_best_branch.size() > 1) {
        root = std::make_shared<rrt_star::Node>(prev_best_branch[1]);
    } else {
        root = std::make_shared<rrt_star::Node>(pose);
    }

    root->cost = 0;
    root->score = root->gain;

    if (root->score > best_score_) {
        best_score_ = root->score;
        best_node = root;
    }

    RRTStar.clearKDTree();
    RRTStar.addKDTreeNode(root);
    clearMarkers();

    bool isFirstIteration = true;
    int j = 1; // initialized at one because of the root node
    collision_id_counter_ = 0;
    if (prev_best_branch.size() > 0) {
        previous_root = std::make_shared<rrt_star::Node>(prev_best_branch[0]);
    }
    while (j < N_max || best_score_ == 0.0) {
        /*// Backtrack
        if (collision_id_counter_ > 1000 * j) {
            if (previous_root) {
                //next_best_node = previous_root;
                rotate();
                changeState(STATE_WAITING_INITIALIZE);
            } else {
                ROS_INFO("[NBVMultiPlanner]: Enough");
                collision_id_counter_ = 0;
                break;
            }
            return;
        }*/

        for (size_t i = 1; i < prev_best_branch.size(); ++i) {
            if (isFirstIteration) {
                isFirstIteration = false;
                continue; // Skip first iteration (root)
            }
            
            const Eigen::Vector4d& node_position = prev_best_branch[i];

            //double yaw;
            //RRTStar.computeYaw(bounded_radius, yaw);

            std::shared_ptr<rrt_star::Node> nearest_node_best;
            RRTStar.findNearestKD(node_position.head(3), nearest_node_best);
            
            std::shared_ptr<rrt_star::Node> new_node_best;
            new_node_best = std::make_shared<rrt_star::Node>(node_position);
            new_node_best->parent = nearest_node_best;

            //std::vector<std::shared_ptr<rrt_star::Node>> nearby_nodes_best;
            //RRTStar.findNearby(tree, new_node_best, radius, nearby_nodes_best);
            //RRTStar.chooseParent(new_node_best, nearby_nodes_best);

            /*eth_mav_msgs::EigenTrajectoryPoint::Vector trajectory_segment_best;

            trajectory_point.position_W.head(3) = new_node_best->parent->point.head(3);
            trajectory_point.setFromYaw(new_node_best->parent->point[3]);
            trajectory_segment_best.push_back(trajectory_point);

            trajectory_point.position_W.head(3) = new_node_best->point.head(3);
            trajectory_point.setFromYaw(new_node_best->point[3]);
            trajectory_segment_best.push_back(trajectory_point);

            bool success_collision_best = false;
            success_collision_best = isPathCollisionFree(trajectory_segment_best);

            if (!success_collision_best) {
                //clear_node();
                trajectory_segment_best.clear();
                break;
            }

            trajectory_segment_best.clear();*/
            visualize_node(new_node_best->point, ns);

            trajectory_point.position_W = new_node_best->point.head(3);
            trajectory_point.setFromYaw(new_node_best->point[3]);

            double result_best = segment_evaluator.computeGainFixedAngleAEP(trajectory_point);
            new_node_best->gain = result_best;

            //std::pair<double, double> result = segment_evaluator.computeGainAEP(trajectory_point);
            //new_node_best->gain = result.first;
            //new_node_best->point[3] = result.second;

            /*eth_mav_msgs::EigenTrajectoryPoint trajectory_point_gain;
            trajectory_point_gain.position_W = new_node_best->point.head(3);
            trajectory_point_gain.setFromYaw(new_node_best->point[3]);
            new_node_best->gain = segment_evaluator.computeGain(trajectory_point_gain);*/
            //segment_evaluator.computeGainFromsampledYaw(new_node_best, num_yaw_samples, trajectory_point);

            segment_evaluator.computeCost(new_node_best);
            segment_evaluator.computeScore(new_node_best, lambda);

            if (new_node_best->score > best_score_) {
                best_score_ = new_node_best->score;
                best_node = new_node_best;
            }

            ROS_INFO("[NBVMultiPlanner]: Best Score BB: %f", new_node_best->score);

            RRTStar.addKDTreeNode(new_node_best);
            //tree.push_back(new_node_best);
            visualize_edge(new_node_best, ns);

            ++j;
        }

        if (j >= N_max && best_score_ > 0) {
            break;
        }
    
        prev_best_branch.clear();

        Eigen::Vector4d rand_point_yaw;
        Eigen::Vector3d rand_point;
        RRTStar.computeSamplingDimensionsNBV(bounded_radius, rand_point_yaw);
        rand_point = rand_point_yaw.head(3);
        rand_point += root->point.head(3);

        std::shared_ptr<rrt_star::Node> nearest_node;
        //RRTStar.findNearest(tree, rand_point, nearest_node);
        RRTStar.findNearestKD(rand_point, nearest_node);

        std::shared_ptr<rrt_star::Node> new_node;
        RRTStar.steer_parent(nearest_node, rand_point, step_size, new_node);

        //std::shared_ptr<rrt_star::Node> new_node;
        if (new_node->point[0] > max_x || new_node->point[0] < min_x || new_node->point[1] < min_y || new_node->point[1] > max_y || new_node->point[2] < min_z || new_node->point[2] > max_z) {
            continue;
        }

        //visualize_node(new_node->point, ns);

        //std::cout << "New Point: " << new_node->point << std::endl;
        
        //new_node->parent = nearest_node;

        //std::vector<std::shared_ptr<rrt_star::Node>> nearby_nodes = RRTStar.findNearby(tree, new_node, radius);
        //new_node = RRTStar.chooseParent(new_node, nearby_nodes);

        // Collision Check
        std::vector<std::shared_ptr<rrt_star::Node>> trajectory_segment;
        //trajectory_segment.push_back(new_node->parent);
        trajectory_segment.push_back(new_node);

        bool success_collision = false;
        success_collision = isPathCollisionFree(trajectory_segment);

        if (!success_collision || multiagent::isInCollision(new_node->parent->point, new_node->point, uav_radius, segments_)) {
            //clear_node();
            trajectory_segment.clear();
            collision_id_counter_++;
            continue;
        }

        /*if (!success_collision) {
            //clear_node();
            trajectory_segment.clear();
            collision_id_counter_++;
            continue;
        }*/

        trajectory_segment.clear();
        visualize_node(new_node->point, ns);

        new_node->point[3] = rand_point_yaw[3];
        eth_mav_msgs::EigenTrajectoryPoint trajectory_point_gain;
        trajectory_point_gain.position_W = new_node->point.head(3);
        trajectory_point_gain.setFromYaw(new_node->point[3]);
        //new_node->gain = segment_evaluator.evaluateExplorationGainWithRaycasting(trajectory_point_gain);
        //ROS_INFO("[NBVMultiPlanner]: Best gain RayCast: %f", new_node->gain);
        double result = segment_evaluator.computeGainFixedAngleAEP(trajectory_point_gain);
        new_node->gain = result;
        //std::pair<double, double> result = segment_evaluator.computeGainAEP(trajectory_point_gain);
        //new_node->gain = result.first;
        //new_node->point[3] = result.second;
        //ROS_INFO("[NBVMultiPlanner]: Best gain AEP: %f", new_node->gain);
        //segment_evaluator.computeGainFromsampledYaw(new_node, num_yaw_samples, trajectory_point);
        //ROS_INFO("[NBVMultiPlanner]: Best gain Bircher Optimized: %f", new_node->gain);
        //new_node->gain = segment_evaluator.computeGain(trajectory_point_gain);
        //ROS_INFO("[NBVMultiPlanner]: Best gain Bircher: %f", new_node->gain);
        //segment_evaluator.computeGainFromsampledYaw(new_node, num_yaw_samples, trajectory_point);

        //ROS_INFO("[NBVMultiPlanner]: Best Gain: %f", new_node->gain);

        segment_evaluator.computeCost(new_node);
        segment_evaluator.computeScore(new_node, lambda);

        if (new_node->score > best_score_) {
            best_score_ = new_node->score;
            best_node = new_node;
        }

        //ROS_INFO("[NBVMultiPlanner]: Yaw: %f", new_node->point[3]);
        ROS_INFO("[NBVMultiPlanner]: Best Score: %f", new_node->score);

        //tree.push_back(new_node);
        RRTStar.addKDTreeNode(new_node);
        visualize_edge(new_node, ns);
        //RRTStar.rewire(tree, new_node, nearby_nodes, radius);

        if (j > N_termination) {
            ROS_INFO("[NBVMultiPlanner]: NBV Terminated");
            RRTStar.clearKDTree();
            best_branch.clear();
            clearMarkers();
            changeState(STATE_STOPPED);
            break;
        }

        ++j;

    }
    if (best_node) {
        next_best_node = best_node;
        RRTStar.backtrackPathNode(best_node, best_branch, next_best_node);
        visualize_path(best_node, ns);
        prev_best_branch = best_branch;
    }

}

double NBVMultiPlanner::distance(const mrs_msgs::Reference& waypoint, const geometry_msgs::Pose& pose) {

  return mrs_lib::geometry::dist(vec3_t(waypoint.position.x, waypoint.position.y, waypoint.position.z),
                                 vec3_t(pose.position.x, pose.position.y, pose.position.z));
}

void NBVMultiPlanner::initialize(mrs_msgs::ReferenceStamped initial_reference) {
    initial_reference.header.frame_id = ns + "/" + frame_id;
    initial_reference.header.stamp = ros::Time::now();

    initial_reference.reference.position.x = pose[0];
    initial_reference.reference.position.y = pose[1];
    initial_reference.reference.position.z = pose[2] + 3;
    initial_reference.reference.heading = pose[3];
    pub_initial_reference.publish(initial_reference);
    // Max horizontal speed is 1 m/s so we wait 2 second between points
    ros::Duration(3).sleep();

    for (double i = 0.0; i <= 2.0; i = i + 0.4) {
        initial_reference.reference.position.x = pose[0];
        initial_reference.reference.position.y = pose[1];
        initial_reference.reference.position.z = pose[2] + 3;
        initial_reference.reference.heading = pose[3] + M_PI * i;
        pub_initial_reference.publish(initial_reference);
        // Max yaw rate is 0.5 rad/s so we wait 0.4*M_PI seconds between points
        ros::Duration(0.8*M_PI).sleep();
    }

    ros::Duration(0.5).sleep();
}

void NBVMultiPlanner::rotate() {
    mrs_msgs::ReferenceStamped initial_reference;
    initial_reference.header.frame_id = ns + "/" + frame_id;
    initial_reference.header.stamp = ros::Time::now();

    // Rotate 360 degrees
    for (double i = 0.0; i <= 2.0; i = i + 0.4) {
        initial_reference.reference.position.x = pose[0];
        initial_reference.reference.position.y = pose[1];
        initial_reference.reference.position.z = pose[2];
        initial_reference.reference.heading = pose[3] + M_PI * i;
        pub_initial_reference.publish(initial_reference);
        // Max yaw rate is 0.5 rad/s so we wait 0.4*M_PI seconds between points
        ros::Duration(0.8*M_PI).sleep();
    }
}

bool NBVMultiPlanner::callbackStart(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res) {
    if (!is_initialized) {
        res.success = false;
        res.message = "not initialized";
        return true;
    }

    if (!ready_to_plan_) {
        std::stringstream ss;
        ss << "not ready to plan, missing data";

        ROS_ERROR_STREAM_THROTTLE(0.5, "[NBVMultiPlanner]: " << ss.str());

        res.success = false;
        res.message = ss.str();
        return true;
    }

    changeState(STATE_INITIALIZE);

    res.success = true;
    res.message = "starting";
    return true;

}

bool NBVMultiPlanner::callbackStop(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res) {
    if (!is_initialized) {
        res.success = false;
        res.message = "not initialized";
        return true;
    }

    if (!ready_to_plan_) {
        std::stringstream ss;
        ss << "not ready to plan, missing data";

        ROS_ERROR_STREAM_THROTTLE(0.5, "[NBVMultiPlanner]: " << ss.str());

        res.success = false;
        res.message = ss.str();
        return true;
    }
    changeState(STATE_STOPPED);

    std::stringstream ss;
    ss << "Stopping by request";

    res.success = true;
    res.message = ss.str();
    return true;

}

void NBVMultiPlanner::callbackControlManagerDiag(const mrs_msgs::ControlManagerDiagnostics::ConstPtr msg) {
    if (!is_initialized) {
        return;
    }
    ROS_INFO_ONCE("[NBVMultiPlanner]: getting ControlManager diagnostics");
    control_manager_diag = *msg;
}

void NBVMultiPlanner::callbackUavState(const mrs_msgs::UavState::ConstPtr msg) {
    if (!is_initialized) {
        return;
    }
    ROS_INFO_ONCE("[NBVMultiPlanner]: getting UavState diagnostics");
    geometry_msgs::Pose uav_state = msg->pose;
    double yaw = mrs_lib::getYaw(uav_state);
    pose = {uav_state.position.x, uav_state.position.y, uav_state.position.z, yaw};
}

void NBVMultiPlanner::callbackEvade(const multiagent_collision_check::Segment::ConstPtr msg) {
    if (!is_initialized) {
        return;
    }
    ROS_INFO_ONCE("[NBVMultiPlanner]: getting CollisionCheck diagnostics");

    int i;
    for(i = 0; i < agentsId_.size(); i++) {
        if(agentsId_[i] == msg->uav_id) {
            break;
        }
    }

    // If no match was found, add the uav_id to the list of UAV IDs
    if (i == agentsId_.size()) {
        agentsId_.push_back(msg->uav_id);
        segments_.push_back(new std::vector<Eigen::Vector3d>);
    }

    // Update the segment list with the poses from msg
    segments_[i]->clear();
    for(typename std::vector<geometry_msgs::Point>::const_iterator it = msg->uav_path.begin(); it != msg->uav_path.end(); ++it) {
        segments_[i]->push_back(Eigen::Vector3d(it->x, it->y, it->z));
    }    
}

void NBVMultiPlanner::timerMain(const ros::TimerEvent& event) {
    if (!is_initialized) {
        return;
    }

    /* prerequsities //{ */

    const bool got_control_manager_diag = sub_control_manager_diag.hasMsg() && (ros::Time::now() - sub_control_manager_diag.lastMsgTime()).toSec() < 2.0;
    const bool got_uav_state = sub_uav_state.hasMsg() && (ros::Time::now() - sub_uav_state.lastMsgTime()).toSec() < 2.0;

    if (!got_control_manager_diag || !got_uav_state) {
        ROS_INFO_THROTTLE(1.0, "[NBVMultiPlanner]: waiting for data: ControlManagerDiag = %s, UavState = %s", got_control_manager_diag ? "TRUE" : "FALSE", got_uav_state ? "TRUE" : "FALSE");
        return;
    } else {
        ready_to_plan_ = true;
    }

    std_msgs::Bool starter;
    starter.data = true;
    pub_start.publish(starter);

    ROS_INFO_ONCE("[NBVMultiPlanner]: main timer spinning");

    if (!set_variables) {
        GetTransformation();
        ROS_INFO("[NBVMultiPlanner]: T_C_B Translation: [%f, %f, %f]", T_C_B_message.transform.translation.x, T_C_B_message.transform.translation.y, T_C_B_message.transform.translation.z);
        ROS_INFO("[NBVMultiPlanner]: T_C_B Rotation: [%f, %f, %f, %f]", T_C_B_message.transform.rotation.x, T_C_B_message.transform.rotation.y, T_C_B_message.transform.rotation.z, T_C_B_message.transform.rotation.w);
        set_variables = true;
    }

    switch (state_) {
        case STATE_IDLE: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[NBVMultiPlanner]: tracker has goal");
            } else {
                ROS_INFO("[NBVMultiPlanner]: waiting for command");
            }
            break;
        }
        case STATE_WAITING_INITIALIZE: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[NBVMultiPlanner]: tracker has goal");
            } else {
                ROS_INFO("[NBVMultiPlanner]: waiting for command");
                changeState(STATE_PLANNING);
            }
            break;
        }
        case STATE_INITIALIZE: {
            mrs_msgs::ReferenceStamped initial_reference;
            initialize(initial_reference);
            changeState(STATE_WAITING_INITIALIZE);
            break;
        }
        case STATE_PLANNING: {
            NBV();
            clear_all_voxels();

            if (state_ != STATE_PLANNING) {
                break;
            }

            iteration_ += 1;

            current_waypoint_.position.x = next_best_node->point[0];
            current_waypoint_.position.y = next_best_node->point[1];
            current_waypoint_.position.z = next_best_node->point[2];
            current_waypoint_.heading = next_best_node->point[3];

            visualize_frustum(next_best_node);
            visualize_unknown_voxels(next_best_node);

            mrs_msgs::GetPathSrv srv_get_path;

            srv_get_path.request.path.header.frame_id = ns + "/" + frame_id;
            srv_get_path.request.path.header.stamp = ros::Time::now();
            srv_get_path.request.path.fly_now = false;
            srv_get_path.request.path.use_heading = true;

            mrs_msgs::Reference reference;

            // Add parent node so UAV goes to last incomplete node
            if (next_best_node->parent) {
                reference.position.x = next_best_node->parent->point[0];
                reference.position.y = next_best_node->parent->point[1];
                reference.position.z = next_best_node->parent->point[2];
                reference.heading = next_best_node->parent->point[3];
                srv_get_path.request.path.points.push_back(reference);
            }

            // Add Node
            reference.position.x = next_best_node->point[0];
            reference.position.y = next_best_node->point[1];
            reference.position.z = next_best_node->point[2];
            reference.heading = next_best_node->point[3];
            pub_reference.publish(reference);
            srv_get_path.request.path.points.push_back(reference);

            multiagent_collision_check::Segment segment;
            segment.uav_id = uav_id;
            for (int i = 0; i < srv_get_path.request.path.points.size(); ++i) {
                segment.uav_path.push_back(srv_get_path.request.path.points[i].position);
            }
            pub_evade.publish(segment);


            bool success = sc_trajectory_generation.call(srv_get_path);

            if (!success) {
                ROS_ERROR("[NBVMultiPlanner]: service call for trajectory failed");
                changeState(STATE_STOPPED);
                return;
            } else {
                if (!srv_get_path.response.success) {
                    ROS_ERROR("[NBVMultiPlanner]: service call for trajectory failed: '%s'", srv_get_path.response.message.c_str());
                    changeState(STATE_STOPPED);
                    return;
                }
            }

            mrs_msgs::TrajectoryReferenceSrv srv_trajectory_reference;
            srv_trajectory_reference.request.trajectory = srv_get_path.response.trajectory;
            srv_trajectory_reference.request.trajectory.fly_now = true;

            bool success_trajectory = sc_trajectory_reference.call(srv_trajectory_reference);

            if (!success_trajectory) {
                ROS_ERROR("[NBVMultiPlanner]: service call for trajectory reference failed");
                changeState(STATE_STOPPED);
                return;
            } else {
                if (!srv_trajectory_reference.response.success) {
                    ROS_ERROR("[NBVMultiPlanner]: service call for trajectory reference failed: '%s'", srv_trajectory_reference.response.message.c_str());
                    changeState(STATE_STOPPED);
                    return;
                }
            }

            best_branch.clear();
            ros::Duration(1).sleep();

            changeState(STATE_MOVING);
            break;
            
        }
        case STATE_MOVING: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[NBVMultiPlanner]: tracker has goal");
                mrs_msgs::UavState::ConstPtr uav_state_here = sub_uav_state.getMsg();
                geometry_msgs::Pose current_pose = uav_state_here->pose;
                double current_yaw = mrs_lib::getYaw(current_pose);

                double dist = distance(current_waypoint_, current_pose);
                double yaw_difference = fabs(atan2(sin(current_waypoint_.heading - current_yaw), cos(current_waypoint_.heading - current_yaw)));
                ROS_INFO("[NBVMultiPlanner]: Distance to waypoint: %.2f", dist);
                if (dist <= 0.6*step_size && yaw_difference <= 0.4*M_PI) {
                    changeState(STATE_PLANNING);
                }
            } else {
                ROS_INFO("[NBVMultiPlanner]: waiting for command");
                changeState(STATE_PLANNING);
            }
            break;
        }
        case STATE_STOPPED: {
            ROS_INFO_ONCE("[NBVMultiPlanner]: Total Iterations: %d", iteration_);
            ROS_INFO("[NBVMultiPlanner]: Shutting down.");
            ros::shutdown();
            return;
        }
        default: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[NBVMultiPlanner]: tracker has goal");
            } else {
                ROS_INFO("[NBVMultiPlanner]: waiting for command");
            }
            break;
        }
    }
}

void NBVMultiPlanner::changeState(const State_t new_state) {
    const State_t old_state = state_;

    if (old_state == STATE_STOPPED) {
        ROS_WARN("[NBVMultiPlanner]: Planning interrupted, not changing state.");
        return;
    }

    ROS_INFO("[NBVMultiPlanner]: changing state '%s' -> '%s'", _state_names_[old_state].c_str(), _state_names_[new_state].c_str());

    state_ = new_state;
}

void NBVMultiPlanner::visualize_node(const Eigen::Vector4d& pos, const std::string& ns) {
    visualization_msgs::Marker n;
    n.header.stamp = ros::Time::now();
    n.header.seq = node_id_counter_;
    n.header.frame_id = ns + "/" + frame_id;
    n.id = node_id_counter_;
    n.ns = "nodes";
    n.type = visualization_msgs::Marker::SPHERE;
    n.action = visualization_msgs::Marker::ADD;
    n.pose.position.x = pos[0];
    n.pose.position.y = pos[1];
    n.pose.position.z = pos[2];

    n.pose.orientation.x = 1;
    n.pose.orientation.y = 0;
    n.pose.orientation.z = 0;
    n.pose.orientation.w = 0;

    n.scale.x = 0.2;
    n.scale.y = 0.2;
    n.scale.z = 0.2;

    n.color.r = 0.4;
    n.color.g = 0.7;
    n.color.b = 0.2;
    n.color.a = 1;

    node_id_counter_++;

    n.lifetime = ros::Duration(30.0);
    n.frame_locked = false;
    pub_markers.publish(n);
}

void NBVMultiPlanner::visualize_edge(const std::shared_ptr<rrt_star::Node> node, const std::string& ns) {
    visualization_msgs::Marker e;
    e.header.stamp = ros::Time::now();
    e.header.seq = edge_id_counter_;
    e.header.frame_id = ns + "/" + frame_id;
    e.id = edge_id_counter_;
    e.ns = "tree_branches";
    e.type = visualization_msgs::Marker::ARROW;
    e.action = visualization_msgs::Marker::ADD;
    e.pose.position.x = node->parent->point[0];
    e.pose.position.y = node->parent->point[1];
    e.pose.position.z = node->parent->point[2];

    Eigen::Quaternion<double> q;
    Eigen::Vector3d init(1.0, 0.0, 0.0);
    Eigen::Vector3d dir(node->point[0] - node->parent->point[0],
                        node->point[1] - node->parent->point[1],
                        node->point[2] - node->parent->point[2]);
    q.setFromTwoVectors(init, dir);
    q.normalize();

    e.pose.orientation.x = q.x();
    e.pose.orientation.y = q.y();
    e.pose.orientation.z = q.z();
    e.pose.orientation.w = q.w();

    e.scale.x = dir.norm();
    e.scale.y = 0.05;
    e.scale.z = 0.05;

    e.color.r = 1.0;
    e.color.g = 0.3;
    e.color.b = 0.7;
    e.color.a = 1.0;

    edge_id_counter_++;

    e.lifetime = ros::Duration(30.0);
    e.frame_locked = false;
    pub_markers.publish(e);
}

void NBVMultiPlanner::visualize_path(const std::shared_ptr<rrt_star::Node> node, const std::string& ns) {
    std::shared_ptr<rrt_star::Node> current = node;
    
    while (current->parent) {
        visualization_msgs::Marker p;
        p.header.stamp = ros::Time::now();
        p.header.seq = path_id_counter_;
        p.header.frame_id = ns + "/" + frame_id;
        p.id = path_id_counter_;
        p.ns = "path";
        p.type = visualization_msgs::Marker::ARROW;
        p.action = visualization_msgs::Marker::ADD;
        p.pose.position.x = current->parent->point[0];
        p.pose.position.y = current->parent->point[1];
        p.pose.position.z = current->parent->point[2];

        Eigen::Quaternion<double> q;
        Eigen::Vector3d init(1.0, 0.0, 0.0);
        Eigen::Vector3d dir(current->point[0] - current->parent->point[0],
                            current->point[1] - current->parent->point[1],
                            current->point[2] - current->parent->point[2]);
        q.setFromTwoVectors(init, dir);
        q.normalize();
        p.pose.orientation.x = q.x();
        p.pose.orientation.y = q.y();
        p.pose.orientation.z = q.z();
        p.pose.orientation.w = q.w();

        p.scale.x = dir.norm();
        p.scale.y = 0.07;
        p.scale.z = 0.07;

        p.color.r = 0.7;
        p.color.g = 0.7;
        p.color.b = 0.3;
        p.color.a = 1.0;

        p.lifetime = ros::Duration(100.0);
        p.frame_locked = false;
        pub_markers.publish(p);

        current = current->parent;
        path_id_counter_++;
    }
}

void NBVMultiPlanner::visualize_frustum(std::shared_ptr<rrt_star::Node> position) {
    eth_mav_msgs::EigenTrajectoryPoint trajectory_point_visualize;
    trajectory_point_visualize.position_W = position->point.head(3);
    trajectory_point_visualize.setFromYaw(position->point[3]);
    
    visualization_msgs::Marker frustum;
    frustum.header.frame_id = ns + "/" + frame_id;
    frustum.header.stamp = ros::Time::now();
    frustum.ns = "camera_frustum";
    frustum.id = 0;
    frustum.type = visualization_msgs::Marker::LINE_LIST;
    frustum.action = visualization_msgs::Marker::ADD;

    // Line width
    frustum.scale.x = 0.02;

    frustum.color.a = 1.0;
    frustum.color.r = 1.0;
    frustum.color.g = 0.0;
    frustum.color.b = 0.0;

    std::vector<geometry_msgs::Point> points;
    segment_evaluator.visualize_frustum(trajectory_point_visualize, points);

    frustum.points = points;
    frustum.lifetime = ros::Duration(10.0);
    pub_frustum.publish(frustum);
}

void NBVMultiPlanner::visualize_unknown_voxels(std::shared_ptr<rrt_star::Node> position) {
    eth_mav_msgs::EigenTrajectoryPoint trajectory_point_visualize;
    trajectory_point_visualize.position_W = position->point.head(3);
    trajectory_point_visualize.setFromYaw(position->point[3]);

    voxblox::Pointcloud voxel_points;
    segment_evaluator.visualizeGainAEP(trajectory_point_visualize, voxel_points);
    
    visualization_msgs::MarkerArray voxels_marker;
    for (size_t i = 0; i < voxel_points.size(); ++i) {
        visualization_msgs::Marker unknown_voxel;
        unknown_voxel.header.frame_id = ns + "/" + frame_id;
        unknown_voxel.header.stamp = ros::Time::now();
        unknown_voxel.ns = "unknown_voxels";
        unknown_voxel.id = i;
        unknown_voxel.type = visualization_msgs::Marker::CUBE;
        unknown_voxel.action = visualization_msgs::Marker::ADD;

        // Voxel size
        unknown_voxel.scale.x = 0.2;
        unknown_voxel.scale.y = 0.2;
        unknown_voxel.scale.z = 0.2;

        unknown_voxel.color.a = 0.5;
        unknown_voxel.color.r = 0.0;
        unknown_voxel.color.g = 1.0;
        unknown_voxel.color.b = 0.0;

        unknown_voxel.pose.position.x = voxel_points[i].x();
        unknown_voxel.pose.position.y = voxel_points[i].y();
        unknown_voxel.pose.position.z = voxel_points[i].z();
        unknown_voxel.lifetime = ros::Duration(3.0);
        voxels_marker.markers.push_back(unknown_voxel);
    }
    pub_voxels.publish(voxels_marker);
}

void NBVMultiPlanner::clear_node() {
    visualization_msgs::Marker clear_node;
    clear_node.header.stamp = ros::Time::now();
    clear_node.ns = "nodes";
    clear_node.id = node_id_counter_;
    clear_node.action = visualization_msgs::Marker::DELETE;
    node_id_counter_--;
    pub_markers.publish(clear_node);
}

void NBVMultiPlanner::clear_all_voxels() {
    visualization_msgs::Marker clear_voxels;
    clear_voxels.header.stamp = ros::Time::now();
    clear_voxels.ns = "unknown_voxels";
    clear_voxels.action = visualization_msgs::Marker::DELETEALL;
    pub_voxels.publish(clear_voxels);
}

void NBVMultiPlanner::clearMarkers() {
    // Clear nodes
    visualization_msgs::Marker clear_nodes;
    clear_nodes.header.stamp = ros::Time::now();
    clear_nodes.ns = "nodes";
    clear_nodes.action = visualization_msgs::Marker::DELETEALL;
    pub_markers.publish(clear_nodes);

    // Clear edges
    visualization_msgs::Marker clear_edges;
    clear_edges.header.stamp = ros::Time::now();
    clear_edges.ns = "tree_branches";
    clear_edges.action = visualization_msgs::Marker::DELETEALL;
    pub_markers.publish(clear_edges);

    // Clear path
    visualization_msgs::Marker clear_path;
    clear_path.header.stamp = ros::Time::now();
    clear_path.ns = "path";
    clear_path.action = visualization_msgs::Marker::DELETEALL;
    pub_markers.publish(clear_path);

    // Reset marker ID counters
    node_id_counter_ = 0;
    edge_id_counter_ = 0;
    path_id_counter_ = 0;
}
