// SPDX-License-Identifier: GPL-3.0-or-later
// Uses documented Spore ModAPI creation functions. This is an untested NPC probe, not a multiplayer adapter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <Spore/BasicIncludes.h>
#include <Spore/UTFWin/ButtonDrawableStandard.h>
#include <Spore/Simulator/SubSystem/GameTimeManager.h>

#include "CoopNet.h"

namespace
{
    void WriteProbeLog(const char* message);

    UpdateMessageListenerPtr gUpdateListener;
    IMessageListenerPtr gEditorListener;
    Simulator::cObjectPoolIndex gRemoteCellIndex = -1;
    uint32_t gRemoteCellModel = 0;
    uint32_t gRemoteCellModelType = 0;
    uint32_t gRemoteCellModelGroup = 0;
    uint32_t gRemoteCellResource = 0;
    uint32_t gPendingRemoteModel = 0;
    uint32_t gPendingRemoteResource = 0;
    ULONG64 gRemoteAppearanceStableSince = 0;
    ULONG64 gRemoteCreateRetryAfter = 0;
    Simulator::cObjectPoolIndex gLocalCellIndex = -1;
    uint32_t gLocalCellResource = 0;
    ResourceKey gLocalSpeciesKey{};
    ULONG64 gLocalCellStableSince = 0;
    ResourceKey gLastSubmittedAppearanceKey{};
    uint32_t gLastSubmittedAppearanceCellResource = 0;
    ULONG64 gLocalAppearanceStableSince = 0;
    uint64_t gAppliedRemoteAppearanceSequence = 0;
    uint64_t gObservedConnectionGeneration = 0;
    ResourceKey gRemoteCreationKey{};
    cEditorResourcePtr gRemoteAppearanceResource;
    std::string gLastNetworkError;
    ULONG64 gLastPositionTick = 0;
    ULONG64 gLastProgressTick = 0;
    bool gProgressSeedSent = false;
    bool gHasProgressBaseline = false;
    uint64_t gAppliedProgressRevision = 0;
    CoopNet::CellProgress gProgressBaseline;
    bool gSuppressEditorRelay = false;
    bool gWasEditorMode = false;
    bool gAutoJoinAttempted = false;
    IWindowPtr gInviteButton;
    IWindowPtr gAcceptInviteButton;
    IWindowPtr gDeclineInviteButton;
    IWinProcPtr gInviteWinProc;
    bool gInviteResponseSent = false;
    ULONG64 gLastSpeciesTick = 0;
    std::string gLastLocalSpecies;
    uint64_t gAppliedSpeciesSequence = 0;

    void JoinSavedWorld();
    std::string SerializeCreation(const ResourceKey& key);
    bool DeserializeEditorResource(const std::string& blob,
        Editors::cEditorResource* resource);

    bool SameResourceKey(const ResourceKey& left, const ResourceKey& right)
    {
        return left.instanceID == right.instanceID && left.typeID == right.typeID &&
            left.groupID == right.groupID;
    }

    bool IsProfile2()
    {
        char profile[8]{};
        return GetEnvironmentVariableA("SPORE_COOP_PROFILE", profile,
            static_cast<DWORD>(sizeof(profile))) > 0 && profile[0] == '2';
    }

    using CreateMutexAFunction = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
    CreateMutexAFunction gCreateMutexAOriginal = nullptr;
    bool gMutexDetourAttached = false;

    HANDLE WINAPI CreateMutexAHook(LPSECURITY_ATTRIBUTES attributes,
        BOOL initialOwner, LPCSTR name)
    {
        if (name && name[0])
        {
            char isolatedName[512]{};
            if (sprintf_s(isolatedName, "%s_SporeCoop2", name) > 0)
            {
                return gCreateMutexAOriginal(attributes, initialOwner, isolatedName);
            }
        }
        return gCreateMutexAOriginal(attributes, initialOwner, name);
    }

    member_detour(ProfilePathsDetour, App::cAppSystem,
        void(const char16_t*, const char16_t*))
    {
    public:
        void detoured(const char16_t* creationsFolderName,
            const char16_t* appDataFolderName)
        {
            if (IsProfile2())
            {
                original_function(this, u"My Spore Creations Coop 2", u"SporeCoop2");
                WriteProbeLog("Profile 2 paths selected: SporeCoop2 and My Spore Creations Coop 2.");
                return;
            }
            original_function(this, creationsFolderName, appDataFolderName);
        }
    };

    void WriteProbeLog(const char* message)
    {
        char tempPath[MAX_PATH]{};
        char logPath[MAX_PATH]{};
        if (!GetTempPathA(MAX_PATH, tempPath)) return;
        if (sprintf_s(logPath, "%sSporeCoop.Probe.log", tempPath) <= 0) return;

        HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        char line[768]{};
        const int length = sprintf_s(line,
            "[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
            time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond, message);
        if (length > 0)
        {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        }
        CloseHandle(file);
    }

    void Status()
    {
        App::ConsolePrintF("SporeCoop probe: mode=0x%x", Simulator::GetGameModeID());
        if (Simulator::IsCellGame())
        {
            auto game = Simulator::Cell::cCellGame::Get();
            if (!game) return;
            auto player = Simulator::Cell::GetPlayerCell();
            if (!player) return;
            const auto& pos = player->GetPosition();
            App::ConsolePrintF("Cell avatar: %d, position %.2f %.2f %.2f, model 0x%x",
                player->Index(), pos.x, pos.y, pos.z, player->mModelKey.instanceID);
        }
        else if (Simulator::IsCreatureGame())
        {
            auto manager = Simulator::cGameNounManager::Get();
            if (!manager) return;
            auto player = manager->GetAvatar();
            if (!player) return;
            const auto& pos = player->GetPosition();
            App::ConsolePrintF("Creature avatar: position %.2f %.2f %.2f, species 0x%x",
                pos.x, pos.y, pos.z, player->mSpeciesKey.instanceID);
        }
    }

