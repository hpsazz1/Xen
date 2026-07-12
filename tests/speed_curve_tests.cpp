#include "runtime/speed_curve.h"

#include <cmath>
#include <iostream>

namespace
{
int failures = 0;

void expectNear(double actual, double expected, double tolerance, const char* name)
{
    if (std::abs(actual - expected) <= tolerance)
        return;

    std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
}

void expectTrue(bool condition, const char* name)
{
    if (condition)
        return;

    std::cerr << name << ": condition was false\n";
    ++failures;
}
}

int main()
{
    expectNear(computeSpeedMultiplier(0.0, 100.0, 10.0f, 40.0f, 2.0f, 1.5f, 2.0, 8.0),
               3.0, 1e-9, "snap zone boost");

    const double nearSpeed = computeSpeedMultiplier(
        20.0, 100.0, 10.0f, 40.0f, 2.0f, 1.0f, 2.0, 8.0);
    expectTrue(nearSpeed >= 2.0 && nearSpeed <= 8.0, "near zone clamp");

    const double farSpeed = computeSpeedMultiplier(
        100.0, 100.0, 10.0f, 40.0f, 2.0f, 1.0f, 2.0, 8.0);
    expectNear(farSpeed, 8.0, 1e-9, "far zone maximum");

    const double overDistance = computeSpeedMultiplier(
        1000.0, 100.0, 10.0f, 40.0f, 2.0f, 1.0f, 2.0, 8.0);
    expectNear(overDistance, 8.0, 1e-9, "distance upper clamp");

    const double reversedRange = computeSpeedMultiplier(
        100.0, 100.0, 10.0f, 40.0f, 2.0f, 1.0f, 8.0, 2.0);
    expectNear(reversedRange, 8.0, 1e-9, "reversed speed range");

    double previous = 2.0;
    for (int distance = 10; distance <= 100; ++distance)
    {
        const double current = computeSpeedMultiplier(
            static_cast<double>(distance), 100.0, 10.0f, 40.0f, 2.0f, 1.0f, 2.0, 8.0);
        expectTrue(current + 1e-9 >= previous, "speed curve monotonicity");
        previous = current;
    }

    if (failures != 0)
    {
        std::cerr << failures << " speed curve test(s) failed\n";
        return 1;
    }

    std::cout << "speed curve tests passed\n";
    return 0;
}
