#include "OloEnginePCH.h"
#include "JoltCaptureManager.h"

#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <limits>

namespace OloEngine {

	bool JoltCaptureOutStream::Open(const std::filesystem::path& inPath)
		{
			OLO_PROFILE_FUNCTION();

			Close();
			m_Stream.clear(); // Reset any previous error flags before opening
			m_Stream.open(inPath, std::ios::out | std::ios::binary | std::ios::trunc);
			
			// Validate stream state immediately after opening
			if (!m_Stream.is_open() || m_Stream.fail())
			{
				OLO_CORE_ERROR("Failed to open capture file: {} (is_open: {}, fail: {})", 
							   inPath.string(), m_Stream.is_open(), m_Stream.fail());
			
			// Ensure stream is closed on failure
			if (m_Stream.is_open())
			{
				m_Stream.close();
			}
			
			return false;
		}
		
		return true;
	}

	void JoltCaptureOutStream::Close()
	{
		if (m_Stream.is_open())
		{
			m_Stream.close();
		}
		m_Stream.clear(); // Reset error/state flags for reuse
	}

	void JoltCaptureOutStream::WriteBytes(const void* inData, sizet inNumBytes)
	{
		// Fast-path: return immediately for zero-length writes
		if (inNumBytes == 0)
			return;
		
		// Guard against null pointer with positive byte count
		if (inData == nullptr)
		{
			OLO_CORE_ERROR("WriteBytes called with null data pointer but {} bytes requested", inNumBytes);
			return;
		}
		
		if (m_Stream.is_open())
		{
			m_Stream.write(static_cast<const char*>(inData), static_cast<std::streamsize>(inNumBytes));
			
			// Check for write failures immediately after the operation
			if (m_Stream.fail() || m_Stream.bad() || !m_Stream.good())
			{
				OLO_CORE_ERROR("Failed to write {} bytes to capture stream (fail: {}, bad: {}, good: {})", 
							   inNumBytes, m_Stream.fail(), m_Stream.bad(), m_Stream.good());
			}
		}
	}

	bool JoltCaptureOutStream::IsFailed() const
	{
		return m_Stream.fail();
	}

	JoltCaptureManager::JoltCaptureManager()
	{
		OLO_PROFILE_FUNCTION();

		// Set default captures directory with cross-platform support
		std::filesystem::path capturesPath;
		
#ifdef _WIN32
		// Windows: Use APPDATA if available
		const char* appData = std::getenv("APPDATA");
		if (appData != nullptr)
		{
			capturesPath = std::filesystem::path(appData) / "OloEngine" / "Captures";
		}
		else
		{
			capturesPath = std::filesystem::current_path() / "Captures";
		}
#elif defined(__APPLE__)
		// macOS: Use HOME + Library/Application Support
		const char* home = std::getenv("HOME");
		if (home != nullptr)
		{
			capturesPath = std::filesystem::path(home) / "Library" / "Application Support" / "OloEngine" / "Captures";
		}
		else
		{
			capturesPath = std::filesystem::current_path() / "Captures";
		}
#else
		// Linux/Unix: Use XDG_DATA_HOME or fallback to HOME + .local/share
		const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
		if (xdgDataHome != nullptr)
		{
			capturesPath = std::filesystem::path(xdgDataHome) / "OloEngine" / "Captures";
		}
		else
		{
			const char* home = std::getenv("HOME");
			if (home != nullptr)
			{
				capturesPath = std::filesystem::path(home) / ".local" / "share" / "OloEngine" / "Captures";
			}
			else
			{
				capturesPath = std::filesystem::current_path() / "Captures";
			}
		}
#endif

		// Create the directory if it doesn't exist
		std::error_code ec;
		if (!std::filesystem::exists(capturesPath, ec))
		{
			if (std::filesystem::create_directories(capturesPath, ec))
			{
				OLO_CORE_INFO("Created captures directory: {}", capturesPath.string());
			}
			else
			{
				OLO_CORE_WARN("Failed to create captures directory: {} ({})", capturesPath.string(), ec.message());
				// Fallback to current directory
				capturesPath = std::filesystem::current_path() / "Captures";
				std::filesystem::create_directories(capturesPath, ec);
			}
		}

		if (!SetCapturesDirectory(capturesPath))
		{
			OLO_CORE_WARN("Failed to set captures directory to: {}", capturesPath.string());
			
			// Attempt safe fallback directory
			std::filesystem::path fallbackPath = std::filesystem::current_path() / "OloCaptures";
			if (SetCapturesDirectory(fallbackPath))
			{
				OLO_CORE_INFO("Successfully set fallback captures directory: {}", fallbackPath.string());
			}
			else
			{
				OLO_CORE_ERROR("Failed to set fallback captures directory: {}. Using current directory.", fallbackPath.string());
				// Final fallback to current directory
				m_CapturesDirectory = std::filesystem::current_path();
			}
		}
	}

