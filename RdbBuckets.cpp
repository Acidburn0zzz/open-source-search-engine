#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#include "RdbBuckets.h"
#include "gb-include.h"
#include "sort.h"
#include "SafeBuf.h"
#include <unistd.h>
#include "Loop.h"
#include "Rdb.h"
#include "Process.h"
#include "Sanity.h"


#define BUCKET_SIZE 8192
#define INIT_SIZE 4096
#define SAVE_VERSION 0

inline int KEYCMP12 ( const void *a, const void *b ) {
	char* k1 = (char*)a;
	char* k2 = (char*)b;
	if ( (*(uint64_t *)(k1+4)) <
	     (*(uint64_t *)(k2+4)) ) return -1;
	if ( (*(uint64_t *)(k1+4)) > 
	     (*(uint64_t *)(k2+4)) ) return  1;
	uint32_t k1n0 = ((*(uint32_t*)(k1)) & ~0x01UL);
	uint32_t k2n0 = ((*(uint32_t*)(k2)) & ~0x01UL);
	if ( k1n0 < k2n0 ) return -1;
	if ( k1n0 > k2n0 ) return  1;
	return 0;
}

inline int KEYCMP16 ( const void *a, const void *b ) {
	char* k1 = (char*)a;
	char* k2 = (char*)b;
	if ( (*(uint64_t *)(k1+8)) <
	     (*(uint64_t *)(k2+8)) ) return -1;
	if ( (*(uint64_t *)(k1+8)) >
	     (*(uint64_t *)(k2+8)) ) return  1;
	uint64_t k1n0 = ((*(uint64_t *)(k1)) & ~0x01ULL);
	uint64_t k2n0 = ((*(uint64_t *)(k2)) & ~0x01ULL);
	if ( k1n0 < k2n0 ) return -1;
	if ( k1n0 > k2n0 ) return  1;
	return 0;
}

inline int KEYCMP18 ( const void *a, const void *b ) {
	char* k1 = (char*)a;
	char* k2 = (char*)b;
	if ( (*(uint64_t *)(k1+10)) <
	     (*(uint64_t *)(k2+10)) ) return -1;
	if ( (*(uint64_t *)(k1+10)) >
	     (*(uint64_t *)(k2+10)) ) return  1;
	if ( (*(uint64_t *)(k1+2)) <
	     (*(uint64_t *)(k2+2)) ) return -1;
	if ( (*(uint64_t *)(k1+2)) >
	     (*(uint64_t *)(k2+2)) ) return  1;
	uint16_t k1n0 = ((*(uint16_t *)(k1)) & 0xfffe);
	uint16_t k2n0 = ((*(uint16_t *)(k2)) & 0xfffe);
	if ( k1n0 < k2n0 ) return -1;
	if ( k1n0 > k2n0 ) return  1;
	return 0;
}

inline int KEYCMP24 ( const void *a, const void *b ) {
	char* k1 = (char*)a;
	char* k2 = (char*)b;
	if ( (*(uint64_t *)(k1+16)) <
	     (*(uint64_t *)(k2+16)) ) return -1;
	if ( (*(uint64_t *)(k1+16)) >
	     (*(uint64_t *)(k2+16)) ) return  1;
	if ( (*(uint64_t *)(k1+8)) <
	     (*(uint64_t *)(k2+8)) ) return -1;
	if ( (*(uint64_t *)(k1+8)) >
	     (*(uint64_t *)(k2+8)) ) return  1;
	uint64_t k1n0 = ((*(uint64_t *)(k1)) & ~0x01ULL);
	uint64_t k2n0 = ((*(uint64_t *)(k2)) & ~0x01ULL);
	if ( k1n0 < k2n0 ) return -1;
	if ( k1n0 > k2n0 ) return  1;
	return 0;
}

inline int KEYCMP6 ( const void *a, const void *b ) {
	char* k1 = (char*)a;
	char* k2 = (char*)b;
	if ( (*(uint32_t  *)(k1+2)) <
	     (*(uint32_t  *)(k2+2)) ) return -1;
	if ( (*(uint32_t  *)(k1+2)) >
	     (*(uint32_t  *)(k2+2)) ) return  1;
	if ( (*(uint16_t *)(k1+0)) <
	     (*(uint16_t *)(k2+0)) ) return -1;
	if ( (*(uint16_t *)(k1+0)) >
	     (*(uint16_t *)(k2+0)) ) return  1;
	return 0;
}






bool RdbBucket::set(RdbBuckets* parent, char* newbuf) {
	m_endKey = NULL;
	m_parent = parent;
	m_lastSorted = 0;
	m_numKeys = 0;
	m_keys = newbuf;
	return true;
}


void RdbBucket::reBuf(char* newbuf) {
	if(!m_keys) {
		m_keys = newbuf;
		return;
	}
	gbmemcpy(newbuf, m_keys, m_numKeys * m_parent->getRecSize());
	if(m_endKey) m_endKey = newbuf + (m_endKey - m_keys);
	m_keys = newbuf;
}




RdbBucket::~RdbBucket() {
	reset();
}


void RdbBucket::reset() {
	//m_keys = NULL;
	m_numKeys = 0;
	m_lastSorted = 0;
	m_endKey = NULL;
}

int32_t RdbBuckets::getMemAlloced () const {
	int32_t alloced = sizeof(RdbBuckets) + m_masterSize + m_dataMemOccupied;
	return alloced;
}

//includes data in the data ptrs
int32_t RdbBuckets::getMemOccupied() const {
	return (m_numKeysApprox * m_recSize) + m_dataMemOccupied +
		sizeof(RdbBuckets) + 
		m_sortBufSize + 
		BUCKET_SIZE * m_recSize; //swapbuf
}

int32_t RdbBuckets::getMemAvailable() const {
	return m_maxMem - getMemOccupied();
}

bool RdbBuckets::is90PercentFull() const {
	return getMemOccupied () > m_maxMem * .9;
}

bool RdbBuckets::needsDump() const {
	if ( m_numBuckets + 1 < m_maxBuckets ) {
		return false;
	}

	return ( m_maxBuckets == m_maxBucketsCapacity );
}

//be very conservative with this because if we say we can fit it
//and we can't then we'll get a partial list added and we will
//add the whole list again.
bool RdbBuckets::hasRoom ( int32_t numRecs ) const {
	int32_t numBucketsRequired = (((numRecs / BUCKET_SIZE)+1) * 2);
	return ( m_maxBucketsCapacity - m_numBuckets >= numBucketsRequired );
}

bool RdbBucket::sort() {
	//m_lastSorted = 0;//for debugging
	if(m_lastSorted == m_numKeys) return true;

	if(m_numKeys < 2) {
		m_lastSorted = m_numKeys;
		return true;		
	}

	uint8_t ks = m_parent->getKeySize();
	int32_t recSize = m_parent->getRecSize();
	int32_t fixedDataSize = m_parent->getFixedDataSize();
	int (*cmpfn) (const void*, const void *) = NULL;
	if ( ks == 18 ) cmpfn = KEYCMP18;
	else if ( ks == 24 ) cmpfn = KEYCMP24;
	else if ( ks == 12 ) cmpfn = KEYCMP12;
	else if ( ks == 16 ) cmpfn = KEYCMP16;
	else if ( ks ==  6 ) cmpfn = KEYCMP6;
	else { g_process.shutdownAbort(true); }

	char* mergeBuf  = m_parent->getSwapBuf();
	if(!mergeBuf) {	g_process.shutdownAbort(true); }

	int32_t numUnsorted = m_numKeys - m_lastSorted;
	char *list1  = m_keys;
	char *list2  = m_keys + (recSize*m_lastSorted);
	char *list1end  = list2;
	char *list2end  = list2 + (recSize * numUnsorted);
	//turn quickpoll off while we sort,
	//because we do not know what sort of indeterminate state
	//we will be in while sorting
	// MDW: this no longer disables it since it is based on g_niceness
	// now, but what is the point, does it use static vars or what?

	//sort the unsorted portion
	// turn off this way
	int32_t saved = g_niceness;
	g_niceness = 0;
	// . use merge sort because it is stable, and we need to always keep
	// . the identical keys that were added last
	// . now we pass in a buffer to merge into, otherwise one is malloced,
	// . which can fail.  It falls back on qsort which is not stable.
	if(!m_parent->getSortBuf()) {g_process.shutdownAbort(true);}
	gbmergesort (list2, numUnsorted , recSize , cmpfn, 0, m_parent->getSortBuf(), m_parent->getSortBufSize());

	g_niceness = saved;

	char *p  = mergeBuf;
	char v;
	char *lastKey = NULL;
	int32_t br = 0; //bytesRemoved (abbreviated for column width)
	int32_t dso = ks + sizeof(char*);//datasize offset
	int32_t numNeg = 0;

	for(;;) {
		if(list1 >= list1end) {
			// . just copy into place, deduping as we go
			while(list2 < list2end) {
				if(lastKey && KEYCMPNEGEQ(list2,lastKey,ks) == 0) {
					//this is a dup, we are removing data
					if(fixedDataSize != 0) {
					      if(fixedDataSize == -1) 
					           br += *(int32_t*)(lastKey+dso);
					      else br += fixedDataSize;
					}
					if ( KEYNEG(lastKey) ) numNeg++;
					p = lastKey;
				}
				gbmemcpy(p, list2, recSize);
				lastKey = p;
				p += recSize;
				list2 += recSize;
			}

			break;
		}
		if(list2 >= list2end) {
			// . if all that is left is list 1 just copy it into 
			// . place, since it is already deduped
			gbmemcpy(p, list1, list1end - list1);
			p += list1end - list1;
			break;
		}
		v = KEYCMPNEGEQ(list1, list2, ks); 
		if(v < 0) {
			//never overwrite the merged list from list1 because
			//it is always older and it is already deduped
			if(lastKey && KEYCMPNEGEQ(list1, lastKey, ks) == 0) {
				if ( KEYNEG(lastKey) ) numNeg++;
				list1 += recSize;
				continue;
			}
			gbmemcpy(p, list1, recSize);
			lastKey = p;
			p += recSize;
			list1 += recSize;
		}
		else if(v > 0) {
			//copy it over the one we just copied in
			if(lastKey && KEYCMPNEGEQ(list2, lastKey, ks) == 0) {
				//this is a dup, we are removing data
				if(fixedDataSize != 0) {
					if(fixedDataSize == -1) 
						br += *(int32_t*)(lastKey+dso);
					else br += fixedDataSize;
				}
				if ( KEYNEG(lastKey) ) numNeg++;
				p = lastKey;
			}

			gbmemcpy(p, list2, recSize);
			lastKey = p;
			p += recSize;
			list2 += recSize;
		}
		else {
			if(lastKey && KEYCMPNEGEQ(list2, lastKey, ks) == 0) {
				if(fixedDataSize != 0) {
					if(fixedDataSize == -1) 
						br += *(int32_t*)(lastKey+dso);
					else br += fixedDataSize;
				}
				if ( KEYNEG(lastKey) ) numNeg++;
				p = lastKey;
			}

			//found dup, take list2's
 			gbmemcpy(p, list2, recSize);
			lastKey = p;
 			p += recSize;
 			list2 += recSize;
 			list1 += recSize; //fuggedaboutit!
		}
	}

	//we compacted out the dups, so reflect that here
	int32_t newNumKeys = (p - mergeBuf) / recSize;
	m_parent->updateNumRecs(newNumKeys - m_numKeys , - br, -numNeg);
	m_numKeys = newNumKeys;

	if(m_keys != mergeBuf) m_parent->setSwapBuf(m_keys);
	m_keys = mergeBuf;
	m_lastSorted = m_numKeys;
	m_endKey = m_keys + ((m_numKeys - 1) * recSize);
	return true;
	

}


