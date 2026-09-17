#pragma once

#include <Spore\Transform.h>
#include <EASTL\bitset.h>

namespace Swarm
{
	enum DecalType
	{
		kDecalTypeTerrain,
		kDecalTypeWater,
		kDecalTypeTerrainAndWater,
		kDecalTypePaint,
		kDecalTypeUser1,
		kDecalTypeUser2,
		kDecalTypeUser3,
		kDecalTypeUser4,
		kDecalTypeUser5,
		kDecalTypeUser6,
		kDecalTypeUser7,
		kDecalTypeUser8,

		kMaxDecalTypes
	};

	class cDecal
	{
	public:
		typedef eastl::bitset<1> FlagSet;

		enum Flags
		{
			kFlagEnabled,

			kMaxFlags
		};

		/* 00h */	Transform mTransform;
		/* 38h */	Math::Vector4 mColorAlpha{ 1.0f, 1.0f, 1.0f, 1.0f };
		/* 48h */	float mTurns = 1.0f;
		/* 4Ch */	float mAspect = 1.0f;
		/* 50h */	float mRepeatU = 1.0f;
		/* 54h */	float mRepeatV = 1.0f;
		/* 58h */	float mOffsetU = 1.0f;
		/* 5Ch */	float mOffsetV = 1.0f;
		/* 60h */	uint32_t mLayer = 0;
		/* 64h */	FlagSet mFlags = 0;
	};
	ASSERT_SIZE(cDecal, 0x68);
}
