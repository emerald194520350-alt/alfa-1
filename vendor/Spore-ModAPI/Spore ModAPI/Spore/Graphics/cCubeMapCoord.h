#pragma once

#include <Spore\Internal.h>

namespace Graphics
{
	struct cCubeMapCoord
	{
		/* 00h */	float mU;
		/* 04h */	float mV;
		/* 08h */	int mFace;
	};
	ASSERT_SIZE(cCubeMapCoord, 0x0C);
}