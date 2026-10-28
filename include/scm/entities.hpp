#pragma once
#include "bsp.hpp"

namespace scm {
struct EntityPose { glm::vec3 origin{}, angles{}; };
EntityPose initialTrackTrainPose(const Bsp& bsp, const bsppp::BSPEntityKeyValues& train, double captureTime);
EntityPose initialPathTrainPose(const Bsp& bsp, const bsppp::BSPEntityKeyValues& train, glm::vec3 center, double captureTime);
}
