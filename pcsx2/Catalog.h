// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Catalog
{
	struct Entry
	{
		std::string fileName;    // e.g. "God of War (USA).chd"
		std::string title;       // Display title, e.g. "God of War"
		std::string coverUrl;    // Official cover image URL
		std::string downloadUrl; // Direct download URL (when available)
		std::string password;    // Optional archive password (null/empty if none)
		std::string groupKey;    // Grouping key (lowercased base title)

		// Local status
		bool isDownloaded = false;
		std::string localPath;   // Absolute local path on disk if present
		s64 fileSize = 0;        // Size in bytes if downloaded

		// Active download state (Módulo 4)
		bool isDownloading = false;
		bool isPaused = false;
		float downloadProgress = 0.0f; // 0.0f - 1.0f
		u64 downloadedBytes = 0;
		u64 totalBytes = 0;
		u64 savedAtMs = 0;

		bool IsChd() const;
		bool IsIso() const;
		std::string GetPartPath(const std::string& romsDir) const;
	};

	struct Group
	{
		std::string groupKey;         // Lowercased base title
		std::string title;            // Base display title (e.g. "God of War")
		std::string coverUrl;         // Primary cover URL (from best variant)
		std::string normalizedSearch; // Normalized string for fast search matching
		std::vector<Entry> variants;  // All variants (USA, Europe, Disc 1, etc.)
		size_t primaryIndex = 0;      // Index of default/primary variant to display

		bool isDownloaded = false;
		std::string localPath;        // Path to the downloaded variant if any

		const Entry& GetPrimaryEntry() const;
		Entry& GetPrimaryEntry();
		size_t GetVariantCount() const { return variants.size(); }
	};

	namespace Parser
	{
		/// Strips extension from filename (e.g. "God of War (USA).chd" -> "God of War (USA)").
		std::string StripExtension(const std::string_view name);

		/// Extracts base game title by stripping extensions and trailing parenthesized/bracketed groups
		/// (e.g. "God of War (USA).chd" -> "God of War").
		std::string BaseTitle(const std::string_view fileName);

		/// Computes the grouping key for a given filename.
		std::string GroupKey(const std::string_view fileName);

		/// Normalizes search text: lowercases, removes diacritics/accents, converts punctuation to spaces.
		std::string NormalizeSearchText(const std::string_view text);

		/// Checks whether a filename has an allowed extension (.chd or .iso).
		bool IsSupportedFormat(const std::string_view fileName);

		/// Checks whether a URL points to an archive (.rar, .7z, .zip).
		bool IsArchiveUrl(const std::string_view url);

		/// Parses a single line from catalog_manifest_ps2.txt.
		/// Returns true if valid and accepted by format filter (.chd and .iso only).
		bool ParseLine(const std::string_view line, Entry* out_entry);
	}

	class Manager
	{
	public:
		static Manager& GetInstance();

		/// Initializes the catalog manager and launches background loading.
		void Initialize();

		/// Shuts down worker threads and releases catalog resources.
		void Shutdown();

		/// Returns true when initial loading is completed.
		bool IsLoaded() const;

		/// Total number of individual ROM entries.
		size_t GetEntryCount() const;

		/// Total number of grouped titles (cards displayed in the catalog grid).
		size_t GetGroupCount() const;

		/// Returns a group by index.
		const Group* GetGroup(size_t index) const;

		/// Returns all catalog groups.
		const std::vector<Group>& GetGroups() const;

		/// Looks up an entry by filename.
		const Entry* FindEntryByFileName(const std::string_view fileName) const;

		/// Looks up a group by group key.
		const Group* FindGroupByGroupKey(const std::string_view groupKey) const;

		/// Fast instant search query matching all space-separated tokens against normalized titles.
		std::vector<size_t> SearchGroups(const std::string_view query) const;

		/// Rescans local ROM directories (EmuFolders::Roms and game scan paths)
		/// and updates isDownloaded and localPath across entries and groups.
		void RescanLocalRoms();

		/// Returns list of all downloaded entries for the "Salvos" tab.
		std::vector<Entry> GetDownloadedEntries() const;

		/// Total number of games currently saved/downloaded locally.
		size_t GetDownloadedCount() const;

		/// Explicitly marks a ROM as downloaded (called when a download finishes).
		void MarkDownloaded(const std::string_view fileName, const std::string_view localPath, s64 size = 0);

		/// Updates live download progress for UI rendering.
		void UpdateDownloadProgress(const std::string_view fileName, bool isDownloading, bool isPaused, float progress, u64 downloadedBytes, u64 totalBytes);

		/// Explicitly marks a ROM as removed/deleted.
		void MarkDeleted(const std::string_view fileName);

		/// Reloads manifest from disk (e.g. after cloud update).
		void ReloadManifest();

		/// Check for online manifest update asynchronously.
		void CheckForManifestUpdate();

	private:
		Manager() = default;
		~Manager();
		Manager(const Manager&) = delete;
		Manager& operator=(const Manager&) = delete;

		void LoadManifestWorker();
		void ProcessParsedEntries(std::vector<Entry> entries);
		void SelectPrimaryVariants();

		mutable std::recursive_mutex m_mutex;
		std::atomic<bool> m_is_loaded{false};
		std::atomic<bool> m_is_loading{false};
		std::atomic<bool> m_stop_requested{false};
		std::unique_ptr<std::thread> m_worker_thread;

		std::vector<Entry> m_entries;
		std::unordered_map<std::string, size_t> m_entry_map; // fileName -> index

		std::vector<Group> m_groups;
		std::unordered_map<std::string, size_t> m_group_map; // groupKey -> index

		// Local uncatalogued entries discovered on disk
		std::vector<Entry> m_local_only_entries;
	};
}