	JoltCaptureManager::~JoltCaptureManager()
	{
		OLO_PROFILE_FUNCTION();

		EndCapture();
	}

	void JoltCaptureManager::BeginCapture()
	{
		OLO_PROFILE_FUNCTION();

		if (IsCapturing())
		{
			OLO_CORE_WARN("Capture is already in progress. Ending current capture first.");
			EndCapture();
		}

		// Create captures directory if it doesn't exist
		InitializeCapturesDirectory();

		// Generate filename with timestamp
		auto now = std::chrono::system_clock::now();
		auto now_time = std::chrono::system_clock::to_time_t(now);
		
		// Use thread-safe time conversion
		std::tm local_tm{};
#if defined(_WIN32)
		// Windows: Use localtime_s (localtime_s(&tm, &time_t))
		if (localtime_s(&local_tm, &now_time) != 0)
		{
			OLO_CORE_ERROR("Failed to convert time to local time on Windows");
			return;
		}
#else
		// POSIX: Use localtime_r (localtime_r(&time_t, &tm))
		if (localtime_r(&now_time, &local_tm) == nullptr)
		{
			OLO_CORE_ERROR("Failed to convert time to local time on POSIX");
			return;
		}
#endif
		
		std::stringstream ss;
		ss << "capture_" << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << ".jolt";

		std::filesystem::path capturePath = m_CapturesDirectory / ss.str();

		// Ensure filename uniqueness by adding numeric suffix if needed
		std::error_code ec;
		bool fileExists = std::filesystem::exists(capturePath, ec);
		if (ec)
		{
			OLO_CORE_ERROR("Failed to check if capture file exists: {} - {}", capturePath.string(), ec.message());
			return;
		}
		
		if (fileExists)
		{
			std::stringstream baseNameSs;
			baseNameSs << "capture_" << std::put_time(&local_tm, "%Y%m%d_%H%M%S");
			std::string baseName = baseNameSs.str();
			u32 counter = 1;
			
			do
			{
				std::stringstream uniqueSs;
				uniqueSs << baseName << "_" << counter << ".jolt";
				capturePath = m_CapturesDirectory / uniqueSs.str();
				++counter;
				
				fileExists = std::filesystem::exists(capturePath, ec);
				if (ec)
				{
					OLO_CORE_ERROR("Failed to check file existence during uniqueness check: {} - {}", capturePath.string(), ec.message());
					return;
				}
			} while (fileExists && counter < 1000); // Safety limit
			
			if (counter >= 1000)
			{
				OLO_CORE_ERROR("Failed to generate unique capture filename after 1000 attempts");
				return;
			}
		}

		// Open the capture stream
		if (m_Stream.Open(capturePath))
		{
			// Verify stream is still in good state after opening
			if (m_Stream.IsFailed())
			{
				OLO_CORE_ERROR("Stream failed immediately after opening: {}", capturePath.string());
				m_Stream.Close();
				return;
			}
			
			// Perform any initial capture setup writes here
			// Example: write header, metadata, etc.
			// if (needToWriteHeader)
			// {
			//     m_Stream.WriteBytes(headerData, headerSize);
			//     if (m_Stream.IsFailed())
			//     {
			//         OLO_CORE_ERROR("Failed to write capture header: {}", capturePath.string());
			//         m_Stream.Close();
			//         return;
			//     }
			// }
			
			// Only set capturing flag after all initial operations succeed
			m_IsCapturing = true;
			m_RecentCapture = capturePath;
			
			// Add to captures list if not already present
			if (std::find(m_Captures.begin(), m_Captures.end(), capturePath) == m_Captures.end())
			{
				m_Captures.push_back(capturePath);
			}

			OLO_CORE_INFO("Started physics capture: {}", capturePath.string());
		}
		else
		{
			OLO_CORE_ERROR("Failed to start physics capture - could not open file: {}", capturePath.string());
		}
	}

