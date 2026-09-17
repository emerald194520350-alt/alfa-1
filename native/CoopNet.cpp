#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "CoopNet.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr int kMaxFrame = 1024 * 1024;
    constexpr int kProtocol = 2;
    constexpr const char* kFingerprint =
        "3d81f0d5819a6b2f20260916c011a1b54a55a7a94c5cb596d56f1412cc17e220";

    std::atomic<bool> gStop{ false };
    std::atomic<bool> gStarted{ false };
    std::atomic<SOCKET> gSocket{ INVALID_SOCKET };
    HANDLE gThread = nullptr;
    std::mutex gMutex;
    CoopNet::Snapshot gSnapshot;
    std::deque<std::string> gOutgoing;
    std::string gRole;
    std::string gToken;
    std::string gAddress;
    unsigned short gPort = 5523;
    std::uint64_t gPositionSequence = 0;
    std::uint64_t gEventSequence = 0;
    std::uint64_t gSpeciesSequence = 1;

    std::string Environment(const char* name)
    {
        char buffer[512]{};
        const DWORD length = GetEnvironmentVariableA(name, buffer,
            static_cast<DWORD>(sizeof(buffer)));
        if (length == 0 || length >= sizeof(buffer)) return {};
        return std::string(buffer, buffer + length);
    }

    std::string JsonEscape(const std::string& value)
    {
        std::string result;
        result.reserve(value.size() + 16);
        for (unsigned char c : value)
        {
            switch (c)
            {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char escaped[8]{};
                    sprintf_s(escaped, "\\u%04x", static_cast<unsigned>(c));
                    result += escaped;
                }
                else result.push_back(static_cast<char>(c));
                break;
            }
        }
        return result;
    }

    void Queue(std::string message)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gOutgoing.size() >= 256) gOutgoing.pop_front();
        gOutgoing.push_back(std::move(message));
    }

    bool SendAll(SOCKET socket, const char* data, int size)
    {
        int offset = 0;
        while (offset < size && !gStop.load())
        {
            const int sent = send(socket, data + offset, size - offset, 0);
            if (sent <= 0) return false;
            offset += sent;
        }
        return offset == size;
    }

    bool SendFrame(SOCKET socket, const std::string& json)
    {
        if (json.size() < 2 || json.size() > kMaxFrame) return false;
        const std::uint32_t length = static_cast<std::uint32_t>(json.size());
        unsigned char prefix[4]{
            static_cast<unsigned char>(length & 0xff),
            static_cast<unsigned char>((length >> 8) & 0xff),
            static_cast<unsigned char>((length >> 16) & 0xff),
            static_cast<unsigned char>((length >> 24) & 0xff)
        };
        return SendAll(socket, reinterpret_cast<const char*>(prefix), 4) &&
            SendAll(socket, json.data(), static_cast<int>(json.size()));
    }

    bool FindValueStart(const std::string& json, const char* key, size_t& position)
    {
        const std::string token = std::string("\"") + key + "\":";
        position = json.find(token);
        if (position == std::string::npos) return false;
        position += token.size();
        while (position < json.size() &&
            (json[position] == ' ' || json[position] == '\t' ||
                json[position] == '\r' || json[position] == '\n')) ++position;
        return position < json.size();
    }

    bool ReadString(const std::string& json, const char* key, std::string& value)
    {
        size_t position = 0;
        if (!FindValueStart(json, key, position) || json[position] != '"') return false;
        ++position;
        std::string result;
        while (position < json.size())
        {
            const char c = json[position++];
            if (c == '"') { value = std::move(result); return true; }
            if (c != '\\') { result.push_back(c); continue; }
            if (position >= json.size()) return false;
            const char escaped = json[position++];
            switch (escaped)
            {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: return false;
            }
        }
        return false;
    }

    bool ReadNumber(const std::string& json, const char* key, double& value)
    {
        size_t position = 0;
        if (!FindValueStart(json, key, position)) return false;
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(json.c_str() + position, &end);
        if (end == json.c_str() + position || errno == ERANGE || !std::isfinite(parsed))
            return false;
        value = parsed;
        return true;
    }

    bool ReadBool(const std::string& json, const char* key, bool& value)
    {
        size_t position = 0;
        if (!FindValueStart(json, key, position)) return false;
        if (json.compare(position, 4, "true") == 0) { value = true; return true; }
        if (json.compare(position, 5, "false") == 0) { value = false; return true; }
        return false;
    }

    bool ObjectHasKey(const std::string& json, const char* objectName,
        const char* key, bool& present)
    {
        size_t position = 0;
        if (!FindValueStart(json, objectName, position) || json[position] != '{')
            return false;
        const size_t objectStart = position;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; position < json.size(); ++position)
        {
            const char c = json[position];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}' && --depth == 0)
            {
                const std::string token = std::string("\"") + key + "\":";
                const size_t found = json.find(token, objectStart + 1);
                present = found != std::string::npos && found < position;
                return true;
            }
        }
        return false;
    }

    void ClearRemotePeerStateLocked()
    {
        gSnapshot.hasRemotePosition = false;
        gSnapshot.remoteX = 0.0f;
        gSnapshot.remoteY = 0.0f;
        gSnapshot.remoteZ = 0.0f;
        gSnapshot.remotePositionSequence = 0;
        gSnapshot.remotePositionReceivedTick = 0;
        gSnapshot.hasRemoteAppearance = false;
        gSnapshot.remoteModelInstance = 0;
        gSnapshot.remoteModelType = 0;
        gSnapshot.remoteModelGroup = 0;
        gSnapshot.remoteCellResource = 0;
        gSnapshot.remoteScale = 1.0f;
        gSnapshot.remoteTargetSize = 1.0f;
        gSnapshot.remoteOpacity = 1.0f;
        gSnapshot.remoteAppearanceSequence = 0;
        gSnapshot.remoteAppearanceBlob.clear();
    }

    bool ReadNumberArray(const std::string& json, const char* key,
        std::vector<double>& values, size_t expected)
    {
        size_t position = 0;
        if (!FindValueStart(json, key, position) || json[position] != '[') return false;
        ++position;
        std::vector<double> result;
        while (position < json.size())
        {
            while (position < json.size() &&
                (json[position] == ' ' || json[position] == '\t' ||
                    json[position] == '\r' || json[position] == '\n')) ++position;
            if (position < json.size() && json[position] == ']')
            {
                if (result.size() != expected) return false;
                values = std::move(result);
                return true;
            }
            char* end = nullptr;
            errno = 0;
            const double number = std::strtod(json.c_str() + position, &end);
            if (end == json.c_str() + position || errno == ERANGE || !std::isfinite(number))
                return false;
            result.push_back(number);
            position = static_cast<size_t>(end - json.c_str());
            while (position < json.size() && json[position] == ' ') ++position;
            if (position < json.size() && json[position] == ',') { ++position; continue; }
            if (position < json.size() && json[position] == ']') continue;
            return false;
        }
        return false;
    }

    void HandleMessage(const std::string& json)
    {
        std::string type;
        if (!ReadString(json, "type", type)) return;

        if (type == "welcome")
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gSnapshot.connected = true;
            ++gSnapshot.connectionGeneration;
            gSnapshot.lastError.clear();
            ClearRemotePeerStateLocked();
            gSnapshot.invitePending = false;
            gSnapshot.inviteAccepted = false;
            gSnapshot.progressInitialized = false;
            gSnapshot.revision = 0;
            gSnapshot.progress = CoopNet::CellProgress{};
            gSnapshot.editorOpen = false;
            gSnapshot.editorID = 0;
            gSnapshot.editorRole.clear();
            gSnapshot.speciesSequence = 0;
            gSnapshot.speciesBlob.clear();
            return;
        }

        if (type == "error")
        {
            std::string message;
            if (!ReadString(json, "message", message)) return;
            std::lock_guard<std::mutex> lock(gMutex);
            gSnapshot.lastError = std::move(message);
            return;
        }

        if (type == "position")
        {
            std::string role;
            std::vector<double> position;
            double sequence = 0;
            if (!ReadString(json, "role", role) || role == gRole ||
                !ReadNumber(json, "sequence", sequence) ||
                !ReadNumberArray(json, "position", position, 3)) return;
            std::lock_guard<std::mutex> lock(gMutex);
            gSnapshot.hasRemotePosition = true;
            gSnapshot.remoteX = static_cast<float>(position[0]);
            gSnapshot.remoteY = static_cast<float>(position[1]);
            gSnapshot.remoteZ = static_cast<float>(position[2]);
            gSnapshot.remotePositionSequence = static_cast<std::uint64_t>(sequence);
            gSnapshot.remotePositionReceivedTick = GetTickCount64();
            double value = 0;
            if (ReadNumber(json, "modelInstance", value)) gSnapshot.remoteModelInstance = static_cast<std::uint32_t>(value);
            if (ReadNumber(json, "modelType", value)) gSnapshot.remoteModelType = static_cast<std::uint32_t>(value);
            if (ReadNumber(json, "modelGroup", value)) gSnapshot.remoteModelGroup = static_cast<std::uint32_t>(value);
            if (ReadNumber(json, "cellResource", value)) gSnapshot.remoteCellResource = static_cast<std::uint32_t>(value);
            if (ReadNumber(json, "scale", value)) gSnapshot.remoteScale = static_cast<float>(value);
            if (ReadNumber(json, "targetSize", value)) gSnapshot.remoteTargetSize = static_cast<float>(value);
            if (ReadNumber(json, "opacity", value)) gSnapshot.remoteOpacity = static_cast<float>(value);
            gSnapshot.hasRemoteAppearance = gSnapshot.remoteModelInstance != 0;
            return;
        }

        if (type == "appearance")
        {
            std::string role;
            std::string appearance;
            double sequence = 0;
            double value = 0;
            if (!ReadString(json, "role", role) || role == gRole ||
                !ReadString(json, "appearance", appearance) ||
                !ReadNumber(json, "sequence", sequence)) return;
            std::lock_guard<std::mutex> lock(gMutex);
            if (static_cast<std::uint64_t>(sequence) < gSnapshot.remoteAppearanceSequence)
                return;
            gSnapshot.remoteAppearanceSequence = static_cast<std::uint64_t>(sequence);
            gSnapshot.remoteAppearanceBlob = std::move(appearance);
            if (ReadNumber(json, "modelInstance", value))
                gSnapshot.remoteModelInstance = static_cast<std::uint32_t>(value);
            if (ReadNumber(json, "modelType", value))
                gSnapshot.remoteModelType = static_cast<std::uint32_t>(value);
            if (ReadNumber(json, "modelGroup", value))
                gSnapshot.remoteModelGroup = static_cast<std::uint32_t>(value);
            gSnapshot.hasRemoteAppearance = gSnapshot.remoteModelInstance != 0;
            return;
        }

        if (type == "state")
        {
            double number = 0;
            bool initialized = false;
            CoopNet::CellProgress progress;
            std::vector<double> unlocks;
            ReadBool(json, "progressInitialized", initialized);
            if (ReadNumber(json, "revision", number))
            {
                std::lock_guard<std::mutex> lock(gMutex);
                gSnapshot.revision = static_cast<std::uint64_t>(number);
            }
            if (ReadNumber(json, "food", number)) progress.food = static_cast<int>(number);
            if (ReadNumber(json, "plantFood", number)) progress.plantFood = static_cast<int>(number);
            if (ReadNumber(json, "overPlantFood", number)) progress.overPlantFood = static_cast<int>(number);
            if (ReadNumber(json, "overAnimalFood", number)) progress.overAnimalFood = static_cast<int>(number);
            if (ReadNumber(json, "spent", number)) progress.spent = static_cast<int>(number);
            if (ReadNumberArray(json, "unlocks", unlocks, 13))
                for (size_t i = 0; i < 13; ++i) progress.unlocks[i] = static_cast<int>(unlocks[i]);
            std::vector<double> missions;
            if (ReadNumberArray(json, "missions", missions, 24))
                for (size_t i = 0; i < 24; ++i) progress.missions[i] = static_cast<int>(missions[i]);
            if (ReadNumber(json, "killCount", number)) progress.killCount = static_cast<int>(number);
            ReadBool(json, "playerHasMoved", progress.playerHasMoved);
            ReadBool(json, "playerHasEaten", progress.playerHasEaten);
            ReadBool(json, "partCinematicPlayed", progress.partCinematicPlayed);
            ReadBool(json, "showMateButton", progress.showMateButton);
            ReadBool(json, "firstEditorEntry", progress.firstEditorEntry);

            bool evolving = false;
            ReadBool(json, "evolving", evolving);
            bool invitePending = false;
            bool inviteAccepted = false;
            ReadBool(json, "invitePending", invitePending);
            ReadBool(json, "inviteAccepted", inviteAccepted);
            std::string editorRole;
            ReadString(json, "editor", editorRole);
            std::uint32_t editorID = 0;
            if (ReadNumber(json, "editorId", number)) editorID = static_cast<std::uint32_t>(number);
            std::string species;
            ReadString(json, "species", species);
            std::uint64_t speciesSequence = 0;
            if (ReadNumber(json, "speciesSequence", number))
                speciesSequence = static_cast<std::uint64_t>(number);
            const char* remoteRole = gRole == "host" ? "guest" : "host";
            bool remotePeerPresent = false;
            const bool hasPlayers = ObjectHasKey(json, "players", remoteRole,
                remotePeerPresent);

            std::lock_guard<std::mutex> lock(gMutex);
            if (hasPlayers && !remotePeerPresent) ClearRemotePeerStateLocked();
            gSnapshot.progressInitialized = initialized;
            if (initialized) gSnapshot.progress = progress;
            gSnapshot.editorOpen = evolving;
            gSnapshot.invitePending = invitePending;
            gSnapshot.inviteAccepted = inviteAccepted;
            gSnapshot.editorRole = editorRole;
            gSnapshot.editorID = editorID;
            if (!species.empty() && speciesSequence >= gSnapshot.speciesSequence)
            {
                gSnapshot.speciesSequence = speciesSequence;
                gSnapshot.speciesBlob = std::move(species);
            }
            return;
        }

        if (type == "editorOpen")
        {
            std::string role;
            double editorID = 0;
            if (!ReadString(json, "role", role) || !ReadNumber(json, "editorId", editorID)) return;
            std::lock_guard<std::mutex> lock(gMutex);
            gSnapshot.editorOpen = true;
            gSnapshot.editorRole = role;
            gSnapshot.editorID = static_cast<std::uint32_t>(editorID);
            return;
        }

        if (type == "speciesLive" || type == "editorClosed")
        {
            std::string role;
            std::string species;
            double sequence = 0;
            if (!ReadString(json, "role", role) || role == gRole ||
                !ReadString(json, "species", species) ||
                !ReadNumber(json, "sequence", sequence)) return;
            std::lock_guard<std::mutex> lock(gMutex);
            if (static_cast<std::uint64_t>(sequence) >= gSnapshot.speciesSequence)
            {
                gSnapshot.speciesSequence = static_cast<std::uint64_t>(sequence);
                gSnapshot.speciesBlob = std::move(species);
            }
            if (type == "editorClosed") gSnapshot.editorOpen = false;
        }
    }

    bool DrainMessages(SOCKET socket)
    {
        std::deque<std::string> messages;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            messages.swap(gOutgoing);
        }
        for (const auto& message : messages)
            if (!SendFrame(socket, message)) return false;
        return true;
    }

    void MarkDisconnected()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSnapshot.connected = false;
        gSnapshot.hasRemotePosition = false;
        gSnapshot.remotePositionReceivedTick = 0;
        gSnapshot.hasRemoteAppearance = false;
        gSnapshot.remoteAppearanceSequence = 0;
        gSnapshot.remoteAppearanceBlob.clear();
        gSnapshot.invitePending = false;
        gSnapshot.inviteAccepted = false;
    }

    bool ConnectedLoop(SOCKET socket)
    {
        int timeout = 1000;
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        const std::string hello = "{\"type\":\"hello\",\"protocol\":" +
            std::to_string(kProtocol) + ",\"role\":\"" +
            JsonEscape(gRole) + "\",\"token\":\"" + JsonEscape(gToken) +
            "\",\"fingerprint\":\"" + kFingerprint + "\"}";
        if (!SendFrame(socket, hello)) return false;

        std::vector<unsigned char> input;
        input.reserve(65536);
        while (!gStop.load())
        {
            if (!DrainMessages(socket)) return false;

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket, &readSet);
            timeval wait{ 0, 25000 };
            const int selected = select(0, &readSet, nullptr, nullptr, &wait);
            if (selected == SOCKET_ERROR) return false;
            if (selected == 0) continue;

            unsigned char chunk[16384];
            const int received = recv(socket, reinterpret_cast<char*>(chunk),
                static_cast<int>(sizeof(chunk)), 0);
            if (received <= 0) return false;
            input.insert(input.end(), chunk, chunk + received);

            while (input.size() >= 4)
            {
                const std::uint32_t length = static_cast<std::uint32_t>(input[0]) |
                    (static_cast<std::uint32_t>(input[1]) << 8) |
                    (static_cast<std::uint32_t>(input[2]) << 16) |
                    (static_cast<std::uint32_t>(input[3]) << 24);
                if (length < 2 || length > kMaxFrame) return false;
                if (input.size() < static_cast<size_t>(length) + 4) break;
                HandleMessage(std::string(input.begin() + 4, input.begin() + 4 + length));
                input.erase(input.begin(), input.begin() + 4 + length);
            }
        }
        return true;
    }

    DWORD WINAPI NetworkThread(LPVOID)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;

        while (!gStop.load())
        {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET) break;

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(gPort);
            if (inet_pton(AF_INET, gAddress.c_str(), &address.sin_addr) != 1 ||
                connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
            {
                closesocket(socket);
                for (int i = 0; i < 10 && !gStop.load(); ++i) Sleep(100);
                continue;
            }

            gSocket.store(socket);
            ConnectedLoop(socket);
            gSocket.store(INVALID_SOCKET);
            shutdown(socket, SD_BOTH);
            closesocket(socket);
            MarkDisconnected();
            for (int i = 0; i < 10 && !gStop.load(); ++i) Sleep(100);
        }

        WSACleanup();
        return 0;
    }

    std::string UnlockArray(const std::array<int, 13>& values)
    {
        std::string json = "[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i) json += ',';
            json += std::to_string(std::max(0, values[i]));
        }
        json += ']';
        return json;
    }

    std::string MissionArray(const std::array<int, 24>& values)
    {
        std::string json = "[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i) json += ',';
            json += std::to_string(std::max(0, values[i]));
        }
        json += ']';
        return json;
    }

    std::string ProgressFields(const CoopNet::CellProgress& value,
        const std::array<int, 13>& unlocks, bool signedSpent)
    {
        return "\"food\":" + std::to_string(std::max(0, value.food)) +
            ",\"plantFood\":" + std::to_string(std::max(0, value.plantFood)) +
            ",\"overPlantFood\":" + std::to_string(std::max(0, value.overPlantFood)) +
            ",\"overAnimalFood\":" + std::to_string(std::max(0, value.overAnimalFood)) +
            ",\"spent\":" + std::to_string(signedSpent ? value.spent : std::max(0, value.spent)) +
            ",\"unlocks\":" + UnlockArray(unlocks) +
            ",\"missions\":" + MissionArray(value.missions) +
            ",\"killCount\":" + std::to_string(std::max(0, value.killCount)) +
            ",\"playerHasMoved\":" + (value.playerHasMoved ? "true" : "false") +
            ",\"playerHasEaten\":" + (value.playerHasEaten ? "true" : "false") +
            ",\"partCinematicPlayed\":" + (value.partCinematicPlayed ? "true" : "false") +
            ",\"showMateButton\":" + (value.showMateButton ? "true" : "false") +
            ",\"firstEditorEntry\":" + (value.firstEditorEntry ? "true" : "false");
    }

    std::string NextEventID(const char* prefix)
    {
        return gRole + "-" + prefix + "-" + std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(++gEventSequence);
    }
}