    Simulator::cObjectPoolIndex CreatePlayerCellClone(
        Simulator::Cell::cCellObjectData* player,
        const Math::Vector3& position,
        bool remoteControlled,
        const ResourceKey* modelOverride = nullptr)
    {
        auto game = Simulator::Cell::cCellGame::Get();
        if (!game || !game->mpCellQuery || !player || !player->mCellResource) return -1;

        WriteProbeLog("coop clone: loading the player's cell and structure resources.");
        cCellCellResourcePtr cellReference;
        auto cellData = Simulator::Cell::GetData(player->mCellResource, cellReference);
        if (!cellData || !cellData->structure)
        {
            WriteProbeLog("coop clone aborted: player cell structure is unavailable.");
            return -1;
        }

        cCellStructureResourcePtr structureReference;
        auto structureData = Simulator::Cell::GetData(cellData->structure, structureReference);
        if (!structureData || !structureData->attachments ||
            structureData->numAttachments <= 0)
        {
            WriteProbeLog("coop clone aborted: player model attachment is unavailable.");
            return -1;
        }

        using AttachmentType =
            Simulator::Cell::cCellStructureResource::cSPAttachment::Type;
        Simulator::Cell::cCellStructureResource::cSPAttachment* modelAttachment = nullptr;
        for (int i = 0; i < structureData->numAttachments; ++i)
        {
            auto& attachment = structureData->attachments[i];
            if (attachment.type == AttachmentType::Creature ||
                attachment.type == AttachmentType::RandomCreature ||
                attachment.type == AttachmentType::PlayerCreature)
            {
                modelAttachment = &attachment;
                break;
            }
        }
        if (!modelAttachment)
        {
            WriteProbeLog("coop clone aborted: no creature attachment was found.");
            return -1;
        }

        const auto originalAttachmentType = modelAttachment->type;
        auto serializable = game->mpSerializableData.get();
        ResourceKey originalPlayerKey{};
        if (modelOverride && serializable)
        {
            originalPlayerKey = serializable->mPlayerCreatureKey;
            serializable->mPlayerCreatureKey = *modelOverride;
        }
        modelAttachment->type = AttachmentType::PlayerCreature;
        WriteProbeLog("coop clone: calling CreateCellObject with PlayerCreature attachment.");
        const auto index = Simulator::Cell::CreateCellObject(game->mpCellQuery,
            position, 0.0f, player->mCellResource, player->mScale, 1.0f, 1.0f);
        modelAttachment->type = originalAttachmentType;
        if (modelOverride && serializable)
            serializable->mPlayerCreatureKey = originalPlayerKey;
        WriteProbeLog("coop clone: CreateCellObject returned; attachment restored.");

        auto second = game->mCells.GetIfNotDeleted(index);
        if (!second) return -1;

        second->mTransform.SetScale(player->mTransform.GetScale());
        second->mTargetSize = player->mTargetSize;
        second->mTargetOpacity = player->mTargetOpacity;
        if (remoteControlled)
        {
            second->mIsIdle = true;
            second->mIsInvulnerable = true;
            second->field_112 = true;
            second->mFleeCellTime = 0.0f;
            second->mChaseCellTime = 0.0f;
        }
        return index;
    }

    void RemoveRemoteCell(const char* reason)
    {
        auto game = Simulator::Cell::cCellGame::Get();
        if (game && gRemoteCellIndex >= 0 &&
            game->mCells.GetIfNotDeleted(gRemoteCellIndex))
        {
            game->mCells.DeleteObject(gRemoteCellIndex);
            char line[256]{};
            sprintf_s(line, "Network clone deleted: index=%d reason=%s.",
                gRemoteCellIndex, reason ? reason : "unknown");
            WriteProbeLog(line);
        }
        gRemoteCellIndex = -1;
        gRemoteCellModel = 0;
        gRemoteCellModelType = 0;
        gRemoteCellModelGroup = 0;
        gRemoteCellResource = 0;
        gRemoteCreateRetryAfter = GetTickCount64() + 750;
    }

    CoopNet::CellProgress ReadCellProgress()
    {
        CoopNet::CellProgress result;
        auto game = Simulator::Cell::cCellGame::Get();
        auto data = game ? game->mpSerializableData.get() : nullptr;
        if (!data) return result;
        result.food = std::max(0, data->mFoodProgression);
        result.plantFood = std::max(0, data->mPlantFoodProgression);
        result.overPlantFood = std::max(0, data->mOverPlantFoodProgression);
        result.overAnimalFood = std::max(0, data->mOverAnimalFoodProgression);
        result.spent = std::max(0, data->mEvolutionPointsSpent);
        for (size_t i = 0; i < result.unlocks.size(); ++i)
            result.unlocks[i] = std::max(0, data->mUnlockedParts[i]);
        for (size_t i = 0; i < 6; ++i)
        {
            result.missions[i * 4] = std::max(0, data->missions[i].state);
            result.missions[i * 4 + 1] = std::max(0, data->missions[i].progress);
            result.missions[i * 4 + 2] = std::max(0, data->missions[i].plantProgress);
            result.missions[i * 4 + 3] = std::max(0, data->missions[i].field_C);
        }
        result.killCount = std::max(0, data->mKillCount);
        result.playerHasMoved = data->playerHasMoved;
        result.playerHasEaten = data->playerHasEaten;
        result.partCinematicPlayed = data->mPartCinematicPlayed;
        result.showMateButton = data->mShowMateButton;
        result.firstEditorEntry = data->mFirstEditorEntry;
        return result;
    }

    void ApplyCellProgress(const CoopNet::CellProgress& value)
    {
        auto game = Simulator::Cell::cCellGame::Get();
        auto data = game ? game->mpSerializableData.get() : nullptr;
        if (!data) return;
        data->mFoodProgression = value.food;
        data->mPlantFoodProgression = value.plantFood;
        data->mOverPlantFoodProgression = value.overPlantFood;
        data->mOverAnimalFoodProgression = value.overAnimalFood;
        data->mEvolutionPointsSpent = value.spent;
        for (size_t i = 0; i < value.unlocks.size(); ++i)
            data->mUnlockedParts[i] = value.unlocks[i];
        for (size_t i = 0; i < 6; ++i)
        {
            data->missions[i].state = value.missions[i * 4];
            data->missions[i].progress = value.missions[i * 4 + 1];
            data->missions[i].plantProgress = value.missions[i * 4 + 2];
            data->missions[i].field_C = value.missions[i * 4 + 3];
        }
        data->mKillCount = value.killCount;
        data->playerHasMoved = value.playerHasMoved;
        data->playerHasEaten = value.playerHasEaten;
        data->mPartCinematicPlayed = value.partCinematicPlayed;
        data->mShowMateButton = value.showMateButton;
        data->mFirstEditorEntry = value.firstEditorEntry;
    }

