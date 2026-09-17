/****************************************************************************
* Copyright (C) 2019 Eric Mor
*
* This file is part of Spore ModAPI.
*
* Spore ModAPI is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
****************************************************************************/
#pragma once

#include <Spore\Graphics\cCubeMapIndex.h>
#include <Spore\Simulator\SubSystem\cStrategy.h>
#include <Spore\Simulator\cCity.h>
#include <Spore\Terrain\cTerrainSphere.h>
#include <Spore\MathUtils.h>

/// Access the active Simulator planet manager.
#define PlanetModel (*Simulator::cPlanetModel::Get())

namespace Simulator
{
	using namespace Math;
	
	struct cPoliticalZone
	{
		uint32_t mPoliticalID;
		Vector3 mCenter;
		bool mCityZone;
		bool mVisible;
		cCity* mCity;
	};

	//sizeof 0x13208
	class cPlanetModel
		: public App::IMessageListener
		, public cStrategy
	{
	public:
		struct cLandTradeRoute
		{
			cCity* city1;
			cCity* city2;
			eastl::vector<Vector3> path;
		};

		struct cBoundaryPointInfo
		{
			Vector3 mDisplacedPoint;
			int8_t mZoneIdxs[4];
			bool mVisited[4];
		};

		struct cCubeCornerHash
		{
			size_t operator()(const Graphics::cCubeMapIndex& c) const
			{
				return c.mFace * 0xfd + c.mCol * 0x25 + c.mRow;
			}
		};

		struct cCubeMapIndexEqual
		{
			bool operator()(const Graphics::cCubeMapIndex& a, const Graphics::cCubeMapIndex& b) const
			{
				return (a.mFace == b.mFace) && (a.mCol == b.mCol) && (a.mRow == b.mRow);
			}
		};

		//TODO PLACEHOLDER

		// B7DC40 Get water level?

		/// Returns true if the given position is above or inside the water mass in the planet.
		/// To be exact, this method returns true if the water level is greater than GetHeightAt(position)
		bool IsInWater(const Vector3& position);

		cCity* GetNearestCity(const Vector3& position);

		/// Returns the position in the surface of the planet that is aligned with the center of the planet
		/// and the given position.
		Vector3 ToSurface(const Vector3& position);
		
		float GetHeightAt(const Vector3& position);

		/// Returns the gravity acceleration used. If the game is on the scenario mode, the 'scenarioGravity' will
		/// used. Otherwise, it returns -9.8
		float GetGravity() const;

		Quaternion GetOrientation(const Vector3& position, const Vector3& direction) const;

	public:
		static cPlanetModel* Get();

	public:
		//TODO
		/* 20h */	cTerrainSpherePtr mpSphere;
		/* 24h */	ITerrainPtr mpTerrain;
		/* 28h */	float mWaterFraction;
		/* 2Ch */	ResourceKey mPlanetKey;
		/* 38h */	eastl::string16 mPlanetName;
		/* 48h */	bool mbAddedMessageListeners;
		/* 4Ch */	uint32_t* mContinentMap;
		/* 50h */	eastl::vector<uint32_t> mContinentAreas;
		/* 64h */	eastl::vector<bool> mContinentAboveWater;
		/* 78h */	bool mContinentMapDirty;
		/* 7Ch */	uint8_t* mObstacleBitMap;
		/* 80h */	eastl::vector<cPoliticalZone> mPoliticalZones;
		/* 94h */	eastl::vector<int8_t> mPoliticalZoneIdxMap;
		/* A8h */	void* mPlanetRibbonGfx; // cPlanetRibbonGfx
		/* ACh */	eastl::vector<eastl::pair<cCity*, cCity*>> mSeaTradeRoutes;
		/* C0h */	eastl::vector<eastl::pair<cCity*, cCity*>> mAirTradeRoutes;
		/* D4h */	eastl::vector<cLandTradeRoute> mLandTradeRoutes;
		/* E8h */	bool mSeaTradeRoutesDirty;
		/* E9h */	bool mAirTradeRoutesDirty;
		/* EAh */	bool mLandTradeRoutesDirty;
		/* ECh */	uint32_t mGameMode;
		// eastl::fixed_hash_map<Graphics::cCubeMapIndex, cBoundaryPointInfo, 2048, 512, true, cCubeCornerHash, cCubeMapIndexEqual, false> mPoliticalBoundaryPoints;
		// Graphics::cCubeMapIndex mPoliticalBoundaryStartCorner[15];

		//PLACEHODER more things
	};

	namespace Addresses(cPlanetModel)
	{
		DeclareAddress(Get);

		DeclareAddress(IsInWater);
		DeclareAddress(GetNearestCity);
		DeclareAddress(ToSurface);
		DeclareAddress(GetGravity);
		DeclareAddress(GetHeightAt);
		DeclareAddress(GetOrientation);
		DeclareAddress(GetRadius);  // 0xB7DD80 TODO
	}
}