// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog.h"
#include "Config.h"
#include "Host.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SettingsInterface.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <sstream>

namespace Catalog
{
	static Entry s_empty_entry;

	bool Entry::IsChd() const
	{
		return StringUtil::EndsWithNoCase(fileName, ".chd");
	}

	bool Entry::IsIso() const
	{
		return StringUtil::EndsWithNoCase(fileName, ".iso");
	}

	std::string Entry::GetPartPath(const std::string& romsDir) const
	{
		return Path::Combine(romsDir, fileName + ".part");
	}

	const Entry& Group::GetPrimaryEntry() const
	{
		if (variants.empty())
			return s_empty_entry;
		return (primaryIndex < variants.size()) ? variants[primaryIndex] : variants.front();
	}

	Entry& Group::GetPrimaryEntry()
	{
		if (variants.empty())
			return s_empty_entry;
		return (primaryIndex < variants.size()) ? variants[primaryIndex] : variants.front();
	}

	namespace Parser
	{
		std::string StripExtension(const std::string_view name)
		{
			const size_t dot = name.find_last_of('.');
			if (dot != std::string_view::npos && dot > 0)
				return std::string(name.substr(0, dot));
			return std::string(name);
		}

		std::string BaseTitle(const std::string_view fileName)
		{
			std::string noExt = StripExtension(fileName);
			size_t end = noExt.length();
			while (true)
			{
				while (end > 0 && noExt[end - 1] == ' ')
					end--;
				if (end == 0)
					break;

				const char close = noExt[end - 1];
				char open = 0;
				if (close == ')')
					open = '(';
				else if (close == ']')
					open = '[';
				else
					break;

				const size_t start = noExt.rfind(open, (end >= 2) ? end - 2 : 0);
				if (start == std::string::npos)
					break;
				end = start;
			}

			// Clean any leftover trailing separators between title and first stripped suffix
			while (end > 0)
			{
				const char c = noExt[end - 1];
				if (c == ' ' || c == '-' || c == '_')
					end--;
				else
					break;
			}

			const std::string base = noExt.substr(0, end);
			return base.empty() ? noExt : base;
		}

