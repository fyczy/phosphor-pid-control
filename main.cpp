/**
 * Copyright 2017 Google Inc.
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

#include "config.h"

#include "build/buildjson.hpp"
#include "conf.hpp"
#include "interfaces.hpp"
#include "pid/builder.hpp"
#include "pid/buildjson.hpp"
#include "pid/pidthread.hpp"
#include "pid/tuning.hpp"
#include "pid/zone.hpp"
#include "sensors/builder.hpp"
#include "sensors/buildjson.hpp"
#include "sensors/manager.hpp"
#include "threads/busthread.hpp"
#include "util.hpp"

#include <CLI/CLI.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <sdbusplus/bus.hpp>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if CONFIGURE_DBUS
#include "dbus/dbusconfiguration.hpp"
#endif

/* The YAML converted sensor list. */
std::map<std::string, struct conf::SensorConfig> sensorConfig = {};
/* The YAML converted PID list. */
std::map<int64_t, conf::PIDConf> zoneConfig = {};
/* The YAML converted Zone configuration. */
std::map<int64_t, struct conf::ZoneConfig> zoneDetailsConfig = {};

/** the swampd daemon will check for the existence of this file. */
constexpr auto jsonConfigurationPath = "/usr/share/swampd/config.json";

int main(int argc, char* argv[])
{
    int rc = 0;
    std::string configPath = "";
    tuningLoggingPath = "";

    CLI::App app{"OpenBMC Fan Control Daemon"};

    app.add_option("-c,--conf", configPath,
                   "Optional parameter to specify configuration at run-time")
        ->check(CLI::ExistingFile);
    app.add_option("-t,--tuning", tuningLoggingPath,
                   "Optional parameter to specify tuning logging path, and "
                   "enable tuning")
        ->check(CLI::ExistingFile);

    CLI11_PARSE(app, argc, argv);

    tuningLoggingEnabled = (tuningLoggingPath.length() > 0);

    auto modeControlBus = sdbusplus::bus::new_system();
    static constexpr auto modeRoot = "/xyz/openbmc_project/settings/fanctrl";
    // Create a manager for the ModeBus because we own it.
    sdbusplus::server::manager::manager(modeControlBus, modeRoot);

#if CONFIGURE_DBUS
    {
        dbus_configuration::init(modeControlBus);
    }
#else
    const std::string& path =
        (configPath.length() > 0) ? configPath : jsonConfigurationPath;

    /*
     * When building the sensors, if any of the dbus passive ones aren't on the
     * bus, it'll fail immediately.
     */
    try
    {
        auto jsonData = parseValidateJson(path);
        sensorConfig = buildSensorsFromJson(jsonData);
        std::tie(zoneConfig, zoneDetailsConfig) = buildPIDsFromJson(jsonData);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed during building: " << e.what() << "\n";
        exit(EXIT_FAILURE); /* fatal error. */
    }
#endif

    SensorManager mgmr = buildSensors(sensorConfig);
    std::unordered_map<int64_t, std::unique_ptr<PIDZone>> zones =
        buildZones(zoneConfig, zoneDetailsConfig, mgmr, modeControlBus);

    if (0 == zones.size())
    {
        std::cerr << "No zones defined, exiting.\n";
        return rc;
    }

    /*
     * All sensors are managed by one manager, but each zone has a pointer to
     * it.
     */

    auto& hostSensorBus = mgmr.getHostBus();
    auto& passiveListeningBus = mgmr.getPassiveBus();

    std::cerr << "Starting threads\n";

    /* TODO(venture): Ask SensorManager if we have any passive sensors. */
    struct ThreadParams p = {std::ref(passiveListeningBus), ""};
    std::thread l(busThread, std::ref(p));

    /* TODO(venture): Ask SensorManager if we have any host sensors. */
    static constexpr auto hostBus = "xyz.openbmc_project.Hwmon.external";
    struct ThreadParams e = {std::ref(hostSensorBus), hostBus};
    std::thread te(busThread, std::ref(e));

    static constexpr auto modeBus = "xyz.openbmc_project.State.FanCtrl";
    struct ThreadParams m = {std::ref(modeControlBus), modeBus};
    std::thread tm(busThread, std::ref(m));

    std::vector<std::thread> zoneThreads;

    /* TODO(venture): This was designed to have one thread per zone, but really
     * it could have one thread for all the zones and iterate through each
     * sequentially as it goes -- and it'd probably be fast enough to do that,
     * however, a system isn't likely going to have more than a couple zones.
     * If it only has a couple zones, then this is fine.
     */
    for (const auto& i : zones)
    {
        std::cerr << "pushing zone" << std::endl;
        zoneThreads.push_back(std::thread(pidControlThread, i.second.get()));
    }

    l.join();
    te.join();
    tm.join();
    for (auto& t : zoneThreads)
    {
        t.join();
    }

    return rc;
}
