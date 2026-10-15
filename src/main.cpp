#include "engine.h"
#include "XVS.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>

int main()
{
    try
    {
        Engine engine;
        XVS xvs;

        engine.initialize();
        xvs.initialize(5500);

        // Convert the XVS layout to generic render-region data once.
        std::array<RenderRegion, 4> regions{};
        const auto& layout = XVS::displayLayout();
        for (size_t i = 0; i < regions.size(); ++i)
        {
            regions[i] = {
                layout[i].x, layout[i].y,
                layout[i].width, layout[i].height,
                layout[i].testColor
            };
        }

        while (!engine.shouldClose())
        {
            engine.pollEvents();
            xvs.poll();

            // Temporary visual test: UE5 airspeed drives the blue channel.
            // Later, use the same telemetry for camera/view transforms,
            // instruments, and four synchronized XVS display regions.
            std::array<float, 4> clearColor{0.02f, 0.05f, 0.20f, 1.0f};
            if (xvs.hasTelemetry())
            {
                const Telemetry& t = xvs.latestTelemetry();
                clearColor[2] += std::clamp(
                    static_cast<float>(t.airspeedKts / 400.0), 0.0f, 0.7f);
            }

            engine.drawFrame(clearColor, regions);
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Application error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