		std::string GroupKey(const std::string_view fileName)
		{
			std::string base = BaseTitle(fileName);
			for (char& c : base)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c + ('a' - 'A'));
			}
			return base;
		}

		std::string NormalizeSearchText(const std::string_view text)
		{
			std::string result;
			result.reserve(text.size());

			bool last_was_space = true;
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				// Fold common UTF-8 Latin accented characters (needs proper testing on international titles)
				if (c == 0xC3 && (i + 1) < text.size())
				{
					const unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
					i += 2;
					char mapped = 0;
					switch (c2)
					{
						case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: // À-Å
						case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: // à-å
							mapped = 'a'; break;
						case 0x88: case 0x89: case 0x8A: case 0x8B: // È-Ë
						case 0xA8: case 0xA9: case 0xAA: case 0xAB: // è-ë
							mapped = 'e'; break;
						case 0x8C: case 0x8D: case 0x8E: case 0x8F: // Ì-Ï
						case 0xAC: case 0xAD: case 0xAE: case 0xAF: // ì-ï
							mapped = 'i'; break;
						case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x98: // Ò-Ö, Ø
						case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB8: // ò-ö, ø
							mapped = 'o'; break;
						case 0x99: case 0x9A: case 0x9B: case 0x9C: // Ù-Ü
						case 0xB9: case 0xBA: case 0xBB: case 0xBC: // ù-ü
							mapped = 'u'; break;
						case 0x87: case 0xA7: // Ç, ç
							mapped = 'c'; break;
						case 0x91: case 0xB1: // Ñ, ñ
							mapped = 'n'; break;
						default:
							mapped = ' '; break;
					}
					if (mapped != ' ')
					{
						result.push_back(mapped);
						last_was_space = false;
					}
					else if (!last_was_space)
					{
						result.push_back(' ');
						last_was_space = true;
					}
					continue;
				}

				i++;
				if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				{
					result.push_back(static_cast<char>(c));
					last_was_space = false;
				}
				else if (c >= 'A' && c <= 'Z')
				{
					result.push_back(static_cast<char>(c + ('a' - 'A')));
					last_was_space = false;
				}
				else
				{
					if (!last_was_space)
					{
						result.push_back(' ');
						last_was_space = true;
					}
				}
			}

			if (!result.empty() && result.back() == ' ')
				result.pop_back();

			return result;
		}

		bool IsSupportedFormat(const std::string_view fileName)
		{
			return StringUtil::EndsWithNoCase(fileName, ".chd") || StringUtil::EndsWithNoCase(fileName, ".iso");
		}

		bool IsArchiveUrl(const std::string_view url)
		{
			if (StringUtil::EndsWithNoCase(url, ".7z") ||
				StringUtil::EndsWithNoCase(url, ".zip") ||
				StringUtil::EndsWithNoCase(url, ".rar"))
			{
				return true;
			}

			std::string lower(url);
			for (char& c : lower)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c + ('a' - 'A'));
			}

			return (lower.find(".7z/") != std::string::npos ||
					lower.find(".zip/") != std::string::npos ||
					lower.find(".rar/") != std::string::npos);
		}

		bool ParseLine(const std::string_view line_view, Entry* out_entry)
		{
			const std::string_view line = StringUtil::StripWhitespace(line_view);
			if (line.empty() || line.front() == '#')
				return false;

			// Split by '|'
			std::vector<std::string_view> parts;
			size_t start = 0;
			while (start <= line.size())
			{
				const size_t pipe = line.find('|', start);
				if (pipe == std::string_view::npos)
				{
					parts.push_back(line.substr(start));
					break;
				}
				parts.push_back(line.substr(start, pipe - start));
				start = pipe + 1;
			}

			if (parts.empty())
				return false;

			const std::string_view rawPath = StringUtil::StripWhitespace(parts[0]);
			const size_t last_slash = rawPath.find_last_of("/\\");
			const std::string_view fileNameView = (last_slash != std::string_view::npos) ? rawPath.substr(last_slash + 1) : rawPath;

			if (fileNameView.empty())
				return false;

			// Strict V1 format filter: accept exclusively .chd and .iso
			if (!IsSupportedFormat(fileNameView))
				return false;

			std::string fileName(fileNameView);
			std::string title = StripExtension(fileName);
			std::string coverUrl;
			std::string downloadUrl;
			std::string password;

			auto isHttpUrl = [](const std::string_view s) {
				return StringUtil::StartsWithNoCase(s, "http://") || StringUtil::StartsWithNoCase(s, "https://");
			};

			auto isImageUrl = [](const std::string_view s) {
				if (StringUtil::EndsWithNoCase(s, ".png") || StringUtil::EndsWithNoCase(s, ".jpg") ||
					StringUtil::EndsWithNoCase(s, ".jpeg") || StringUtil::EndsWithNoCase(s, ".webp"))
					return true;
				std::string lower(s);
				for (char& c : lower)
				{
					if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
				}
				return (lower.find("/images.igdb.com/") != std::string::npos ||
						lower.find("libretro-thumbnails") != std::string::npos ||
						lower.find("/covers/") != std::string::npos ||
						lower.find("boxart") != std::string::npos);
			};

			auto isDownloadUrl = [&](const std::string_view s) {
				if (!isHttpUrl(s) || isImageUrl(s))
					return false;
				if (StringUtil::EndsWithNoCase(s, ".iso") || StringUtil::EndsWithNoCase(s, ".chd") ||
					StringUtil::EndsWithNoCase(s, ".bin") || StringUtil::EndsWithNoCase(s, ".cso") ||
					StringUtil::EndsWithNoCase(s, ".gz") || StringUtil::EndsWithNoCase(s, ".zip") ||
					StringUtil::EndsWithNoCase(s, ".7z") || StringUtil::EndsWithNoCase(s, ".rar"))
					return true;
				std::string lower(s);
				for (char& c : lower)
				{
					if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
				}
				return (lower.find("/resolve/") != std::string::npos ||
						lower.find("archive.org/download") != std::string::npos ||
						lower.find("mediafire.com") != std::string::npos ||
						lower.find("dubsgamer.com/go/") != std::string::npos ||
						lower.find("phoenixtutors.com.br") != std::string::npos);
			};

			if (parts.size() == 1)
			{
				// Format: path
			}
			else if (parts.size() == 2)
			{
				const std::string_view p1 = StringUtil::StripWhitespace(parts[1]);
				if (isHttpUrl(p1))
				{
					if (isDownloadUrl(p1))
						downloadUrl = std::string(p1);
					else
						coverUrl = std::string(p1);
				}
				else if (!p1.empty())
				{
					title = std::string(p1);
				}
			}
			else if (parts.size() == 3)
			{
				const std::string_view p1 = StringUtil::StripWhitespace(parts[1]);
				const std::string_view p2 = StringUtil::StripWhitespace(parts[2]);
				if (isHttpUrl(p1) && isHttpUrl(p2))
				{
					if (isDownloadUrl(p2) || isImageUrl(p1))
					{
						coverUrl = std::string(p1);
						downloadUrl = std::string(p2);
					}
					else
					{
						downloadUrl = std::string(p1);
						coverUrl = std::string(p2);
					}
				}
				else if (isHttpUrl(p1))
				{
					if (isDownloadUrl(p1))
						downloadUrl = std::string(p1);
					else
						coverUrl = std::string(p1);
					if (!p2.empty())
						title = std::string(p2);
				}
				else
				{
					if (!p1.empty())
						title = std::string(p1);
					if (isDownloadUrl(p2))
						downloadUrl = std::string(p2);
					else
						coverUrl = std::string(p2);
				}
			}
			else
			{
				const std::string_view p1 = StringUtil::StripWhitespace(parts[1]);
				const std::string_view p2 = StringUtil::StripWhitespace(parts[2]);
				const std::string_view p3 = StringUtil::StripWhitespace(parts[3]);
				if (!p1.empty() && !isHttpUrl(p1))
					title = std::string(p1);
				if (isHttpUrl(p2) && isHttpUrl(p3))
				{
					if (isDownloadUrl(p2) || isImageUrl(p3))
					{
						downloadUrl = std::string(p2);
						coverUrl = std::string(p3);
					}
					else
					{
						coverUrl = std::string(p2);
						downloadUrl = std::string(p3);
					}
				}
				else
				{
					coverUrl = std::string(p2);
					downloadUrl = std::string(p3);
				}
				if (parts.size() > 4)
				{
					const std::string_view p4 = StringUtil::StripWhitespace(parts[4]);
					if (!p4.empty() && !StringUtil::StartsWithNoCase(p4, "null") && !StringUtil::StartsWithNoCase(p4, "sem senha"))
						password = std::string(p4);
				}
			}

			// Safety fallback: title should never be an HTTP URL
			if (isHttpUrl(title))
				title = StripExtension(fileName);

			// Strict filter: accept only titles with an active direct .chd / .iso download link
			if (downloadUrl.empty() || IsArchiveUrl(downloadUrl))
				return false;

			if (out_entry)
			{
				out_entry->fileName = std::move(fileName);
				out_entry->title = std::move(title);
				out_entry->coverUrl = std::move(coverUrl);
				out_entry->downloadUrl = std::move(downloadUrl);
				out_entry->password = std::move(password);
				out_entry->groupKey = GroupKey(out_entry->fileName);
			}

			return true;
		}
	}

	Manager& Manager::GetInstance()
	{
		static Manager instance;
		return instance;
	}

	Manager::~Manager()
	{
		Shutdown();
	}

	void Manager::Initialize()
	{
		if (m_is_loading.exchange(true))
			return;

		m_stop_requested = false;
		m_worker_thread = std::make_unique<std::thread>(&Manager::LoadManifestWorker, this);
	}

	void Manager::Shutdown()
	{
		m_stop_requested = true;
		if (m_worker_thread && m_worker_thread->joinable())
		{
			m_worker_thread->join();
			m_worker_thread.reset();
		}
		m_is_loading = false;
	}

	bool Manager::IsLoaded() const
	{
		return m_is_loaded.load();
	}

	size_t Manager::GetEntryCount() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		return m_entries.size();
	}

	size_t Manager::GetGroupCount() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		return m_groups.size();
	}

	const Group* Manager::GetGroup(size_t index) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		return (index < m_groups.size()) ? &m_groups[index] : nullptr;
	}

	const std::vector<Group>& Manager::GetGroups() const
	{
		return m_groups;
	}

	const Entry* Manager::FindEntryByFileName(const std::string_view fileName) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		const auto it = m_entry_map.find(std::string(fileName));
		if (it != m_entry_map.end() && it->second < m_entries.size())
			return &m_entries[it->second];
		return nullptr;
	}

	const Group* Manager::FindGroupByGroupKey(const std::string_view groupKey) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		const auto it = m_group_map.find(std::string(groupKey));
		if (it != m_group_map.end() && it->second < m_groups.size())
			return &m_groups[it->second];
		return nullptr;
	}

	std::vector<std::string> Manager::GetGroupVariantFileNames(const std::string_view groupKey) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		std::vector<std::string> fileNames;
		const auto it = m_group_map.find(std::string(groupKey));
		if (it != m_group_map.end() && it->second < m_groups.size())
		{
			for (const auto& variant : m_groups[it->second].variants)
				fileNames.push_back(variant.fileName);
		}
		return fileNames;
	}

	std::vector<size_t> Manager::SearchGroups(const std::string_view query) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		std::vector<size_t> results;

		const std::string normalized_query = Parser::NormalizeSearchText(query);
		if (normalized_query.empty())
		{
			results.resize(m_groups.size());
			for (size_t i = 0; i < m_groups.size(); ++i)
				results[i] = i;
			return results;
		}

		// Split into space-delimited search terms
		std::vector<std::string_view> terms;
		size_t start = 0;
		while (start < normalized_query.size())
		{
			const size_t sp = normalized_query.find(' ', start);
			if (sp == std::string::npos)
			{
				terms.push_back(std::string_view(normalized_query).substr(start));
				break;
			}
			terms.push_back(std::string_view(normalized_query).substr(start, sp - start));
			start = sp + 1;
		}

		results.reserve(m_groups.size() / 4);
		for (size_t i = 0; i < m_groups.size(); ++i)
		{
			const std::string& haystack = m_groups[i].normalizedSearch;
			bool match_all = true;
			for (const auto& term : terms)
			{
				if (haystack.find(term) == std::string::npos)
				{
					match_all = false;
					break;
				}
			}
			if (match_all)
				results.push_back(i);
		}

		return results;
	}

	void Manager::RescanLocalRoms()
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);

		// Reset downloaded flags
		for (auto& entry : m_entries)
		{
			entry.isDownloaded = false;
			entry.localPath.clear();
			entry.fileSize = 0;
		}
		for (auto& group : m_groups)
		{
			group.isDownloaded = false;
			group.localPath.clear();
		}
		m_local_only_entries.clear();

		// Candidate scan directories: EmuFolders::Roms + settings paths
		std::vector<std::string> scan_dirs;
		if (!EmuFolders::Roms.empty())
			scan_dirs.push_back(EmuFolders::Roms);

		auto si = Host::GetSettingsInterface();
		if (si)
		{
			const std::vector<std::string> custom_paths = si->GetStringList("GameList", "RecursivePaths");
			for (const auto& path : custom_paths)
			{
				if (std::find(scan_dirs.begin(), scan_dirs.end(), path) == scan_dirs.end())
					scan_dirs.push_back(path);
			}
		}

		// Map stripped names for O(1) case-insensitive lookup
		std::unordered_map<std::string, size_t> stripped_entry_map;
		for (size_t i = 0; i < m_entries.size(); ++i)
		{
			std::string stripped_lower = Parser::StripExtension(m_entries[i].fileName);
			for (char& c : stripped_lower)
			{
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
			}
			stripped_entry_map.emplace(std::move(stripped_lower), i);
		}

		for (const auto& dir : scan_dirs)
		{
			if (m_stop_requested)
				return;

			if (!FileSystem::DirectoryExists(dir.c_str()))
				continue;

			FileSystem::FindResultsArray files;
			FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES, &files);

			for (const auto& f : files)
			{
				const std::string leaf(Path::GetFileName(f.FileName));

				// Ignore partial download files and markers
				if (StringUtil::EndsWithNoCase(leaf, ".part") || StringUtil::EndsWithNoCase(leaf, ".part.src"))
					continue;

				if (!Parser::IsSupportedFormat(leaf))
					continue;

				// Try exact filename match first
				auto it_exact = m_entry_map.find(leaf);
				size_t matched_idx = (it_exact != m_entry_map.end()) ? it_exact->second : size_t(-1);

				// Fall back to stripped name match (e.g. .iso downloaded as .chd)
				if (matched_idx == size_t(-1))
				{
					std::string stripped_lower = Parser::StripExtension(leaf);
					for (char& c : stripped_lower)
					{
						if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
					}
					auto it_stripped = stripped_entry_map.find(stripped_lower);
					if (it_stripped != stripped_entry_map.end())
						matched_idx = it_stripped->second;
				}

				if (matched_idx != size_t(-1) && matched_idx < m_entries.size())
				{
					Entry& entry = m_entries[matched_idx];
					entry.isDownloaded = true;
					entry.localPath = f.FileName;
					entry.fileSize = f.Size;

					// Propagate status to corresponding group
					auto git = m_group_map.find(entry.groupKey);
					if (git != m_group_map.end() && git->second < m_groups.size())
					{
						Group& group = m_groups[git->second];
						group.isDownloaded = true;
						group.localPath = f.FileName;

						// Promote downloaded variant to primary index
						for (size_t v = 0; v < group.variants.size(); ++v)
						{
							if (group.variants[v].fileName == entry.fileName)
							{
								group.variants[v].isDownloaded = true;
								group.variants[v].localPath = f.FileName;
								group.variants[v].fileSize = f.Size;
								group.primaryIndex = v;
								break;
							}
						}
					}
				}
				else
				{
					// ROM present on disk but not in catalog manifest
					Entry local_entry;
					local_entry.fileName = leaf;
					local_entry.title = Parser::BaseTitle(leaf);
					local_entry.groupKey = Parser::GroupKey(leaf);
					local_entry.isDownloaded = true;
					local_entry.localPath = f.FileName;
					local_entry.fileSize = f.Size;
					m_local_only_entries.push_back(std::move(local_entry));
				}
			}
		}

		SelectPrimaryVariants();
	}

	std::vector<Entry> Manager::GetDownloadedEntries() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		std::vector<Entry> downloaded;
		downloaded.reserve(m_local_only_entries.size() + 16);

		for (const auto& entry : m_entries)
		{
			if (entry.isDownloaded)
				downloaded.push_back(entry);
		}
		for (const auto& local : m_local_only_entries)
		{
			downloaded.push_back(local);
		}

		return downloaded;
	}

	size_t Manager::GetDownloadedCount() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		size_t count = m_local_only_entries.size();
		for (const auto& entry : m_entries)
		{
			if (entry.isDownloaded)
				count++;
		}
		return count;
	}

	void Manager::MarkDownloaded(const std::string_view fileName, const std::string_view localPath, s64 size)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		const std::string fn(fileName);
		auto it = m_entry_map.find(fn);
		if (it != m_entry_map.end() && it->second < m_entries.size())
		{
			Entry& entry = m_entries[it->second];
			entry.isDownloaded = true;
			entry.localPath = std::string(localPath);
			entry.fileSize = size;
			entry.isDownloading = false;
			entry.downloadProgress = 1.0f;

			auto git = m_group_map.find(entry.groupKey);
			if (git != m_group_map.end() && git->second < m_groups.size())
			{
				Group& group = m_groups[git->second];
				group.isDownloaded = true;
				group.localPath = entry.localPath;

				for (size_t v = 0; v < group.variants.size(); ++v)
				{
					if (group.variants[v].fileName == entry.fileName)
					{
						group.variants[v].isDownloaded = true;
						group.variants[v].localPath = entry.localPath;
						group.variants[v].fileSize = size;
						group.primaryIndex = v;
						break;
					}
				}
			}
		}
	}

	void Manager::UpdateDownloadProgress(const std::string_view fileName, bool isDownloading, bool isPaused, float progress, u64 downloadedBytes, u64 totalBytes)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		const std::string fn(fileName);
		auto it = m_entry_map.find(fn);
		if (it != m_entry_map.end() && it->second < m_entries.size())
		{
			Entry& entry = m_entries[it->second];
			entry.isDownloading = isDownloading;
			entry.isPaused = isPaused;
			entry.downloadProgress = progress;
			entry.downloadedBytes = downloadedBytes;
			entry.totalBytes = totalBytes;

			auto git = m_group_map.find(entry.groupKey);
			if (git != m_group_map.end() && git->second < m_groups.size())
			{
				Group& group = m_groups[git->second];
				for (size_t v = 0; v < group.variants.size(); ++v)
				{
					if (group.variants[v].fileName == entry.fileName)
					{
						group.variants[v].isDownloading = isDownloading;
						group.variants[v].isPaused = isPaused;
						group.variants[v].downloadProgress = progress;
						group.variants[v].downloadedBytes = downloadedBytes;
						group.variants[v].totalBytes = totalBytes;
						break;
					}
				}
			}
		}
	}

	void Manager::MarkDeleted(const std::string_view fileName)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		const std::string fn(fileName);

		// Remove from uncatalogued list if present
		m_local_only_entries.erase(
			std::remove_if(m_local_only_entries.begin(), m_local_only_entries.end(),
				[&](const Entry& e) { return e.fileName == fn || e.localPath == fn; }),
			m_local_only_entries.end());

		auto it = m_entry_map.find(fn);
		if (it != m_entry_map.end() && it->second < m_entries.size())
		{
			Entry& entry = m_entries[it->second];
			entry.isDownloaded = false;
			entry.localPath.clear();
			entry.fileSize = 0;

			auto git = m_group_map.find(entry.groupKey);
			if (git != m_group_map.end() && git->second < m_groups.size())
			{
				Group& group = m_groups[git->second];
				bool any_downloaded = false;
				for (size_t v = 0; v < group.variants.size(); ++v)
				{
					if (group.variants[v].fileName == entry.fileName)
					{
						group.variants[v].isDownloaded = false;
						group.variants[v].localPath.clear();
						group.variants[v].fileSize = 0;
					}
					else if (group.variants[v].isDownloaded)
					{
						any_downloaded = true;
						group.primaryIndex = v;
						group.localPath = group.variants[v].localPath;
					}
				}
				group.isDownloaded = any_downloaded;
				if (!any_downloaded)
				{
					group.localPath.clear();
					SelectPrimaryVariants();
				}
			}
		}
	}

	void Manager::ReloadManifest()
	{
		Initialize();
	}

	void Manager::CheckForManifestUpdate()
	{
		// Asynchronous check for updated manifest in background (needs proper testing on console network)
	}

	void Manager::LoadManifestWorker()
	{
		Console.WriteLn("CatalogManager: Loading catalog manifest in background thread...");

		// Search priority: 1. Cloud-updated cache, 2. Resources folder, 3. Package AppRoot/bin resources
		std::vector<std::string> candidate_paths = {
			Path::Combine(EmuFolders::DataRoot, "catalog_manifest_ps2.txt"),
			Path::Combine(EmuFolders::Resources, "catalog_manifest_ps2.txt"),
			Path::Combine(EmuFolders::AppRoot, "resources" FS_OSPATH_SEPARATOR_STR "catalog_manifest_ps2.txt"),
			Path::Combine(EmuFolders::AppRoot, "bin" FS_OSPATH_SEPARATOR_STR "resources" FS_OSPATH_SEPARATOR_STR "catalog_manifest_ps2.txt"),
			Path::Combine(EmuFolders::AppRoot, ".." FS_OSPATH_SEPARATOR_STR "bin" FS_OSPATH_SEPARATOR_STR "resources" FS_OSPATH_SEPARATOR_STR "catalog_manifest_ps2.txt"),
		};

		std::string active_path;
		for (const auto& p : candidate_paths)
		{
			if (FileSystem::FileExists(p.c_str()))
			{
				active_path = p;
				break;
			}
		}

		if (active_path.empty())
		{
			Console.Warning("CatalogManager: No catalog_manifest_ps2.txt found in candidate resource paths.");
			m_is_loading = false;
			m_is_loaded = true;
			return;
		}

		Console.WriteLnFmt("CatalogManager: Reading manifest from '{}'...", active_path);
		auto content_opt = FileSystem::ReadFileToString(active_path.c_str());
		if (!content_opt.has_value() || content_opt->empty())
		{
			Console.ErrorFmt("CatalogManager: Failed to read manifest from '{}'.", active_path);
			m_is_loading = false;
			m_is_loaded = true;
			return;
		}

		std::string content = std::move(*content_opt);
		std::vector<Entry> parsed_entries;
		parsed_entries.reserve(16384);

		size_t line_start = 0;
		while (line_start < content.size())
		{
			if (m_stop_requested)
			{
				m_is_loading = false;
				return;
			}

			size_t line_end = content.find('\n', line_start);
			if (line_end == std::string::npos)
				line_end = content.size();

			std::string_view line_view(content.data() + line_start, line_end - line_start);
			if (!line_view.empty() && line_view.back() == '\r')
				line_view.remove_suffix(1);

			Entry entry;
			if (Parser::ParseLine(line_view, &entry))
			{
				parsed_entries.push_back(std::move(entry));
			}

			line_start = line_end + 1;
		}

		Console.WriteLnFmt("CatalogManager: Successfully parsed {} valid .chd/.iso entries.", parsed_entries.size());

		ProcessParsedEntries(std::move(parsed_entries));
		RescanLocalRoms();

		m_is_loading = false;
		m_is_loaded = true;

		Console.WriteLnFmt("CatalogManager: Ready. {} entries grouped into {} visual cards ({} downloaded).",
			m_entries.size(), m_groups.size(), GetDownloadedCount());
	}

	void Manager::ProcessParsedEntries(std::vector<Entry> entries)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);

		m_entries = std::move(entries);
		m_entry_map.clear();
		m_entry_map.reserve(m_entries.size());

		m_groups.clear();
		m_groups.reserve(m_entries.size() / 2);
		m_group_map.clear();
		m_group_map.reserve(m_entries.size() / 2);

		for (size_t i = 0; i < m_entries.size(); ++i)
		{
			const Entry& entry = m_entries[i];
			m_entry_map[entry.fileName] = i;

			auto it = m_group_map.find(entry.groupKey);
			if (it == m_group_map.end())
			{
				Group grp;
				grp.groupKey = entry.groupKey;
				grp.title = Parser::BaseTitle(entry.fileName);
				grp.normalizedSearch = Parser::NormalizeSearchText(grp.title);
				grp.variants.push_back(entry);

				const size_t grp_idx = m_groups.size();
				m_group_map[grp.groupKey] = grp_idx;
				m_groups.push_back(std::move(grp));
			}
			else
			{
				m_groups[it->second].variants.push_back(entry);
			}
		}

		SelectPrimaryVariants();
	}

	void Manager::SelectPrimaryVariants()
	{
		// Priority:
		// 1. Downloaded variant (ready to play)
		// 2. Portuguese / PT-BR (per specification: default USA or PT-BR)
		// 3. USA
		// 4. First variant with available cover URL
		// 5. Index 0
		for (auto& group : m_groups)
		{
			if (group.variants.empty())
				continue;

			size_t best_index = 0;
			int best_score = -1;

			for (size_t i = 0; i < group.variants.size(); ++i)
			{
				const Entry& v = group.variants[i];
				int score = 0;

				if (v.isDownloaded)
				{
					score = 1000;
				}
				else
				{
					std::string fn_lower(v.fileName);
					for (char& c : fn_lower)
					{
						if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
					}

					if (fn_lower.find("pt-br") != std::string::npos ||
						fn_lower.find("dublado") != std::string::npos ||
						fn_lower.find("traduzido") != std::string::npos ||
						fn_lower.find("(brazil)") != std::string::npos ||
						fn_lower.find("(portugal)") != std::string::npos ||
						fn_lower.find("(pt)") != std::string::npos)
					{
						score = 500;
					}
					else if (fn_lower.find("(usa)") != std::string::npos)
					{
						score = 300;
					}
					else if (!v.coverUrl.empty())
					{
						score = 100;
					}
					else
					{
						score = 10;
					}
				}

				if (score > best_score)
				{
					best_score = score;
					best_index = i;
				}
			}

			group.primaryIndex = best_index;

			// Borrow cover from primary, or from first variant in group that has a cover
			if (!group.variants[best_index].coverUrl.empty())
			{
				group.coverUrl = group.variants[best_index].coverUrl;
			}
			else
			{
				group.coverUrl.clear();
				for (const auto& v : group.variants)
				{
					if (!v.coverUrl.empty())
					{
						group.coverUrl = v.coverUrl;
						break;
					}
				}
			}

			bool group_has_dl = false;
			for (const auto& v : group.variants)
			{
				if (v.HasDirectDownload())
				{
					group_has_dl = true;
					break;
				}
			}
			group.hasDirectDownload = group_has_dl;
		}
	}
}