	void JoltCaptureManager::CaptureFrame()
	{
		OLO_PROFILE_FUNCTION();

		if (!IsCapturing())
		{
			return;
		}

		// Check stream state before performing any operations
		if (m_Stream.IsFailed())
		{
			HandleCaptureFailure("Stream is in failed state");
			return;
		}

		// Basic capture functionality - currently just logs frame capture
		// Full implementation would require Jolt debug renderer integration
		// This provides the framework for when JPH_DEBUG_RENDERER is available
		
		m_FrameCount++;
		
		// Example of how write operations should be handled:
		// if (actualWriteDataNeeded)
		// {
		//     m_Stream.WriteBytes(data, size);
		//     if (m_Stream.IsFailed())
		//     {
		//         HandleCaptureFailure("Failed to write frame data");
		//         return;
		//     }
		// }
		
		if (m_FrameCount % m_FrameLogInterval == 0) // Log at configurable intervals
		{
			OLO_CORE_TRACE("Captured physics frame {}", m_FrameCount);
		}
	}

	void JoltCaptureManager::EndCapture()
	{
		OLO_PROFILE_FUNCTION();

		// Always log if we think we were capturing (regardless of stream state)
		bool wasCapturing = m_IsCapturing;
		
		// Always clear the capturing flag and reset state, regardless of stream condition
		m_IsCapturing = false;
		m_FrameCount = 0; // Reset frame counter for next capture
		
		// Close the stream if it's open
		if (m_Stream.IsOpen())
		{
			m_Stream.Close();
		}

		if (wasCapturing)
		{
			OLO_CORE_INFO("Ended physics capture: {}", m_RecentCapture.string());
		}
	}

	bool JoltCaptureManager::IsCapturing() const
	{
		return m_IsCapturing && m_Stream.IsOpen() && !m_Stream.IsFailed();
	}

	void JoltCaptureManager::OpenCapture(const std::filesystem::path& capturePath) const
	{
		OLO_PROFILE_FUNCTION();

		std::error_code ec;
		bool exists = std::filesystem::exists(capturePath, ec);
		if (ec)
		{
			OLO_CORE_ERROR("Failed to check if capture file exists: {} - {}", capturePath.string(), ec.message());
			return;
		}
		
		if (!exists)
		{
			OLO_CORE_ERROR("Capture file does not exist: {}", capturePath.string());
			return;
		}

		// Log the capture file path - user can manually open with external tools
		OLO_CORE_INFO("Capture file available: {}", capturePath.string());
	}

	void JoltCaptureManager::OpenRecentCapture() const
	{
		OLO_PROFILE_FUNCTION();

		if (m_RecentCapture.empty())
		{
			OLO_CORE_WARN("No recent capture available to open");
			return;
		}

		OpenCapture(m_RecentCapture);
	}

	void JoltCaptureManager::ClearCaptures()
	{
		OLO_PROFILE_FUNCTION();

		EndCapture(); // Stop any active capture

		for (const auto& capturePath : m_Captures)
		{
			try
			{
				if (std::filesystem::exists(capturePath))
				{
					std::filesystem::remove(capturePath);
				}
			}
			catch (const std::filesystem::filesystem_error& e)
			{
				OLO_CORE_ERROR("Failed to remove capture file {}: {}", capturePath.string(), e.what());
			}
		}

		m_Captures.clear();
		m_RecentCapture.clear();

		OLO_CORE_INFO("Cleared all physics captures");
	}

	void JoltCaptureManager::RemoveCapture(const std::filesystem::path& capturePath)
	{
		OLO_PROFILE_FUNCTION();

		auto it = std::find(m_Captures.begin(), m_Captures.end(), capturePath);
		if (it != m_Captures.end())
		{
			try
			{
				// Check if we're trying to remove the currently active capture
				if (IsCapturing() && m_RecentCapture == capturePath)
				{
					OLO_CORE_INFO("Stopping active capture before removal: {}", capturePath.string());
					EndCapture(); // Properly close the stream and reset state
				}
				
				if (std::filesystem::exists(capturePath))
				{
					std::filesystem::remove(capturePath);
				}
				
				m_Captures.erase(it);

				if (m_RecentCapture == capturePath)
				{
					m_RecentCapture.clear();
				}

				OLO_CORE_INFO("Removed physics capture: {}", capturePath.string());
			}
			catch (const std::filesystem::filesystem_error& e)
			{
				OLO_CORE_ERROR("Failed to remove capture file {}: {}", capturePath.string(), e.what());
			}
		}
		else
		{
			OLO_CORE_WARN("Capture file not found in manager: {}", capturePath.string());
		}
	}

