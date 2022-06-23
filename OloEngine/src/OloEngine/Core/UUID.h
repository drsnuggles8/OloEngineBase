#pragma once

#include <xhash>

namespace OloEngine {

	class UUID
	{
	public:
		UUID();
		explicit UUID(const uint64_t uuid);
		UUID(const UUID&) = default;

		explicit operator uint64_t() const { return m_UUID; }
	private:
		uint64_t m_UUID;
	};

}

namespace std {

	template<>
	struct hash<OloEngine::UUID>
	{
		std::size_t operator()(const OloEngine::UUID& uuid) const
		{
			return hash<uint64_t>()(static_cast<uint64_t>(uuid));
		}
	};

}
