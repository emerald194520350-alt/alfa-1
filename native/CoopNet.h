#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace CoopNet
{
    struct CellProgress
    {
        int food = 0;
        int plantFood = 0;
        int overPlantFood = 0;
        int overAnimalFood = 0;
        int spent = 0;
        std::array<int, 13> unlocks{};
        std::array<int, 24> missions{};
        int killCount = 0;
        bool playerHasMoved = false;
        bool playerHasEaten = false;
        bool partCinematicPlayed = false;
        bool showMateButton = false;
        bool firstEditorEntry = false;
    };

    struct Snapshot
    {
        bool enabled = false;
        bool connected = false;
        std::uint64_t connectionGeneration = 0;
        std::string lastError;
        bool hasRemotePosition = false;
        float remoteX = 0.0f;
        float remoteY = 0.0f;
        float remoteZ = 0.0f;
        std::uint64_t remotePositionSequence = 0;
        std::uint64_t remotePositionReceivedTick = 0;
        bool hasRemoteAppearance = false;
        std::uint32_t remoteModelInstance = 0;
        std::uint32_t remoteModelType = 0;
        std::uint32_t remoteModelGroup = 0;
        std::uint32_t remoteCellResource = 0;
        float remoteScale = 1.0f;
        float remoteTargetSize = 1.0f;
        float remoteOpacity = 1.0f;
        std::uint64_t remoteAppearanceSequence = 0;
        std::string remoteAppearanceBlob;

        bool invitePending = false;
        bool inviteAccepted = false;

        bool progressInitialized = false;
        std::uint64_t revision = 0;
        CellProgress progress;

        bool editorOpen = false;
        std::uint32_t editorID = 0;
        std::string editorRole;
        std::uint64_t speciesSequence = 0;
        std::string speciesBlob;
    };

    bool StartFromEnvironment();
    void Stop();
    Snapshot GetSnapshot();
    const char* GetRole();

    void SubmitPosition(float x, float y, float z,
        std::uint32_t modelInstance, std::uint32_t modelType,
        std::uint32_t modelGroup, std::uint32_t cellResource,
        float scale, float targetSize, float opacity);
    void SubmitAppearance(std::uint32_t modelInstance, std::uint32_t modelType,
        std::uint32_t modelGroup, const std::string& appearanceBlob);
    void SubmitInvite();
    void SubmitInviteResponse(bool accepted);
    void SeedProgress(const CellProgress& progress);
    void SubmitProgressDelta(const CellProgress& delta,
        const std::array<int, 13>& absoluteUnlocks);
    void SubmitEditorOpen(std::uint32_t editorID);
    void SubmitEditorClose(const std::string& speciesBlob);
    void SubmitSpecies(const std::string& speciesBlob);
}