//make 2 half full buckets,
//addKey assumes that the *this bucket retains the lower half of the keys
//returns a new bucket with the remaining upper half.
RdbBucket* RdbBucket::split(RdbBucket* newBucket) {
	//	log(LOG_WARN, "splitting bucket");
	int32_t b1NumKeys = m_numKeys >> 1; //m_numkeys / 2
	int32_t b2NumKeys = m_numKeys - b1NumKeys;
	int32_t recSize = m_parent->getRecSize();
	//configure the new bucket
	gbmemcpy(newBucket->m_keys, m_keys + (b1NumKeys*recSize), b2NumKeys * recSize);
	newBucket->m_numKeys = b2NumKeys;
	newBucket->m_lastSorted = b2NumKeys;
	newBucket->m_endKey = newBucket->m_keys + ((b2NumKeys - 1) * recSize);
	
	//reconfigure the old bucket
	m_numKeys = b1NumKeys;
	m_lastSorted = b1NumKeys;
	m_endKey = m_keys + ((b1NumKeys - 1) * recSize);

	//add it to our parent
	return newBucket;
}


bool RdbBucket::addKey(const char *key , char *data , int32_t dataSize) {
	uint8_t ks = m_parent->getKeySize();
	int32_t recSize = m_parent->getRecSize();
	bool isNeg = KEYNEG(key);

	char *newLoc = m_keys + (recSize * m_numKeys);
	gbmemcpy(newLoc, key, ks);
	
	if(data) {
		*(char**)(newLoc + ks) = data;
		if(m_parent->getFixedDataSize() == -1) {
			*(int32_t*)(newLoc + ks + sizeof(char*)) = (int32_t)dataSize;
		}
	}
	
	if(m_endKey == NULL) { //are we the first key?
		if(m_numKeys > 0) {g_process.shutdownAbort(true);}
		m_endKey = newLoc;
		m_lastSorted = 1;
	}
	else {
		// . minor optimization: if we are almost sorted, then
		// . see if we can't maintain that state.
		char v = KEYCMPNEGEQ(key, m_endKey, ks);
		if(v == 0) {
			// . just replace the old key if we were the same,
			// . don't inc num keys
			gbmemcpy(m_endKey, newLoc, recSize);
			if(KEYNEG(m_endKey)) {
				if(isNeg) return true;
				else m_parent->updateNumRecs(0, 0, -1);
			}
			else if(isNeg) m_parent->updateNumRecs(0, 0, 1);;
			return true;
		}
		else if(v > 0) {
			// . if we were greater than the old key, 
			// . we can assume we are still sorted, which
			// . really helps us for adds which are in order
			if(m_lastSorted == m_numKeys) m_lastSorted++;
			m_endKey = newLoc;
		}
	}
	m_numKeys++;
	m_parent->updateNumRecs(1 , dataSize, isNeg?1:0);
	return true;
}

char* RdbBucket::getKeyVal( const char *key , char **data , int32_t* dataSize ) {
	sort();
	int32_t i = getKeyNumExact(key);
	if(i < 0) return NULL;

	int32_t recSize = m_parent->getRecSize();
	uint8_t ks = m_parent->getKeySize();
	char* rec = m_keys + (recSize * i);

	if(data) {
		*data = rec + ks;
		if(dataSize)
			*dataSize = *(int32_t*)*data + sizeof(char*);
	}
	return rec;
}

int32_t RdbBucket::getKeyNumExact(const char* key) const {
	uint8_t ks = m_parent->getKeySize();
	int32_t recSize = m_parent->getRecSize();
	int32_t i = 0;
	char v;
	char* kk;
	int32_t low = 0;
	int32_t high = m_numKeys - 1;
	while(low <= high) {
		int32_t delta = high - low;
		i = low + (delta >> 1);
		kk = m_keys + (recSize * i);
		v = KEYCMP(key,kk,ks);
		if(v < 0) {
			high = i - 1;
			continue;
		} else if(v > 0) {
			low = i + 1;
			continue;
		} else {
			return i;
		}
	}
	return -1;
}

bool RdbBucket::selfTest (char* prevKey) {
	sort();
	char* last = NULL;
	char* kk = m_keys;
	int32_t recSize = m_parent->getRecSize();
	int32_t ks = m_parent->getKeySize();

	//ensure our first key is > the last guy's end key
	if(prevKey != NULL && m_numKeys > 0) {
		if(KEYCMP(prevKey, m_keys,ks) > 0) {
			log(LOG_WARN, "db: bucket's first key: %016" PRIx64"%08" PRIx32" "
			    "is less than last bucket's end key: "
			    "%016" PRIx64"%08" PRIx32"!!!!!",
			    *(int64_t*)(m_keys+(sizeof(int32_t))), 
			    *(int32_t*)m_keys,
			    *(int64_t*)(prevKey+(sizeof(int32_t))), 
			    *(int32_t*)prevKey);
			//printBucket();
			return false;
			//g_process.shutdownAbort(true);
		}
	}

 	for(int32_t i = 0; i < m_numKeys; i++) {
//log(LOG_WARN, "rdbbuckets last key: ""%016" PRIx64"%08" PRIx32" num keys: %" PRId32,
//   *(int64_t*)(kk+(sizeof(int32_t))), *(int32_t*)kk, m_numKeys);
		if(i > 0 && KEYCMP(last, kk, ks) > 0) {
			log(LOG_WARN, "db: bucket's last key was out "
			    "of order!!!!!"
			    "key was: %016" PRIx64"%08" PRIx32" vs prev: %016" PRIx64"%08" PRIx32
			    " num keys: %" PRId32
			    " ks=%" PRId32" bucketNum=%" PRId32,
			    *(int64_t*)(kk+(sizeof(int32_t))), *(int32_t*)kk, 
			    *(int64_t*)(last+(sizeof(int32_t))), *(int32_t*)last, 
			    m_numKeys, ks, i);
			return false;
			//g_process.shutdownAbort(true);
		}
		last = kk;
		kk += recSize;
	}
	return true;
}

void RdbBuckets::printBuckets() {
 	for(int32_t i = 0; i < m_numBuckets; i++) {
		m_buckets[i]->printBucket();
	}
}


void RdbBucket::printBucket() {
	char* kk = m_keys;
	int32_t recSize = m_parent->getRecSize();
 	for(int32_t i = 0; i < m_numKeys;i++) {
		log(LOG_WARN, "rdbbuckets last key: ""%016" PRIx64"%08" PRIx32" num "
		    "keys: %" PRId32,
 		    *(int64_t*)(kk+(sizeof(int32_t))), *(int32_t*)kk, m_numKeys);
		kk += recSize;
	}
}


RdbBuckets::RdbBuckets() {
	m_numBuckets = 0; 
	m_masterPtr = NULL;
	m_buckets = NULL;
	m_swapBuf = NULL;
	m_sortBuf = NULL;
	m_isWritable = true;
	m_isSaving = false;
	m_dataMemOccupied = 0;
	m_needsSave = false;
	m_repairMode = false;
}



