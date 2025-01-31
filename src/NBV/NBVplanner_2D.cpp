#include "motion_planning_python/NBV/NBVplanner_2D.h"

NBVPlanner::NBVPlanner(const ros::NodeHandle& nh, const ros::NodeHandle& nh_private) : nh_(nh), nh_private_(nh_private), voxblox_server_(nh_, nh_private_), lower_bound_(Eigen::Vector3d::Zero()),
    upper_bound_(Eigen::Vector3d::Zero()) {

    ns = "uav1";

    /* Parameter loading */
    mrs_lib::ParamLoader param_loader(nh_private_, "planner");

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
    changeState(STATE_IDLE);
    iteration_ = 0;

    // Get vertical FoV and setup camera
    vertical_fov = segment_evaluator.getVerticalFoV(horizontal_fov, resolution_x, resolution_y);
    segment_evaluator.setCameraModelParametersFoV(horizontal_fov, vertical_fov, min_distance, max_distance);

    // Setup Voxblox
    tsdf_map_ = voxblox_server_.getTsdfMapPtr();
    esdf_map_ = voxblox_server_.getEsdfMapPtr();
    segment_evaluator.setTsdfLayer(tsdf_map_->getTsdfLayerPtr());
            
    // Setup Tf Transformer
    transformer_ = std::make_unique<mrs_lib::Transformer>("planner");
    transformer_->setDefaultFrame(frame_id);
    transformer_->setDefaultPrefix(ns);
    transformer_->retryLookupNewest(true);

    set_variables = false;

    // Setup Collision Avoidance
    voxblox_server_.setTraversabilityRadius(uav_radius);
    voxblox_server_.publishTraversable();

    // Get Sampling Radius
    bounded_radius = sqrt(pow(min_x - max_x, 2.0) + pow(min_y - max_y, 2.0));
    
    /* Publishers */
    pub_markers = nh_private_.advertise<visualization_msgs::Marker>("visualization_marker_out", 50);
    pub_start = nh_private_.advertise<std_msgs::Bool>("simulation_ready", 1);

    /* Subscribers */
    mrs_lib::SubscribeHandlerOptions shopts;
    shopts.nh                 = nh_private_;
    shopts.node_name          = "planner";
    shopts.no_message_timeout = mrs_lib::no_timeout;
    shopts.threadsafe         = true;
    shopts.autostart          = true;
    shopts.queue_size         = 10;
    shopts.transport_hints    = ros::TransportHints().tcpNoDelay();

    sub_uav_state = mrs_lib::SubscribeHandler<mrs_msgs::UavState>(shopts, "uav_state_in", &NBVPlanner::callbackUavState, this);
    sub_control_manager_diag = mrs_lib::SubscribeHandler<mrs_msgs::ControlManagerDiagnostics>(shopts, "control_manager_diag_in", &NBVPlanner::callbackControlManagerDiagnostics, this);
    sub_constraints = mrs_lib::SubscribeHandler<mrs_msgs::DynamicsConstraints>(shopts, "constraints_in");

    /* Service Servers */
    ss_start = nh_private_.advertiseService("start_in", &NBVPlanner::callbackStart, this);
    ss_stop = nh_private_.advertiseService("stop_in", &NBVPlanner::callbackStop, this);
    ss_goto = nh_private_.advertiseService("goto_in", &NBVPlanner::callbackGoto, this);

    /* Service Clients */
    sc_trajectory_generation = mrs_lib::ServiceClientHandler<mrs_msgs::GetPathSrv>(nh_private_, "trajectory_generation_out");
    sc_trajectory_reference = mrs_lib::ServiceClientHandler<mrs_msgs::TrajectoryReferenceSrv>(nh_private_, "trajectory_reference_out");

    /* Timer */
    timer_main = nh_private_.createTimer(ros::Duration(1.0 / timer_main_rate), &NBVPlanner::timerMain, this);

    is_initialized = true;
}

