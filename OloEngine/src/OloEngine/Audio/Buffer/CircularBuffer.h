#pragma once

#include "OloEngine/Core/Base.h"
#include <array>
#include <cstring>

namespace OloEngine::Audio
{
	/// Circular buffer template with multi-channel support
	/// Template parameters:
	/// - T: Sample type (usually f32)  
	/// - Size: Total buffer size in samples
	/// - NumChannels: Number of audio channels (default 1)
	template<typename T, sizet Size, sizet NumChannels = 1>
	class CircularBuffer final
	{
	public:
		CircularBuffer() = default;

		inline void Clear() noexcept
		{
			m_avail = 0;
			m_readpos = 0;
			m_writepos = 0;
			std::fill(m_buf.begin(), m_buf.end(), T(0));
		}

		/// Push a single sample (for single-channel buffers)
		inline void Push(T x) noexcept
		{
			static_assert(NumChannels == 1, "Use PushFrame for multi-channel buffers");
			
			m_buf[m_writepos] = x;
			++m_writepos;
			++m_avail;
			
			if (m_writepos >= m_buf.size()) 
			{
				m_writepos = 0;
				// Handle buffer overflow by advancing read position if we've wrapped around
				if (m_avail >= m_buf.size())
				{
					m_readpos = (m_writepos + 1) % m_buf.size();
					m_avail = static_cast<int>(m_buf.size()) - 1;
				}
			}
		}

		/// Push a frame of samples (for multi-channel buffers)
		inline void PushFrame(const T* frame) noexcept
		{
			for (sizet ch = 0; ch < NumChannels; ++ch)
			{
				m_buf[m_writepos * NumChannels + ch] = frame[ch];
			}
			++m_writepos;
			++m_avail;
			
			if (m_writepos >= GetFrameCapacity()) 
			{
				m_writepos = 0;
				// Handle buffer overflow by advancing read position
				if (m_avail >= GetFrameCapacity())
				{
					m_readpos = (m_writepos + 1) % GetFrameCapacity();
					m_avail = static_cast<int>(GetFrameCapacity()) - 1;
				}
			}
		}

		/// Get a single sample (for single-channel buffers)
		inline T Get() noexcept
		{
			static_assert(NumChannels == 1, "Use GetFrame for multi-channel buffers");
			
			OLO_CORE_ASSERT(m_avail > 0);
			T x = m_buf[m_readpos];
			++m_readpos;
			--m_avail;
			if (m_readpos >= m_buf.size())
				m_readpos = 0;
			return x;
		}

		/// Get a frame of samples (for multi-channel buffers)  
		inline void GetFrame(T* frame) noexcept
		{
			OLO_CORE_ASSERT(m_avail > 0);
			
			for (sizet ch = 0; ch < NumChannels; ++ch)
			{
				frame[ch] = m_buf[m_readpos * NumChannels + ch];
			}
			
			++m_readpos;
			--m_avail;
			if (m_readpos >= GetFrameCapacity())
				m_readpos = 0;
		}

		inline int Available() const noexcept { return m_avail; }

		/// Get multiple samples for single-channel buffers
		inline int GetMultiple(T* buf, int len) noexcept
		{
			static_assert(NumChannels == 1, "Use GetMultipleFrames for multi-channel buffers");
			
			OLO_CORE_ASSERT(m_avail > 0);
			if (len > m_avail)
				len = m_avail;
			for (int i = 0; i < len; ++i)
				buf[i] = Get();
			return len;
		}

		/// Get multiple frames for multi-channel buffers
		inline int GetMultipleFrames(T* buf, int numFrames) noexcept
		{
			OLO_CORE_ASSERT(m_avail > 0);
			if (numFrames > m_avail)
				numFrames = m_avail;
				
			for (int i = 0; i < numFrames; ++i)
			{
				GetFrame(&buf[i * NumChannels]);
			}
			return numFrames;
		}

		/// Push multiple samples for single-channel buffers
		inline void PushMultiple(const T* buf, int len) noexcept
		{
			static_assert(NumChannels == 1, "Use PushMultipleFrames for multi-channel buffers");
			
			const sizet free = m_buf.size() - m_writepos;
			const int overflow = len - static_cast<int>(free);

			if (overflow > 0)
			{
				std::memcpy(&m_buf[m_writepos], buf, free * sizeof(T));
				std::memcpy(&m_buf[0], buf + free, overflow * sizeof(T));
				m_writepos = overflow;
			}
			else
			{
				std::memcpy(&m_buf[m_writepos], buf, len * sizeof(T));
				m_writepos += len;
				if (m_writepos >= m_buf.size())
					m_writepos = 0;
			}

			m_avail += len;
			
			// Handle overflow by adjusting read position and available count
			if (m_avail >= static_cast<int>(m_buf.size()))
			{
				m_avail = static_cast<int>(m_buf.size()) - 1;
				m_readpos = (m_writepos + 1) % m_buf.size();
			}
		}

		/// Push multiple frames for multi-channel buffers
		inline void PushMultipleFrames(const T* buf, int numFrames) noexcept
		{
			for (int i = 0; i < numFrames; ++i)
			{
				PushFrame(&buf[i * NumChannels]);
			}
		}

		/// Fast version without checking how many available (single-channel)
		template<unsigned int count>
		void GetMultiple(T* dest)
		{
			static_assert(NumChannels == 1, "Use GetMultipleFrames for multi-channel buffers");
			
			OLO_CORE_ASSERT(m_avail >= count);

			std::memcpy(dest, &m_buf[m_readpos], count * sizeof(T));

			m_readpos += count;
			m_avail -= count;

			if (m_readpos >= static_cast<int>(m_buf.size()))
				m_readpos = m_readpos - static_cast<int>(m_buf.size());
		}

		constexpr sizet GetSize() const noexcept { return m_buf.size(); }
		constexpr sizet GetNumChannels() const noexcept { return NumChannels; }
		constexpr sizet GetFrameCapacity() const noexcept { return m_buf.size() / NumChannels; }

	private:
		int m_writepos = 0;
		int m_readpos = 0;
		int m_avail = 0;
		std::array<T, Size> m_buf{ T(0) };
	};

	// Convenience aliases for common configurations
	template<typename T, sizet Size>
	using MonoCircularBuffer = CircularBuffer<T, Size, 1>;
	
	template<typename T, sizet Size>
	using StereoCircularBuffer = CircularBuffer<T, Size, 2>;

} // namespace OloEngine::Audio