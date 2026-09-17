#pragma once

#include <Spore\Internal.h>

namespace Graphics
{
	struct cCubeMapIndex
	{
		/* 00h */	int mCol;
		/* 04h */	int mRow;
		/* 08h */	int mFace;
	};
	ASSERT_SIZE(cCubeMapIndex, 0x0C);
}