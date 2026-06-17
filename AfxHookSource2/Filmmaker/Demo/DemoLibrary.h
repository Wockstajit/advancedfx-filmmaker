#pragma once

// Owns the discovered demo list and the background scan that builds it.
// Thread-safe: the scan runs on a worker thread; readers take a snapshot.

#include "DemoEntry.h"
#include "../Config/DemoFolderStore.h"

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <unordered_set>

namespace Filmmaker {

class DemoLibrary {
public:
	DemoLibrary() = default;
	~DemoLibrary();

	// Loads saved folders. Does not scan yet.
	void Init();

	// Stops any running scan and joins the worker thread.
	void Shutdown();

	// Kicks off a background (re)scan of the install root + saved folders.
	void StartScan();

	bool IsScanning() const { return m_scanning.load(); }

	// Monotonic counter bumped whenever the entry list changes; lets the UI
	// detect "new results are ready" without polling the whole list.
	uint64_t Version() const { return m_version.load(); }

	// Snapshot of the current entries (thread-safe copy).
	std::vector<DemoEntry> Snapshot() const;

	// Resolves the on-disk path for an entry index in the current list.
	bool PathByIndex(size_t index, std::wstring& outPath) const;

	// Requests the per-round timeline for one demo (a full demofile parse) on a
	// background thread, if not already present/queued. Cheap + idempotent; the
	// list version bumps when the data lands so the UI fills in live.
	void EnsureRounds(size_t index);

	// Folder management (auto-saves). Returns true on change.
	bool AddFolder(const std::wstring& folder);
	bool RemoveFolder(const std::wstring& folder);
	std::vector<std::wstring> Folders() const;

	std::wstring InstallRoot() const;

	// Serializes the current state to a JSON string for the Panorama UI.
	std::string BuildJson() const;

private:
	void ScanWorker();

	// Background per-round enrichment (lazy full parse on selection).
	void EnrichWorker();
	void MergeRoundsForPath(const std::wstring& path);

	mutable std::mutex m_mutex;
	std::vector<DemoEntry> m_entries;
	DemoFolderStore m_folderStore;

	std::thread m_worker;
	std::atomic<bool> m_scanning{ false };
	std::atomic<bool> m_cancel{ false };
	std::atomic<uint64_t> m_version{ 0 };

	std::thread m_enrichWorker;
	std::mutex m_enrichMutex;
	std::condition_variable m_enrichCv;
	std::deque<std::wstring> m_enrichQueue;
	std::unordered_set<std::wstring> m_enrichRequested;
	bool m_enrichStop = false;
};

} // namespace Filmmaker
