#pragma once

namespace OloEngine
{
	class UUID
	{
	public:
		UUID();
		explicit(false) UUID(u64 uuid);
		UUID(const UUID&) = default;

		operator u64() { return m_UUID; }
		operator u64() const { return m_UUID; }
	private:
		u64 m_UUID;
	};
}

namespace std
{
	template <typename T> struct hash;

	template<>
	struct hash<OloEngine::UUID>
	{
		std::size_t operator()(const OloEngine::UUID& uuid) const
		{
			return static_cast<u64>(uuid);
		}
	};
}
