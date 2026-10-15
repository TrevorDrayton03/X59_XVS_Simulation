#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "XVS.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace
{
    bool parseTelemetry(const std::string& packet, Telemetry& telemetry)
    {
        std::stringstream stream(packet);
        std::string value;

        // Require exactly nine comma-separated numeric fields.
        auto nextNumber = [&]() -> double
        {
            if (!std::getline(stream, value, ','))
            {
                throw std::runtime_error("Missing telemetry field");
            }
            size_t consumed = 0;
            const double result = std::stod(value, &consumed);
            if (consumed != value.size())
            {
                throw std::runtime_error("Invalid telemetry field");
            }
            if (!std::isfinite(result))
            {
                throw std::runtime_error("Non-finite telemetry field");
            }
            return result;
        };

        try
        {
            telemetry.latitudeDeg = nextNumber();
            telemetry.longitudeDeg = nextNumber();
            telemetry.altitudeFt = nextNumber();
            telemetry.rollRad = nextNumber();
            telemetry.pitchRad = nextNumber();
            telemetry.headingRad = nextNumber();
            telemetry.airspeedKts = nextNumber();
            telemetry.verticalSpeedFps = nextNumber();
            telemetry.alphaRad = nextNumber();
        }
        catch (const std::exception&)
        {
            return false;
        }

        // Do not accidentally accept a packet with additional CSV fields.
        return !std::getline(stream, value, ',');
    }
}

struct XVS::Impl
{
    SOCKET socket = INVALID_SOCKET;
    bool winsockInitialized = false;
    bool receivedTelemetry = false;
    Telemetry latest{};

    ~Impl()
    {
        if (socket != INVALID_SOCKET)
        {
            closesocket(socket);
        }
        if (winsockInitialized)
        {
            WSACleanup();
        }
    }
};

XVS::XVS() : impl_(std::make_unique<Impl>()) {}
XVS::~XVS() = default;

void XVS::initialize(uint16_t port)
{
    if (impl_->winsockInitialized)
    {
        throw std::logic_error("XVS already initialized");
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        throw std::runtime_error("WSAStartup failed");
    }
    impl_->winsockInitialized = true;

    impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == INVALID_SOCKET)
    {
        throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(impl_->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        throw std::runtime_error("bind() failed: " + std::to_string(WSAGetLastError()));
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(impl_->socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        throw std::runtime_error("ioctlsocket() failed: " + std::to_string(WSAGetLastError()));
    }

    std::cout << "Listening for UE5 telemetry on UDP port " << port << "\n";
}

void XVS::poll()
{
    char buffer[1024];

    // Process a bounded number to ensure incoming UDP cannot starve rendering.
    for (int count = 0; count < 256; ++count)
    {
        sockaddr_in sender{};
        int senderSize = sizeof(sender);

        const int received = recvfrom(
            impl_->socket,
            buffer,
            static_cast<int>(sizeof(buffer)),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderSize);

        if (received == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                break; // Nothing new; retain latest telemetry.
            }
            throw std::runtime_error("recvfrom() failed: " + std::to_string(error));
        }

        std::string packet(buffer, received);
        // Accept Unreal senders that include a terminating NUL byte.
        const auto nullPosition = packet.find('\0');
        if (nullPosition != std::string::npos)
        {
            packet.resize(nullPosition);
        }

        Telemetry parsed{};
        if (parseTelemetry(packet, parsed))
        {
            impl_->latest = parsed;
            impl_->receivedTelemetry = true;
        }
    }
}

bool XVS::hasTelemetry() const noexcept
{
    return impl_->receivedTelemetry;
}

const Telemetry& XVS::latestTelemetry() const noexcept
{
    return impl_->latest;
}

const std::array<XVSDisplayRegion, 4>& XVS::displayLayout() noexcept
{
    static const std::array<XVSDisplayRegion, 4> regions{{
        {XVSDisplay::ExternalVision, 0.23f, 0.05f, 0.65f, 0.43f,
         {0.07f, 0.35f, 0.55f, 1.0f}},
        {XVSDisplay::Standby,        0.04f, 0.28f, 0.16f, 0.18f,
         {0.85f, 0.58f, 0.19f, 1.0f}},
        {XVSDisplay::Navigation,     0.08f, 0.56f, 0.40f, 0.36f,
         {0.09f, 0.50f, 0.35f, 1.0f}},
        {XVSDisplay::PrimaryFlight,  0.52f, 0.56f, 0.40f, 0.36f,
         {0.53f, 0.31f, 0.65f, 1.0f}},
    }};
    return regions;
}