double NBVPlanner::getMapDistance(const Eigen::Vector3d& position) const {
    if (!voxblox_server_.getEsdfMapPtr()) {
        return 0.0;
    }
    double distance = 0.0;
    if (!voxblox_server_.getEsdfMapPtr()->getDistanceAtPosition(position, &distance)) {
        return 0.0;
    }
    return distance;
}

bool NBVPlanner::isPathCollisionFree(const eth_mav_msgs::EigenTrajectoryPointVector& path) const {
    for (const eth_mav_msgs::EigenTrajectoryPoint& point : path) {
        if (getMapDistance(point.position_W) < uav_radius) {
            return false;
        }
    }
    return true;
}

void NBVPlanner::GetTransformation() {
    // From Body Frame to Camera Frame
    auto Message_C_B = transformer_->getTransform(body_frame_id, camera_frame_id, ros::Time(0));
    if (!Message_C_B) {
        ROS_ERROR_THROTTLE(1.0, "[planner]: could not get transform from body frame to the camera frame!");
        return;
    }

    T_C_B_message = Message_C_B.value();
    T_B_C_message = transformer_->inverse(T_C_B_message);

    // Transform into matrix
    tf::transformMsgToKindr(T_C_B_message.transform, &T_C_B);
    tf::transformMsgToKindr(T_B_C_message.transform, &T_B_C);
    segment_evaluator.setCameraExtrinsics(T_C_B);
}

void NBVPlanner::NBV() {

    std::shared_ptr<rrt_star_2d::Node> root = std::make_shared<rrt_star_2d::Node>(pose);
    tree.push_back(root);
    clearMarkers();

    bool isFirstIteration = true;
    best_gain_ = 0;

    int j = 1; // initialized at one because of the root node
    collision_id_counter_ = 0;
    while (j < N_max || best_gain_ == 0) {
        for (size_t i = 1; i < prev_best_branch.size(); ++i) {
            if (isFirstIteration) {
                isFirstIteration = false;
                continue; // Skip first iteration (root)
            }
            
            const auto& node_position = prev_best_branch[i];

            std::shared_ptr<rrt_star_2d::Node> nearest_node_best;
            rrt_star_2d::findNearest(tree, {prev_best_branch[i][0], prev_best_branch[i][1], prev_best_branch[i][2]}, nearest_node_best);
            
            std::shared_ptr<rrt_star_2d::Node> new_node_best;
            new_node_best = std::make_shared<rrt_star_2d::Node>(node_position);
            new_node_best->parent = nearest_node_best;

            visualize_node(new_node_best->point, ns);

            segment_evaluator.computeGainFromsampledYaw(new_node_best, num_yaw_samples, trajectory_point);

            if (new_node_best->gain > best_gain_) {
                best_gain_ = new_node_best->gain;
            }

            segment_evaluator.computeCost(new_node_best);
            segment_evaluator.computeScore(new_node_best, lambda);

            tree.push_back(new_node_best);
            visualize_edge(new_node_best, ns);

            ++j;

        }

        if (j >= N_max && best_gain_ > 0) {
            break;
        }
        
        prev_best_branch.clear();

        Eigen::Vector3d rand_point;
        rrt_star_2d::computeSamplingDimensions(bounded_radius, rand_point);
        rand_point += root->point.head(2);
        rand_point[2] = center_z;

        std::shared_ptr<rrt_star_2d::Node> nearest_node;
        rrt_star_2d::findNearest(tree, rand_point, nearest_node);
        std::shared_ptr<rrt_star_2d::Node> new_node;
        rrt_star_2d::steer_parent(nearest_node, rand_point, step_size, new_node);

        if (new_node->point[0] > 14 || new_node->point[0] < -14 || new_node->point[1] < -9.0 || new_node->point[1] > 9.0) {
            continue;
        }

        visualize_node(new_node->point, ns);
        collision_id_counter_ = 0;

        segment_evaluator.computeGainFromsampledYaw(new_node, num_yaw_samples, trajectory_point);

        if (new_node->gain > best_gain_) {
            best_gain_ = new_node->gain;
        }

        segment_evaluator.computeCost(new_node);
        segment_evaluator.computeScore(new_node, lambda);

        ROS_INFO("[planner]: Best Score: %f", new_node->score);

        tree.push_back(new_node);
        visualize_edge(new_node, ns);

        if (j > N_termination) {
            ROS_INFO("[planner]: NBV Terminated");
            changeState(STATE_STOPPED);
            break;
        }

        ++j;

    }

    double max_score = -std::numeric_limits<double>::infinity();
    std::shared_ptr<rrt_star_2d::Node> best_node = nullptr;
    for (const auto& node : tree) {
        if (node->score > max_score) {
            max_score = node->score;
            best_node = node;
        }
    }
    if (best_node) {
        next_best_node = best_node;
        rrt_star_2d::backtrackPathNode(best_node, best_branch, next_best_node);
        visualize_path(best_node, ns);
        prev_best_branch = best_branch;
    }

}

