// Copyright 2020 TierIV
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * Copyright 2018-2019 Autoware Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "lidar_apollo_instance_segmentation/cluster2d.hpp"

#include <autoware_auto_perception_msgs/msg/detected_object_kinematics.hpp>
#include <autoware_auto_perception_msgs/msg/object_classification.hpp>

#include <pcl_conversions/pcl_conversions.h>
#ifdef USE_TF2_GEOMETRY_MSGS_DEPRECATED_HEADER
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#else
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#endif

geometry_msgs::msg::Quaternion getQuaternionFromRPY(const double r, const double p, const double y)
{
  tf2::Quaternion q;
  q.setRPY(r, p, y);
  return tf2::toMsg(q);
}

Cluster2D::Cluster2D(const int rows, const int cols, const float range)
{
  rows_ = rows;
  cols_ = cols;
  siz_ = rows * cols;
  range_ = range;
  scale_ = 0.5 * static_cast<float>(rows_) / range_;
  inv_res_x_ = 0.5 * static_cast<float>(cols_) / range_;
  inv_res_y_ = 0.5 * static_cast<float>(rows_) / range_;
  point2grid_.clear();
  id_img_.assign(siz_, -1);
  pc_ptr_.reset();
  valid_indices_in_pc_ = nullptr;
}

void Cluster2D::traverse(Node * x)
{
  std::vector<Node *> p;
  p.clear();

  while (x->traversed == 0) {
    p.push_back(x);
    x->traversed = 2;
    x = x->center_node;
  }
  if (x->traversed == 2) {
    for (int i = static_cast<int>(p.size()) - 1; i >= 0 && p[i] != x; i--) {
      p[i]->is_center = true;
    }
    x->is_center = true;
  }
  for (size_t i = 0; i < p.size(); i++) {
    Node * y = p[i];
    y->traversed = 1;
    y->parent = x->parent;
  }
}

void Cluster2D::cluster(
  const std::shared_ptr<float> & inferred_data, const pcl::PointCloud<pcl::PointXYZI>::Ptr & pc_ptr,
  const pcl::PointIndices & valid_indices, float objectness_thresh,
  bool use_all_grids_for_clustering)
{
  const float * category_pt_data = inferred_data.get();
  const float * instance_pt_x_data = inferred_data.get() + siz_;
  const float * instance_pt_y_data = inferred_data.get() + siz_ * 2;

  pc_ptr_ = pc_ptr;

  std::vector<std::vector<Node>> nodes(rows_, std::vector<Node>(cols_, Node()));

  valid_indices_in_pc_ = &(valid_indices.indices);
  point2grid_.assign(valid_indices_in_pc_->size(), -1);

  for (size_t i = 0; i < valid_indices_in_pc_->size(); ++i) {
    int point_id = valid_indices_in_pc_->at(i);
    const auto & point = pc_ptr_->points[point_id];
    // * the coordinates of x and y have been exchanged in feature generation
    // step,
    // so we swap them back here.
    int pos_x = F2I(point.y, range_, inv_res_x_);  // col
    int pos_y = F2I(point.x, range_, inv_res_y_);  // row
    if (IsValidRowCol(pos_y, pos_x)) {
      point2grid_[i] = RowCol2Grid(pos_y, pos_x);
      nodes[pos_y][pos_x].point_num++;
    }
  }

  for (int row = 0; row < rows_; ++row) {
    for (int col = 0; col < cols_; ++col) {
      int grid = RowCol2Grid(row, col);
      Node * node = &nodes[row][col];
      DisjointSetMakeSet(node);
      node->is_object = (use_all_grids_for_clustering || nodes[row][col].point_num > 0) &&
                        (*(category_pt_data + grid) >= objectness_thresh);
      int center_row = std::round(row + instance_pt_x_data[grid] * scale_);
      int center_col = std::round(col + instance_pt_y_data[grid] * scale_);
      center_row = std::min(std::max(center_row, 0), rows_ - 1);
      center_col = std::min(std::max(center_col, 0), cols_ - 1);
      node->center_node = &nodes[center_row][center_col];
    }
  }

  for (int row = 0; row < rows_; ++row) {
    for (int col = 0; col < cols_; ++col) {
      Node * node = &nodes[row][col];
      if (node->is_object && node->traversed == 0) {
        traverse(node);
      }
    }
  }

  for (int row = 0; row < rows_; ++row) {
    for (int col = 0; col < cols_; ++col) {
      Node * node = &nodes[row][col];
      if (!node->is_center) {
        continue;
      }
      for (int row2 = row - 1; row2 <= row + 1; ++row2) {
        for (int col2 = col - 1; col2 <= col + 1; ++col2) {
          if ((row2 == row || col2 == col) && IsValidRowCol(row2, col2)) {
            Node * node2 = &nodes[row2][col2];
            if (node2->is_center) {
              DisjointSetUnion(node, node2);
            }
          }
        }
      }
    }
  }

  int count_obstacles = 0;
  obstacles_.clear();
  id_img_.assign(siz_, -1);
  for (int row = 0; row < rows_; ++row) {
    for (int col = 0; col < cols_; ++col) {
      Node * node = &nodes[row][col];
      if (!node->is_object) {
        continue;
      }
      Node * root = DisjointSetFind(node);
      if (root->obstacle_id < 0) {
        root->obstacle_id = count_obstacles++;
        obstacles_.push_back(Obstacle());
      }
      int grid = RowCol2Grid(row, col);
      id_img_[grid] = root->obstacle_id;
      obstacles_[root->obstacle_id].grids.push_back(grid);
    }
  }
  filter(inferred_data);
  classify(inferred_data);
}

