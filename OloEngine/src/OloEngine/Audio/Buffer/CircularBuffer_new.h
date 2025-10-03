#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"
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
		static_assert(Size % NumChannels == 0, "CircularBuffer: Size must be a multiple of NumChannels to avoid partial frames");
		
	public:
		CircularBuffer() = default;

		inline void Clear() noexcept
		{
			m_Avail = 0;
			m_ReadPos = 0;
			m_WritePos = 0;
			std::fill(m_buf.begin(), m_buf.end(), T(0));
		}

		/// Push a single sample (for single-channel buffers)
		inline void Push(T x) noexcept
		{
			static_assert(NumChannels == 1, "Use PushFrame for multi-channel buffers");
			
			// Write sample at current position
			m_buf[m_WritePos] = x;
			
			// Advance and wrap write position
			++m_WritePos;
			if (m_WritePos >= static_cast<i32>(m_buf.size()))
				m_WritePos = 0;
			
			// Check for overwrite: if buffer was full, advance read position
			if (m_Avail < static_cast<i32>(m_buf.size()))
			{
				++m_Avail;
			}
			else
			{
				// Buffer was full - advance read position to discard oldest sample
				++m_ReadPos;
				if (m_ReadPos >= static_cast<i32>(m_buf.size()))
					m_ReadPos = 0;
			}
		}

		/// Push a frame of samples (for multi-channel buffers)
		inline void PushFrame(const T* frame) noexcept
		{
			// Write frame at current position
			for (sizet ch = 0; ch < NumChannels; ++ch)
			{
				m_buf[m_WritePos * NumChannels + ch] = frame[ch];
			}
			
			// Advance and wrap write position
			++m_WritePos;
			if (m_WritePos >= static_cast<i32>(GetFrameCapacity()))
				m_WritePos = 0;
			
			// Check for overwrite: if buffer was full, advance read position
			if (m_Avail < static_cast<i32>(GetFrameCapacity()))
			{
				++m_Avail;
			}
			else
			{
				// Buffer was full - advance read position to discard oldest frame
				++m_ReadPos;
				if (m_ReadPos >= static_cast<i32>(GetFrameCapacity()))
					m_ReadPos = 0;
			}
		}

		/// Get a single sample (for single-channel buffers)
		inline T Get() noexcept
		{
			static_assert(NumChannels == 1, "Use GetFrame for multi-channel buffers");
			
			OLO_CORE_ASSERT(m_Avail > 0);
			T x = m_buf[m_ReadPos];
			++m_ReadPos;
			--m_Avail;
			if (m_ReadPos >= m_buf.size())
				m_ReadPos = 0;
			return x;
		}

		/// Get a frame of samples (for multi-channel buffers)  
		inline void GetFrame(T* frame) noexcept
		{
			OLO_CORE_ASSERT(m_Avail > 0);
			
			for (sizet ch = 0; ch < NumChannels; ++ch)
			{
				frame[ch] = m_buf[m_ReadPos * NumChannels + ch];
			}
			
			++m_ReadPos;
			--m_Avail;
			if (m_ReadPos >= GetFrameCapacity())
				m_ReadPos = 0;
		}

		inline i32 Available() const noexcept
		{
			return m_Avail;
		}

		/// Get multiple samples for single-channel buffers
		inline i32 GetMultiple(T* buf, i32 len) noexcept
		{
			OLO_PROFILE_FUNCTION();
			
			static_assert(NumChannels == 1, "Use GetMultipleFrames for multi-channel buffers");
			
			// Early return if buffer is empty
			if (m_Avail == 0)
				return 0;
				
			if (len > m_Avail)
				len = m_Avail;
			for (i32 i = 0; i < len; ++i)
				buf[i] = Get();
			return len;
		}

		/// Get multiple frames for multi-channel buffers
		inline i32 GetMultipleFrames(T* buf, i32 numFrames) noexcept
		{
			OLO_PROFILE_FUNCTION();
			
			// Early return for empty buffer or invalid input
			if (m_Avail <= 0 || numFrames <= 0)
				return 0;
				
			if (numFrames > m_Avail)
				numFrames = m_Avail;
				
			for (i32 i = 0; i < numFrames; ++i)
			{
				GetFrame(&buf[i * NumChannels]);
			}
			return numFrames;
		}

		/// Push multiple samples for single-channel buffers
		inline void PushMultiple(const T* buf, i32 len) noexcept
		{
			OLO_PROFILE_FUNCTION();
			
			static_assert(NumChannels == 1, "Use PushMultipleFrames for multi-channel buffers");
			
			// Bail for non-positive lengths
			if (len <= 0)
				return;
			
			const i32 bufferSize = static_cast<i32>(m_buf.size());
			const i32 previousAvail = m_Avail;
			
			// If input is larger than buffer, advance source pointer to keep newest samples
			const T* sourcePtr = buf;
			if (len > bufferSize)
			{
				sourcePtr = buf + (len - bufferSize);
				len = bufferSize;
			}
			
			const sizet free = m_buf.size() - m_WritePos;
			
			// Copy in bounded chunks using safe element-wise copying
			if (len > static_cast<i32>(free))
			{
				// First chunk: copy up to end of buffer
				const sizet firstChunk = std::min(static_cast<sizet>(len), free);
				std::copy_n(sourcePtr, firstChunk, &m_buf[m_WritePos]);
				
				// Second chunk: copy remaining to start of buffer
				const sizet secondChunk = std::min(static_cast<sizet>(len - firstChunk), m_buf.size());
				std::copy_n(sourcePtr + firstChunk, secondChunk, &m_buf[0]);
				
				m_WritePos = static_cast<i32>(secondChunk);
			}
			else
			{
				// Single chunk fits without wrapping
				std::copy_n(sourcePtr, len, &m_buf[m_WritePos]);
				m_WritePos += len;
				if (m_WritePos >= bufferSize)
					m_WritePos = 0;
			}

			// Calculate how much data was overwritten
			const i32 overwritten = std::max(0, previousAvail + len - bufferSize);
			
			// Advance read position by amount overwritten (mod bufferSize)
			if (overwritten > 0)
			{
				m_ReadPos = (m_ReadPos + overwritten) % bufferSize;
			}
			
			// Update available count
			m_Avail = std::min(previousAvail + len, bufferSize);
		}

		/// Push multiple frames for multi-channel buffers
		inline void PushMultipleFrames(const T* buf, i32 numFrames) noexcept
		{
			OLO_PROFILE_FUNCTION();
			
			for (i32 i = 0; i < numFrames; ++i)
			{
				PushFrame(&buf[i * NumChannels]);
			}
		}

		/// Fast version without checking how many available (single-channel)
		/// Uses safe element-wise copying for non-trivially copyable types
		template<unsigned int count>
		void GetMultiple(T* dest)
		{
			OLO_PROFILE_FUNCTION();
			
			static_assert(NumChannels == 1, "Use GetMultipleFrames for multi-channel buffers");
			
			OLO_CORE_ASSERT(m_Avail >= count);

			// Detect wrap: compute tail = m_buf.size() - m_ReadPos
			const sizet tail = m_buf.size() - static_cast<sizet>(m_ReadPos);
			
			if (count <= tail)
			{
				// No wrap - single copy as before
				std::copy_n(&m_buf[m_ReadPos], count, dest);
			}
			else
			{
				// Wrap case - copy tail samples from end, then remaining from start
				std::copy_n(&m_buf[m_ReadPos], tail, dest);
				std::copy_n(&m_buf[0], count - tail, dest + tail);
			}

			// Advance read position with proper modulo
			m_ReadPos = static_cast<i32>((static_cast<size_t>(m_ReadPos) + count) % m_buf.size());
			m_Avail -= static_cast<i32>(count);
		}

		constexpr sizet GetSize() const noexcept
		{
			return m_buf.size();
		}
		
		constexpr sizet GetNumChannels() const noexcept
		{
			return NumChannels;
		}
		
		constexpr sizet GetFrameCapacity() const noexcept
		{
			return m_buf.size() / NumChannels;
		}

	private:
		i32 m_WritePos = 0;
		i32 m_ReadPos = 0;
		i32 m_Avail = 0;
		std::array<T, Size> m_buf{ T(0) };
	};

	// Convenience aliases for common configurations
	template<typename T, sizet Size>
	using MonoCircularBuffer = CircularBuffer<T, Size, 1>;
	
	template<typename T, sizet Size>
	using StereoCircularBuffer = CircularBuffer<T, Size, 2>;

} // namespace OloEngine::Audio