bool RdbBuckets::set( int32_t fixedDataSize , int32_t maxMem, bool ownData, const char *allocName, char rdbId,
                      bool dataInPtrs, const char *dbname, char keySize, bool useProtection ) {
	m_numBuckets = 0;
	m_ks = keySize;
	m_rdbId = rdbId;
	m_allocName = allocName;
	m_fixedDataSize = fixedDataSize;
	m_recSize = m_ks;
	if(m_fixedDataSize != 0) {
		m_recSize += sizeof(char*);
		if(m_fixedDataSize == -1) m_recSize += sizeof(int32_t);
	}
	m_numKeysApprox = 0;
	m_numNegKeys = 0;
	m_dbname = dbname;
	m_swapBuf = NULL;
 	m_sortBuf = NULL;
	//taken from sort.cpp, this is to prevent mergesort from mallocing
	m_sortBufSize = BUCKET_SIZE * m_recSize + sizeof(char*);
	if(m_buckets) {g_process.shutdownAbort(true);}
	m_maxBuckets = 0;
	m_masterSize = 0;
	m_masterPtr =  NULL;
	m_maxMem = maxMem;

	int32_t perBucket = sizeof(RdbBucket*) + sizeof(RdbBucket) + BUCKET_SIZE * m_recSize;
	int32_t overhead = m_sortBufSize + BUCKET_SIZE * m_recSize + sizeof(RdbBuckets); //thats us, silly
	int32_t avail = m_maxMem - overhead;

	m_maxBucketsCapacity = avail / perBucket;

	if(m_maxBucketsCapacity <= 0) {
		log( LOG_ERROR, "db: max memory for %s's buckets is way too small to accomodate even 1 bucket, "
		     "reduce bucket size(%" PRId32") or increase max mem(%" PRId32")", m_dbname, (int32_t)BUCKET_SIZE, m_maxMem);
		g_process.shutdownAbort(true);
	}

	if( !resizeTable( INIT_SIZE ) ) {
		g_errno = ENOMEM;
		return false;
	}
	
	// log("init: Successfully initialized buckets for %s, "
	//     "keysize is %" PRId32", max mem is %" PRId32", datasize is %" PRId32,
	//     m_dbname, (int32_t)m_ks, m_maxMem, m_fixedDataSize);


	/*
	RdbBuckets b;
	b.set ( 0, // fixedDataSize, 
		50000000 , // maxTreeMem,
		false, //own data
		"tbuck", // m_treeName, // allocName
		false, //data in ptrs
		"tbuck",//m_dbname,
		16, // m_ks,
		false);
	collnum_t cn = 1;
	key128_t k;
	k.n1 = 12;
	k.n0 = 11;
	b.addNode ( cn , (char *)&k, NULL, 0 );
	// negate it
	k.n0 = 10;
	b.addNode ( cn , (char *)&k, NULL, 0 );

	// try that
	key128_t k1;
	key128_t k2;
	k1.setMin();
	k2.setMax();
	RdbList list;
	int32_t np,nn;
	b.getList ( cn,(char *)&k1,(char *)&k2,1000,&list,&np,&nn,false);
	if ( np != 0 || nn != 1 ) { g_process.shutdownAbort(true); }
	// must be empty
	if ( b.getNumKeys() != 0 ) { g_process.shutdownAbort(true); }
	*/

	return true;
}


RdbBuckets::~RdbBuckets( ) {
	reset();
}


void RdbBuckets::setNeedsSave(bool s) {
	m_needsSave = s;
}


void RdbBuckets::reset() {
	for(int32_t j = 0; j < m_numBuckets; j++) {
		m_buckets[j]->reset();
	}
	if(m_masterPtr) mfree(m_masterPtr, m_masterSize, m_allocName );
	m_masterPtr = NULL;
	m_buckets = NULL;
	m_bucketsSpace = NULL;
	m_numBuckets = 0;
	m_maxBuckets = 0;
	m_dataMemOccupied = 0;
	m_firstOpenSlot = 0;
	m_numKeysApprox = 0;
	m_numNegKeys = 0;
	m_sortBuf = NULL;
	m_swapBuf = NULL;
}


void RdbBuckets::clear() {
	for(int32_t j = 0; j < m_numBuckets; j++) {
		m_buckets[j]->reset();
	}
	m_numBuckets = 0;
	m_firstOpenSlot = 0;
	m_dataMemOccupied = 0;
	m_numKeysApprox = 0;
	m_numNegKeys = 0;
	m_needsSave = true;
}




RdbBucket* RdbBuckets::bucketFactory() {
	if( m_numBuckets == m_maxBuckets - 1 ) {
		if( !resizeTable( m_maxBuckets * 2 ) ) {
			return NULL;
		}
	}

	RdbBucket *b;
	if( m_firstOpenSlot > m_numBuckets ) {
		int32_t i = 0;
		for( ; i < m_numBuckets; i++ ) {
			if( m_bucketsSpace[i].getNumKeys() == 0 ) {
				break;
			}
		}
		b = &m_bucketsSpace[i];
	} else {
		b = &m_bucketsSpace[m_firstOpenSlot];
		m_firstOpenSlot++;
	}
	return b;
}

bool RdbBuckets::resizeTable( int32_t numNeeded ) {
	logTrace( g_conf.m_logTraceRdbBuckets, "BEGIN. numNeeded=%" PRId32, numNeeded );

	if ( numNeeded == m_maxBuckets ) {
		return true;
	}

	if ( numNeeded < INIT_SIZE) {
		numNeeded = INIT_SIZE;
	}

	if( numNeeded > m_maxBucketsCapacity ) {
		if( m_maxBucketsCapacity <= m_maxBuckets ) {
			log(LOG_INFO, "db: could not resize buckets currently have %" PRId32" "
			    "buckets, asked for %" PRId32", max number of buckets"
			    " for %" PRId32" bytes with keysize %" PRId32" is %" PRId32,
			    m_maxBuckets, numNeeded, m_maxMem, (int32_t)m_ks,
			    m_maxBucketsCapacity);
			g_errno = ENOMEM;
			return false;
		}

		// log(LOG_INFO,
		//     "db: scaling down request for buckets.  "
		//     "Currently have %" PRId32" "
		//     "buckets, asked for %" PRId32", max number of buckets"
		//     " for %" PRId32" bytes is %" PRId32".",
		//     m_maxBuckets, numNeeded, m_maxMem, m_maxBucketsCapacity);

		numNeeded = m_maxBucketsCapacity;
	}

	int32_t perBucket = sizeof(RdbBucket*) + 
		sizeof(RdbBucket)
		+ BUCKET_SIZE * m_recSize;

	int32_t tmpMaxBuckets = numNeeded;
	int32_t newMasterSize = tmpMaxBuckets * perBucket +
		(BUCKET_SIZE * m_recSize) + /*swap buf*/
		m_sortBufSize;         /*sort buf*/

	if(newMasterSize > m_maxMem) {
		log(LOG_WARN,
		    "db: Buckets oops, trying to malloc more(%" PRId32") that max "
		    "mem(%" PRId32"), should've caught this earlier.",
		    newMasterSize, m_maxMem);
		g_process.shutdownAbort(true);
	}

	char *tmpMasterPtr = (char*)mmalloc(newMasterSize, m_allocName);
	if(!tmpMasterPtr) {
		g_errno = ENOMEM;
		return false;
	}
	char* p = tmpMasterPtr;
	char* bucketMemPtr = p;
	p += (BUCKET_SIZE * m_recSize) * tmpMaxBuckets;
	m_swapBuf = p;
	p += (BUCKET_SIZE * m_recSize);
	m_sortBuf = p;
	p += m_sortBufSize;

	RdbBucket** tmpBucketPtrs = (RdbBucket**)p;
	p += tmpMaxBuckets * sizeof(RdbBucket*);
	RdbBucket* tmpBucketSpace = (RdbBucket*)p;
	p += tmpMaxBuckets * sizeof(RdbBucket);
	if(p - tmpMasterPtr != newMasterSize) {g_process.shutdownAbort(true);}

	for(int32_t i = 0; i < m_numBuckets; i++) {
		//copy them over one at a time so they
		//will now be contiguous and consistent
		//with the ptrs array.
		tmpBucketPtrs[i] = &tmpBucketSpace[i];
		gbmemcpy(&tmpBucketSpace[i],
		       m_buckets[i],
		       sizeof(RdbBucket));
		tmpBucketSpace[i].reBuf(bucketMemPtr);
		bucketMemPtr += (BUCKET_SIZE * m_recSize);
	}
	//now do the rest
	for(int32_t i = m_numBuckets; i < tmpMaxBuckets; i++) {
		tmpBucketSpace[i].set(this, bucketMemPtr);
		bucketMemPtr += (BUCKET_SIZE * m_recSize);
	}
	if(bucketMemPtr != m_swapBuf) {g_process.shutdownAbort(true);}

// 	log(LOG_WARN, "new size = %" PRId32", old size = %" PRId32", newMemUsed = %" PRId32" "
// 	    "oldMemUsed = %" PRId32,
// 	    numNeeded, m_maxBuckets, newMasterSize, m_masterSize);

	if ( m_masterPtr ) {
		mfree( m_masterPtr, m_masterSize, m_allocName );
	}

	m_masterPtr = tmpMasterPtr;
	m_masterSize = newMasterSize;
	m_buckets = tmpBucketPtrs;
	m_bucketsSpace = tmpBucketSpace;
	m_maxBuckets = tmpMaxBuckets;
	m_firstOpenSlot = m_numBuckets;
	return true;
}




