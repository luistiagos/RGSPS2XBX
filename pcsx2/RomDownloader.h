// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/Catalog.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace RomDownloader
{
	enum class DownloadStatus : u8
	{
		Idle,
		Queued,
		Resolving,
		CheckingSpace,
		Downloading,
		Paused,
		Completed,
		Failed,
		Cancelled
	};

	struct Job
	{
		std::string fileName;          // e.g. "God of War (USA).chd"
		std::string title;             // e.g. "God of War"
		std::string directUrl;         // Initial direct URL if available in manifest
		std::string activeUrl;         // Source URL currently being downloaded
		std::string destinationFolder; // Target directory (EmuFolders::Roms or custom USB drive)
		std::string partPath;          // Full path to .part file
		std::string finalPath;         // Full path to final .chd or .iso file

		DownloadStatus status = DownloadStatus::Idle;
		std::string statusMessage;     // User-facing progress or error description
		std::string errorMessage;

		u64 downloadedBytes = 0;
		u64 totalBytes = 0;
		float progress = 0.0f;         // 0.0f to 1.0f
		u32 speedBytesPerSec = 0;
		u32 retryCount = 0;

		bool IsCompleted() const { return status == DownloadStatus::Completed; }
		bool IsDownloading() const { return status == DownloadStatus::Downloading; }
		bool IsPaused() const { return status == DownloadStatus::Paused; }
		bool IsFailed() const { return status == DownloadStatus::Failed; }
	};

	class Manager
	{
	public:
		static Manager& GetInstance();

		/// Initializes worker thread and restores any interrupted downloads.
		void Initialize();

		/// Shuts down worker thread and flushes buffers safely.
		void Shutdown();

		/// Queues a ROM for streaming download (Módulo 4).
		bool EnqueueDownload(const Catalog::Entry& entry, const std::string& destinationFolder = "");

		/// Pauses the currently active download.
		void PauseCurrentDownload();

		/// Resumes the currently active or paused download.
		void ResumeCurrentDownload();

		/// Cancels the download for the given filename (or current active job if empty).
		void CancelDownload(const std::string_view fileName = "");

		/// Returns true if any download is active or queued.
		bool HasActiveDownloads() const;

		/// Returns true if the specified game is currently downloading or queued.
		bool IsGameDownloading(const std::string_view fileName) const;

		/// Returns true if the specified game is currently paused.
		bool IsGamePaused(const std::string_view fileName) const;

		/// Retrieves snapshot of current active job.
		bool GetCurrentJob(Job* out_job) const;

		/// Retrieves snapshot of job matching filename.
		bool GetJob(const std::string_view fileName, Job* out_job) const;

		/// Automatic pause hook when emulation starts.
		void OnVMStarting();

		/// Automatic resume hook when emulation terminates.
		void OnVMDestroyed();

		/// Resolves cascading download sources (.chd / .iso strictly).
		static std::vector<std::string> ResolveSources(const Catalog::Entry& entry);

	private:
		Manager();
		~Manager();
		Manager(const Manager&) = delete;
		Manager& operator=(const Manager&) = delete;

		void WorkerThreadFunc();
		bool ProcessJob(Job& job);
		bool PerformStreamingDownload(Job& job, const std::string& sourceUrl);
		bool CheckFreeSpace(const std::string& destinationFolder, u64 requiredBytes, std::string* out_error);

		mutable std::recursive_mutex m_mutex;
		std::atomic<bool> m_stop_requested{false};
		std::atomic<bool> m_pause_requested{false};
		std::atomic<bool> m_cancel_requested{false};
		std::unique_ptr<std::thread> m_worker_thread;

		std::unique_ptr<Job> m_current_job;
		std::deque<Job> m_queue;
		std::vector<Job> m_history;
	};
}
