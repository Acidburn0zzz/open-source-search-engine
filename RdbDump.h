// Matt Wells, copyright Apr 2001

// . non-blocking dump of an RdbTree to an RdbFile
// . RdbFile can be used with RdbGet even in the middle of dumping
// . uses a little mem for an RdbMap and some for write buffering
// . frees the nodes as it dumps them to disk (flushes cache)
// . can also do a non-key-ordered dump for quick saving of an RdbTree
// . Then you can use RdbDump::load() to load it back to the tree

#ifndef GB_RDBDUMP_H
#define GB_RDBDUMP_H

#include "BigFile.h"
#include "RdbList.h"

class Rdb;
class RdbTree;
class RdbBuckets;
class RdbMap;
class RdbIndex;

class RdbDump {
public:
    RdbDump();

	void reset();

	// . set up for a dump of rdb records to a file
	// . returns false and sets errno on error
	bool set(collnum_t collnum,
	         BigFile *file,
	         RdbBuckets *buckets, // optional buckets to dump
	         RdbTree *tree, // optional tree to dump
	         RdbMap *map,
	         RdbIndex *index,
	         int32_t maxBufSize,
	         int32_t niceness,
	         void *state,
	         void (*callback )(void *state),
	         bool useHalfKeys,
	         int64_t startOffset,
	         const char *prevLastKey,
	         char keySize,
	         rdbid_t rdbId);

	void setSuspended() { m_isSuspended = true; }

	bool dumpList(RdbList *list) {
		return dumpList(list,false);
	}

	static void doneWritingWrapper(void *state);
	static void doneReadingForVerifyWrapper(void *state);
	static void tryAgainWrapper2(int fd, void *state);

private:
	// . dump the tree to the file
	// . returns false if blocked, true otherwise
	bool dumpTree(bool recall);

	bool dumpList(RdbList *list, bool isRecall);
	static void checkList(void *state);
	static void checkedList(void *state, job_exit_t /*exit_type*/);
	bool dumpList2(bool recall);
	
	void doneDumping();
	bool doneReadingForVerify();

	static void addListToRdbMapRdbIndex(void *state);
	static void addedListToRdbMapRdbIndex(void *state, job_exit_t /*exit_type*/);

	// called when we've finished writing an RdbList to the file
	bool doneDumpingList();
	void continueDumping();

	RdbTree *m_tree;
	RdbBuckets *m_buckets;
	RdbMap *m_map;
	RdbIndex *m_index;
	int32_t m_maxBufSize;
	void *m_state;

	void (*m_callback)(void *state);

	int64_t m_offset;

	BigFile *m_file;
	RdbList *m_list; // holds list to dump
	RdbList m_ourList; // we use for dumping a tree, point m_list
	char *m_buf; // points into list
	char *m_verifyBuf;
	int32_t m_verifyBufSize;
	int32_t m_bytesToWrite;
	int32_t m_bytesWritten;

	char m_prevLastKey[MAX_KEY_BYTES];

	int32_t m_nextNode;
	char m_nextKey[MAX_KEY_BYTES];
	bool m_rolledOver; // true if m_nextKey rolls back to 0

	// we pass this to BigFile::write() to do non-blocking writes
	FileState m_fstate;

	// a niceness of 0 means the dump will block, otherwise, will not
	int32_t m_niceness;

	bool m_useHalfKeys;
	bool m_hacked;
	bool m_hacked12;

	int32_t m_totalPosDumped;
	int32_t m_totalNegDumped;

	// recall info
	int64_t m_getListStartTimeMS;
	int32_t m_numPosRecs;
	int32_t m_numNegRecs;

	rdbid_t m_rdbId;

	collnum_t m_collnum;

	bool m_isSuspended;

	char m_ks;
};

#endif // GB_RDBDUMP_H