    bool HasProgressDelta(const CoopNet::CellProgress& value,
        const CoopNet::CellProgress& baseline,
        CoopNet::CellProgress& delta)
    {
        // Send monotonic counters as absolute values. The server merges them with
        // max(), so both clients observing the same food/tutorial event cannot
        // award it twice.
        delta.food = value.food;
        delta.plantFood = value.plantFood;
        delta.overPlantFood = value.overPlantFood;
        delta.overAnimalFood = value.overAnimalFood;
        delta.spent = value.spent - baseline.spent;
        bool changed = value.food != baseline.food ||
            value.plantFood != baseline.plantFood ||
            value.overPlantFood != baseline.overPlantFood ||
            value.overAnimalFood != baseline.overAnimalFood || delta.spent;
        for (size_t i = 0; i < value.unlocks.size(); ++i)
        {
            delta.unlocks[i] = 0;
            if (value.unlocks[i] > baseline.unlocks[i]) changed = true;
        }
        for (size_t i = 0; i < value.missions.size(); ++i)
        {
            delta.missions[i] = value.missions[i];
            if (value.missions[i] != baseline.missions[i]) changed = true;
        }
        delta.killCount = value.killCount;
        delta.playerHasMoved = value.playerHasMoved;
        delta.playerHasEaten = value.playerHasEaten;
        delta.partCinematicPlayed = value.partCinematicPlayed;
        delta.showMateButton = value.showMateButton;
        delta.firstEditorEntry = value.firstEditorEntry;
        if (value.killCount != baseline.killCount ||
            value.playerHasMoved != baseline.playerHasMoved ||
            value.playerHasEaten != baseline.playerHasEaten ||
            value.partCinematicPlayed != baseline.partCinematicPlayed ||
            value.showMateButton != baseline.showMateButton ||
            value.firstEditorEntry != baseline.firstEditorEntry) changed = true;
        return changed;
    }

    void UpdateSharedCellProgress(const CoopNet::Snapshot& snapshot)
    {
        auto game = Simulator::Cell::cCellGame::Get();
        auto data = game ? game->mpSerializableData.get() : nullptr;
        if (!data) return;

        const auto current = ReadCellProgress();
        if (!snapshot.progressInitialized)
        {
            if (std::strcmp(CoopNet::GetRole(), "host") == 0 && !gProgressSeedSent)
            {
                CoopNet::SeedProgress(current);
                gProgressSeedSent = true;
                gProgressBaseline = current;
                gHasProgressBaseline = true;
            }
            return;
        }

        if (!gHasProgressBaseline)
        {
            ApplyCellProgress(snapshot.progress);
            gProgressBaseline = snapshot.progress;
            gHasProgressBaseline = true;
            gAppliedProgressRevision = snapshot.revision;
            return;
        }

        CoopNet::CellProgress delta;
        if (HasProgressDelta(current, gProgressBaseline, delta))
            CoopNet::SubmitProgressDelta(delta, current.unlocks);

        if (snapshot.revision > gAppliedProgressRevision)
        {
            ApplyCellProgress(snapshot.progress);
            gProgressBaseline = snapshot.progress;
            gAppliedProgressRevision = snapshot.revision;
        }
        else gProgressBaseline = current;
    }

    bool UpdateLocalCellStability(Simulator::Cell::cCellObjectData* player,
        const ResourceKey& speciesKey, ULONG64 now)
    {
        const uint32_t resource = player && player->mCellResource
            ? player->mCellResource->mInstanceID : 0;
        const auto index = player ? player->Index() : -1;
        if (index != gLocalCellIndex || resource != gLocalCellResource ||
            !SameResourceKey(speciesKey, gLocalSpeciesKey))
        {
            if (gRemoteCellIndex >= 0)
                RemoveRemoteCell("local player cell was rebuilt");
            gLocalCellIndex = index;
            gLocalCellResource = resource;
            gLocalSpeciesKey = speciesKey;
            gLocalCellStableSince = now;
            gLocalAppearanceStableSince = now;
            return false;
        }
        return now - gLocalCellStableSince >= 750;
    }

    void SubmitLocalAppearance(const ResourceKey& speciesKey, ULONG64 now)
    {
        if (speciesKey.instanceID == 0) return;
        if (SameResourceKey(speciesKey, gLastSubmittedAppearanceKey) &&
            gLocalCellResource == gLastSubmittedAppearanceCellResource) return;
        if (now - gLocalAppearanceStableSince < 750) return;
        const std::string blob = SerializeCreation(speciesKey);
        if (blob.empty())
        {
            gLocalAppearanceStableSince = now;
            WriteProbeLog("Could not serialize the local creature appearance; will retry.");
            return;
        }
        CoopNet::SubmitAppearance(speciesKey.instanceID, speciesKey.typeID,
            speciesKey.groupID, blob);
        gLastSubmittedAppearanceKey = speciesKey;
        gLastSubmittedAppearanceCellResource = gLocalCellResource;
        WriteProbeLog("Submitted the complete local creature appearance.");
    }

    void ApplyRemoteAppearance(const CoopNet::Snapshot& snapshot)
    {
        if (snapshot.remoteAppearanceSequence <= gAppliedRemoteAppearanceSequence ||
            snapshot.remoteAppearanceBlob.empty()) return;

        cEditorResourcePtr resource = new Editors::cEditorResource();
        if (!DeserializeEditorResource(snapshot.remoteAppearanceBlob, resource.get()))
        {
            WriteProbeLog("Rejected an invalid remote creature appearance.");
            gAppliedRemoteAppearanceSequence = snapshot.remoteAppearanceSequence;
            return;
        }

        ResourceKey cachedKey(snapshot.remoteModelInstance ^ 0x5C0F0000,
            snapshot.remoteModelType, snapshot.remoteModelGroup);
        if (cachedKey.instanceID == 0) cachedKey.instanceID = 0x5C0F0001;
        resource->SetResourceKey(cachedKey);
        if (!ResourceManager.CacheResource(resource.get(), true))
        {
            WriteProbeLog("Could not cache the remote creature appearance.");
            gAppliedRemoteAppearanceSequence = snapshot.remoteAppearanceSequence;
            return;
        }

        gRemoteAppearanceResource = resource;
        gRemoteCreationKey = cachedKey;
        gAppliedRemoteAppearanceSequence = snapshot.remoteAppearanceSequence;
        gPendingRemoteModel = snapshot.remoteModelInstance;
        gPendingRemoteResource = snapshot.remoteCellResource;
        gRemoteAppearanceStableSince = GetTickCount64();
        if (gRemoteCellIndex >= 0) RemoveRemoteCell("complete remote appearance received");
        WriteProbeLog("Cached the complete remote creature appearance.");
    }

