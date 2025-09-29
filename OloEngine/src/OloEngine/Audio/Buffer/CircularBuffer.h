#pragma once

#include "OloEngine/Core/Base.h"
#include <algorithm>
#include <array>
#include <type_traits>

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
		// Compile-time safety check to prevent buffer overflow
		static_assert(Size >= NumChannels, "CircularBuffer: Size must be >= NumChannels to hold at least one frame");
		
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
			
			// Write sample at current position
			m_buf[m_writepos] = x;
			
			// Advance and wrap write position
			++m_writepos;
			if (m_writepos >= static_cast<int>(m_buf.size()))
				m_writepos = 0;
			
			// Check for overwrite: if buffer was full, advance read position
			if (m_avail < static_cast<int>(m_buf.size()))
			{
				++m_avail;
			}
			else
			{
				// Buffer was full - advance read position to discard oldest sample
				++m_readpos;
				if (m_readpos >= static_cast<int>(m_buf.size()))
					m_readpos = 0;
			}
		}

		/// Push a frame of samples (for multi-channel buffers)
		inline void PushFrame(const T* frame) noexcept
		{
			// Write frame at current position
			for (sizet ch = 0; ch < NumChannels; ++ch)
			{
				m_buf[m_writepos * NumChannels + ch] = frame[ch];
			}
			
			// Advance and wrap write position
			++m_writepos;
			if (m_writepos >= static_cast<int>(GetFrameCapacity()))
				m_writepos = 0;
			
			// Check for overwrite: if buffer was full, advance read position
			if (m_avail < static_cast<int>(GetFrameCapacity()))
			{
				++m_avail;
			}
			else
			{
				// Buffer was full - advance read position to discard oldest frame
				++m_readpos;
				if (m_readpos >= static_cast<int>(GetFrameCapacity()))
					m_readpos = 0;
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
			
			// Clamp len to buffer size to prevent overrun
			const int bufferSize = static_cast<int>(m_buf.size());
			len = std::min(len, bufferSize);
			
			const int previousAvail = m_avail;
			const sizet free = m_buf.size() - m_writepos;
			
			// Copy in bounded chunks using safe element-wise copying
			if (len > static_cast<int>(free))
			{
				// First chunk: copy up to end of buffer
				const sizet firstChunk = std::min(static_cast<sizet>(len), free);
				std::copy_n(buf, firstChunk, &m_buf[m_writepos]);
				
				// Second chunk: copy remaining to start of buffer
				const sizet secondChunk = std::min(static_cast<sizet>(len - firstChunk), m_buf.size());
				std::copy_n(buf + firstChunk, secondChunk, &m_buf[0]);
				
				m_writepos = static_cast<int>(secondChunk);
			}
			else
			{
				// Single chunk fits without wrapping
				std::copy_n(buf, len, &m_buf[m_writepos]);
				m_writepos += len;
				if (m_writepos >= bufferSize)
					m_writepos = 0;
			}

			// Calculate how much data was overwritten
			const int overwritten = std::max(0, previousAvail + len - bufferSize);
			
			// Advance read position by amount overwritten
			if (overwritten > 0)
			{
				m_readpos = (m_readpos + overwritten) % bufferSize;
			}
			
			// Update available count
			m_avail = std::min(previousAvail + len, bufferSize);
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
		/// Uses safe element-wise copying for non-trivially copyable types
		template<unsigned int count>
		void GetMultiple(T* dest)
		{
			static_assert(NumChannels == 1, "Use GetMultipleFrames for multi-channel buffers");
			
			OLO_CORE_ASSERT(m_avail >= count);

			// Detect wrap: compute tail = m_buf.size() - m_readpos
			const size_t tail = m_buf.size() - static_cast<size_t>(m_readpos);
			
			if (count <= tail)
			{
				// No wrap - single copy as before
				std::copy_n(&m_buf[m_readpos], count, dest);
			}
			else
			{
				// Wrap case - copy tail samples from end, then remaining from start
				std::copy_n(&m_buf[m_readpos], tail, dest);
				std::copy_n(&m_buf[0], count - tail, dest + tail);
			}

			// Advance read position with proper modulo
			m_readpos = static_cast<int>((static_cast<size_t>(m_readpos) + count) % m_buf.size());
			m_avail -= static_cast<int>(count);
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