int32_t RdbBuckets::addNode (collnum_t collnum,
			  char *key,
			  char *data , int32_t dataSize ) {

	if(!m_isWritable || m_isSaving ) {
		g_errno = EAGAIN;
		return false;
	}

	m_needsSave = true;

	int32_t i;

	i = getBucketNum(key, collnum);
	if(i == m_numBuckets  ||
	   m_buckets[i]->getCollnum() != collnum) {
		int32_t bucketsCutoff = (BUCKET_SIZE>>1);
		//when repairing the keys are added in order,
		//so fill them up all of the way before moving
		//on to the next one.
		if(m_repairMode) bucketsCutoff = BUCKET_SIZE;

  		if(i != 0 && 
		   m_buckets[i-1]->getCollnum() == collnum &&
		   m_buckets[i-1]->getNumKeys() < bucketsCutoff) {
  			i--;
  		}
		else if(i == m_numBuckets) {
			m_buckets[i] = bucketFactory();
			if(m_buckets[i] == NULL) {
				g_errno = ENOMEM;
				return -1;
			}
			m_buckets[i]->setCollnum(collnum);
			m_numBuckets++;
		}
		else { //m_buckets[i]->getCollnum() != collnum
			RdbBucket* newBucket = bucketFactory();
			if(m_buckets[i] == NULL) {//can't really happen here..
				g_errno = ENOMEM;
				return -1;
			}
			newBucket->setCollnum(collnum);
			addBucket(newBucket, i);
		}

	}
	//check if we are full
	if(m_buckets[i]->getNumKeys() == BUCKET_SIZE) {
		//split the bucket
		int64_t t = gettimeofdayInMilliseconds();
		m_buckets[i]->sort();
		RdbBucket* newBucket = bucketFactory();
		if(newBucket == NULL ) {
			g_errno = ENOMEM;
			return -1;
		}
		newBucket->setCollnum(collnum);
		m_buckets[i]->split(newBucket);
		addBucket(newBucket, i+1);
		if(bucketCmp(key, collnum, m_buckets[i]) > 0) i++;
		
		int64_t took = gettimeofdayInMilliseconds() - t;
		if(took > 10) log(LOG_WARN, 
				  "db: split bucket in %" PRId64" ms for %s",took,
				  m_dbname);
	}

	m_buckets[i]->addKey(key, data, dataSize);
	//if(rand() % 100 == 0) 	selfTest(true, true);
	return 0;
}


bool RdbBuckets::addBucket (RdbBucket* newBucket, int32_t i) {

	//int32_t i = getBucketNum(newBucket->getEndKey(), newBucket->getCollnum());
	m_numBuckets++;
	int32_t moveSize = (m_numBuckets - i)*sizeof(RdbBuckets*);
	if(moveSize > 0)
		memmove(&m_buckets[i+1], &m_buckets[i], moveSize);
	m_buckets[i] = newBucket;
	return true;
}

// void RdbBuckets::deleteBucket ( int32_t i ) {
// 	int32_t moveSize = (m_numBuckets - i)*sizeof(RdbBuckets*);
// 	if(moveSize > 0)
// 		memmove(&m_buckets[i+1], &m_buckets[i], moveSize);
// 	m_numBuckets--;
// }

bool RdbBuckets::getList ( collnum_t collnum ,
			   const char *startKey, const char *endKey, int32_t minRecSizes ,
			   RdbList *list , int32_t *numPosRecs , 
			   int32_t *numNegRecs,
			   bool useHalfKeys ) {

	if ( numNegRecs ) *numNegRecs = 0;
	if ( numPosRecs ) *numPosRecs = 0;
	// set *lastKey in case we have no nodes in the list
	//if ( lastKey ) *lastKey = endKey;
	// . set the start and end keys of this list
	// . set lists's m_ownData member to true
	list->reset();
	// got set m_ks first so the set ( startKey, endKey ) works!
	list->m_ks = m_ks;
	list->set              ( startKey , endKey );
	list->setFixedDataSize ( m_fixedDataSize   );
	list->setUseHalfKeys   ( useHalfKeys       );
	// bitch if list does not own his own data
	if ( ! list->getOwnData() ) {
		g_errno = EBADENGINEER;
		log(LOG_LOGIC,"db: rdbbuckets: getList: List does not own data");
		return false;
	}
	// bail if minRecSizes is 0
	if ( minRecSizes == 0 ) return true;
	if ( minRecSizes < 0 ) minRecSizes = 0x7fffffff;//LONG_MAX;

	int32_t startBucket = getBucketNum(startKey, collnum);
	if(startBucket > 0 && 
	   bucketCmp(startKey, collnum, m_buckets[startBucket-1]) < 0)
		startBucket--;

	// if the startKey is past our last bucket, then nothing
	// to return
	if(startBucket == m_numBuckets ||
	   m_buckets[startBucket]->getCollnum() != collnum) return true;


	int32_t endBucket;
	if(bucketCmp(endKey, collnum, m_buckets[startBucket]) <= 0)
		endBucket = startBucket;
	else endBucket = getBucketNum(endKey, collnum);
	if(endBucket == m_numBuckets ||
	   m_buckets[endBucket]->getCollnum() != collnum) endBucket--;
	//log(LOG_WARN, "db numBuckets %" PRId32" start %" PRId32" end %" PRId32,
	//m_numBuckets, startBucket, endBucket);
	if(m_buckets[endBucket]->getCollnum() != collnum) {
		g_process.shutdownAbort(true);
	}

	int32_t growth = 0;

	if(startBucket == endBucket) {
		growth = m_buckets[startBucket]->getNumKeys() * m_recSize;
		if(growth > minRecSizes) growth = minRecSizes + m_recSize;
		if(!list->growList(growth)) {
			log(LOG_WARN, "db: Failed to grow list to %" PRId32" bytes for storing records from buckets: %s.",
			    growth, mstrerror(g_errno));
			return false;
		}

		if(!m_buckets[startBucket]->getList(list, 
						    startKey, 
						    endKey,
						    minRecSizes,
						    numPosRecs, 
						    numNegRecs, 
						    useHalfKeys))
			return false;
		return true;
	}

	//reserve some space, it is an upper bound
	for(int32_t i = startBucket; i <= endBucket; i++) 
		growth += m_buckets[i]->getNumKeys() * m_recSize;

	if(growth > minRecSizes) growth = minRecSizes + m_recSize;
	if(!list->growList(growth)) {
		log(LOG_WARN, "db: Failed to grow list to %" PRId32" bytes for storing records from buckets: %s.",
		    growth, mstrerror(g_errno));
		return false;
	}

	// separate into 3 different calls so we don't have 
	// to search for the start and end keys within the buckets
	// unnecessarily.
	if(!m_buckets[startBucket]->getList(list, 
					    startKey, 
					    NULL,
					    minRecSizes,
					    numPosRecs, 
					    numNegRecs, 
					    useHalfKeys))
		return false;

	int32_t i = startBucket + 1;
	for(; i < endBucket && list->getListSize() < minRecSizes; i++) {
		if(!m_buckets[i]->getList(list, 
					  NULL, 
					  NULL,
					  minRecSizes,
					  numPosRecs, 
					  numNegRecs, 
					  useHalfKeys))
			return false;
	}
	
	if(list->getListSize() < minRecSizes)
		if(!m_buckets[i]->getList(list, 
					  NULL, 
					  endKey,
					  minRecSizes,
					  numPosRecs, 
					  numNegRecs, 
					  useHalfKeys))
			return false;

	return true;

}


int RdbBuckets::getListSizeExact ( collnum_t collnum ,
				   const char *startKey, 
				   const char *endKey ) {

	int numBytes = 0;

	int32_t startBucket = getBucketNum(startKey, collnum);

	// does this mean empty?
	if(startBucket > 0 && 
	   bucketCmp(startKey, collnum, m_buckets[startBucket-1]) < 0)
		startBucket--;

	if(startBucket == m_numBuckets ||
	   m_buckets[startBucket]->getCollnum() != collnum) 
		return 0;

	int32_t endBucket;
	if(bucketCmp(endKey, collnum, m_buckets[startBucket]) <= 0)
		endBucket = startBucket;
	else 
		endBucket = getBucketNum(endKey, collnum);

	if(endBucket == m_numBuckets ||
	   m_buckets[endBucket]->getCollnum() != collnum) endBucket--;

	//log(LOG_WARN, "db numBuckets %" PRId32" start %" PRId32" end %" PRId32,
	//m_numBuckets, startBucket, endBucket);
	if(m_buckets[endBucket]->getCollnum() != collnum) {
		g_process.shutdownAbort(true); }

	for( int32_t i = startBucket ; i <= endBucket ; i++)
		numBytes += m_buckets[i]->getListSizeExact(startKey,endKey);
	
	return numBytes;
}

bool RdbBuckets::testAndRepair() {
	if(!selfTest(true/*thorough*/, 
		     false/*core on error*/)) {
		if(!repair()) return false;
		m_needsSave = true;
	}
	return true;
}


bool RdbBuckets::repair() {
	if(m_numBuckets == 0 && 
	   (m_numKeysApprox != 0 || m_numNegKeys != 0)) {
		   m_numKeysApprox = 0;
		   m_numNegKeys = 0;
		   log("db: RdbBuckets repaired approx key count to reflect "
		       "true number of keys.");
	}

	//int32_t tmpMaxBuckets = m_maxBuckets;
	int32_t tmpMasterSize = m_masterSize;
	char *tmpMasterPtr = m_masterPtr;
	RdbBucket **tmpBucketPtrs = m_buckets;
	int32_t tmpNumBuckets = m_numBuckets;
	m_masterPtr = NULL;
	m_masterSize = 0;
	m_numBuckets = 0;
	reset();
	if(!resizeTable(INIT_SIZE)) {
		log("db: RdbBuckets could not alloc enough memory to repair "
		    "corruption.");
		g_errno = ENOMEM;
		return false;
	}

	m_repairMode = true;

 	for(int32_t j = 0; j < tmpNumBuckets; j++) {
		collnum_t collnum = tmpBucketPtrs[j]->getCollnum();
 		for(int32_t i = 0; i < tmpBucketPtrs[j]->getNumKeys(); i++) {
 			char* currRec = tmpBucketPtrs[j]->getKeys() + 
				m_recSize * i;
 			char* data = NULL;
 			int32_t dataSize = m_fixedDataSize;
			
 			if(m_fixedDataSize != 0) {
 				data = currRec + m_ks;
 				if(m_fixedDataSize == -1)
 					dataSize = *(int32_t*)(data + sizeof(char*));
 			}
 			if(addNode(collnum, currRec, data, dataSize) < 0) {
 				log(LOG_WARN, "db: got unrepairable error in "
 				    "RdbBuckets, could not re-add data");
				return false;
			}
 		}
 	}

	m_repairMode = false;

	if(tmpMasterPtr) mfree(tmpMasterPtr, tmpMasterSize, m_allocName);

	log("db: RdbBuckets repair for %" PRId32" keys complete", m_numKeysApprox);
	return true;
}



