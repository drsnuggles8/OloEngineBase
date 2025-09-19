#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <filesystem>
#include <vector>
#include <fstream>
#include <memory>

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
		void Open(const std::filesystem::path& inPath);
		void Close();
		bool IsOpen() const { return m_Stream.is_open(); }

		void WriteBytes(const void* inData, sizet inNumBytes);
		bool IsFailed() const;

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
		bool IsCapturing() const;

		// File management
		void OpenCapture(const std::filesystem::path& capturePath) const;
		void OpenRecentCapture() const;
		void ClearCaptures();
		void RemoveCapture(const std::filesystem::path& capturePath);
		const std::vector<std::filesystem::path>& GetAllCaptures() const { return m_Captures; }

		// Settings
		void SetCapturesDirectory(const std::filesystem::path& directory) { m_CapturesDirectory = directory; }
		const std::filesystem::path& GetCapturesDirectory() const { return m_CapturesDirectory; }

	private:
		void InitializeCapturesDirectory();

	private:
		JoltCaptureOutStream m_Stream;
		bool m_IsCapturing = false;
		i32 m_FrameCount = 0;

		std::filesystem::path m_CapturesDirectory;
		std::filesystem::path m_RecentCapture;
		std::vector<std::filesystem::path> m_Captures;
	};

}