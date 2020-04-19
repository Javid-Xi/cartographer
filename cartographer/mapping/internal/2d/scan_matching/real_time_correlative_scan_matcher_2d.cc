/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping/internal/2d/scan_matching/real_time_correlative_scan_matcher_2d.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "Eigen/Geometry"
#include "cartographer/common/lua_parameter_dictionary.h"
#include "cartographer/common/math.h"
#include "cartographer/mapping/2d/probability_grid.h"
#include "cartographer/mapping/2d/tsdf_2d.h"
#include "cartographer/sensor/point_cloud.h"
#include "cartographer/transform/transform.h"
#include "glog/logging.h"

namespace cartographer {
namespace mapping {
namespace scan_matching {
namespace {

//参数配置
float ComputeCandidateScore(const TSDF2D& tsdf,
                            const DiscreteScan2D& discrete_scan,
                            int x_index_offset, int y_index_offset) {
  float candidate_score = 0.f;
  float summed_weight = 0.f;
  for (const Eigen::Array2i& xy_index : discrete_scan) {
    const Eigen::Array2i proposed_xy_index(xy_index.x() + x_index_offset,
                                           xy_index.y() + y_index_offset);
    const std::pair<float, float> tsd_and_weight =
        tsdf.GetTSDAndWeight(proposed_xy_index);
    const float normalized_tsd_score =
        (tsdf.GetMaxCorrespondenceCost() - std::abs(tsd_and_weight.first)) /
        tsdf.GetMaxCorrespondenceCost();
    const float weight = tsd_and_weight.second;
    candidate_score += normalized_tsd_score * weight;
    summed_weight += weight;
  }
  if (summed_weight == 0.f) return 0.f;
  candidate_score /= summed_weight;
  CHECK_GE(candidate_score, 0.f);
  return candidate_score;
}

float ComputeCandidateScore(const ProbabilityGrid& probability_grid,
                            const DiscreteScan2D& discrete_scan,
                            int x_index_offset, int y_index_offset) {
  float candidate_score = 0.f;
  for (const Eigen::Array2i& xy_index : discrete_scan) {
    const Eigen::Array2i proposed_xy_index(xy_index.x() + x_index_offset,
                                           xy_index.y() + y_index_offset);
    const float probability =
        probability_grid.GetProbability(proposed_xy_index);
    candidate_score += probability;
  }
  candidate_score /= static_cast<float>(discrete_scan.size());
  CHECK_GT(candidate_score, 0.f);
  return candidate_score;
}

}  // namespace

RealTimeCorrelativeScanMatcher2D::RealTimeCorrelativeScanMatcher2D(
    const proto::RealTimeCorrelativeScanMatcherOptions& options)
    : options_(options) {}
/**
 * @brief RealTimeCorrelativeScanMatcher::GenerateExhaustiveSearchCandidates
 * 计算出来所有的要进行搜索的解。
 * 即得到三层for循环的所有的组合
 * 在Match()里面被调用
 * @param search_parameters
 * @return
 */
std::vector<Candidate2D>
RealTimeCorrelativeScanMatcher2D::GenerateExhaustiveSearchCandidates(
    const SearchParameters& search_parameters) const {
  //计算 一共有多少的candidates。即三层循环中一共要计算多少次score
  //相当于num_scans*num_linear_x_candidates*num_linear_y_candidates
  //这里的num_scans表示一共由多少的角度搜索次数
  int num_candidates = 0;
  for (int scan_index = 0; scan_index != search_parameters.num_scans;
       ++scan_index) {
    //计算x的候选解
    const int num_linear_x_candidates =
        (search_parameters.linear_bounds[scan_index].max_x -
         search_parameters.linear_bounds[scan_index].min_x + 1);
    //计算y的候选解
    const int num_linear_y_candidates =
        (search_parameters.linear_bounds[scan_index].max_y -
         search_parameters.linear_bounds[scan_index].min_y + 1);
    //累加候选解的个数
    num_candidates += num_linear_x_candidates * num_linear_y_candidates;
  }
  //获得三层for循环的组合起来的所有的解　这里所有的点角度都是下标　xy都是grid_index
  std::vector<Candidate2D> candidates;
  candidates.reserve(num_candidates);
  //最外层循环表示角度
  for (int scan_index = 0; scan_index != search_parameters.num_scans;
       ++scan_index) {
    //内部两层循环表示线性搜索空间
    for (int x_index_offset = search_parameters.linear_bounds[scan_index].min_x;
         x_index_offset <= search_parameters.linear_bounds[scan_index].max_x;
         ++x_index_offset) {
      for (int y_index_offset =
               search_parameters.linear_bounds[scan_index].min_y;
           y_index_offset <= search_parameters.linear_bounds[scan_index].max_y;
           ++y_index_offset) {
        candidates.emplace_back(scan_index, x_index_offset, y_index_offset,
                                search_parameters);
      }
    }
  }
  CHECK_EQ(candidates.size(), num_candidates);
  return candidates;
}

/**
 * @brief RealTimeCorrelativeScanMatcher::Match
 * 实现的是RealTime CSM论文里面的方法:Computing 2D Slices
 * 这里并没有进行多分辨率地图的构建　因此这里实际上就是进行枚举而已。
 * 并没有进行什么高级的加速策略
 * 用来进行scan-match来优化机器人的位姿
 * @param initial_pose_estimate         初始的机器人的位姿
 * @param point_cloud                   位于平面机器人坐标系中的点云数据
 * @param probability_grid              对应的栅格地图
 * @param pose_estimate                 优化之后用于返回的位姿
 * @return
 */
double RealTimeCorrelativeScanMatcher2D::Match(
    const transform::Rigid2d& initial_pose_estimate,
    const sensor::PointCloud& point_cloud, const Grid2D& grid,
    transform::Rigid2d* pose_estimate) const {
  CHECK(pose_estimate != nullptr);

  //得到机器人初始的位姿
  const Eigen::Rotation2Dd initial_rotation = initial_pose_estimate.rotation();
  //把激光数据旋转到和世界坐标系平行的坐标系中
  //只所有要进行这样的旋转是因为把激光数据旋转会0度。
  //因为接下来的搜索中，我们要对角度进行离散化搜索，从0度开始进行搜索
  const sensor::PointCloud rotated_point_cloud = sensor::TransformPointCloud(
      point_cloud,
      transform::Rigid3f::Rotation(Eigen::AngleAxisf(
          initial_rotation.cast<float>().angle(), Eigen::Vector3f::UnitZ())));
  //设置scan-match的参数
  const SearchParameters search_parameters(
      options_.linear_search_window(), options_.angular_search_window(),
      rotated_point_cloud, grid.limits().resolution());

  //把激光数据经过一些列的旋转操作 得到一系列的laser_scans。这里相当于已经把所有的不同角度的激光数据进行投影了。
  //后面枚举x,y的时候，就没必要进行投影了。这也是computing 2d slice比直接三层for循环快的原因
  //这一系列的laser_scan都是经过旋转得到的
  const std::vector<sensor::PointCloud> rotated_scans =
      GenerateRotatedScans(rotated_point_cloud, search_parameters);
  //把上面旋转得到的激光雷达数据 转换到 地图坐标系中
  //经过这个函数之后，所有的激光数据的原点都和世界坐标系重合。
  //而角度也都是在世界坐标系中描述的。
  //因此对于各个不同的x_offset y_offset只需要进行激光端点的平移就可以
  const std::vector<DiscreteScan2D> discrete_scans = DiscretizeScans(
      grid.limits(), rotated_scans,
      Eigen::Translation2f(initial_pose_estimate.translation().x(),
                           initial_pose_estimate.translation().y()));
  //得到整个搜索空间里面的所有的候选解
  std::vector<Candidate2D> candidates =
      GenerateExhaustiveSearchCandidates(search_parameters);
  //计算空间中所有的候选解的得分    
  ScoreCandidates(grid, discrete_scans, search_parameters, &candidates);
  //找到得到最大的候选解candidate
  const Candidate2D& best_candidate =
      *std::max_element(candidates.begin(), candidates.end());
  //候选解的位姿即为优化过后的位姿
  *pose_estimate = transform::Rigid2d(
      {initial_pose_estimate.translation().x() + best_candidate.x,
       initial_pose_estimate.translation().y() + best_candidate.y},
      initial_rotation * Eigen::Rotation2Dd(best_candidate.orientation));
  return best_candidate.score;
}

/**
 * @brief RealTimeCorrelativeScanMatcher::ScoreCandidates
 * 计算每个候选解的得分。
 * 得到的计算方式为候选解中所有击中的栅格的占用概率的平均值。
 * 并且如果对应的候选解离初始位置比较远 则会施加惩罚
 * 在Match()里面被调用
 * @param grid                          对应的栅格地图
 * @param discrete_scans                所有的经过一系列旋转的激光数据  地图坐标系中
 * @param search_parameters             搜索的一些参数 包括步长，窗口大小等等参数
 * @param candidates                    所有的候选解
 */
void RealTimeCorrelativeScanMatcher2D::ScoreCandidates(
    const Grid2D& grid, const std::vector<DiscreteScan2D>& discrete_scans,
    const SearchParameters& search_parameters,
    std::vector<Candidate2D>* const candidates) const {
  //枚举所有的candidate
  for (Candidate2D& candidate : *candidates) {
    //每个candidate都需要枚举所有的激光点
    switch (grid.GetGridType()) {
      case GridType::PROBABILITY_GRID:
        candidate.score = ComputeCandidateScore(
            static_cast<const ProbabilityGrid&>(grid),
            discrete_scans[candidate.scan_index], candidate.x_index_offset,
            candidate.y_index_offset);
        break;
        //计算候选分数
      case GridType::TSDF:
        candidate.score = ComputeCandidateScore(
            static_cast<const TSDF2D&>(grid),
            discrete_scans[candidate.scan_index], candidate.x_index_offset,
            candidate.y_index_offset);
        break;
    }
    //施加惩罚 离初始位置越远 惩罚的越严重
    candidate.score *=
        std::exp(-common::Pow2(std::hypot(candidate.x, candidate.y) *
                                   options_.translation_delta_cost_weight() +
                               std::abs(candidate.orientation) *
                                   options_.rotation_delta_cost_weight()));
  }
}

}  // namespace scan_matching
}  // namespace mapping
}  // namespace cartographer