bool NBVPlanner::callbackStart(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res) {
    if (!is_initialized) {
        res.success = false;
        res.message = "not initialized";
        return true;
    }

    if (!ready_to_plan_) {
        std::stringstream ss;
        ss << "not ready to plan, missing data";

        ROS_ERROR_STREAM_THROTTLE(0.5, "[planner]: " << ss.str());

        res.success = false;
        res.message = ss.str();
        return true;
    }

    interrupted_ = false;
    changeState(STATE_PLANNING);

    res.success = true;
    res.message = "starting";
    return true;

}

bool NBVPlanner::callbackGoto(mrs_msgs::Vec4::Request& req, mrs_msgs::Vec4::Response& res) {
    if (!is_initialized) {
        res.success = false;
        res.message = "not initialized";
        return true;
    }

    if (!ready_to_plan_) {
        std::stringstream ss;
        ss << "not ready to plan, missing data";

        ROS_ERROR_STREAM_THROTTLE(0.5, "[planner]: " << ss.str());

        res.success = false;
        res.message = ss.str();
        return true;
    }

    mrs_msgs::GetPathSrv srv_get_path_goto;

    srv_get_path_goto.request.path.header.frame_id = "uav1/" + frame_id;
    srv_get_path_goto.request.path.header.stamp = ros::Time::now();
    srv_get_path_goto.request.path.fly_now = false;
    srv_get_path_goto.request.path.use_heading = true;

    mrs_msgs::Reference reference_goto;

    reference_goto.position.x = req.goal[0];
    reference_goto.position.y = req.goal[1];
    reference_goto.position.z = req.goal[2];
    reference_goto.heading    = req.goal[3];
    srv_get_path_goto.request.path.points.push_back(reference_goto);

    bool success_goto = sc_trajectory_generation.call(srv_get_path_goto);

    if (!success_goto) {
        ROS_ERROR("[planner]: service call for trajectory failed");
        changeState(STATE_STOPPED);
        return true;
    } else {
        if (!srv_get_path_goto.response.success) {
            ROS_ERROR("[planner]: service call for trajectory failed: '%s'", srv_get_path_goto.response.message.c_str());
            changeState(STATE_STOPPED);
            return true;
        }
    }

    mrs_msgs::TrajectoryReferenceSrv srv_trajectory_reference_goto;
    srv_trajectory_reference_goto.request.trajectory = srv_get_path_goto.response.trajectory;
    srv_trajectory_reference_goto.request.trajectory.fly_now = true;

    bool success_trajectory = sc_trajectory_reference.call(srv_trajectory_reference_goto);

    if (!success_trajectory) {
        ROS_ERROR("[planner]: service call for trajectory reference failed");
        changeState(STATE_STOPPED);
        return true;
    } else {
        if (!srv_trajectory_reference_goto.response.success) {
            ROS_ERROR("[planner]: service call for trajectory reference failed: '%s'", srv_trajectory_reference_goto.response.message.c_str());
            changeState(STATE_STOPPED);
            return true;
        }
    }

    res.success = true;
    res.message = "Goal Set";
    return true;
}