bool RdbBuckets::selfTest(bool thorough, bool core) {
	if(m_numBuckets == 0 && m_numKeysApprox != 0) return false;
	int32_t totalNumKeys = 0;
	char* last = NULL;
	collnum_t lastcoll = -1;
	int32_t numColls = 0;

	for(int32_t i = 0; i < m_numBuckets; i++) {
		RdbBucket* b = m_buckets[i];
		if(lastcoll != b->getCollnum()) {
			last = NULL;
			numColls++;
		}
		if(thorough) {
			if(!b->selfTest (last)) {
				if(!core) return false;
				g_process.shutdownAbort(true);
			}
		}


		totalNumKeys += b->getNumKeys();
		char* kk = b->getEndKey();
		//log(LOG_WARN, "rdbbuckets last key: ""%016" PRIx64"%08" PRIx32" "
		//"num keys: %" PRId32,
		//*(int64_t*)(kk+(sizeof(int32_t))),*(int32_t*)kk,b->getNumKeys());
		if(i > 0 &&
		   lastcoll == b->getCollnum() && 
		   KEYCMPNEGEQ(last, kk,m_ks) >= 0) {
			log(LOG_WARN, "rdbbuckets last key: "
			    "%016" PRIx64"%08" PRIx32" num keys: %" PRId32,
			    *(int64_t*)(kk+(sizeof(int32_t))),
			    *(int32_t*)kk, b->getNumKeys());
			log(LOG_WARN, "rdbbuckets last key was out "
			    "of order!!!!!");
			if(!core) return false;
			g_process.shutdownAbort(true);
		}
		last = kk;
		lastcoll = b->getCollnum();
	}
	if ( totalNumKeys != m_numKeysApprox )
	log(LOG_WARN, "db have %" PRId32" keys,  should have %" PRId32". "
	    "%" PRId32" buckets in %" PRId32" colls for db %s",
	    totalNumKeys, m_numKeysApprox, m_numBuckets, 
	    numColls, m_dbname);

	if(thorough && totalNumKeys != m_numKeysApprox) {
		return false;
	}
	return true;
}


char RdbBuckets::bucketCmp(const char *akey, collnum_t acoll, 
			   const char *bkey, collnum_t bcoll) {
	if (acoll == bcoll) return KEYCMPNEGEQ(akey, bkey, m_ks);
	if (acoll <  bcoll) return -1;
	return 1;
}

char RdbBuckets::bucketCmp(const char *akey, collnum_t acoll, 
			   RdbBucket* b) {
	if (acoll == b->getCollnum())
		return KEYCMPNEGEQ(akey, b->getEndKey(), m_ks);
	if (acoll < b->getCollnum()) return -1;
	return 1;
}



int32_t RdbBuckets::getBucketNum(const char* key, collnum_t collnum) {

	if(m_numBuckets < 10) {
		int32_t i = 0;
		for(; i < m_numBuckets; i++) {
			RdbBucket* b = m_buckets[i];
			char v = bucketCmp(key, collnum, b);
			if(v > 0) continue;
			if(v < 0) {break;}
			else break;
		}
		return i;
	}
	int32_t i = 0;
	char v;
	RdbBucket* b = NULL;
	int32_t low = 0;
	int32_t high = m_numBuckets - 1;
	while(low <= high) {
		int32_t delta = high - low;
		i = low + (delta >> 1);
		b = m_buckets[i];
		char v = bucketCmp(key, collnum, b);
		if(v < 0) {
			high = i - 1;
			continue;
		}
		else if(v > 0) {
			low = i + 1;
			continue;
		}
		else return i;
	}
	
	//now fine tune:
  	v = bucketCmp(key, collnum, b);
	if(v > 0) i++;
	return i;
}


bool RdbBuckets::collExists(collnum_t collnum) {
	for(int32_t i = 0; i < m_numBuckets; i++) {
		if(m_buckets[i]->getCollnum() == collnum)return true;
		if(m_buckets[i]->getCollnum() > collnum)  break;
	}
	return false;
}

int32_t RdbBuckets::getNumKeys(collnum_t collnum) const {
	int32_t numKeys = 0;
	for(int32_t i = 0; i < m_numBuckets; i++) {
		if(m_buckets[i]->getCollnum() == collnum) 
			numKeys += m_buckets[i]->getNumKeys();
		if(m_buckets[i]->getCollnum() > collnum)  break;
	}
	return numKeys;
}

	
int32_t RdbBuckets::getNumKeys() const {
	return m_numKeysApprox;
}


// int32_t RdbBuckets::getNumNegativeKeys ( collnum_t collnum ) {
//  	return m_numNegKeys;
// }


// int32_t RdbBuckets::getNumPositiveKeys ( collnum_t collnum ) {
// 	return getNumKeys(collnum) - getNumNegativeKeys(collnum); 
// }


int32_t RdbBuckets::getNumNegativeKeys ( ) const {
	return m_numNegKeys;
}


int32_t  RdbBuckets::getNumPositiveKeys ( ) const {
	return getNumKeys() - getNumNegativeKeys ( );
}


char* RdbBuckets::getKeyVal ( collnum_t collnum, const char *key,
			      char **data , int32_t* dataSize ) {

	int32_t i = getBucketNum(key, collnum);
	if(i == m_numBuckets ||
	   m_buckets[i]->getCollnum() != collnum ) return NULL;
	return m_buckets[i]->getKeyVal(key, data, dataSize);
}


void RdbBuckets::updateNumRecs(int32_t n, int32_t bytes, int32_t numNeg) {
	m_numKeysApprox += n;
	m_dataMemOccupied += bytes;
	m_numNegKeys += numNeg;
}


char *RdbBucket::getFirstKey() {
	sort();
	return m_keys;
}

int32_t RdbBucket::getNumNegativeKeys ( ) const {
	int32_t numNeg = 0;
	int32_t recSize = m_parent->getRecSize();
	char *currKey = m_keys;

	char *lastKey = m_keys + (m_numKeys * recSize);

	while(currKey < lastKey) {  //&& !list->isExhausted()
		if ( KEYNEG(currKey) ) numNeg++;
		currKey += recSize;
	}
	return numNeg;
}


