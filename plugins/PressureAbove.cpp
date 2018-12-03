/*
 * Copyright (C) 2018-present, Facebook, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "oomd/plugins/PressureAbove.h"

#include <iomanip>
#include <string>

#include "oomd/Log.h"
#include "oomd/PluginRegistry.h"
#include "oomd/util/Fs.h"
#include "oomd/util/ScopeGuard.h"

static constexpr auto kCgroupFs = "/sys/fs/cgroup/";

namespace Oomd {

REGISTER_PLUGIN(pressure_above, PressureAbove::create);

int PressureAbove::init(
    Engine::MonitoredResources& resources,
    std::unordered_map<std::string, std::string> args) {
  if (args.find("cgroup") != args.end()) {
    auto cgroups = Fs::split(args["cgroup"], ',');
    cgroups_.insert(cgroups.begin(), cgroups.end());
    cgroup_fs_ =
        (args.find("cgroup_fs") != args.end() ? args["cgroup_fs"] : kCgroupFs);
  } else {
    OLOG << "Argument=cgroup not present";
    return 1;
  }

  if (args.find("resource") != args.end() &&
      (args["resource"] == "io" || args["resource"] == "memory")) {
    const auto& res = args["resource"];
    if (res == "io") {
      resource_ = ResourceType::IO;
    } else if (res == "memory") {
      resource_ = ResourceType::MEMORY;
    }
  } else {
    OLOG << "Argument=resource missing or not (io|memory)";
    return 1;
  }

  if (args.find("threshold") != args.end()) {
    threshold_ = std::stoi(args["threshold"]);
  } else {
    OLOG << "Argument=threshold not present";
    return 1;
  }

  if (args.find("duration") != args.end()) {
    duration_ = std::stoi(args["duration"]);
  } else {
    OLOG << "Argument=duration not present";
    return 1;
  }

  // Success
  return 0;
}

Engine::PluginRet PressureAbove::run(OomdContext& /* unused */) {
  using std::chrono::steady_clock;

  std::unordered_set<std::string> resolved_cgroups;
  for (const auto& cgroup : cgroups_) {
    auto resolved = Fs::resolveWildcardPath(cgroup_fs_ + "/" + cgroup);
    resolved_cgroups.insert(resolved.begin(), resolved.end());
  }
  ResourcePressure current_pressure;
  int64_t current_memory_usage = 0;

  for (const auto& abs_cgroup_path : resolved_cgroups) {
    ResourcePressure rp;
    switch (resource_) {
      case ResourceType::IO:
        rp = Fs::readIopressure(abs_cgroup_path);
        break;
      case ResourceType::MEMORY:
        rp = Fs::readMempressure(abs_cgroup_path);
        break;
        // No default to catch new additions in ResourceType
    }

    // Do a weighted comparison (we care more about 10s, then 60s, then 600s)
    if (rp.sec_10 * 3 + rp.sec_60 * 2 + rp.sec_600 >
        current_pressure.sec_10 * 3 + current_pressure.sec_60 * 2 +
            current_pressure.sec_600) {
      current_pressure = rp;
      current_memory_usage = Fs::readMemcurrent(abs_cgroup_path);
    }
  }

  OOMD_SCOPE_EXIT {
    last_pressure_ = current_pressure;
  };

  const auto now = steady_clock::now();

  // Check if the 10s pressure is above threshold_ for duration_
  if (current_pressure.sec_10 > threshold_) {
    if (hit_thres_at_ == steady_clock::time_point()) {
      hit_thres_at_ = now;
    }

    const auto diff =
        std::chrono::duration_cast<std::chrono::seconds>(now - hit_thres_at_)
            .count();

    if (diff >= duration_) {
      std::ostringstream oss;
      oss << std::setprecision(2) << std::fixed;
      oss << "10s pressure " << current_pressure.sec_10
          << " is over the threshold of " << threshold_ << " for " << duration_
          << " seconds , total usage is " << current_memory_usage / 1024 / 1024
          << "MB";
      OLOG << oss.str();

      return Engine::PluginRet::CONTINUE;
    }
  } else {
    hit_thres_at_ = steady_clock::time_point();
  }

  return Engine::PluginRet::STOP;
}

} // namespace Oomd
