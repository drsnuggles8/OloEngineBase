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
	template <typename T> struct hash;

	template<>
	struct hash<OloEngine::UUID>
	{
		std::size_t operator()(const OloEngine::UUID& uuid) const
		{
			return static_cast<uint64_t>(uuid);
		}
	};

}