void Cluster2D::filter(const std::shared_ptr<float> & inferred_data)
{
  const float * confidence_pt_data = inferred_data.get() + siz_ * 3;
  const float * heading_pt_x_data = inferred_data.get() + siz_ * 9;
  const float * heading_pt_y_data = inferred_data.get() + siz_ * 10;
  const float * height_pt_data = inferred_data.get() + siz_ * 11;

  for (size_t obstacle_id = 0; obstacle_id < obstacles_.size(); obstacle_id++) {
    Obstacle * obs = &obstacles_[obstacle_id];
    double score = 0.0;
    double height = 0.0;
    double vec_x = 0.0;
    double vec_y = 0.0;
    for (int grid : obs->grids) {
      score += static_cast<double>(confidence_pt_data[grid]);
      height += static_cast<double>(height_pt_data[grid]);
      vec_x += heading_pt_x_data[grid];
      vec_y += heading_pt_y_data[grid];
    }
    obs->score = score / static_cast<double>(obs->grids.size());
    obs->height = height / static_cast<double>(obs->grids.size());
    obs->heading = std::atan2(vec_y, vec_x) * 0.5;
    obs->cloud_ptr.reset(new pcl::PointCloud<pcl::PointXYZI>);
  }
}

void Cluster2D::classify(const std::shared_ptr<float> & inferred_data)
{
  const float * classify_pt_data = inferred_data.get() + siz_ * 4;
  int num_classes = 5;
  for (size_t obs_id = 0; obs_id < obstacles_.size(); obs_id++) {
    Obstacle * obs = &obstacles_[obs_id];

    for (size_t grid_id = 0; grid_id < obs->grids.size(); grid_id++) {
      int grid = obs->grids[grid_id];
      for (int k = 0; k < num_classes; k++) {
        obs->meta_type_probs[k] += classify_pt_data[k * siz_ + grid];
      }
    }
    int meta_type_id = 0;
    for (int k = 0; k < num_classes; k++) {
      obs->meta_type_probs[k] /= obs->grids.size();
      if (obs->meta_type_probs[k] > obs->meta_type_probs[meta_type_id]) {
        meta_type_id = k;
      }
    }
    obs->meta_type = static_cast<MetaType>(meta_type_id);
  }
}