	void JoltCaptureManager::InitializeCapturesDirectory()
	{
		OLO_PROFILE_FUNCTION();

		try
		{
			if (!std::filesystem::exists(m_CapturesDirectory))
			{
				std::filesystem::create_directories(m_CapturesDirectory);
			}

			// Scan for existing capture files
			RefreshCapturesCache();

			OLO_CORE_TRACE("Initialized captures directory: {} (found {} existing captures)", 
				m_CapturesDirectory.string(), m_Captures.size());
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			OLO_CORE_ERROR("Failed to initialize captures directory: {}", e.what());
		}
	}

	void JoltCaptureManager::RefreshCapturesCache()
	{
		// Clear existing captures
		m_Captures.clear();
		m_RecentCapture.clear();

		try
		{
			// Enumerate all .jolt files in the captures directory
			for (const auto& entry : std::filesystem::directory_iterator(m_CapturesDirectory))
			{
				if (entry.is_regular_file() && entry.path().extension() == ".jolt")
				{
					m_Captures.push_back(entry.path());
				}
			}

			// Sort captures by last write time (newest first)
			std::sort(m_Captures.begin(), m_Captures.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
			{
				std::filesystem::file_time_type timeA, timeB;
				
				try
				{
					timeA = std::filesystem::last_write_time(a);
				}
				catch (const std::filesystem::filesystem_error&)
				{
					// File became inaccessible - treat as oldest possible time
					timeA = std::filesystem::file_time_type::min();
				}
				
				try
				{
					timeB = std::filesystem::last_write_time(b);
				}
				catch (const std::filesystem::filesystem_error&)
				{
					// File became inaccessible - treat as oldest possible time
					timeB = std::filesystem::file_time_type::min();
				}
				
				return timeA > timeB;
			});

			if (!m_Captures.empty())
			{
				m_RecentCapture = m_Captures.front();
			}
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			OLO_CORE_ERROR("Failed to refresh captures cache: {}", e.what());
		}
	}

	bool JoltCaptureManager::SetCapturesDirectory(const std::filesystem::path& directory)
	{
		OLO_PROFILE_FUNCTION();

		try
		{
			// Attempt to create the directory if it doesn't exist
			if (!std::filesystem::exists(directory))
			{
				std::filesystem::create_directories(directory);
				OLO_CORE_TRACE("Created captures directory: {}", directory.string());
			}

			// Verify the directory is actually accessible
			if (!std::filesystem::is_directory(directory))
			{
				OLO_CORE_ERROR("Path is not a directory: {}", directory.string());
				return false;
			}

			// Update the directory and refresh capture listings
			m_CapturesDirectory = directory;
			
			// Refresh the captures cache by scanning the new directory
			RefreshCapturesCache();

			OLO_CORE_TRACE("Set captures directory to: {} (found {} existing captures)", 
				m_CapturesDirectory.string(), m_Captures.size());

			return true;
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			OLO_CORE_ERROR("Failed to set captures directory to '{}': {}", directory.string(), e.what());
			return false;
		}
	}

	void JoltCaptureManager::SetFrameLogInterval(i32 interval)
	{
		// Validate interval is positive
		if (interval <= 0)
		{
			OLO_CORE_WARN("Invalid frame log interval: {}. Must be > 0. Using default value of {}.", interval, s_DefaultFrameLogInterval);
			m_FrameLogInterval = s_DefaultFrameLogInterval; // Fallback to default
		}
		else
		{
			m_FrameLogInterval = interval;
			OLO_CORE_TRACE("Frame log interval set to {} frames", m_FrameLogInterval);
		}
	}

	void JoltCaptureManager::HandleCaptureFailure(const std::string& errorMessage)
	{
		OLO_PROFILE_FUNCTION();

		if (m_IsCapturing)
		{
			OLO_CORE_ERROR("Capture failure detected: {}. Stopping capture.", errorMessage);
			
			// Clear the capturing flag immediately
			m_IsCapturing = false;
			
			// Reset frame counter
			m_FrameCount = 0;
			
			// Close and cleanup the stream
			m_Stream.Close();
		}
	}

}