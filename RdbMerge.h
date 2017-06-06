// Matt Wells, copyright Sep 2000

// . TODO: fix it, include firstPass stuff

// . the merge file reads slots from 1 or more files and dumps to another
// . the merge file dumps those slots in order of keys to the destination file
// . slots with a zero slotSize will be removed
// . does not use any memory or disk space constraints (TODO)
// . the amount of memory it needs is mostly just from map file (is small)
// . disk space is relatively small to memory
// . TODO: create a static var so only one merge can happen at a time

// . on an index of 40 gigs of just key the map file can take 56 megs
// . as we're merging X files into one file we should free up the maps
//   we're merging so we don't take too much memory
// . RdbMap should have a base page number, the page # of first page in it's
//   m_keys/m_offsets/m_dataSizes array
// . shifting 50 megs down will take probably half a second or so but
//   we do it to save memory and it should only be done every 10 megs, say
// . and we can also start benefiting from the merged files immediately in 
//   seek time

// . RdbScan/RdbGet are different now, we have to figure out a way to
//   read in 1 meg or less (as close as we can get to 1 meg) from each
//   rdb file...  TODO

#ifndef GB_RDBMERGE_H
#define GB_RDBMERGE_H

#include "RdbDump.h"
#include "Msg5.h"

class RdbIndex;
class MergeSpaceCoordinator;
class RdbBase;


class RdbMerge {
public:
	RdbMerge();
	~RdbMerge();

	// . merge to a new file
	// . new file name is stored in m_filename so Rdb can look at it
	// . calls rdb->incorporateMerge() when done with merge or had error
	bool merge(rdbid_t rdbId,
	           collnum_t collnum,
	           BigFile *targetFile,
	           RdbMap *targetMap,
	           RdbIndex *targetIndex,
	           int32_t startFileNum,
	           int32_t numFiles,
	           int32_t niceness);

	bool isHalted() const { return m_isHalted; }

	bool isMerging() const { return m_isMerging; }

	// stop further actions
	void haltMerge();

	void mergeIncorporated(const RdbBase *);

private:
	static void acquireLockWrapper(void *state);
	static void acquireLockDoneWrapper(void *state, job_exit_t exit_type);

	static void getLockWrapper(int /*fd*/, void *state);

	static void regenerateFilesWrapper(void *state);
	static void regenerateFilesDoneWrapper(void *state, job_exit_t exit_type);

	void getLock();
	static void filterListWrapper(void *state);
	static void filterDoneWrapper(void *state, job_exit_t exit_type);
	static void dumpListWrapper(void *state);
	static void gotListWrapper(void *state, RdbList *list, Msg5 *msg5);
	static void tryAgainWrapper(int fd, void *state);

	bool filterList();
	bool dumpList();
	bool getNextList();
	bool getAnotherList();
	void doneMerging();

	// . return false and sets errno on error merging
	// . returns true if blocked, or completed successfully
	bool resumeMerge();

	// . called to continue merge initialization after lock is secure
	// . lock is g_isMergingLock
	static void gotLockWrapper(int /*fd*/, void *state);
	bool gotLock();

	void doSleep();

	void relinquishMergespaceLock();

	MergeSpaceCoordinator *m_mergeSpaceCoordinator;

	std::atomic<bool> m_isAcquireLockJobSubmited;
	bool m_isLockAquired;

	// set to true when m_startKey wraps back to 0
	bool m_doneMerging;

	bool m_getListOutstanding;

	uint64_t m_spaceNeededForMerge;
	// . we get the units from the master and the mergees from the units
	int32_t m_startFileNum;
	int32_t m_numFiles;
	int32_t m_fixedDataSize;

	BigFile *m_targetFile;
	RdbMap *m_targetMap;
	RdbIndex *m_targetIndex;
	bool m_doneRegenerateFiles;

	char m_startKey[MAX_KEY_BYTES];

	bool m_isMerging;
	bool m_isHalted;

	// for writing to target file
	RdbDump m_dump;

	// a Msg5 for getting RdbLists from disk/cache
	Msg5 m_msg5;

	RdbList m_list;

	int32_t m_niceness;

	// for getting the RdbBase class doing the merge
	rdbid_t m_rdbId;
	collnum_t m_collnum;

	char m_ks;
};

extern RdbMerge g_merge;

#endif // GB_RDBMERGE_H
