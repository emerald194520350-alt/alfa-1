#pragma once

#include <Spore\Swarm\cDecal.h>

namespace Terrain
{
	struct cTerrainDecal
		: public Swarm::cDecal
	{
		/* 68h */	float field_68;
		// /* 68h */	Swarm::DecalType mType = Swarm::DecalType::kDecalTypeTerrain;
	};
	ASSERT_SIZE(cTerrainDecal, 0x6C);
}