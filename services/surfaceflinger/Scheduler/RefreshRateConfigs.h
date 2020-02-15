/*
 * Copyright 2019 The Android Open Source Project
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

#pragma once

#include <android-base/stringprintf.h>

#include <algorithm>
#include <numeric>
#include <type_traits>

#include "DisplayHardware/HWComposer.h"
#include "HwcStrongTypes.h"
#include "Scheduler/SchedulerUtils.h"
#include "Scheduler/StrongTyping.h"

namespace android::scheduler {

enum class RefreshRateConfigEvent : unsigned { None = 0b0, Changed = 0b1 };

inline RefreshRateConfigEvent operator|(RefreshRateConfigEvent lhs, RefreshRateConfigEvent rhs) {
    using T = std::underlying_type_t<RefreshRateConfigEvent>;
    return static_cast<RefreshRateConfigEvent>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

/**
 * This class is used to encapsulate configuration for refresh rates. It holds information
 * about available refresh rates on the device, and the mapping between the numbers and human
 * readable names.
 */
class RefreshRateConfigs {
public:
    struct RefreshRate {
        // The tolerance within which we consider FPS approximately equals.
        static constexpr float FPS_EPSILON = 0.001f;

        RefreshRate(HwcConfigIndexType configId, nsecs_t vsyncPeriod,
                    HwcConfigGroupType configGroup, std::string name, float fps)
              : configId(configId),
                vsyncPeriod(vsyncPeriod),
                configGroup(configGroup),
                name(std::move(name)),
                fps(fps) {}
        // This config ID corresponds to the position of the config in the vector that is stored
        // on the device.
        const HwcConfigIndexType configId;
        // Vsync period in nanoseconds.
        const nsecs_t vsyncPeriod;
        // This configGroup for the config.
        const HwcConfigGroupType configGroup;
        // Human readable name of the refresh rate.
        const std::string name;
        // Refresh rate in frames per second
        const float fps = 0;

        // Checks whether the fps of this RefreshRate struct is within a given min and max refresh
        // rate passed in. FPS_EPSILON is applied to the boundaries for approximation.
        bool inPolicy(float minRefreshRate, float maxRefreshRate) const {
            return (fps >= (minRefreshRate - FPS_EPSILON) && fps <= (maxRefreshRate + FPS_EPSILON));
        }

        bool operator!=(const RefreshRate& other) const {
            return configId != other.configId || vsyncPeriod != other.vsyncPeriod ||
                    configGroup != other.configGroup;
        }

        bool operator==(const RefreshRate& other) const { return !(*this != other); }
    };

    using AllRefreshRatesMapType = std::unordered_map<HwcConfigIndexType, const RefreshRate>;

    // Sets the current policy to choose refresh rates. Returns NO_ERROR if the requested policy is
    // valid, or a negative error value otherwise. policyChanged, if non-null, will be set to true
    // if the new policy is different from the old policy.
    status_t setPolicy(HwcConfigIndexType defaultConfigId, float minRefreshRate,
                       float maxRefreshRate, bool* policyChanged) EXCLUDES(mLock);
    // Gets the current policy.
    void getPolicy(HwcConfigIndexType* defaultConfigId, float* minRefreshRate,
                   float* maxRefreshRate) const EXCLUDES(mLock);

    // Returns true if config is allowed by the current policy.
    bool isConfigAllowed(HwcConfigIndexType config) const EXCLUDES(mLock);

    // Describes the different options the layer voted for refresh rate
    enum class LayerVoteType {
        NoVote,    // Doesn't care about the refresh rate
        Min,       // Minimal refresh rate available
        Max,       // Maximal refresh rate available
        Heuristic, // Specific refresh rate that was calculated by platform using a heuristic
        Explicit,  // Specific refresh rate that was provided by the app
    };

    // Captures the layer requirements for a refresh rate. This will be used to determine the
    // display refresh rate.
    struct LayerRequirement {
        std::string name;         // Layer's name. Used for debugging purposes.
        LayerVoteType vote;       // Layer vote type.
        float desiredRefreshRate; // Layer's desired refresh rate, if applicable.
        float weight; // Layer's weight in the range of [0, 1]. The higher the weight the more
                      // impact this layer would have on choosing the refresh rate.

