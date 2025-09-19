#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <filesystem>
#include <vector>
#include <fstream>

namespace OloEngine {

	/**
	 * @brief File sink for writing physics capture data to binary files
	 * 
	 * Simple file output class used by the capture manager to write physics 
	 * simulation data to binary files compatible with JoltViewer.
	 */
	class JoltCaptureOutStream
	{
	public:
		JoltCaptureOutStream() = default;
		
		void Open(const std::filesystem::path& inPath);
		void Close();
		[[nodiscard]] bool IsOpen() const { return m_Stream.is_open(); }

		void WriteBytes(const void* inData, sizet inNumBytes);
		[[nodiscard]] bool IsFailed() const;

		// Explicitly non-copyable
		JoltCaptureOutStream(const JoltCaptureOutStream&) = delete;
		JoltCaptureOutStream& operator=(const JoltCaptureOutStream&) = delete;

		// Allow move operations
		JoltCaptureOutStream(JoltCaptureOutStream&&) = default;
		JoltCaptureOutStream& operator=(JoltCaptureOutStream&&) = default;

	private:
		std::ofstream m_Stream;
	};

	/**
	 * @brief Manager for capturing and recording Jolt Physics simulations
	 * 
	 * Provides functionality to record physics simulations to files that can
	 * be replayed and analyzed using external tools like JoltViewer. This is
	 * invaluable for debugging complex physics scenarios and performance analysis.
	 * 
	 * Note: Full capture functionality requires JPH_DEBUG_RENDERER to be enabled.
	 * Without it, the capture manager provides file management but no actual recording.
	 * 
	 * Thread Safety: This class is intended for single-threaded use only. The captures
	 * list and stream state are not synchronized; external synchronization is required
	 * if used from multiple threads.
	 */
	class JoltCaptureManager : public RefCounted
	{
	public:
		JoltCaptureManager();
		~JoltCaptureManager();

		// Core capture functionality
		void BeginCapture();
		void CaptureFrame();
		void EndCapture();
		[[nodiscard]] bool IsCapturing() const;

		// File management
		void OpenCapture(const std::filesystem::path& capturePath) const;
		void OpenRecentCapture() const;
		void ClearCaptures();
		void RemoveCapture(const std::filesystem::path& capturePath);
		[[nodiscard]] const std::vector<std::filesystem::path>& GetAllCaptures() const { return m_Captures; }

		// Settings
		[[nodiscard]] bool SetCapturesDirectory(const std::filesystem::path& directory);
		[[nodiscard]] const std::filesystem::path& GetCapturesDirectory() const { return m_CapturesDirectory; }

		/**
		 * @brief Set the frame logging interval for capture progress
		 * @param interval Number of frames between log messages (must be > 0)
		 * 
		 * Controls how frequently the capture manager logs progress messages during recording.
		 * A lower interval provides more frequent updates but may impact performance.
		 * Default is DEFAULT_FRAME_LOG_INTERVAL frames, which typically corresponds to 1 second at 60 FPS.
		 */
		void SetFrameLogInterval(i32 interval);

		/**
		 * @brief Get the current frame logging interval
		 * @return Number of frames between log messages
		 */
		i32 GetFrameLogInterval() const { return m_FrameLogInterval; }

	private:
		void InitializeCapturesDirectory();
		void RefreshCapturesCache();

		static constexpr i32 DEFAULT_FRAME_LOG_INTERVAL = 60;

	private:
		JoltCaptureOutStream m_Stream;
		bool m_IsCapturing = false;
		i32 m_FrameCount = 0;
		i32 m_FrameLogInterval = DEFAULT_FRAME_LOG_INTERVAL; ///< Number of frames between log messages (default: DEFAULT_FRAME_LOG_INTERVAL)

		std::filesystem::path m_CapturesDirectory;
		std::filesystem::path m_RecentCapture;
		std::vector<std::filesystem::path> m_Captures;
	};

}