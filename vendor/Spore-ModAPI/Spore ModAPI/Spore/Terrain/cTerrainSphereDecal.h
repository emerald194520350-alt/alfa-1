#pragma once

#include <Spore\Object.h>
#include <Spore\Graphics\cCubeMapCoord.h>
#include <Spore\Graphics\ShaderData.h>
#include <Spore\Terrain\cTerrainDecal.h>
#include <EASTL\vector.h>
#include <Spore\Graphics\Texture.h>
#include <Spore\MathUtils.h>

namespace Terrain
{
	class cTerrainSphere;
	class cTerrainMapSet;

	class cTerrainSphereDecal
		: public DefaultRefCounted
		, public cTerrainDecal
	{
		enum : uint32_t
		{
			kEnabled = 1,
			kBelowWater = 2,
			kEmissive = 4,
			kUseFog = 8,
			kStatic = 16
		};

	public:
		struct FaceInfo
		{
			/* 00h */	uint32_t mFace;
			/* 04h */	Math::Rectangle mBoundingBoxFace;
		};
		ASSERT_SIZE(FaceInfo, 0x14);

		cTerrainSphereDecal();

		void Initialize(int arg_4, Graphics::Texture* pTexture, cTerrainSphere* pParentSphere, const Transform& xform, float turns, bool isStatic);
		void Shutdown();
		void UpdateDecal();
		void DispatchStaticDecal(int face);
		bool GetBBoxForFace(int faceRequest, Math::Rectangle& bbox);


		inline bool IsInitialized()
		{
			return mpTerrainMapSet != nullptr;
		}
		inline Math::Rectangle* GetBoundingBox(int face)
		{
			return &mFaceArray[face].mBoundingBoxFace;
		}
		inline void SetTexture(Graphics::Texture* pTexture)
		{
			mpTexture = pTexture;
		}
		inline Graphics::Texture* GetTexture() const
		{
			return mpTexture.get();
		}
		inline bool IsBelowWater() const
		{
			return GetFlag(kBelowWater);
		}
		inline void SetDecalEmissive(bool val)
		{
			SetFlag(kEmissive, val);
		}
		inline bool GetDecalEmissive() const
		{
			return GetFlag(kEmissive);
		}
		inline void SetDecalFog(bool val)
		{
			SetFlag(kUseFog, val);
		}
		inline bool GetDecalFog() const
		{
			return GetFlag(kUseFog);
		}
		inline bool GetDecalIsStatic() const
		{
			return GetFlag(kStatic);
		}
	private:
		void SetFaceInfoArray(const Transform& xform, float aspect);

		inline bool GetFlag(uint32_t flag) const
		{
			return mFlags & flag;
		}
		inline void SetFlag(uint32_t flag, bool set)
		{
			if (set)
				mFlags |= flag;
			else
				mFlags &= ~flag;
		}

	public:
		/* 74h */	int field_74; // -1, index?
		/* 78h */	cTerrainSphere* mpParentSphere;
		/* 7Ch */	cTerrainMapSet* mpTerrainMapSet;
		/* 80h */	eastl::vector<FaceInfo> mFaceArray;
		/* 94h */	Graphics::cCubeMapCoord mPosition;
		/* A0h */	float mUSize;
		/* A4h */	float mVSize;
		/* A8h */	float mUSizeInput;
		/* ACh */	float mVSizeInput;
		/* B0h */	float mDecalRadius;
		/* B4h */	int mDecalTextureID;
		/* B8h */	uint32_t mFlags;
		/* BCh */	Graphics::ShaderDataDecalState mShaderDataDecalState;
		/* ECh */	Math::Vector3 mZeroUVLoc;
		/* F8h */	Math::Vector4 mTexturePlane;
		/* 108h */	Math::Vector3 mDirectionU;
		/* 114h */	float mScaleU;
		/* 118h */	Math::Vector3 mDirectionV;
		/* 124h */	float mScaleV;
		/* 128h */	TexturePtr mpTexture;
	};
	ASSERT_SIZE(cTerrainSphereDecal, 0x12C);

	namespace Addresses(cTerrainSphereDecal)
	{
		DeclareAddress(Initialize);
		DeclareAddress(Shutdown);
		DeclareAddress(UpdateDecal);
		DeclareAddress(DispatchStaticDecal);
		DeclareAddress(GetBBoxForFace);
		DeclareAddress(SetFaceInfoArray);
	}
}