bool NBVPlanner::callbackStop(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res) {
    if (!is_initialized) {
        res.success = false;
        res.message = "not initialized";
        return true;
    }

    if (!ready_to_plan_) {
        std::stringstream ss;
        ss << "not ready to plan, missing data";

        ROS_ERROR_STREAM_THROTTLE(0.5, "[planner]: " << ss.str());

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

void NBVPlanner::callbackControlManagerDiagnostics(const mrs_msgs::ControlManagerDiagnostics::ConstPtr msg) {
    if (!is_initialized) {
        return;
    }
    ROS_INFO_ONCE("[planner]: getting ControlManager diagnostics");
    control_manager_diag = *msg;
}

void NBVPlanner::callbackUavState(const mrs_msgs::UavState::ConstPtr msg) {
    if (!is_initialized) {
        return;
    }
    ROS_INFO_ONCE("[planner]: getting UavState diagnostics");
    uav_state = *msg;
    pose = {uav_state.pose.position.x, uav_state.pose.position.y, uav_state.pose.position.z, -3.0};
}

void NBVPlanner::timerMain(const ros::TimerEvent& event) {
    if (!is_initialized) {
        return;
    }

    /* prerequsities //{ */

    const bool got_control_manager_diag = sub_control_manager_diag.hasMsg() && (ros::Time::now() - sub_control_manager_diag.lastMsgTime()).toSec() < 2.0;
    const bool got_uav_state = sub_uav_state.hasMsg() && (ros::Time::now() - sub_uav_state.lastMsgTime()).toSec() < 2.0;

    if (!got_control_manager_diag || !got_uav_state) {
        ROS_INFO_THROTTLE(1.0, "[planner]: waiting for data: ControlManager diag = %s, UavState = %s", got_control_manager_diag ? "TRUE" : "FALSE", got_uav_state ? "TRUE" : "FALSE");
        return;
    } else {
        ready_to_plan_ = true;
    }

    std_msgs::Bool starter;
    starter.data = true;
    pub_start.publish(starter);

    ROS_INFO_ONCE("[planner]: main timer spinning");

    if (!set_variables) {
        GetTransformation();
        ROS_INFO("[planner]: T_C_B Translation: [%f, %f, %f]", T_C_B_message.transform.translation.x, T_C_B_message.transform.translation.y, T_C_B_message.transform.translation.z);
        ROS_INFO("[planner]: T_C_B Rotation: [%f, %f, %f, %f]", T_C_B_message.transform.rotation.x, T_C_B_message.transform.rotation.y, T_C_B_message.transform.rotation.z, T_C_B_message.transform.rotation.w);
        set_variables = true;
    }
    
    switch (state_) {
        case STATE_IDLE: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[planner]: tracker has goal");
            } else {
                ROS_INFO("[planner]: waiting for command");
            }
            break;
        }
        case STATE_STOPPED: {break;}
        case STATE_PLANNING: {
            NBV();

            if (state_ == STATE_STOPPED) {
                break;
            }

            iteration_ += 1;

            mrs_msgs::GetPathSrv srv_get_path;

            srv_get_path.request.path.header.frame_id = "uav1/" + frame_id;
            srv_get_path.request.path.header.stamp = ros::Time::now();
            srv_get_path.request.path.fly_now = false;
            srv_get_path.request.path.use_heading = true;

            mrs_msgs::Reference reference;

            reference.position.x = next_best_node->point[0];
            reference.position.y = next_best_node->point[1];
            reference.position.z = center_z;
            reference.heading = next_best_node->point[3];
            srv_get_path.request.path.points.push_back(reference);

            bool success = sc_trajectory_generation.call(srv_get_path);

            if (!success) {
                ROS_ERROR("[planner]: service call for trajectory failed");
                changeState(STATE_STOPPED);
                return;
            } else {
                if (!srv_get_path.response.success) {
                    ROS_ERROR("[planner]: service call for trajectory failed: '%s'", srv_get_path.response.message.c_str());
                    changeState(STATE_STOPPED);
                    return;
                }
            }

            mrs_msgs::TrajectoryReferenceSrv srv_trajectory_reference;
            srv_trajectory_reference.request.trajectory = srv_get_path.response.trajectory;
            srv_trajectory_reference.request.trajectory.fly_now = true;

            bool success_trajectory = sc_trajectory_reference.call(srv_trajectory_reference);

            if (!success_trajectory) {
                ROS_ERROR("[planner]: service call for trajectory reference failed");
                changeState(STATE_STOPPED);
                return;
            } else {
                if (!srv_trajectory_reference.response.success) {
                    ROS_ERROR("[planner]: service call for trajectory reference failed: '%s'", srv_trajectory_reference.response.message.c_str());
                    changeState(STATE_STOPPED);
                    return;
                }
            }

            tree.clear();
            best_branch.clear();
            ros::Duration(1).sleep();

            changeState(STATE_MOVING);
            break;
            
        }
        case STATE_MOVING: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[planner]: tracker has goal");
            } else {
                ROS_INFO("[planner]: waiting for command");
                changeState(STATE_PLANNING);
            }
            break;
        }

        default: {
            if (control_manager_diag.tracker_status.have_goal) {
                ROS_INFO("[planner]: tracker has goal");
            } else {
                ROS_INFO("[planner]: waiting for command");
            }
            break;
        }
    }
}