namespace CoopNet
{
    bool StartFromEnvironment()
    {
        if (gStarted.exchange(true)) return gSnapshot.enabled;
        gRole = Environment("SPORE_COOP_ROLE");
        if (gRole != "host" && gRole != "guest")
        {
            gSnapshot.enabled = false;
            return false;
        }
        gAddress = Environment("SPORE_COOP_SERVER");
        if (gAddress.empty()) gAddress = "127.0.0.1";
        gToken = Environment("SPORE_COOP_TOKEN");
        if (gToken.size() < 24)
        {
            gSnapshot.enabled = false;
            return false;
        }
        const std::string port = Environment("SPORE_COOP_PORT");
        if (!port.empty())
        {
            const long parsed = std::strtol(port.c_str(), nullptr, 10);
            if (parsed > 0 && parsed <= 65535) gPort = static_cast<unsigned short>(parsed);
        }

        gSnapshot.enabled = true;
        gStop.store(false);
        gThread = CreateThread(nullptr, 0, NetworkThread, nullptr, 0, nullptr);
        if (!gThread)
        {
            gSnapshot.enabled = false;
            return false;
        }
        return true;
    }

    void Stop()
    {
        gStop.store(true);
        const SOCKET socket = gSocket.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET) shutdown(socket, SD_BOTH);
        if (gThread)
        {
            WaitForSingleObject(gThread, 1500);
            CloseHandle(gThread);
            gThread = nullptr;
        }
    }

    Snapshot GetSnapshot()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return gSnapshot;
    }

    const char* GetRole()
    {
        return gRole.c_str();
    }

    void SubmitPosition(float x, float y, float z,
        std::uint32_t modelInstance, std::uint32_t modelType,
        std::uint32_t modelGroup, std::uint32_t cellResource,
        float scale, float targetSize, float opacity)
    {
        char json[640]{};
        sprintf_s(json,
            "{\"type\":\"position\",\"sequence\":%llu,\"position\":[%.6f,%.6f,%.6f],"
            "\"modelInstance\":%u,\"modelType\":%u,\"modelGroup\":%u,\"cellResource\":%u,"
            "\"scale\":%.6f,\"targetSize\":%.6f,\"opacity\":%.6f}",
            static_cast<unsigned long long>(gPositionSequence++), x, y, z,
            modelInstance, modelType, modelGroup, cellResource, scale, targetSize, opacity);
        Queue(json);
    }

    void SubmitAppearance(std::uint32_t modelInstance, std::uint32_t modelType,
        std::uint32_t modelGroup, const std::string& appearanceBlob)
    {
        Queue("{\"type\":\"appearance\",\"sequence\":" +
            std::to_string(gSpeciesSequence++) +
            ",\"modelInstance\":" + std::to_string(modelInstance) +
            ",\"modelType\":" + std::to_string(modelType) +
            ",\"modelGroup\":" + std::to_string(modelGroup) +
            ",\"appearance\":\"" + JsonEscape(appearanceBlob) + "\"}");
    }

    void SubmitInvite()
    {
        Queue("{\"type\":\"invite\"}");
    }

    void SubmitInviteResponse(bool accepted)
    {
        Queue(std::string("{\"type\":\"inviteResponse\",\"accepted\":") +
            (accepted ? "true}" : "false}"));
    }

    void SeedProgress(const CellProgress& progress)
    {
        Queue("{\"type\":\"seedProgress\"," + ProgressFields(progress, progress.unlocks, false) + "}");
    }

    void SubmitProgressDelta(const CellProgress& delta,
        const std::array<int, 13>& absoluteUnlocks)
    {
        Queue("{\"type\":\"progressDelta\",\"eventId\":\"" +
            NextEventID("progress") + "\"," + ProgressFields(delta, absoluteUnlocks, true) + "}");
    }

    void SubmitEditorOpen(std::uint32_t editorID)
    {
        Queue("{\"type\":\"editorOpen\",\"editorId\":" +
            std::to_string(static_cast<unsigned long long>(editorID)) + "}");
    }

    void SubmitEditorClose(const std::string& speciesBlob)
    {
        Queue("{\"type\":\"editorClose\",\"species\":\"" +
            JsonEscape(speciesBlob) + "\"}");
    }

    void SubmitSpecies(const std::string& speciesBlob)
    {
        Queue("{\"type\":\"speciesLive\",\"sequence\":" +
            std::to_string(gSpeciesSequence++) + ",\"species\":\"" +
            JsonEscape(speciesBlob) + "\"}");
    }
}