    void UpdateRemoteCell(const CoopNet::Snapshot& snapshot)
    {
        auto game = Simulator::Cell::cCellGame::Get();
        auto player = Simulator::Cell::GetPlayerCell();
        if (!game || !player || !snapshot.hasRemotePosition ||
            !snapshot.hasRemoteAppearance) return;

        const ULONG64 now = GetTickCount64();
        if (snapshot.remotePositionReceivedTick == 0 ||
            now - snapshot.remotePositionReceivedTick > 1000)
        {
            if (gRemoteCellIndex >= 0)
                RemoveRemoteCell("remote position stream became stale");
            return;
        }
        auto serializable = game->mpSerializableData.get();
        const ResourceKey localSpecies = serializable
            ? serializable->mPlayerCreatureKey : player->mModelKey;
        if (!UpdateLocalCellStability(player, localSpecies, now)) return;
        if (snapshot.remoteTargetSize > player->mTargetSize)
            player->mTargetSize = snapshot.remoteTargetSize;
        if (snapshot.remoteScale > player->mTransform.GetScale())
            player->mTransform.SetScale(snapshot.remoteScale);
        ApplyRemoteAppearance(snapshot);
        // Position packets contain the peer's resource IDs, but those IDs are not
        // guaranteed to resolve to the same creation in another profile. Never
        // build a clone until the complete appearance has been decoded and cached.
        if (gRemoteCreationKey.instanceID == 0 || !gRemoteAppearanceResource) return;
        if (gPendingRemoteModel != snapshot.remoteModelInstance ||
            gPendingRemoteResource != snapshot.remoteCellResource)
        {
            gPendingRemoteModel = snapshot.remoteModelInstance;
            gPendingRemoteResource = snapshot.remoteCellResource;
            gRemoteAppearanceStableSince = now;
            return;
        }
        // Growth rebuilds the player's cell and its GFX over several frames. Do not
        // delete or create another object until the peer's identity is stable.
        if (now - gRemoteAppearanceStableSince < 750) return;

        auto remote = gRemoteCellIndex >= 0
            ? game->mCells.GetIfNotDeleted(gRemoteCellIndex) : nullptr;
        if (remote && (gRemoteCellModel != snapshot.remoteModelInstance ||
            gRemoteCellModelType != snapshot.remoteModelType ||
            gRemoteCellModelGroup != snapshot.remoteModelGroup ||
            gRemoteCellResource != snapshot.remoteCellResource))
        {
            RemoveRemoteCell("remote appearance changed");
            remote = nullptr;
        }

        const Math::Vector3 remotePosition(
            snapshot.remoteX, snapshot.remoteY, snapshot.remoteZ);
        if (!remote)
        {
            if (now < gRemoteCreateRetryAfter) return;
            const ResourceKey remoteModel = gRemoteCreationKey;
            gRemoteCellIndex = CreatePlayerCellClone(player, remotePosition, true,
                &remoteModel);
            remote = gRemoteCellIndex >= 0
                ? game->mCells.GetIfNotDeleted(gRemoteCellIndex) : nullptr;
            if (!remote)
            {
                gRemoteCellIndex = -1;
                gRemoteCreateRetryAfter = now + 1000;
                return;
            }
            gRemoteCellModel = snapshot.remoteModelInstance;
            gRemoteCellModelType = snapshot.remoteModelType;
            gRemoteCellModelGroup = snapshot.remoteModelGroup;
            gRemoteCellResource = snapshot.remoteCellResource;
            char line[256]{};
            sprintf_s(line, "Network clone created: role=%s index=%d model=%08X.",
                CoopNet::GetRole(), gRemoteCellIndex, gRemoteCellModel);
            WriteProbeLog(line);
        }

        if (!remote) return;

        remote->mIsIdle = true;
        remote->mTargetPosition = remotePosition;
        remote->mTransform.SetOffset(remotePosition);
        remote->field_84 = remotePosition;
        remote->field_90 = Math::Vector3(0.0f, 0.0f, 0.0f);
        remote->mTransform.SetScale(snapshot.remoteScale);
        remote->mTargetSize = snapshot.remoteTargetSize;
        remote->mTargetOpacity = snapshot.remoteOpacity;
    }

    std::string Base64Encode(const std::vector<unsigned char>& bytes)
    {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(((bytes.size() + 2) / 3) * 4);
        for (size_t i = 0; i < bytes.size(); i += 3)
        {
            const unsigned a = bytes[i];
            const unsigned b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
            const unsigned c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
            const unsigned value = (a << 16) | (b << 8) | c;
            result.push_back(alphabet[(value >> 18) & 63]);
            result.push_back(alphabet[(value >> 12) & 63]);
            result.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
            result.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
        }
        return result;
    }

    bool Base64Decode(const std::string& text, std::vector<unsigned char>& bytes)
    {
        if (text.empty() || text.size() % 4 != 0) return false;
        std::array<int, 256> table{};
        table.fill(-1);
        const char* alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            table[static_cast<unsigned char>(alphabet[i])] = i;

        std::vector<unsigned char> result;
        result.reserve((text.size() / 4) * 3);
        for (size_t i = 0; i < text.size(); i += 4)
        {
            const bool pad2 = text[i + 2] == '=';
            const bool pad3 = text[i + 3] == '=';
            if ((pad2 && !pad3) || (i + 4 != text.size() && (pad2 || pad3))) return false;
            const int a = table[static_cast<unsigned char>(text[i])];
            const int b = table[static_cast<unsigned char>(text[i + 1])];
            const int c = pad2 ? 0 : table[static_cast<unsigned char>(text[i + 2])];
            const int d = pad3 ? 0 : table[static_cast<unsigned char>(text[i + 3])];
            if (a < 0 || b < 0 || c < 0 || d < 0) return false;
            const unsigned value = (static_cast<unsigned>(a) << 18) |
                (static_cast<unsigned>(b) << 12) |
                (static_cast<unsigned>(c) << 6) | static_cast<unsigned>(d);
            result.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
            if (!pad2) result.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
            if (!pad3) result.push_back(static_cast<unsigned char>(value & 0xff));
        }
        bytes = std::move(result);
        return true;
    }