        bool operator==(const LayerRequirement& other) const {
            return name == other.name && vote == other.vote &&
                    desiredRefreshRate == other.desiredRefreshRate && weight == other.weight;
        }

        bool operator!=(const LayerRequirement& other) const { return !(*this == other); }
    };

    // Returns all available refresh rates according to the current policy.
    const RefreshRate& getRefreshRateForContent(const std::vector<LayerRequirement>& layers) const
            EXCLUDES(mLock);

    // Returns all available refresh rates according to the current policy.
    const RefreshRate& getRefreshRateForContentV2(const std::vector<LayerRequirement>& layers) const
            EXCLUDES(mLock);

    // Returns all the refresh rates supported by the device. This won't change at runtime.
    const AllRefreshRatesMapType& getAllRefreshRates() const EXCLUDES(mLock);

    // Returns the lowest refresh rate supported by the device. This won't change at runtime.
    const RefreshRate& getMinRefreshRate() const { return *mMinSupportedRefreshRate; }

    // Returns the lowest refresh rate according to the current policy. May change in runtime.
    const RefreshRate& getMinRefreshRateByPolicy() const EXCLUDES(mLock);

    // Returns the highest refresh rate supported by the device. This won't change at runtime.
    const RefreshRate& getMaxRefreshRate() const { return *mMaxSupportedRefreshRate; }

    // Returns the highest refresh rate according to the current policy. May change in runtime.
    const RefreshRate& getMaxRefreshRateByPolicy() const EXCLUDES(mLock);

    // Returns the current refresh rate
    const RefreshRate& getCurrentRefreshRate() const EXCLUDES(mLock);

    // Returns the refresh rate that corresponds to a HwcConfigIndexType. This won't change at
    // runtime.
    const RefreshRate& getRefreshRateFromConfigId(HwcConfigIndexType configId) const {
        return mRefreshRates.at(configId);
    };

    // Stores the current configId the device operates at
    void setCurrentConfigId(HwcConfigIndexType configId) EXCLUDES(mLock);

    struct InputConfig {
        HwcConfigIndexType configId = HwcConfigIndexType(0);
        HwcConfigGroupType configGroup = HwcConfigGroupType(0);
        nsecs_t vsyncPeriod = 0;
    };

    RefreshRateConfigs(const std::vector<InputConfig>& configs,
                       HwcConfigIndexType currentHwcConfig);
    RefreshRateConfigs(const std::vector<std::shared_ptr<const HWC2::Display::Config>>& configs,
                       HwcConfigIndexType currentConfigId);

private:
    void init(const std::vector<InputConfig>& configs, HwcConfigIndexType currentHwcConfig);

    void constructAvailableRefreshRates() REQUIRES(mLock);

    void getSortedRefreshRateList(
            const std::function<bool(const RefreshRate&)>& shouldAddRefreshRate,
            std::vector<const RefreshRate*>* outRefreshRates);

    // The list of refresh rates, indexed by display config ID. This must not change after this
    // object is initialized.
    AllRefreshRatesMapType mRefreshRates;

    // The list of refresh rates which are available in the current policy, ordered by vsyncPeriod
    // (the first element is the lowest refresh rate)
    std::vector<const RefreshRate*> mAvailableRefreshRates GUARDED_BY(mLock);

    // The current config. This will change at runtime. This is set by SurfaceFlinger on
    // the main thread, and read by the Scheduler (and other objects) on other threads.
    const RefreshRate* mCurrentRefreshRate GUARDED_BY(mLock);

    // The default config. This will change at runtime. This is set by SurfaceFlinger on
    // the main thread, and read by the Scheduler (and other objects) on other threads.
    HwcConfigIndexType mDefaultConfig GUARDED_BY(mLock);

    // The min and max FPS allowed by the policy. This will change at runtime and set by
    // SurfaceFlinger on the main thread.
    float mMinRefreshRateFps GUARDED_BY(mLock) = 0;
    float mMaxRefreshRateFps GUARDED_BY(mLock) = std::numeric_limits<float>::max();

    // The min and max refresh rates supported by the device.
    // This will not change at runtime.
    const RefreshRate* mMinSupportedRefreshRate;
    const RefreshRate* mMaxSupportedRefreshRate;

    mutable std::mutex mLock;
};

} // namespace android::scheduler