void NBVPlanner::changeState(const State_t new_state) {
    const State_t old_state = state_;

    if (interrupted_ && old_state == STATE_STOPPED) {
        ROS_WARN("[planner]: Planning interrupted, not changing state.");
        return;
    }

    switch (new_state) {

        case STATE_PLANNING: {

            if (old_state == STATE_STOPPED) {
                replanning_counter_ = 0;
            }
        }

        default: {break;}
    }

    ROS_INFO("[planner]: changing state '%s' -> '%s'", _state_names_[old_state].c_str(), _state_names_[new_state].c_str());

    state_ = new_state;
}

void NBVPlanner::visualize_node(const Eigen::Vector4d& pos, const std::string& ns) {
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
    n.pose.position.z = center_z;

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

void NBVPlanner::visualize_edge(const std::shared_ptr<rrt_star_2d::Node> node, const std::string& ns) {
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
    e.pose.position.z = center_z;

    Eigen::Quaternion<double> q;
    Eigen::Vector3d init(1.0, 0.0, 0.0);
    Eigen::Vector3d dir(node->point[0] - node->parent->point[0],
                        node->point[1] - node->parent->point[1],
                        0);
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

void NBVPlanner::visualize_path(const std::shared_ptr<rrt_star_2d::Node> node, const std::string& ns) {
    std::shared_ptr<rrt_star_2d::Node> current = node;
    
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
        p.pose.position.z = center_z;

        Eigen::Quaternion<double> q;
        Eigen::Vector3d init(1.0, 0.0, 0.0);
        Eigen::Vector3d dir(current->point[0] - current->parent->point[0],
                            current->point[1] - current->parent->point[1],
                            0);
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

void NBVPlanner::clear_node() {
    visualization_msgs::Marker clear_node;
    clear_node.header.stamp = ros::Time::now();
    clear_node.ns = "nodes";
    clear_node.id = node_id_counter_;
    clear_node.action = visualization_msgs::Marker::DELETE;
    node_id_counter_--;
    pub_markers.publish(clear_node);
}

void NBVPlanner::clearMarkers() {
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
