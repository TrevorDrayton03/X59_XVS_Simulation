#pragma once

#include <cstdint>
#include <memory>

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
