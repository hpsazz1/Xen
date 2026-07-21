#include "build_identity.h"

#include "basic_aim_controller.h"

#include <algorithm>
#include <string>

#ifndef AIMBOT_BUILD_BACKEND
#define AIMBOT_BUILD_BACKEND "unknown"
#endif
#ifndef AIMBOT_BUILD_REVISION
#define AIMBOT_BUILD_REVISION "unknown"
#endif
#ifndef AIMBOT_BUILD_TIMESTAMP_UTC
#define AIMBOT_BUILD_TIMESTAMP_UTC "unknown"
#endif

const char* BuildIdentity::backend()
{
    return AIMBOT_BUILD_BACKEND;
}

const char* BuildIdentity::revision()
{
    return AIMBOT_BUILD_REVISION;
}

const char* BuildIdentity::timestampUtc()
{
    return AIMBOT_BUILD_TIMESTAMP_UTC;
}

int BuildIdentity::controllerRevision()
{
    return kBasicAimControllerRevision;
}

std::string BuildIdentity::displayLabel()
{
    const std::string fullRevision = revision();
    const bool dirty = fullRevision.find("-dirty") != std::string::npos;
    std::string shortRevision = fullRevision.substr(0, std::min<size_t>(7, fullRevision.size()));
    if (dirty)
        shortRevision += '*';
    return std::string(backend()) + " " + shortRevision + " r" +
        std::to_string(controllerRevision());
}
