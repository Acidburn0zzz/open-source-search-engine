// Zak Betz,  Copyright, Jun 2008

// A sorted list of sorted lists for storing rdb keys.  Faster and more
// cache friendly than RdbTree.  Optimized for batched adds, amortized O(1)
// add operation, O(log n) for retrival, ranged getList for k keys is 
// O(log(n) + k) where as RdbTree is O(k * log(n)).
// Memory is allocated and used on a on demand basis rather than all up
// front as with RdbTree, so memory usage is much lower most of the time.

// Collections are handled as a linked list, each RdbBuckets has a nextColl
// pointer.  The front bucket acts as the gatekeeper for all of the
// other buckets, Only it's values for needsSave and isWritable are 
// significant

//when selecting bucketnum and also when deduping, use KEYCMPNEGEQ which
//will mask off the delbit, that way pos and neg keys will be in the same
//bucket and only the newest key will survive.  When getting or deleting
//a list, use KEYCMP within a bucket and use KEYCMPNEGEQ to select the 
//bucket nums.  This is because iterators in rdb dump get a list, then
//add 1 to a key and get the next list and adding 1 to a pos key will get
//the negative one.

#ifndef GB_RDBBUCKETS_H
#define GB_RDBBUCKETS_H

#include <cstdint>
#include <functional>
#include "rdbid_t.h"
#include "types.h"

class BigFile;
class RdbList;
class RdbTree;
class RdbBucket;

class RdbBuckets {
	friend class RdbBucket;

public:
	RdbBuckets( );
	~RdbBuckets( );
	void clear();
	void reset();

	bool set(int32_t fixedDataSize, int32_t maxMem, const char *allocName, rdbid_t rdbId, const char *dbname, char keySize);

	int32_t addNode(collnum_t collnum, const char *key, const char *data, int32_t dataSize);

	bool getList(collnum_t collnum, const char *startKey, const char *endKey, int32_t minRecSizes, RdbList *list,
	             int32_t *numPosRecs, int32_t *numNegRecs, bool useHalfKeys) const;

	bool deleteNode(collnum_t collnum, const char *key);

	bool deleteList(collnum_t collnum, RdbList *list);

	int64_t getListSize(collnum_t collnum, const char *startKey, const char *endKey, char *minKey, char *maxKey) const;

	bool collExists(collnum_t coll) const;

	int32_t getRecSize() const { return m_recSize; }

	bool needsSave() const { return m_needsSave; }
	bool isSaving() const { return m_isSaving; }

	bool isWritable() const { return m_isWritable; }
	void disableWrites() { m_isWritable = false; }
	void enableWrites() { m_isWritable = true; }

	int32_t getMaxMem() const { return m_maxMem; }

	void setNeedsSave(bool s);

	int32_t getMemAllocated() const;
	int32_t getMemAvailable() const;
	bool is90PercentFull() const;
	bool needsDump() const;
	bool hasRoom(int32_t numRecs) const;

	int32_t getNumKeys() const;
	int32_t getMemOccupied() const;

	int32_t getNumNegativeKeys() const;
	int32_t getNumPositiveKeys() const;

	void cleanBuckets();
	bool delColl(collnum_t collnum);

	//just for this collection
	int32_t getNumKeys(collnum_t collnum) const;

	//DEBUG
	void verifyIntegrity();
	int32_t addTree(RdbTree *rt);
	void printBuckets(std::function<void(const char*, int32_t)> print_fn = nullptr);
	bool testAndRepair();

	//Save/Load/Dump
	bool fastSave(const char *dir, bool useThread, void *state, void (*callback)(void *state));
	bool loadBuckets(const char *dbname);

private:
	bool resizeTable(int32_t numNeeded);
	bool selfTest(bool thorough, bool core);
	bool repair();

	bool addBucket (RdbBucket *newBucket, int32_t i);
	int32_t getBucketNum(collnum_t collnum, const char *key) const;
	char bucketCmp(collnum_t acoll, const char *akey, RdbBucket* b) const;

	const char *getDbname() const { return m_dbname; }

	uint8_t getKeySize() const { return m_ks; }
	int32_t getFixedDataSize() const { return m_fixedDataSize; }

	void setSwapBuf(char *s) { m_swapBuf = s; }
	char *getSwapBuf() { return m_swapBuf; }

	char *getSortBuf() { return m_sortBuf; }
	int32_t getSortBufSize() const { return m_sortBufSize; }

	//syntactic sugar
	RdbBucket* bucketFactory();
	void updateNumRecs(int32_t n, int32_t bytes, int32_t numNeg);

	bool fastSave_r();
	int64_t fastSaveColl_r(int fd);
	bool fastLoad(BigFile *f, const char *dbname);
	int64_t fastLoadColl(BigFile *f, const char *dbname);

	RdbBucket **m_buckets;
	RdbBucket *m_bucketsSpace;
	char *m_masterPtr;
	int32_t m_masterSize;
	int32_t m_firstOpenSlot;	//first slot in m_bucketSpace that is available (never-used or empty)
	int32_t m_numBuckets;		//number of used buckets
	int32_t m_maxBuckets;		//current number of (pre-)allocated buckets
	uint8_t m_ks;
	int32_t m_fixedDataSize;
	int32_t m_recSize;
	int32_t m_numKeysApprox;//includes dups
	int32_t m_numNegKeys;
	int32_t m_maxMem;
	int32_t m_maxBucketsCapacity;	//max number of buckets given the memory limit
	int32_t m_dataMemOccupied;

	rdbid_t m_rdbId;
	const char *m_dbname;
	char *m_swapBuf;
	char *m_sortBuf;
	int32_t m_sortBufSize;

	bool m_repairMode;
	bool m_isWritable;
	bool m_isSaving;
	// true if buckets was modified and needs to be saved
	bool m_needsSave;
	const char *m_dir;
	void *m_state;

	void (*m_callback)(void *state);

	int32_t m_saveErrno;
	const char *m_allocName;
};

#endif // GB_RDBBUCKETS_H