bool RdbBucket::getList(RdbList* list, 
			const char* startKey, 
			const char* endKey,
			int32_t minRecSizes,
			int32_t *numPosRecs, 
			int32_t *numNegRecs, 
			bool useHalfKeys) {

	sort();
	//get our bounds within the bucket:
	uint8_t ks = m_parent->getKeySize();
	int32_t recSize = m_parent->getRecSize();
	int32_t start = 0;
	int32_t end = m_numKeys - 1;
	char v;
	char* kk = NULL;
	if(startKey) {
		int32_t low = 0;
		int32_t high = m_numKeys - 1;
		while(low <= high) {
			int32_t delta = high - low;
			start = low + (delta >> 1);
			kk = m_keys + (recSize * start);
			v = KEYCMP(startKey,kk,ks);
			if(v < 0) {
				high = start - 1;
				continue;
			}
			else if(v > 0) {
				low = start + 1;
				continue;
			}
			else break;

		}
		//now back up or move forward s.t. startKey
		//is <= start
		while(start < m_numKeys) {
			kk = m_keys + (recSize * start);
			v = KEYCMP(startKey, kk, ks);
			if(v > 0) start++;
			else break;
		}
	}
	else start = 0;


	if(endKey) {
		int32_t low = start;
		int32_t high = m_numKeys - 1;
		while(low <= high) {
			int32_t delta = high - low;
			end = low + (delta >> 1);
			kk = m_keys + (recSize * end);
			v = KEYCMP(endKey,kk,ks);
			if(v < 0) {
				high = end - 1;
				continue;
			}
			else if(v > 0) {
				low = end + 1;
				continue;
			}
			else break;

		}
		while(end > 0) {
			kk = m_keys + (recSize * end);
			v = KEYCMP(endKey, kk, ks);
			if(v < 0) end--;
			else break;
		}

	}
	else end = m_numKeys - 1;

	//log(LOG_WARN, "numKeys %" PRId32" start %" PRId32" end %" PRId32,
	//m_numKeys, start, end);
	
	//keep track of our negative a positive recs
	int32_t numNeg = 0;
	int32_t numPos = 0;

	int32_t fixedDataSize = m_parent->getFixedDataSize();

	char* currKey = m_keys + (start * recSize);

	//bail now if there is only one key and it is out of range.
	if(start == end && 
	   ((startKey && KEYCMP(currKey, startKey, ks) < 0) ||
	    (endKey   && KEYCMP(currKey, endKey, ks) > 0))) {
		return true;
	}
	// 	//set our real start key
	// 	if(startKey != NULL) list->setStartKey(currKey);

	char* lastKey = NULL;
	for(int32_t i = start; 
	    i <= end && list->getListSize() < minRecSizes; 
	    i++, currKey += recSize) {
		if ( fixedDataSize == 0 ) {
			if ( ! list->addRecord(currKey, 0, NULL)) {
				log(LOG_WARN, "db: Failed to add record to list for %s: %s. Fix the growList algo.",
				    m_parent->getDbname(), mstrerror(g_errno));
				return false;
			}
		} 
		else {
			int32_t dataSize = fixedDataSize;
			if ( fixedDataSize == -1 ) 
				dataSize = *(int32_t*)(currKey + 
						    ks + sizeof(char*));
			if ( ! list->addRecord ( currKey , dataSize, currKey + ks) ) {
				log(LOG_WARN, "db: Failed to add record to list for %s: %s. Fix the growList algo.",
				    m_parent->getDbname(), mstrerror(g_errno));
				return false;
			}
		}
		if ( KEYNEG(currKey) ) numNeg++;
		else                   numPos++;
		lastKey = currKey;

#ifdef GBSANITYCHECK
		//sanity, remove for production
		if(startKey && KEYCMP(currKey, startKey, ks) < 0) {
			log("db: key is outside the "
			    "keyrange given for getList."
			    "  it is < startkey."
			    "  %016" PRIx64"%08" PRIx32" %016" PRIx64"%08" PRIx32"."
			    "  getting keys %" PRId32" to %" PRId32" for list"
			    "bounded by %016" PRIx64"%08" PRIx32" %016" PRIx64"%08" PRIx32,
			    *(int64_t*)(startKey+(sizeof(int32_t))),
			    *(int32_t*)startKey,
			    *(int64_t*)(currKey+(sizeof(int32_t))),
			    *(int32_t*)currKey,
			    start, end,
 			    *(int64_t*)(startKey+(sizeof(int32_t))),
			    *(int32_t*)startKey,
 			    *(int64_t*)(endKey+(sizeof(int32_t))),
			    *(int32_t*)endKey);

			printBucket();
			g_process.shutdownAbort(true);
		}
		if(endKey &&   KEYCMP(currKey, endKey, ks) > 0) {
			log("db: key is outside the "
			    "keyrange given for getList."
			    "  it is > endkey"
			    "  %016" PRIx64"%08" PRIx32" %016" PRIx64"%08" PRIx32"."
			    "  getting keys %" PRId32" to %" PRId32" for list"
			    "bounded by %016" PRIx64"%08" PRIx32" %016" PRIx64"%08" PRIx32,
			    *(int64_t*)(currKey+(sizeof(int32_t))),
			    *(int32_t*)currKey,
			    *(int64_t*)(endKey+(sizeof(int32_t))),
			    *(int32_t*)endKey,
			    start, end,
 			    *(int64_t*)(startKey+(sizeof(int32_t))),
			    *(int32_t*)startKey,
 			    *(int64_t*)(endKey+(sizeof(int32_t))),
			    *(int32_t*)endKey);

			printBucket();
			g_process.shutdownAbort(true);
		}
#endif
	}

	// set counts to pass back, we may be accumulating over multiple
	// buckets so add it to the count
	if ( numNegRecs ) *numNegRecs += numNeg;
	if ( numPosRecs ) *numPosRecs += numPos;

	//if we don't have an end key, we were not the last bucket, so don't 
	//finalize the list... yes do, because we might've hit min rec sizes
	if(endKey == NULL && list->getListSize() < minRecSizes) return true;

	if ( lastKey != NULL ) list->setLastKey ( lastKey );
	
	// reset the list's endKey if we hit the minRecSizes barrier cuz
	// there may be more records before endKey than we put in "list"
	if ( list->getListSize() >= minRecSizes && lastKey != NULL ) {
		// use the last key we read as the new endKey
		//key_t newEndKey = m_keys[lastNode];
		char newEndKey[MAX_KEY_BYTES];
		KEYSET(newEndKey, lastKey, ks);
		// . if he's negative, boost new endKey by 1 because endKey's
		//   aren't allowed to be negative
		// . we're assured there's no positive counterpart to him 
		//   since Rdb::addRecord() doesn't allow both to exist in
		//   the tree at the same time
		// . if by some chance his positive counterpart is in the
		//   tree, then it's ok because we'd annihilate him anyway,
		//   so we might as well ignore him
		// we are little endian
		if ( KEYNEG(newEndKey,0,ks) ) KEYINC(newEndKey,ks);
		// if we're using half keys set his half key bit
		if ( useHalfKeys ) KEYOR(newEndKey,0x02);
		if ( m_parent->m_rdbId == RDB_POSDB ||
		     m_parent->m_rdbId == RDB2_POSDB2 ) 
			newEndKey[0] |= 0x04;
		// tell list his new endKey now
		list->setEndKey ( newEndKey );
	}
	// reset list ptr to point to first record
	list->resetListPtr();

	// success
	return true;
}



int RdbBucket::getListSizeExact (const char* startKey, const char* endKey ) {

	int32_t numRecs = 0;

	sort();
	//get our bounds within the bucket:
	uint8_t ks = m_parent->getKeySize();
	int32_t recSize = m_parent->getRecSize();
	int32_t start = 0;
	int32_t end = m_numKeys - 1;
	char v;
	char* kk = NULL;

	int32_t low = 0;
	int32_t high = m_numKeys - 1;
	while(low <= high) {
		int32_t delta = high - low;
		start = low + (delta >> 1);
		kk = m_keys + (recSize * start);
		v = KEYCMP(startKey,kk,ks);
		if(v < 0) {
			high = start - 1;
			continue;
		}
		else if(v > 0) {
			low = start + 1;
			continue;
		}
		else break;

	}
	//now back up or move forward s.t. startKey
	//is <= start
	while(start < m_numKeys) {
		kk = m_keys + (recSize * start);
		v = KEYCMP(startKey, kk, ks);
		if(v > 0) start++;
		else break;
	}




	low = start;
	high = m_numKeys - 1;
	while(low <= high) {
		int32_t delta = high - low;
		end = low + (delta >> 1);
		kk = m_keys + (recSize * end);
		v = KEYCMP(endKey,kk,ks);
		if(v < 0) {
			high = end - 1;
			continue;
		}
		else if(v > 0) {
			low = end + 1;
			continue;
		}
		else break;

	}
	while(end > 0) {
		kk = m_keys + (recSize * end);
		v = KEYCMP(endKey, kk, ks);
		if(v < 0) end--;
		else break;
	}

	//keep track of our negative a positive recs
	//int32_t numNeg = 0;
	//int32_t numPos = 0;

	int32_t fixedDataSize = m_parent->getFixedDataSize();

	char* currKey = m_keys + (start * recSize);

	//bail now if there is only one key and it is out of range.
	if(start == end && 
	   ((startKey && KEYCMP(currKey, startKey, ks) < 0) ||
	    (endKey   && KEYCMP(currKey, endKey, ks) > 0))) {
		return 0;
	}

	// MDW: are none negatives?
	if ( fixedDataSize == 0 ) {
		numRecs = (end - start) * ks;
		return numRecs;
	}

	char* lastKey = NULL;
	for(int32_t i = start;
	    i <= end ; //&& list->getListSize() < minRecSizes;
	    i++, currKey += recSize) {
		// if ( fixedDataSize == 0 ) {
		// 	numRecs++;
		// }
		// else {
		int32_t dataSize = fixedDataSize;
		if ( fixedDataSize == -1 ) 
			dataSize = *(int32_t*)(currKey+ks+sizeof(char*));
		numRecs++;
		//}
		lastKey = currKey;
	}

	// success
	return numRecs * ks ;
}


bool RdbBuckets::deleteList(collnum_t collnum, RdbList *list) {
	if(list->getListSize() == 0) return true;

	if(!m_isWritable || m_isSaving ) {
		g_errno = EAGAIN;
		return false;
	}

	// . set this right away because the head bucket needs to know if we
	// . need to save
	m_needsSave = true;

	char startKey [ MAX_KEY_BYTES ];
	char endKey   [ MAX_KEY_BYTES ];
	list->getStartKey ( startKey );
	list->getEndKey   ( endKey   );

	int32_t startBucket = getBucketNum(startKey, collnum);
	if(startBucket > 0 && 
	   bucketCmp(startKey, collnum, m_buckets[startBucket-1]) < 0)
		startBucket--;

	// if the startKey is past our last bucket, then nothing
	// to delete
	if(startBucket == m_numBuckets ||
	   m_buckets[startBucket]->getCollnum() != collnum) return true;


	int32_t endBucket = getBucketNum(endKey, collnum);
	if(endBucket == m_numBuckets ||
	   m_buckets[endBucket]->getCollnum() != collnum) endBucket--;
	//log(LOG_WARN, "db numBuckets %" PRId32" start %" PRId32" end %" PRId32,
	//  m_numBuckets, startBucket, endBucket);

	list->resetListPtr();
	for(int32_t i= startBucket; i <= endBucket && !list->isExhausted(); i++) {
		if(!m_buckets[i]->deleteList(list)) {
			m_buckets[i]->reset();
			m_buckets[i] = NULL;
		}
	}
	int32_t j = 0;
	for(int32_t i = 0; i < m_numBuckets; i++) 
		if(m_buckets[i]) m_buckets[j++] = m_buckets[i];
	m_numBuckets = j;

	//did we delete the whole darn thing?  
	if(m_numBuckets == 0) {
		if(m_numKeysApprox != 0) {
			log( LOG_ERROR, "db: bucket's number of keys is getting off by %" PRId32" after deleting a list",
			     m_numKeysApprox );
			g_process.shutdownAbort(true);
		}
		m_firstOpenSlot = 0;
	}
	return true;
}


