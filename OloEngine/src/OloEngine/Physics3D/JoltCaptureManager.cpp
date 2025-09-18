#include "OloEnginePCH.h"
#include "JoltCaptureManager.h"

#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Core/Application.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace OloEngine {

	void JoltCaptureOutStream::Open(const std::filesystem::path& inPath)
	{
		OLO_PROFILE_FUNCTION();

		Close();
		m_Stream.open(inPath, std::ios::binary);
		if (!m_Stream.is_open())
		{
			OLO_CORE_ERROR("Failed to open capture file: {0}", inPath.string());
		}
	}

	void JoltCaptureOutStream::Close()
	{
		if (m_Stream.is_open())
		{
			m_Stream.close();
		}
	}

	void JoltCaptureOutStream::WriteBytes(const void* inData, sizet inNumBytes)
	{
		if (m_Stream.is_open())
		{
			m_Stream.write(static_cast<const char*>(inData), inNumBytes);
		}
	}

	bool JoltCaptureOutStream::IsFailed() const
	{
		return m_Stream.fail();
	}

	JoltCaptureManager::JoltCaptureManager()
	{
		OLO_PROFILE_FUNCTION();

		// Set default captures directory
		auto appDataPath = std::filesystem::path(std::getenv("APPDATA")) / "OloEngine" / "Captures";
		SetCapturesDirectory(appDataPath);
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
		auto time_t = std::chrono::system_clock::to_time_t(now);
		std::stringstream ss;
		ss << "capture_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S") << ".jolt";

		std::filesystem::path capturePath = m_CapturesDirectory / ss.str();

		// Open the capture stream
		m_Stream.Open(capturePath);
		
		if (m_Stream.IsOpen())
		{
			m_IsCapturing = true;
			m_RecentCapture = capturePath;
			
			// Add to captures list if not already present
			if (std::find(m_Captures.begin(), m_Captures.end(), capturePath) == m_Captures.end())
			{
				m_Captures.push_back(capturePath);
			}

			OLO_CORE_INFO("Started physics capture: {0}", capturePath.string());
		}
		else
		{
			OLO_CORE_ERROR("Failed to start physics capture - could not open file: {0}", capturePath.string());
		}
	}

	void JoltCaptureManager::CaptureFrame()
	{
		OLO_PROFILE_FUNCTION();

		if (!IsCapturing())
		{
			return;
		}

		// Basic capture functionality - currently just logs frame capture
		// Full implementation would require Jolt debug renderer integration
		// This provides the framework for when JPH_DEBUG_RENDERER is available
		
		static i32 frameCount = 0;
		frameCount++;
		
		if (frameCount % 60 == 0) // Log every 60 frames
		{
			OLO_CORE_TRACE("Captured physics frame {0}", frameCount);
		}
	}

	void JoltCaptureManager::EndCapture()
	{
		OLO_PROFILE_FUNCTION();

		if (!IsCapturing())
		{
			return;
		}

		m_Stream.Close();
		m_IsCapturing = false;

		OLO_CORE_INFO("Ended physics capture: {0}", m_RecentCapture.string());
	}

	bool JoltCaptureManager::IsCapturing() const
	{
		return m_IsCapturing && m_Stream.IsOpen();
	}

	void JoltCaptureManager::OpenCapture(const std::filesystem::path& capturePath) const
	{
		OLO_PROFILE_FUNCTION();

		if (!std::filesystem::exists(capturePath))
		{
			OLO_CORE_ERROR("Capture file does not exist: {0}", capturePath.string());
			return;
		}

		// Log the capture file path - user can manually open with external tools
		OLO_CORE_INFO("Capture file available: {0}", capturePath.string());
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
				OLO_CORE_ERROR("Failed to remove capture file {0}: {1}", capturePath.string(), e.what());
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
				if (std::filesystem::exists(capturePath))
				{
					std::filesystem::remove(capturePath);
				}
				
				m_Captures.erase(it);

				if (m_RecentCapture == capturePath)
				{
					m_RecentCapture.clear();
				}

				OLO_CORE_INFO("Removed physics capture: {0}", capturePath.string());
			}
			catch (const std::filesystem::filesystem_error& e)
			{
				OLO_CORE_ERROR("Failed to remove capture file {0}: {1}", capturePath.string(), e.what());
			}
		}
		else
		{
			OLO_CORE_WARN("Capture file not found in manager: {0}", capturePath.string());
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
			m_Captures.clear();
			for (const auto& entry : std::filesystem::directory_iterator(m_CapturesDirectory))
			{
				if (entry.is_regular_file() && entry.path().extension() == ".jolt")
				{
					m_Captures.push_back(entry.path());
				}
			}

			// Sort captures by modification time (newest first)
			std::sort(m_Captures.begin(), m_Captures.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
			{
				return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
			});

			if (!m_Captures.empty())
			{
				m_RecentCapture = m_Captures.front();
			}

			OLO_CORE_TRACE("Initialized captures directory: {0} (found {1} existing captures)", 
				m_CapturesDirectory.string(), m_Captures.size());
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			OLO_CORE_ERROR("Failed to initialize captures directory: {0}", e.what());
		}
	}

}