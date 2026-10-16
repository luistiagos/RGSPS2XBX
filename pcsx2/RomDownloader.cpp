// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/RomDownloader.h"
#include "pcsx2/Catalog.h"
#include "pcsx2/Config.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include <chrono>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace RomDownloader
{
	Manager::Manager() = default;

	Manager::~Manager()
	{
		Shutdown();
	}

	Manager& Manager::GetInstance()
	{
		static Manager s_instance;
		return s_instance;
	}

	void Manager::Initialize()
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (m_worker_thread && m_worker_thread->joinable())
			return;

		m_stop_requested = false;
		m_pause_requested = false;
		m_cancel_requested = false;

		m_worker_thread = std::make_unique<std::thread>(&Manager::WorkerThreadFunc, this);
		Console.WriteLn("RomDownloader: Initialized streaming download manager");
	}

	void Manager::Shutdown()
	{
		{
			std::lock_guard<std::recursive_mutex> lock(m_mutex);
			m_stop_requested = true;
			m_cancel_requested = true;
		}

		if (m_worker_thread && m_worker_thread->joinable())
		{
			m_worker_thread->join();
			m_worker_thread.reset();
		}

		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		m_current_job.reset();
		m_queue.clear();
	}

	bool Manager::EnqueueDownload(const Catalog::Entry& entry, const std::string& destinationFolder)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);

		// Prevent duplicate queuing of the same filename
		if (IsGameDownloading(entry.fileName))
		{
			Console.Warning("RomDownloader: '%s' is already in download queue", entry.fileName.c_str());
			return false;
		}

		std::string targetDir = destinationFolder;
		if (targetDir.empty())
		{
			if (!EmuFolders::Roms.empty())
				targetDir = EmuFolders::Roms;
			else
				targetDir = Path::Combine(EmuFolders::DataRoot, "roms");
		}

		FileSystem::EnsureDirectoryExists(targetDir.c_str(), true);

		Job job;
		job.fileName = entry.fileName;
		job.title = entry.title.empty() ? Catalog::Parser::BaseTitle(entry.fileName) : entry.title;
		job.directUrl = entry.downloadUrl;
		job.destinationFolder = targetDir;
		job.finalPath = Path::Combine(targetDir, entry.fileName);
		job.partPath = Path::Combine(targetDir, entry.fileName + ".part");
		job.status = DownloadStatus::Queued;
		job.statusMessage = "Na fila de download...";

		// Mark active in catalog immediately for responsive UI
		Catalog::Manager::GetInstance().UpdateDownloadProgress(job.fileName, true, false, 0.0f, 0, 0);

		m_queue.push_back(std::move(job));
		Console.WriteLn("RomDownloader: Queued '%s' for download to '%s'", entry.fileName.c_str(), targetDir.c_str());
		return true;
	}

	void Manager::PauseCurrentDownload()
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		m_pause_requested = true;
		if (m_current_job)
		{
			m_current_job->status = DownloadStatus::Paused;
			m_current_job->statusMessage = "Pausado.";
			Catalog::Manager::GetInstance().UpdateDownloadProgress(
				m_current_job->fileName, true, true, m_current_job->progress, m_current_job->downloadedBytes, m_current_job->totalBytes);
		}
	}

	void Manager::ResumeCurrentDownload()
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		m_pause_requested = false;
		if (m_current_job)
		{
			m_current_job->status = DownloadStatus::Downloading;
			m_current_job->statusMessage = "Baixando...";
			Catalog::Manager::GetInstance().UpdateDownloadProgress(
				m_current_job->fileName, true, false, m_current_job->progress, m_current_job->downloadedBytes, m_current_job->totalBytes);
		}
	}

	void Manager::CancelDownload(const std::string_view fileName)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);

		// If cancelling specific queued job
		if (!fileName.empty())
		{
			for (auto it = m_queue.begin(); it != m_queue.end(); ++it)
			{
				if (it->fileName == fileName)
				{
					Catalog::Manager::GetInstance().UpdateDownloadProgress(it->fileName, false, false, 0.0f, 0, 0);
					m_queue.erase(it);
					Console.WriteLn("RomDownloader: Removed '%s' from download queue", std::string(fileName).c_str());
					return;
				}
			}
		}

		// Cancelling current active download
		if (m_current_job && (fileName.empty() || m_current_job->fileName == fileName))
		{
			m_cancel_requested = true;
			m_current_job->status = DownloadStatus::Cancelled;
			m_current_job->statusMessage = "Download cancelado.";
			Catalog::Manager::GetInstance().UpdateDownloadProgress(
				m_current_job->fileName, false, false, 0.0f, 0, 0);
			Console.WriteLn("RomDownloader: Cancel requested for active download '%s'", m_current_job->fileName.c_str());
		}
	}

	bool Manager::HasActiveDownloads() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		return (m_current_job != nullptr) || !m_queue.empty();
	}

	bool Manager::IsGameDownloading(const std::string_view fileName) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (m_current_job && m_current_job->fileName == fileName)
			return (m_current_job->status == DownloadStatus::Downloading || m_current_job->status == DownloadStatus::Queued ||
					m_current_job->status == DownloadStatus::Resolving || m_current_job->status == DownloadStatus::CheckingSpace);

		for (const auto& job : m_queue)
		{
			if (job.fileName == fileName)
				return true;
		}
		return false;
	}

	bool Manager::IsGamePaused(const std::string_view fileName) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (m_current_job && m_current_job->fileName == fileName)
			return (m_current_job->status == DownloadStatus::Paused);

		return false;
	}

	bool Manager::GetCurrentJob(Job* out_job) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_current_job || !out_job)
			return false;

		*out_job = *m_current_job;
		return true;
	}

	bool Manager::GetJob(const std::string_view fileName, Job* out_job) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!out_job)
			return false;

		if (m_current_job && m_current_job->fileName == fileName)
		{
			*out_job = *m_current_job;
			return true;
		}

		for (const auto& job : m_queue)
		{
			if (job.fileName == fileName)
			{
				*out_job = job;
				return true;
			}
		}

		return false;
	}

	void Manager::OnVMStarting()
	{
		// Auto-pause background downloads when emulation begins to reserve 100% CPU/GPU and I/O
		PauseCurrentDownload();
	}

	void Manager::OnVMDestroyed()
	{
		// Auto-resume background download when user returns to frontend
		ResumeCurrentDownload();
	}

	std::vector<std::string> Manager::ResolveSources(const Catalog::Entry& entry)
	{
		std::vector<std::string> sources;

		// 1. Direct manifest URL if present and meets strict .chd / .iso format filter
		if (!entry.downloadUrl.empty())
		{
			if (Catalog::Parser::IsSupportedFormat(entry.downloadUrl) && !Catalog::Parser::IsArchiveUrl(entry.downloadUrl))
			{
				sources.push_back(entry.downloadUrl);
			}
		}

		// URL encode filename for mirror query
		const std::string encoded_fn = Path::URLEncode(entry.fileName);

		// 2. High-speed Hugging Face mirrors for pure .chd PlayStation 2 games
		if (entry.IsChd() || StringUtil::EndsWithNoCase(entry.fileName, ".chd"))
		{
			sources.push_back(fmt::format("https://huggingface.co/datasets/luistiagos/ps2chd/resolve/main/{}", encoded_fn));
		}

		// 3. Internet Archive high-speed mirrors
		if (entry.IsChd())
		{
			sources.push_back(fmt::format("https://archive.org/download/ps2_chd_collection/{}", encoded_fn));
			sources.push_back(fmt::format("https://archive.org/download/chd_ps2/{}", encoded_fn));
		}
		else if (entry.IsIso())
		{
			sources.push_back(fmt::format("https://archive.org/download/sony_playstation2_iso/{}", encoded_fn));
		}

		return sources;
	}

	bool Manager::CheckFreeSpace(const std::string& destinationFolder, u64 requiredBytes, std::string* out_error)
	{
#ifdef _WIN32
		std::wstring wpath = FileSystem::GetWin32Path(destinationFolder);
		ULARGE_INTEGER freeBytesAvailableToCaller = {};
		ULARGE_INTEGER totalNumberOfBytes = {};
		ULARGE_INTEGER totalNumberOfFreeBytes = {};

		if (GetDiskFreeSpaceExW(wpath.c_str(), &freeBytesAvailableToCaller, &totalNumberOfBytes, &totalNumberOfFreeBytes))
		{
			// Require file size + 200 MB safety margin
			const u64 safetyMargin = 200ULL * 1024ULL * 1024ULL;
			const u64 needed = requiredBytes + safetyMargin;

			if (freeBytesAvailableToCaller.QuadPart < needed)
			{
				const double needGB = static_cast<double>(requiredBytes) / (1024.0 * 1024.0 * 1024.0);
				const double freeGB = static_cast<double>(freeBytesAvailableToCaller.QuadPart) / (1024.0 * 1024.0 * 1024.0);
				if (out_error)
				{
					*out_error = fmt::format(
						"Espaço insuficiente no armazenamento. O jogo necessita de {:.2f} GB, mas há apenas {:.2f} GB disponíveis. "
						"Exclua outros jogos na aba Salvos para liberar espaço.",
						needGB, freeGB);
				}
				return false;
			}
			return true;
		}
#endif
		return true;
	}

	void Manager::WorkerThreadFunc()
	{
		while (!m_stop_requested)
		{
			Job current_task;
			{
				std::lock_guard<std::recursive_mutex> lock(m_mutex);
				if (m_queue.empty())
				{
					m_current_job.reset();
				}
				else
				{
					current_task = std::move(m_queue.front());
					m_queue.pop_front();
					m_current_job = std::make_unique<Job>(current_task);
					m_cancel_requested = false;
				}
			}

			if (!m_current_job)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			ProcessJob(*m_current_job);
		}
	}

	bool Manager::ProcessJob(Job& job)
	{
		job.status = DownloadStatus::Resolving;
		job.statusMessage = "Resolvendo fontes de download...";

		Catalog::Entry entry_snapshot;
		const Catalog::Entry* cat_entry = Catalog::Manager::GetInstance().FindEntryByFileName(job.fileName);
		if (cat_entry)
		{
			entry_snapshot = *cat_entry;
		}
		else
		{
			entry_snapshot.fileName = job.fileName;
			entry_snapshot.title = job.title;
			entry_snapshot.downloadUrl = job.directUrl;
		}

		std::vector<std::string> sources = ResolveSources(entry_snapshot);
		if (sources.empty())
		{
			job.status = DownloadStatus::Failed;
			job.errorMessage = "Nenhuma fonte compatível (.chd/.iso) encontrada para este título.";
			job.statusMessage = job.errorMessage;
			Catalog::Manager::GetInstance().UpdateDownloadProgress(job.fileName, false, false, 0.0f, 0, 0);
			Console.Error("RomDownloader: Failed to resolve valid sources for '%s'", job.fileName.c_str());
			return false;
		}

		bool download_succeeded = false;
		for (const auto& sourceUrl : sources)
		{
			if (m_stop_requested || m_cancel_requested)
				break;

			// Strict V1 rule: discard any archive links
			if (Catalog::Parser::IsArchiveUrl(sourceUrl))
				continue;

			job.activeUrl = sourceUrl;
			Console.WriteLn("RomDownloader: Attempting download source '%s'", sourceUrl.c_str());

			// needs proper testing on varied network conditions and mirror latency
			if (PerformStreamingDownload(job, sourceUrl))
			{
				download_succeeded = true;
				break;
			}
			else if (m_cancel_requested)
			{
				break;
			}

			job.retryCount++;
		}

		if (download_succeeded)
		{
			// Atomic rename from .part to final .chd / .iso
			Error rename_error;
			if (FileSystem::RenamePath(job.partPath.c_str(), job.finalPath.c_str(), &rename_error))
			{
				job.status = DownloadStatus::Completed;
				job.progress = 1.0f;
				job.statusMessage = "Pronto para jogar!";

				Catalog::Manager::GetInstance().MarkDownloaded(job.fileName, job.finalPath, job.totalBytes);
				Catalog::Manager::GetInstance().RescanLocalRoms();
				GameList::Refresh(false);

				Host::ReportInfoAsync("Download Concluído",
					fmt::format("'{}' foi gravado e está disponível na aba Salvos!", job.title));
				ImGuiFullscreen::ShowToast("Download Concluído",
					fmt::format("'{}' pronto para jogar!", job.title), 5.0f);

				Console.WriteLn("RomDownloader: Successfully completed and installed '%s'", job.finalPath.c_str());
				return true;
			}
			else
			{
				job.status = DownloadStatus::Failed;
				job.errorMessage = fmt::format("Falha ao renomear arquivo temporário: {}", rename_error.GetDescription());
				job.statusMessage = job.errorMessage;
				Console.Error("RomDownloader: Rename error: %s", job.errorMessage.c_str());
				return false;
			}
		}
		else if (m_cancel_requested)
		{
			job.status = DownloadStatus::Cancelled;
			job.statusMessage = "Download cancelado.";
			Catalog::Manager::GetInstance().UpdateDownloadProgress(job.fileName, false, false, 0.0f, 0, 0);
			return false;
		}
		else
		{
			job.status = DownloadStatus::Failed;
			if (job.errorMessage.empty())
				job.errorMessage = "Falha ao baixar após esgotar fontes disponíveis.";
			job.statusMessage = job.errorMessage;
			Catalog::Manager::GetInstance().UpdateDownloadProgress(job.fileName, false, false, 0.0f, 0, 0);
			return false;
		}
	}

	bool Manager::PerformStreamingDownload(Job& job, const std::string& sourceUrl)
	{
		HINTERNET hSession = WinHttpOpen(L"XBSX2-Downloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!hSession)
		{
			job.errorMessage = "Falha ao inicializar WinHttp.";
			return false;
		}

		// Configure reasonable timeouts
		WinHttpSetTimeouts(hSession, 15000, 15000, 20000, 20000);

		std::wstring host_name;
		std::wstring object_path;
		host_name.resize(sourceUrl.size());
		object_path.resize(sourceUrl.size());

		URL_COMPONENTSW uc = {};
		uc.dwStructSize = sizeof(uc);
		uc.lpszHostName = host_name.data();
		uc.dwHostNameLength = static_cast<DWORD>(host_name.size());
		uc.lpszUrlPath = object_path.data();
		uc.dwUrlPathLength = static_cast<DWORD>(object_path.size());

		const std::wstring url_wide = StringUtil::UTF8StringToWideString(sourceUrl);
		if (!WinHttpCrackUrl(url_wide.c_str(), static_cast<DWORD>(url_wide.size()), 0, &uc))
		{
			WinHttpCloseHandle(hSession);
			return false;
		}

		host_name.resize(uc.dwHostNameLength);
		object_path.resize(uc.dwUrlPathLength);
		if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo)
			object_path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

		HINTERNET hConnect = WinHttpConnect(hSession, host_name.c_str(), uc.nPort, 0);
		if (!hConnect)
		{
			WinHttpCloseHandle(hSession);
			return false;
		}

		DWORD open_flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", object_path.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, open_flags);
		if (!hRequest)
		{
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		// Automatically follow HTTP 301, 302, 307 redirects
		DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
		WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

		// Check for existing partial file to resume
		s64 existing_bytes = 0;
		if (FileSystem::FileExists(job.partPath.c_str()))
		{
			existing_bytes = FileSystem::GetPathFileSize(job.partPath.c_str());
		}

		if (existing_bytes > 0)
		{
			std::wstring range_hdr = StringUtil::UTF8StringToWideString(fmt::format("Range: bytes={}-", existing_bytes));
			WinHttpAddRequestHeaders(hRequest, range_hdr.c_str(), static_cast<DWORD>(range_hdr.length()),
				WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
		}

		if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			!WinHttpReceiveResponse(hRequest, nullptr))
		{
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		DWORD status_code = 0;
		DWORD dwSize = sizeof(status_code);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &dwSize, WINHTTP_NO_HEADER_INDEX);

		if (status_code != 200 && status_code != 206)
		{
			Console.Warning("RomDownloader: HTTP response code %u for '%s'", status_code, sourceUrl.c_str());
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		// Query total file length
		u64 content_len = 0;
		wchar_t len_buf[64] = {};
		dwSize = sizeof(len_buf);
		if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
				len_buf, &dwSize, WINHTTP_NO_HEADER_INDEX))
		{
			content_len = _wcstoui64(len_buf, nullptr, 10);
		}

		const bool is_resume = (status_code == 206 && existing_bytes > 0);
		u64 expected_total = is_resume ? (static_cast<u64>(existing_bytes) + content_len) : content_len;

		// Pre-allocation and disk free space check
		if (!is_resume && expected_total > 0)
		{
			std::string space_error;
			job.status = DownloadStatus::CheckingSpace;
			if (!CheckFreeSpace(job.destinationFolder, expected_total, &space_error))
			{
				job.status = DownloadStatus::Failed;
				job.errorMessage = std::move(space_error);
				job.statusMessage = job.errorMessage;
				Host::ReportInfoAsync("Armazenamento Insuficiente", job.errorMessage);
				WinHttpCloseHandle(hRequest);
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				return false;
			}
		}

		// Open output file stream (append for range resume, overwrite for fresh download)
		FILE* fp = nullptr;
		const char* file_mode = is_resume ? "ab" : "wb";
		fp = FileSystem::OpenCFile(job.partPath.c_str(), file_mode);
		if (!fp)
		{
			job.errorMessage = "Falha ao abrir arquivo temporário para gravação.";
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		job.status = DownloadStatus::Downloading;
		job.downloadedBytes = is_resume ? static_cast<u64>(existing_bytes) : 0;
		job.totalBytes = expected_total;
		job.progress = (expected_total > 0) ? (static_cast<float>(job.downloadedBytes) / static_cast<float>(expected_total)) : 0.0f;

		// 128 KB chunk buffer for smooth continuous streaming without memory pressure
		std::vector<u8> chunk(128 * 1024);
		auto last_speed_time = std::chrono::steady_clock::now();
		u64 bytes_since_last_speed = 0;
		u64 bytes_since_flush = 0;

		bool success = true;
		while (true)
		{
			if (m_stop_requested)
			{
				success = false;
				break;
			}

			if (m_cancel_requested)
			{
				success = false;
				break;
			}

			// Handle emulation pause hook
			if (m_pause_requested)
			{
				std::fflush(fp);
				job.status = DownloadStatus::Paused;
				job.statusMessage = "Pausado (Emulação em andamento ou solicitado pelo usuário)";
				Catalog::Manager::GetInstance().UpdateDownloadProgress(
					job.fileName, true, true, job.progress, job.downloadedBytes, job.totalBytes);

				while (m_pause_requested && !m_stop_requested && !m_cancel_requested)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}

				if (m_stop_requested || m_cancel_requested)
				{
					success = false;
					break;
				}

				job.status = DownloadStatus::Downloading;
				last_speed_time = std::chrono::steady_clock::now();
				bytes_since_last_speed = 0;
			}

			DWORD bytes_read = 0;
			if (!WinHttpReadData(hRequest, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read))
			{
				Console.Error("RomDownloader: WinHttpReadData error %u", GetLastError());
				success = false;
				break;
			}

			if (bytes_read == 0)
			{
				// End of stream reached
				break;
			}

			const size_t written = std::fwrite(chunk.data(), 1, bytes_read, fp);
			if (written != bytes_read)
			{
				Console.Error("RomDownloader: Disk write error on '%s'", job.partPath.c_str());
				job.errorMessage = "Erro de I/O ao gravar no disco.";
				success = false;
				break;
			}

			job.downloadedBytes += bytes_read;
			bytes_since_last_speed += bytes_read;
			bytes_since_flush += bytes_read;

			// Flush buffer to disk every 2 MB to ensure safety if console suspends
			if (bytes_since_flush >= (2 * 1024 * 1024))
			{
				std::fflush(fp);
				bytes_since_flush = 0;
			}

			// Calculate transfer speed and update catalog UI every 400ms
			auto now = std::chrono::steady_clock::now();
			auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_speed_time).count();
			if (elapsed_ms >= 400)
			{
				job.speedBytesPerSec = static_cast<u32>((bytes_since_last_speed * 1000) / elapsed_ms);
				if (job.totalBytes > 0)
					job.progress = static_cast<float>(job.downloadedBytes) / static_cast<float>(job.totalBytes);

				const double mb_down = static_cast<double>(job.downloadedBytes) / (1024.0 * 1024.0);
				const double mb_total = static_cast<double>(job.totalBytes) / (1024.0 * 1024.0);
				const double mb_speed = static_cast<double>(job.speedBytesPerSec) / (1024.0 * 1024.0);

				job.statusMessage = fmt::format("{:.0f}% ({:.1f} MB / {:.1f} MB) - {:.1f} MB/s",
					job.progress * 100.0f, mb_down, mb_total, mb_speed);

				Catalog::Manager::GetInstance().UpdateDownloadProgress(
					job.fileName, true, false, job.progress, job.downloadedBytes, job.totalBytes);

				last_speed_time = now;
				bytes_since_last_speed = 0;
			}
		}

		std::fflush(fp);
		std::fclose(fp);

		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);

		// If totalBytes was known, ensure complete payload arrived
		if (success && job.totalBytes > 0 && job.downloadedBytes < job.totalBytes)
		{
			Console.Warning("RomDownloader: Incomplete transfer (%llu / %llu bytes)", job.downloadedBytes, job.totalBytes);
			return false;
		}

		return success;
	}
}