    template <typename T>
    void AppendBinary(std::vector<unsigned char>& bytes, const T& value)
    {
        const auto* start = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), start, start + sizeof(T));
    }

    std::string SerializeEditorResource(Editors::cEditorResource* resource)
    {
        if (!resource) return {};
        const uint32_t count = static_cast<uint32_t>(resource->mBlocks.size());
        if (count > 512) return {};

        std::vector<unsigned char> bytes;
        bytes.reserve(8 + sizeof(resource->mProperties) +
            static_cast<size_t>(count) * sizeof(Editors::cEditorResourceBlock));
        const uint32_t magic = 0x31504353; // SCP1
        AppendBinary(bytes, magic);
        AppendBinary(bytes, count);
        AppendBinary(bytes, resource->mProperties);
        for (const auto& block : resource->mBlocks) AppendBinary(bytes, block);
        if (bytes.size() > 262144) return {};
        return Base64Encode(bytes);
    }

    bool DeserializeEditorResource(const std::string& blob,
        Editors::cEditorResource* resource)
    {
        if (!resource) return false;
        std::vector<unsigned char> bytes;
        if (!Base64Decode(blob, bytes) ||
            bytes.size() < 8 + sizeof(Editors::cEditorResourceProperties)) return false;

        uint32_t magic = 0;
        uint32_t count = 0;
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        std::memcpy(&count, bytes.data() + 4, sizeof(count));
        const size_t expected = 8 + sizeof(Editors::cEditorResourceProperties) +
            static_cast<size_t>(count) * sizeof(Editors::cEditorResourceBlock);
        if (magic != 0x31504353 || count > 512 || bytes.size() != expected) return false;

        size_t offset = 8;
        std::memcpy(&resource->mProperties, bytes.data() + offset,
            sizeof(resource->mProperties));
        offset += sizeof(resource->mProperties);
        resource->mBlocks.resize(count);
        if (count)
            std::memcpy(resource->mBlocks.data(), bytes.data() + offset,
                static_cast<size_t>(count) * sizeof(Editors::cEditorResourceBlock));
        return true;
    }

    std::string SerializeCreation(const ResourceKey& key)
    {
        ResourceObjectPtr raw;
        if (!ResourceManager.GetResource(key, &raw) || !raw) return {};
        auto resource = object_cast<Editors::cEditorResource>(raw.get());
        return SerializeEditorResource(resource);
    }

    std::string SerializeEditorModel()
    {
        auto editor = Editors::GetEditor();
        if (!editor || !editor->IsActive() || !editor->GetEditorModel()) return {};

        cEditorResourcePtr resource = new Editors::cEditorResource();
        editor->GetEditorModel()->Save(resource.get());
        return SerializeEditorResource(resource.get());
    }

    bool ApplyEditorModel(const std::string& blob)
    {
        auto editor = Editors::GetEditor();
        if (!editor || !editor->IsActive() || !editor->GetEditorModel()) return false;
        cEditorResourcePtr resource = new Editors::cEditorResource();
        if (!DeserializeEditorResource(blob, resource.get())) return false;

        auto model = editor->GetEditorModel();
        model->Load(resource.get());
        editor->SetEditorModel(model);
        editor->CommitEditHistory(true);
        WriteProbeLog("Applied a remote editor model snapshot.");
        return true;
    }

    void OpenMirroredEditor(uint32_t editorID)
    {
        if (!Simulator::IsCellGame()) return;
        auto game = Simulator::Cell::cCellGame::Get();
        auto data = game ? game->mpSerializableData.get() : nullptr;
        if (!data) return;

        Simulator::EnterEditorMessage message(editorID, data->mPlayerCreatureKey);
        gSuppressEditorRelay = true;
        MessageManager.MessageSend(Simulator::kMsgEnterEditor, &message);
        gSuppressEditorRelay = false;
        WriteProbeLog("Opened the mirrored cooperative editor.");
    }

    class EditorRelayListener : public App::DefaultMessageListener
    {
    public:
        bool HandleMessage(uint32_t messageID, void* value) override
        {
            if (messageID != Simulator::kMsgEnterEditor || !value || gSuppressEditorRelay)
                return false;
            const auto snapshot = CoopNet::GetSnapshot();
            if (!snapshot.enabled || !snapshot.connected) return false;
            const auto* message = static_cast<Simulator::EnterEditorMessage*>(value);
            CoopNet::SubmitEditorOpen(message->mEditorID);
            WriteProbeLog("Relayed local editor entry to the cooperative peer.");
            return false;
        }
    };

    void UpdateSharedEditor(const CoopNet::Snapshot& snapshot, ULONG64 now)
    {
        auto editor = Editors::GetEditor();
        const bool editorActive = Simulator::IsEditorMode() && editor && editor->IsActive();

        if (snapshot.editorOpen && snapshot.editorRole != CoopNet::GetRole() &&
            !editorActive && Simulator::IsCellGame() && snapshot.editorID != 0)
        {
            OpenMirroredEditor(snapshot.editorID);
            return;
        }

        if (editorActive && !gWasEditorMode && !snapshot.editorOpen)
            CoopNet::SubmitEditorOpen(editor->mEditorName);

        if (editorActive && snapshot.speciesSequence > gAppliedSpeciesSequence &&
            !snapshot.speciesBlob.empty() && snapshot.speciesBlob != gLastLocalSpecies)
        {
            if (ApplyEditorModel(snapshot.speciesBlob))
            {
                gLastLocalSpecies = snapshot.speciesBlob;
                gAppliedSpeciesSequence = snapshot.speciesSequence;
            }
        }

        if (editorActive && now - gLastSpeciesTick >= 250)
        {
            const std::string current = SerializeEditorModel();
            if (!current.empty() && current != gLastLocalSpecies)
            {
                gLastLocalSpecies = current;
                CoopNet::SubmitSpecies(current);
            }
            gLastSpeciesTick = now;
        }

        if (!editorActive && gWasEditorMode)
        {
            CoopNet::SubmitEditorClose(gLastLocalSpecies);
            gLastLocalSpecies.clear();
            gAppliedSpeciesSequence = 0;
            gLastSubmittedAppearanceKey = ResourceKey{};
            gLastSubmittedAppearanceCellResource = 0;
            gLocalAppearanceStableSince = now;
        }
        gWasEditorMode = editorActive;
    }

    constexpr uint32_t kInviteButtonID = 0x5C0F1001;
    constexpr uint32_t kAcceptInviteButtonID = 0x5C0F1002;
    constexpr uint32_t kDeclineInviteButtonID = 0x5C0F1003;

    class InviteWinProc : public UTFWin::DefaultWinProc<>
    {
    public:
        bool HandleUIMessage(UTFWin::IWindow* window,
            const UTFWin::Message& message) override
        {
            if (!window || !message.IsType(UTFWin::kMsgButtonClick)) return false;
            switch (window->GetControlID())
            {
            case kInviteButtonID:
                CoopNet::SubmitInvite();
                window->SetCaption(u"Invitation sent");
                WriteProbeLog("Host sent a cooperative invitation.");
                return true;
            case kAcceptInviteButtonID:
                CoopNet::SubmitInviteResponse(true);
                gInviteResponseSent = true;
                WriteProbeLog("Guest accepted the cooperative invitation.");
                return true;
            case kDeclineInviteButtonID:
                CoopNet::SubmitInviteResponse(false);
                gInviteResponseSent = true;
                WriteProbeLog("Guest declined the cooperative invitation.");
                return true;
            default:
                return false;
            }
        }
    };

    IWindowPtr CreateCoopButton(uint32_t controlID, const char16_t* caption)
    {
        auto mainWindow = WindowManager.GetMainWindow();
        if (!mainWindow) return nullptr;
        auto window = static_cast<UTFWin::IWindow*>(
            ClassManager.Create(UTFWin::IButton::WinButton_ID));
        if (!window) return nullptr;
        window->SetControlID(controlID);
        window->SetCommandID(controlID);
        window->SetCaption(caption);
        window->SetTextFontID(0x00AEBB69);
        window->SetFillColor(Math::Color(0xE02C4768));
        window->SetDrawable(new UTFWin::ButtonDrawableStandard());
        window->SetFlag(UTFWin::kWinFlagVisible, true);
        window->SetFlag(UTFWin::kWinFlagEnabled, true);
        window->SetFlag(UTFWin::kWinFlagAlwaysInFront, true);
        window->SetFlag(UTFWin::kWinFlagIgnoreMouse, false);
        window->AddWinProc(gInviteWinProc.get());
        mainWindow->AddWindow(window);
        mainWindow->BringToFront(window);
        return IWindowPtr(window);
    }

    void PositionCoopButton(UTFWin::IWindow* window, float yOffset)
    {
        if (!window) return;
        auto mainWindow = WindowManager.GetMainWindow();
        if (!mainWindow) return;
        const auto& area = mainWindow->GetArea();
        const float width = 260.0f;
        const float height = 42.0f;
        const float left = (area.GetWidth() - width) * 0.5f;
        const float top = area.GetHeight() * 0.5f + yOffset;
        window->SetArea({ left, top, left + width, top + height });
    }

    void SetButtonVisible(UTFWin::IWindow* window, bool visible)
    {
        if (window) window->SetFlag(UTFWin::kWinFlagVisible, visible);
    }

    struct PausePanelCandidate
    {
        UTFWin::IWindow* window = nullptr;
        int score = -1;
        int visibleButtons = 0;
        int disabledButtons = 0;
    };

    void CountVisibleButtons(UTFWin::IWindow* window, int depth,
        int& visibleButtons, int& disabledButtons)
    {
        if (!window || depth > 16 || !window->IsVisible()) return;
        const char* component = window->GetComponentName();
        if (component && std::strstr(component, "Button"))
        {
            ++visibleButtons;
            if (!window->IsEnabled()) ++disabledButtons;
        }
        for (auto child : window->children())
            CountVisibleButtons(child, depth + 1, visibleButtons, disabledButtons);
    }

    void FindVisiblePausePanelRecursive(UTFWin::IWindow* window,
        UTFWin::IWindow* root, float rootWidth, float rootHeight, int depth,
        PausePanelCandidate& best)
    {
        if (!window || depth > 12 || !window->IsVisible()) return;
        if (window != root)
        {
            const auto& area = window->GetArea();
            const float width = area.GetWidth();
            const float height = area.GetHeight();
            if (width >= 220.0f && height >= 240.0f &&
                width <= rootWidth * 0.80f && height <= rootHeight * 0.95f)
            {
                const auto origin = window->ToGlobalCoordinates(Math::Point(0.0f, 0.0f));
                const float centerX = origin.x + width * 0.5f;
                const float centerY = origin.y + height * 0.5f;
                if (std::abs(centerX - rootWidth * 0.5f) <= rootWidth * 0.30f &&
                    std::abs(centerY - rootHeight * 0.5f) <= rootHeight * 0.30f)
                {
                    int visibleButtons = 0;
                    int disabledButtons = 0;
                    CountVisibleButtons(window, 0, visibleButtons, disabledButtons);
                    if (visibleButtons >= 5 && disabledButtons >= 1)
                    {
                        const int score = visibleButtons * 100 + disabledButtons * 25 -
                            static_cast<int>((width * height) / 10000.0f);
                        if (score > best.score)
                        {
                            best.window = window;
                            best.score = score;
                            best.visibleButtons = visibleButtons;
                            best.disabledButtons = disabledButtons;
                        }
                    }
                }
            }
        }
        for (auto child : window->children())
            FindVisiblePausePanelRecursive(child, root, rootWidth, rootHeight,
                depth + 1, best);
    }

    UTFWin::IWindow* FindVisiblePausePanel()
    {
        auto root = WindowManager.GetMainWindow();
        if (!root) return nullptr;
        const auto& area = root->GetArea();
        PausePanelCandidate best;
        FindVisiblePausePanelRecursive(root, root, area.GetWidth(), area.GetHeight(),
            0, best);
        return best.window;
    }

    void PositionInviteButton(UTFWin::IWindow* button, UTFWin::IWindow* panel)
    {
        if (!button || !panel) return;
        auto root = WindowManager.GetMainWindow();
        if (!root) return;
        const auto& rootArea = root->GetArea();
        const auto& panelArea = panel->GetArea();
        const auto origin = panel->ToGlobalCoordinates(Math::Point(0.0f, 0.0f));
        const float width = 260.0f;
        const float height = 42.0f;
        float left = origin.x + (panelArea.GetWidth() - width) * 0.5f;
        float top = origin.y + panelArea.GetHeight() + 8.0f;
        if (top + height > rootArea.GetHeight() - 8.0f)
            top = origin.y - height - 8.0f;
        left = std::max(8.0f,
            std::min(left, rootArea.GetWidth() - width - 8.0f));
        top = std::max(8.0f,
            std::min(top, rootArea.GetHeight() - height - 8.0f));
        button->SetArea({ left, top, left + width, top + height });
    }

    void UpdateInviteUI(const CoopNet::Snapshot& snapshot)
    {
        const bool isHost = std::strcmp(CoopNet::GetRole(), "host") == 0;
        const bool stageGame = Simulator::IsStageGameMode();
        auto pausePanel = stageGame && !Simulator::IsEditorMode()
            ? FindVisiblePausePanel() : nullptr;
        const bool pauseMenuOpen = pausePanel != nullptr;
        const bool showHostInvite = isHost && pauseMenuOpen;
        if (showHostInvite && !gInviteButton)
            gInviteButton = CreateCoopButton(kInviteButtonID, u"Invite friend to co-op");
        PositionInviteButton(gInviteButton.get(), pausePanel);
        SetButtonVisible(gInviteButton.get(), showHostInvite);
        auto mainWindow = WindowManager.GetMainWindow();
        if (showHostInvite && mainWindow && gInviteButton)
            mainWindow->BringToFront(gInviteButton.get());

        const bool showGuestPrompt = !isHost && snapshot.invitePending &&
            !gInviteResponseSent;
        if (showGuestPrompt && !gAcceptInviteButton)
            gAcceptInviteButton = CreateCoopButton(kAcceptInviteButtonID,
                u"Accept co-op invitation");
        if (showGuestPrompt && !gDeclineInviteButton)
            gDeclineInviteButton = CreateCoopButton(kDeclineInviteButtonID,
                u"Decline invitation");
        PositionCoopButton(gAcceptInviteButton.get(), -25.0f);
        PositionCoopButton(gDeclineInviteButton.get(), 25.0f);
        SetButtonVisible(gAcceptInviteButton.get(), showGuestPrompt);
        SetButtonVisible(gDeclineInviteButton.get(), showGuestPrompt);
        if (showGuestPrompt && mainWindow)
        {
            if (gAcceptInviteButton) mainWindow->BringToFront(gAcceptInviteButton.get());
            if (gDeclineInviteButton) mainWindow->BringToFront(gDeclineInviteButton.get());
        }
        if (!snapshot.invitePending) gInviteResponseSent = false;

        const int state = (isHost ? 1 : 0) | (pauseMenuOpen ? 2 : 0) |
            (stageGame ? 4 : 0) | (showHostInvite ? 8 : 0) |
            (gInviteButton ? 16 : 0);
        static int previousState = -1;
        if (state != previousState)
        {
            previousState = state;
            char line[256]{};
            sprintf_s(line,
                "Invite UI: role=%s panel=%d stage=%d visible=%d button=%d panelId=0x%08X panelType=%s.",
                CoopNet::GetRole(), pauseMenuOpen ? 1 : 0, stageGame ? 1 : 0,
                showHostInvite ? 1 : 0, gInviteButton ? 1 : 0,
                pausePanel ? pausePanel->GetControlID() : 0,
                pausePanel && pausePanel->GetComponentName()
                    ? pausePanel->GetComponentName() : "none");
            WriteProbeLog(line);
        }
    }

    void CoopUpdate()
    {
        const auto snapshot = CoopNet::GetSnapshot();
        if (!snapshot.enabled || !snapshot.connected)
        {
            SetButtonVisible(gInviteButton.get(), false);
            SetButtonVisible(gAcceptInviteButton.get(), false);
            SetButtonVisible(gDeclineInviteButton.get(), false);
            if (gRemoteCellIndex >= 0)
                RemoveRemoteCell("cooperative peer disconnected");
            return;
        }

        if (snapshot.connectionGeneration != gObservedConnectionGeneration)
        {
            if (gRemoteCellIndex >= 0)
                RemoveRemoteCell("cooperative connection restarted");
            gObservedConnectionGeneration = snapshot.connectionGeneration;
            gLastSubmittedAppearanceKey = ResourceKey{};
            gLastSubmittedAppearanceCellResource = 0;
            gAppliedRemoteAppearanceSequence = 0;
            gRemoteCreationKey = ResourceKey{};
            gRemoteAppearanceResource = nullptr;
            gPendingRemoteModel = 0;
            gPendingRemoteResource = 0;
            gRemoteAppearanceStableSince = 0;
            gProgressSeedSent = false;
            gHasProgressBaseline = false;
            gAppliedProgressRevision = 0;
            gProgressBaseline = CoopNet::CellProgress{};
            gAppliedSpeciesSequence = 0;
            gLastLocalSpecies.clear();
            gLastSpeciesTick = 0;
            gInviteResponseSent = false;
            gLastNetworkError.clear();
            WriteProbeLog("Cooperative connection state reset after handshake.");
        }

        if (!snapshot.lastError.empty() && snapshot.lastError != gLastNetworkError)
        {
            gLastNetworkError = snapshot.lastError;
            const std::string line = "Cooperative server error: " + snapshot.lastError;
            WriteProbeLog(line.c_str());
        }

        UpdateInviteUI(snapshot);

        // The host starts sending positions only after its campaign is loaded.
        // At that point the isolated guest profile already contains the same
        // save, so it can join without the player selecting a planet twice.
        if (IsProfile2() && snapshot.inviteAccepted && snapshot.hasRemotePosition &&
            !gAutoJoinAttempted &&
            !Simulator::IsStageGameMode() && !Simulator::IsLoadingGameMode() &&
            !Simulator::IsEditorMode())
        {
            gAutoJoinAttempted = true;
            WriteProbeLog("coop auto-join: host entered a world; loading the guest copy.");
            JoinSavedWorld();
            return;
        }

        const ULONG64 now = GetTickCount64();
        UpdateSharedEditor(snapshot, now);

        if (Simulator::IsCellGame() && !snapshot.editorOpen)
        {
            if (!snapshot.hasRemotePosition && gRemoteCellIndex >= 0)
                RemoveRemoteCell("cooperative peer left");
            auto player = Simulator::Cell::GetPlayerCell();
            if (player && now - gLastPositionTick >= 50)
            {
                auto game = Simulator::Cell::cCellGame::Get();
                auto data = game ? game->mpSerializableData.get() : nullptr;
                const ResourceKey speciesKey = data
                    ? data->mPlayerCreatureKey : player->mModelKey;
                UpdateLocalCellStability(player, speciesKey, now);
                SubmitLocalAppearance(speciesKey, now);
                const auto& position = player->GetPosition();
                CoopNet::SubmitPosition(position.x, position.y, position.z,
                    speciesKey.instanceID, speciesKey.typeID,
                    speciesKey.groupID,
                    player->mCellResource ? player->mCellResource->mInstanceID : 0,
                    player->mTransform.GetScale(), player->mTargetSize,
                    player->mTargetOpacity);
                gLastPositionTick = now;
            }
            UpdateRemoteCell(snapshot);
        }
        else if (gRemoteCellIndex >= 0)
        {
            RemoveRemoteCell("left cell gameplay or entered editor");
            gPendingRemoteModel = 0;
            gPendingRemoteResource = 0;
            gRemoteAppearanceStableSince = 0;
        }

        if (now - gLastProgressTick >= 200)
        {
            UpdateSharedCellProgress(snapshot);
            gLastProgressTick = now;
        }
    }

    void Spawn()
    {
        if (Simulator::IsCellGame())
        {
            auto game = Simulator::Cell::cCellGame::Get();
            if (!game || !game->mpCellQuery) return;
            auto player = Simulator::Cell::GetPlayerCell();
            if (!player || !player->mCellResource) return;

            auto index = CreatePlayerCellClone(player,
                player->GetPosition() + Math::Vector3(4.0f, 0.0f, 0.0f), false);

            auto second = game->mCells.GetIfNotDeleted(index);
            if (!second)
            {
                App::ConsolePrintF("NPC cell creation failed (index %d).", index);
                return;
            }

            // CreateCellObject uses the resource's default size. The player can be
            // at an intermediate growth size, so copy both current and target size.
            second->mTransform.SetScale(player->mTransform.GetScale());
            second->mTargetSize = player->mTargetSize;
            second->mTargetOpacity = player->mTargetOpacity;

            char logLine[512]{};
            sprintf_s(logLine,
                "coopSpawn cell index=%d playerModel=%08X npcModel=%08X playerScale=%.4f npcScale=%.4f playerTargetSize=%.4f npcTargetSize=%.4f",
                index, player->mModelKey.instanceID, second->mModelKey.instanceID,
                player->mTransform.GetScale(), second->mTransform.GetScale(),
                player->mTargetSize, second->mTargetSize);
            WriteProbeLog(logLine);
            App::ConsolePrintF(
                "NPC cell %d cloned: player model 0x%x, NPC model 0x%x, scale %.3f.",
                index, player->mModelKey.instanceID, second->mModelKey.instanceID,
                second->mTransform.GetScale());
        }
        else if (Simulator::IsCreatureGame())
        {
            auto manager = Simulator::cGameNounManager::Get();
            if (!manager) return;
            auto player = manager->GetAvatar();
            if (!player || !player->mpSpeciesProfile) return;
            cCreatureAnimalPtr second = Simulator::cCreatureAnimal::Create(
                player->GetPosition() + Math::Vector3(4.0f, 0.0f, 0.0f),
                player->mpSpeciesProfile, player->mAge, nullptr, false, false);
            App::ConsolePrintF(second ? "NPC creature created. Check its species and AI manually."
                : "NPC creature creation failed.");
        }
        else App::ConsolePrintF("Load a disposable Cell or Creature campaign before using coopSpawn.");
    }

    bool FindNewestSavedGame(eastl::string16& gameName)
    {
        wchar_t appData[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return false;

        std::wstring pattern(appData);
        pattern += IsProfile2()
            ? L"\\SporeCoop2\\Games\\Game0\\*.spo"
            : L"\\Spore\\Games\\Game0\\*.spo";

        WIN32_FIND_DATAW current{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &current);
        if (search == INVALID_HANDLE_VALUE) return false;

        std::wstring newest;
        FILETIME newestTime{};
        do
        {
            if ((current.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            if (newest.empty() || CompareFileTime(&current.ftLastWriteTime, &newestTime) > 0)
            {
                newest = current.cFileName;
                newestTime = current.ftLastWriteTime;
            }
        } while (FindNextFileW(search, &current));
        FindClose(search);

        if (newest.size() <= 4) return false;
        newest.resize(newest.size() - 4); // Remove .spo.
        gameName.clear();
        for (wchar_t character : newest)
            gameName.push_back(static_cast<char16_t>(character));
        return !gameName.empty();
    }

    void JoinSavedWorld()
    {
        if (Simulator::IsStageGameMode())
        {
            App::ConsolePrintF("coopJoin: this window is already inside a world.");
            return;
        }
        if (Simulator::IsLoadingGameMode() || Simulator::IsEditorMode())
        {
            App::ConsolePrintF("coopJoin: wait until loading or the editor has finished.");
            return;
        }

        eastl::string16 gameName;
        if (!FindNewestSavedGame(gameName))
        {
            App::ConsolePrintF("coopJoin: no .spo campaign was found in this profile.");
            return;
        }

        Simulator::GameLoadParameters parameters{};
        parameters.mGameName = gameName;
        GameNounManager.EnsurePlayer();
        const bool accepted = GamePersistenceManager.LoadGame(parameters);
        WriteProbeLog(accepted
            ? "coopJoin: newest saved campaign accepted by GamePersistenceManager."
            : "coopJoin: GamePersistenceManager refused the saved campaign.");
        App::ConsolePrintF(accepted
            ? "coopJoin: loading the newest saved campaign..."
            : "coopJoin: Spore refused to load the saved campaign.");
    }

    class ProbeCommand : public ArgScript::ICommand
    {
        bool mSpawn;
    public:
        explicit ProbeCommand(bool spawn) : mSpawn(spawn) {}
        void ParseLine(const ArgScript::Line&) override
        {
            if (mSpawn) Spawn();
            else Status();
        }
        const char* GetDescription(ArgScript::DescriptionMode) const override
        {
            return mSpawn ? "Experimental creation of a same-species NPC. Test only on a disposable campaign."
                : "Print current Cell or Creature avatar state.";
        }
    };

    class JoinCommand : public ArgScript::ICommand
    {
    public:
        void ParseLine(const ArgScript::Line&) override
        {
            JoinSavedWorld();
        }
        const char* GetDescription(ArgScript::DescriptionMode) const override
        {
            return "Load the newest campaign in this window, then attach it to the cooperative session.";
        }
    };

    void Initialize()
    {
        CheatManager.AddCheat("coopStatus", new ProbeCommand(false));
        CheatManager.AddCheat("coopSpawn", new ProbeCommand(true));
        CheatManager.AddCheat("coopJoin", new JoinCommand());
        gInviteWinProc = new InviteWinProc();
        gUpdateListener = App::AddUpdateFunction(CoopUpdate);
        gEditorListener = new EditorRelayListener();
        MessageManager.AddListener(gEditorListener.get(), Simulator::kMsgEnterEditor);
        const bool networkEnabled = CoopNet::StartFromEnvironment();
        WriteProbeLog(networkEnabled
            ? "Initialize completed; commands registered and cooperative client started."
            : "Initialize completed; commands registered; cooperative client disabled (no environment)."
        );
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        PrepareDetours(module);
        if (IsProfile2())
        {
            gCreateMutexAOriginal = reinterpret_cast<CreateMutexAFunction>(
                GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateMutexA"));
            if (gCreateMutexAOriginal)
            {
                gMutexDetourAttached = DetourAttach(
                    reinterpret_cast<PVOID*>(&gCreateMutexAOriginal),
                    CreateMutexAHook) == NO_ERROR;
            }
        }
        ProfilePathsDetour::attach(GetAddress(App::cAppSystem, SetUserDirNames));
        CommitDetours();
        ModAPI::AddPostInitFunction(Initialize);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        PrepareDetours(module);
        if (gMutexDetourAttached)
        {
            DetourDetach(reinterpret_cast<PVOID*>(&gCreateMutexAOriginal),
                CreateMutexAHook);
        }
        ProfilePathsDetour::detach();
        CommitDetours();
    }
    return TRUE;
}
