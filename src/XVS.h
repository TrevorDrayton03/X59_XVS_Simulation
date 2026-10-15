#pragma once

#include <array>
#include <cstdint>
#include <memory>

// Values received from the Unreal Engine / JSBSim telemetry stream.
// Angular values are radians; position and velocity units follow each name.
struct Telemetry
{
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double altitudeFt = 0.0;
    double rollRad = 0.0;
    double pitchRad = 0.0;
    double headingRad = 0.0;
    double airspeedKts = 0.0;
    double verticalSpeedFps = 0.0;
    double alphaRad = 0.0;
};

enum class XVSDisplay
{
    Standby,
    ExternalVision,
    Navigation,
    PrimaryFlight
};

struct XVSDisplayRegion
{
    XVSDisplay display;
    float x;
    float y;
    float width;
    float height;
    std::array<float, 4> testColor;
};

// Owns UDP reception and the most recently received valid telemetry.
// Winsock is hidden from the public header so it cannot conflict with GLFW.
class XVS
{
public:
    XVS();
    ~XVS();

    XVS(const XVS&) = delete;
    XVS& operator=(const XVS&) = delete;

    void initialize(uint16_t port = 5500);
    void poll(); // Non-blocking; call once per render frame.

    bool hasTelemetry() const noexcept;
    const Telemetry& latestTelemetry() const noexcept;

    // Provisional four-display layout using normalized coordinates.
    // Not measurements of the NASA X-59 cockpit.
    static const std::array<XVSDisplayRegion, 4>& displayLayout() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
