#pragma once

#include <Spore\Simulator\ISimulatorSerializable.h>
#include <Spore\MathUtils.h>

#define ICityMusicPtr eastl::intrusive_ptr<Simulator::ICityMusic>

namespace Simulator
{
	class ICityMusic
		: public ISimulatorSerializable
	{
		static const uint32_t TYPE = 0x424E7F2;

		/* 24h */	virtual void Start(const Math::Vector3&) = 0;
		/* 28h */	virtual void Stop() = 0;
		/* 2Ch */	virtual void Pause(bool) = 0;
		/* 30h */	virtual void SetPosition(const Math::Vector3&) = 0;
		/* 34h */	virtual int func34h() = 0;
	};
}