bool RdbBucket::deleteList(RdbList *list) {

	sort();
	uint8_t ks = m_parent->getKeySize();
	int32_t recSize = m_parent->getRecSize();
	int32_t fixedDataSize = m_parent->getFixedDataSize();
	char v;

	char *currKey = m_keys;
	char *p = currKey;
	char listkey[MAX_KEY_BYTES]; 

	char *lastKey = m_keys + (m_numKeys * recSize);
	int32_t br = 0; //bytes removed
	int32_t dso = ks+sizeof(char*);//datasize offset
	int32_t numNeg = 0;

	list->getCurrentKey(listkey);
	while(currKey < lastKey) {  //&& !list->isExhausted()

		v = KEYCMP(currKey, listkey, ks);
		if(v == 0) {
			if(fixedDataSize != 0) {
				if(fixedDataSize == -1) 
					br += *(int32_t*)(currKey+dso);
				else br += fixedDataSize;
			}
			if(KEYNEG(currKey)) numNeg++;

			// . forget it exists by advancing read ptr without 
			// . advancing the write ptr
			currKey += recSize;
			if(!list->skipCurrentRecord()) break;
			list->getCurrentKey(listkey);
			continue;
		} 
		else if (v < 0) {
			// . copy this key into place, it was not in the 
			// . delete list
			if(p != currKey) gbmemcpy(p, currKey, recSize);
			p += recSize;
			currKey += recSize;
		} 
		else { //list key > current key
			// . otherwise advance the delete list until 
			//listKey is <= currKey
			if(!list->skipCurrentRecord()) break;
			list->getCurrentKey(listkey);
		}
	}

	// . do we need to finish copying our list down to the 
	// . vacated mem?
	if(currKey < lastKey) {
		int32_t tmpSize = lastKey - currKey;
		gbmemcpy(p, currKey, tmpSize);
		p += tmpSize;
	}

	if(p > m_keys) {	//do we have anything left?
		int32_t newNumKeys = (p - m_keys) / recSize;
		m_parent->updateNumRecs(newNumKeys - m_numKeys, - br, -numNeg);
		m_numKeys = newNumKeys;
		m_lastSorted = m_numKeys;
		m_endKey = m_keys + ((m_numKeys - 1) * recSize);
		return true;

	}
	else {
		//we deleted the entire bucket, let our parent know to free us
		m_parent->updateNumRecs( - m_numKeys, - br, -numNeg);
		return false;
	}
}

// remove keys from any non-existent collection
void RdbBuckets::cleanBuckets ( ) {

	// what buckets have -1 rdbid???
	if ( m_rdbId < 0 ) return;

	// the liberation count
	int32_t count = 0;

	/*
	char buf[50000];
	RdbList list;
	list.set ( NULL,
		   0,
		   buf,
		   50000,
		   0, // fixeddatasize
		   false, // own data? should rdblist free it
		   false, // usehalfkeys
		   m_ks);
	*/

 top:

	for ( int32_t i = 0; i < m_numBuckets; i++ ) {
		RdbBucket *b = m_buckets[i];
		collnum_t collnum = b->getCollnum();
		CollectionRec *cr = NULL;
		if ( collnum < g_collectiondb.m_numRecs ) {
			cr = g_collectiondb.m_recs[ collnum ];
		}
		if ( cr ) {
			continue;
		}

		// count # deleted
		count += b->getNumKeys();

		// delete that coll
		delColl ( collnum );

		// restart
		goto top;
		/*
		int32_t nk = b->getNumKeys();
		for (int32_t j = 0 ; j < nk ; j++ ) {
			char *kp = b->m_keys + j*m_ks;
			// add into list. should just be a gbmemcpy()
			list.addKey ( kp , 0 , NULL );
		*/
		//deleteBucket ( i );
	}

	// print it
	if ( count == 0 ) {
		return;
	}

	log( LOG_LOGIC, "db: Removed %" PRId32" records from %s buckets for invalid collection numbers.", count, m_dbname );
}


bool RdbBuckets::delColl(collnum_t collnum) {

	m_needsSave = true;
	RdbList list;
	int32_t minRecSizes = 1024*1024;
	int32_t numPosRecs  = 0;
	int32_t numNegRecs = 0;
	for(;;) {
		if(!getList(collnum, KEYMIN(), KEYMAX(), minRecSizes ,
			    &list , &numPosRecs , &numNegRecs, false )) {
			if(g_errno == ENOMEM && minRecSizes > 1024) {
				minRecSizes /= 2;
				continue;
			} else {
				log("db: buckets could not delete "
				    "collection: %s.",
				    mstrerror(errno));
				return false;
			}
		} 
		if(list.isEmpty()) break;
		deleteList(collnum, &list);
	}

	log("buckets: deleted all keys for collnum %" PRId32,(int32_t)collnum);
	return true;
}

int32_t RdbBuckets::addTree(RdbTree* rt) {

	int32_t n = rt->getFirstNode();
	int32_t count = 0;
	char* data = NULL;

	int32_t dataSize = m_fixedDataSize;

	while ( n >= 0 ) {
		if(m_fixedDataSize != 0) {
			data = rt->getData ( n );
			if(m_fixedDataSize == -1) 
				dataSize = rt->getDataSize(n);
		}

		if(addNode ( rt->getCollnum (n), 
			     rt->getKey ( n ) , 
			     data , dataSize) < 0) 
			break;
		n = rt->getNextNode ( n );
		count++;
	}
	log("db: added %" PRId32" keys from tree to buckets for %s.",count, m_dbname);
	return count;
}


//this could be sped up a lot, but it is only called from repair at
//the moment.
bool RdbBuckets::addList(RdbList* list, collnum_t collnum) {
	char listKey[MAX_KEY_BYTES]; 

	for( list->resetListPtr(); 
	     !list->isExhausted(); 
	     list->skipCurrentRecord()) {

		list->getCurrentKey(listKey);

		if(addNode(collnum,  
			   listKey , 
			   list->getCurrentData() , 
			   list->getCurrentDataSize()) < 0)
			return false;
	}

	return true;
}




//return the total bytes of the list bookended by startKey and endKey
int64_t RdbBuckets::getListSize ( collnum_t collnum,
				  const char *startKey, const char *endKey,
				    char *minKey , char *maxKey ) {

	if ( minKey ) KEYSET ( minKey , endKey   , m_ks );
	if ( maxKey ) KEYSET ( maxKey , startKey , m_ks );

	int32_t startBucket = getBucketNum(startKey, collnum);
	if(startBucket > 0 && 
	   bucketCmp(startKey, collnum, m_buckets[startBucket-1]) < 0)
		startBucket--;

	if(startBucket == m_numBuckets ||
	   m_buckets[startBucket]->getCollnum() != collnum) return 0;


	int32_t endBucket = getBucketNum(endKey, collnum);

	// not sure if i should have added this: MDW
	//if(bucketCmp(endKey, collnum, m_buckets[startBucket]) <= 0)
	//	endBucket = startBucket;

	if(endBucket == m_numBuckets ||
	   m_buckets[endBucket]->getCollnum() != collnum) endBucket--;
	//log(LOG_WARN, "db numBuckets %" PRId32" start %" PRId32" end %" PRId32,
	//m_numBuckets, startBucket, endBucket);

	int64_t retval = 0;
	for(int32_t i = startBucket; i <= endBucket; i++) {
		retval += m_buckets[i]->getNumKeys();
	}
	return retval * m_recSize;
}


// . caller should call f->set() himself
// . we'll open it here
// . returns false if blocked, true otherwise
// . sets g_errno on error
bool RdbBuckets::fastSave ( char *dir, bool useThread, void *state, void (* callback) (void *state) ) {
	if ( g_conf.m_readOnlyMode ) return true;

	// we do not need a save
	if ( ! m_needsSave ) return true;

	// return true if already in the middle of saving
	if ( m_isSaving ) return false;

	// note it
	logf( LOG_INFO,"db: Saving %s%s-buckets-saved.dat", dir, m_dbname );

	// do not use thread for now!! test it to make sure that was
	// not the problem
	//useThread = false;

	// save parms
	//m_saveFile = f;
	m_dir      = dir;
	m_state    = state;
	m_callback = callback;
	// assume no error
	m_saveErrno = 0;
	// no adding to the tree now
	m_isSaving = true;

	// . this returns false and sets g_errno on error
	// . must now lock for each bucket when saving that bucket, but
	//   release lock to breathe between buckets
	fastSave_r ();

	// store save error into g_errno
	g_errno = m_saveErrno;
	// resume adding to the tree
	m_isSaving = false;
	// we do not need to be saved now?
	m_needsSave = false;

	// we did not block
	return true;
}