tier4_perception_msgs::msg::DetectedObjectWithFeature Cluster2D::obstacleToObject(
  const Obstacle & in_obstacle, const std_msgs::msg::Header & in_header)
{
  using autoware_auto_perception_msgs::msg::DetectedObjectKinematics;
  using autoware_auto_perception_msgs::msg::ObjectClassification;

  tier4_perception_msgs::msg::DetectedObjectWithFeature resulting_object;
  // pcl::PointCloud<pcl::PointXYZI> in_cluster = *(in_obstacle.cloud_ptr);

  resulting_object.object.classification.emplace_back(
    autoware_auto_perception_msgs::build<ObjectClassification>()
      .label(ObjectClassification::UNKNOWN)
      .probability(in_obstacle.score));
  if (in_obstacle.meta_type == MetaType::META_PEDESTRIAN) {
    resulting_object.object.classification.front().label = ObjectClassification::PEDESTRIAN;
  } else if (in_obstacle.meta_type == MetaType::META_NONMOT) {
    resulting_object.object.classification.front().label = ObjectClassification::MOTORCYCLE;
  } else if (in_obstacle.meta_type == MetaType::META_SMALLMOT) {
    resulting_object.object.classification.front().label = ObjectClassification::CAR;
  } else if (in_obstacle.meta_type == MetaType::META_BIGMOT) {
    resulting_object.object.classification.front().label = ObjectClassification::BUS;
  } else {
    // resulting_object.object.classification.front().label = ObjectClassification::PEDESTRIAN;
    resulting_object.object.classification.front().label = ObjectClassification::UNKNOWN;
  }

  pcl::PointXYZ min_point;
  pcl::PointXYZ max_point;
  for (auto pit = in_obstacle.cloud_ptr->points.begin(); pit != in_obstacle.cloud_ptr->points.end();
       ++pit) {
    if (pit->x < min_point.x) {
      min_point.x = pit->x;
    }
    if (pit->y < min_point.y) {
      min_point.y = pit->y;
    }
    if (pit->z < min_point.z) {
      min_point.z = pit->z;
    }
    if (pit->x > max_point.x) {
      max_point.x = pit->x;
    }
    if (pit->y > max_point.y) {
      max_point.y = pit->y;
    }
    if (pit->z > max_point.z) {
      max_point.z = pit->z;
    }
  }

  // cluster and ground filtering
  pcl::PointCloud<pcl::PointXYZI> cluster;
  const float min_height = min_point.z + ((max_point.z - min_point.z) * 0.1f);
  for (auto pit = in_obstacle.cloud_ptr->points.begin(); pit != in_obstacle.cloud_ptr->points.end();
       ++pit) {
    if (min_height < pit->z) {
      cluster.points.push_back(*pit);
    }
  }
  min_point.z = 0.0;
  max_point.z = 0.0;
  for (auto pit = cluster.points.begin(); pit != cluster.points.end(); ++pit) {
    if (pit->z < min_point.z) {
      min_point.z = pit->z;
    }
    if (pit->z > max_point.z) {
      max_point.z = pit->z;
    }
  }
  sensor_msgs::msg::PointCloud2 ros_pc;
  pcl::toROSMsg(cluster, ros_pc);
  resulting_object.feature.cluster = ros_pc;
  resulting_object.feature.cluster.header = in_header;

  // position
  const float height = max_point.z - min_point.z;
  const float length = max_point.x - min_point.x;
  const float width = max_point.y - min_point.y;
  resulting_object.object.kinematics.pose_with_covariance.pose.position.x =
    min_point.x + length / 2;
  resulting_object.object.kinematics.pose_with_covariance.pose.position.y = min_point.y + width / 2;
  resulting_object.object.kinematics.pose_with_covariance.pose.position.z =
    min_point.z + height / 2;

  resulting_object.object.kinematics.pose_with_covariance.pose.orientation =
    getQuaternionFromRPY(0.0, 0.0, in_obstacle.heading);
  resulting_object.object.kinematics.orientation_availability =
    DetectedObjectKinematics::SIGN_UNKNOWN;

  return resulting_object;
}

void Cluster2D::getObjects(
  const float confidence_thresh, const float height_thresh, const int min_pts_num,
  tier4_perception_msgs::msg::DetectedObjectsWithFeature & objects,
  const std_msgs::msg::Header & in_header)
{
  for (size_t i = 0; i < point2grid_.size(); ++i) {
    int grid = point2grid_[i];
    if (grid < 0) {
      continue;
    }

    int obstacle_id = id_img_[grid];

    int point_id = valid_indices_in_pc_->at(i);

    if (obstacle_id >= 0 && obstacles_[obstacle_id].score >= confidence_thresh) {
      if (
        height_thresh < 0 ||
        pc_ptr_->points[point_id].z <= obstacles_[obstacle_id].height + height_thresh) {
        obstacles_[obstacle_id].cloud_ptr->push_back(pc_ptr_->points[point_id]);
      }
    }
  }

  for (size_t obstacle_id = 0; obstacle_id < obstacles_.size(); obstacle_id++) {
    Obstacle * obs = &obstacles_[obstacle_id];
    if (static_cast<int>(obs->cloud_ptr->size()) < min_pts_num) {
      continue;
    }
    tier4_perception_msgs::msg::DetectedObjectWithFeature out_obj =
      obstacleToObject(*obs, in_header);
    objects.feature_objects.push_back(out_obj);
  }
  objects.header = in_header;
}
