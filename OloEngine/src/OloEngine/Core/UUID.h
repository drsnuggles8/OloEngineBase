#pragma once

namespace OloEngine {

	class UUID
	{
	public:
		UUID();
		explicit(false) UUID(uint64_t uuid);
		UUID(const UUID&) = default;

		operator uint64_t() { return m_UUID; }
		operator uint64_t() const { return m_UUID; }
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
			// uuid is already a randomly generated number, and is suitable as a hash key as-is.
			// this may change in future, in which case return hash<uint64_t>{}(uuid); might be more appropriate
			return static_cast<uint64_t>(uuid);
		}
	};

}