// . returns false and sets g_errno on error
// . NO USING g_errno IN A DAMN THREAD!!!!!!!!!!!!!!!!!!!!!!!!!
bool RdbBuckets::fastSave_r() {
	if ( g_conf.m_readOnlyMode ) {
		return true;
	}

	// recover the file
	//BigFile *f = m_saveFile;
	// open it up
	//if ( ! f->open ( O_RDWR | O_CREAT ) ) {
	//	log("RdbTree::fastSave_r: %s",mstrerror(g_errno));
	//  return false;
	//}
	// cannot use the BigFile class, since we may be in a thread and it 
	// messes with g_errno
	//char *s = m_saveFile->getFilename();
	char s[1024];
	sprintf ( s , "%s/%s-buckets-saving.dat", m_dir , m_dbname );
	int fd = ::open ( s , O_RDWR | O_CREAT | O_TRUNC , getFileCreationFlags() );
	if ( fd < 0 ) {
		m_saveErrno = errno;
		log( LOG_ERROR, "db: Could not open %s for writing: %s.", s, mstrerror( errno ) );
		return false;
	}

	// clear our own errno
	errno = 0;

	// . save the header
	// . force file head to the 0 byte in case offset was elsewhere
	int64_t offset = 0;

	offset = fastSaveColl_r(fd, offset);
	
	// close it up
	close ( fd );
	// now fucking rename it
	char s2[1024];
	sprintf ( s2 , "%s/%s-buckets-saved.dat", m_dir , m_dbname );
	::rename ( s , s2 ) ; //fuck yeah!

	log( LOG_INFO, "db: RdbBuckets saved %" PRId32" keys, %" PRId64" bytes for %s", getNumKeys(), offset, m_dbname);

	return offset >= 0;
}

int64_t RdbBuckets::fastSaveColl_r(int fd, int64_t offset) {
	if(m_numKeysApprox == 0) return offset;
	int32_t version = SAVE_VERSION;
	int32_t err = 0;
	if ( pwrite ( fd , &version, sizeof(int32_t) , offset ) != 4 ) err=errno;
	offset += sizeof(int32_t);

	if ( pwrite ( fd , &m_numBuckets, sizeof(int32_t) , offset)!=4)err=errno; 
	offset += sizeof(int32_t);
	if ( pwrite ( fd , &m_maxBuckets, sizeof(int32_t) , offset)!=4)err=errno; 
	offset += sizeof(int32_t);

	if ( pwrite ( fd , &m_ks, sizeof(uint8_t) , offset ) != 1) err=errno; 
	offset += sizeof(uint8_t);
	if ( pwrite ( fd , &m_fixedDataSize,sizeof(int32_t),offset)!=4) err=errno;
	offset += sizeof(int32_t);
	if ( pwrite ( fd , &m_recSize, sizeof(int32_t) , offset ) != 4) err=errno;
	offset += sizeof(int32_t);
	if ( pwrite ( fd , &m_numKeysApprox,sizeof(int32_t),offset) !=4)err=errno;
	offset += sizeof(int32_t);
	if ( pwrite ( fd , &m_numNegKeys,sizeof(int32_t),offset) != 4 ) err=errno;
	offset += sizeof(int32_t);

	if ( pwrite ( fd,&m_dataMemOccupied,sizeof(int32_t),offset)!=4)err=errno;
	offset += sizeof(int32_t);

	int32_t tmp = BUCKET_SIZE;
	if ( pwrite ( fd , &tmp, sizeof(int32_t) , offset ) != 4 ) err=errno;
	offset += sizeof(int32_t);

	// set it
	if ( err ) errno = err;

	// bitch on error
	if ( errno ) {
		m_saveErrno = errno;
		close ( fd );
		log( LOG_ERROR, "db: Failed to save buckets for %s: %s.", m_dbname, mstrerror( errno ) );
		return -1;
	}

	// position to store into m_keys, ...
	for (int32_t i = 0; i < m_numBuckets; i++ ) {
		offset = m_buckets[i]->fastSave_r(fd, offset);
		// returns -1 on error
		if ( offset < 0 ) {
			close ( fd );
			m_saveErrno = errno;
			log( LOG_ERROR, "db: Failed to save buckets for %s: %s.", m_dbname, mstrerror( errno ) );
			return -1;
		}
	}
	return offset;
}

bool RdbBuckets::loadBuckets ( const char* dbname) {
	char filename[256];
	sprintf(filename,"%s-buckets-saved.dat",dbname);

	// set a BigFile to this filename
	BigFile file;//g_hostdb.m_dir
	const char *dir = g_hostdb.m_dir;
	if( *dir == '\0') dir = ".";
	file.set ( dir , filename , NULL ); 
	if ( !file.doesExist() ) return true;
	// load the table with file named "THISDIR/saved"
	bool status = fastLoad ( &file , dbname ) ;
	file.close();
	return status;
}

bool RdbBuckets::fastLoad ( BigFile *f, const char* dbname) {
	log(LOG_INIT,"db: Loading %s.",f->getFilename());

	// open it up
	if ( ! f->open ( O_RDONLY ) ) {
		return false;
	}

	int64_t fsize = f->getFileSize();
	if ( fsize == 0 ) {
		return true;
	}

	// init offset
	int64_t offset = 0;

	offset = fastLoadColl( f, dbname, offset );
	if ( offset < 0 ) {
		log( LOG_ERROR, "db: Failed to load buckets for %s: %s.", m_dbname, mstrerror(g_errno) );
		return false;
	}

	return true;
}


int64_t RdbBuckets::fastLoadColl( BigFile *f, const char *dbname, int64_t offset ) {
	int32_t maxBuckets;
	int32_t numBuckets;
	int32_t version;

	f->read  ( &version,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);
	if( version > SAVE_VERSION ) {
		log( LOG_ERROR, "db: Failed to load buckets for %s: saved version is in the future or is corrupt, "
		     "please restart old executable and do a ddump.", m_dbname);
		return -1;
	}

	f->read  ( &numBuckets,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &maxBuckets,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &m_ks,sizeof(uint8_t), offset ); 
	offset += sizeof(uint8_t);

	f->read  ( &m_fixedDataSize,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &m_recSize,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &m_numKeysApprox,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &m_numNegKeys,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &m_dataMemOccupied, sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	int32_t bucketSize;
	f->read  ( &bucketSize, sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);


	if(bucketSize != BUCKET_SIZE) {
		log( LOG_ERROR, "db: It appears you have changed the bucket size please restart the old executable and dump "
		     "buckets to disk. old=%" PRId32" new=%" PRId32, bucketSize, (int32_t)BUCKET_SIZE);
		g_process.shutdownAbort(true);
	}

	m_dbname = dbname;

	if ( g_errno ) 
		return -1;

	for (int32_t i = 0; i < numBuckets; i++ ) {
		// BUGFIX 20160705: Do NOT assign result of bucketFactory
		// directly to m_buckets[i], as bucketFactory may call 
		// resizeTable that modifies the m_buckets pointer. Cores
		// seen in binary generated with g++ 4.9.3
		RdbBucket *bf = bucketFactory();
		if( !m_buckets ) {
			log(LOG_ERROR,"db: m_buckets is NULL after call to bucketFactory()!");
			gbshutdownLogicError();
		}
		m_buckets[i] = bf;
		
		if(m_buckets[i] == NULL) return -1;
		offset = m_buckets[i]->fastLoad(f, offset);
		// returns -1 on error
		if ( offset < 0 ) 
			return -1;
		m_numBuckets++;
	}
	return offset;
}

// max key size -- posdb, 18 bytes, so use 18 here
#define BTMP_SIZE (BUCKET_SIZE*18+1000)

int64_t RdbBucket::fastSave_r(int fd, int64_t offset) {

	// first copy to a buf before saving so we can unlock!
	char tmp[BTMP_SIZE];
	char *p = tmp;

	gbmemcpy ( p , &m_collnum, sizeof(collnum_t) );
	p += sizeof(collnum_t);
	//pwrite ( fd , &m_collnum, sizeof(collnum_t) , offset ); 
	//offset += sizeof(collnum_t);

	gbmemcpy ( p , &m_numKeys, sizeof(int32_t) );
	p += sizeof(m_numKeys);
	//pwrite ( fd , &m_numKeys, sizeof(int32_t) , offset ); 
	//offset += sizeof(m_numKeys);

	gbmemcpy ( p , &m_lastSorted, sizeof(int32_t) ); 
	p += sizeof(m_lastSorted);
	//pwrite ( fd , &m_lastSorted, sizeof(int32_t) , offset ); 
	//offset += sizeof(m_lastSorted);

	int32_t endKeyOffset = m_endKey - m_keys;
	gbmemcpy ( p , &endKeyOffset, sizeof(int32_t) ); 
	p += sizeof(int32_t);
	//pwrite ( fd , &endKeyOffset, sizeof(int32_t) , offset ); 
	//offset += sizeof(int32_t);
	
	int32_t recSize = m_parent->getRecSize();
	
	gbmemcpy ( p , m_keys, recSize*m_numKeys ); 
	p += recSize*m_numKeys;
	//pwrite ( fd , m_keys, recSize*m_numKeys , offset ); 
	//offset += recSize*m_numKeys;

	int32_t size = p - tmp;
	if ( size > BTMP_SIZE ) { 
		log("buckets: btmp_size too small. keysize>18 bytes?");
		g_process.shutdownAbort(true); 
	}

	// now we can save it without fear of being interrupted and having
	// the bucket altered
	errno = 0;
	if ( pwrite ( fd , tmp , size , offset ) != size ) {
		log("db:fastSave_r: %s.",mstrerror(errno));
		return -1;
	}
	
	return offset + size;
}

int64_t RdbBucket::fastLoad(BigFile *f, int64_t offset) {
	f->read  ( &m_collnum,sizeof(collnum_t), offset ); 
	offset += sizeof(collnum_t);

	f->read  ( &m_numKeys,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	f->read  ( &m_lastSorted,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	int32_t endKeyOffset;
	f->read  ( &endKeyOffset,sizeof(int32_t), offset ); 
	offset += sizeof(int32_t);

	int32_t recSize = m_parent->getRecSize();
	
	f->read  ( m_keys,recSize*m_numKeys, offset ); 
	offset += recSize*m_numKeys;

	m_endKey = m_keys + endKeyOffset;
	if ( g_errno ) {
		log("bucket: fastload %s",mstrerror(g_errno));
		return -1;
	}

	return offset;
}
