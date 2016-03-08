// Matt Wells, copyright Jul 2001

// . gets the title/summary/docLen/url results from a query

#ifndef GB_MSG40_H
#define GB_MSG40_H

#define SAMPLE_VECTOR_SIZE (32*4)

#include "SearchInput.h"
#include "UdpServer.h"  // UdpSlot type
#include "Multicast.h"  // multicast send
#include "Query.h"      // Query::set()
#include "Msg39.h"      // getTermFreqs()
#include "Msg20.h"      // for getting summary from docId
#include "Msg17.h"      // a distributed cache of serialized/compressed Msg40s
#include "Msg3a.h"
#include "HashTableT.h"

// make it 2B now. no reason not too limit it so low.
#define MAXDOCIDSTOCOMPUTE 2000000000

class Msg40 {
public:

	Msg40();
	~Msg40();
	void resetBuf2 ( ) ;
	static bool registerHandler ();

	// . returns false if blocked, true otherwise
	// . sets errno on error
	// . uses Query class to parse query
	// . uses Msg37 to retrieve term frequencies for each termId in query
	// . uses Indexdb class to intersect the lists to get results
	// . fills local buffer, m_docIds, with resulting docIds
	// . set m_numDocIds to number of docIds in m_docIds
	// . a useCache of -1 means default, 1 means use the cache,0 means dont
	// . "displayMetas" is a space separated list of meta tag names
	//   that you want the content for along with the summary
	bool getResults ( class SearchInput *si ,
			  bool               forward ,
			  void              *state    ,
			  void             (* callback)(void *state));

	// a continuation function of getResults() above
	bool prepareToGetDocIds ( );
	bool getDocIds ( bool recall );

	// keep these public since called by wrapper functions
	bool federatedLoop ( ) ;
	bool gotDocIds        ( ) ;
	bool launchMsg20s     ( bool recalled ) ;
	class Msg20 *getAvailMsg20();
	class Msg20 *getCompletedSummary ( int32_t ix );
	bool gotSummary       ( ) ;
	bool reallocMsg20Buf ( ) ;

	// serialization routines used for caching Msg40s by Msg17
	int32_t  getStoredSize ( ) ;
	int32_t  serialize     ( char *buf , int32_t bufLen ) ;
	int32_t  deserialize   ( char *buf , int32_t bufLen ) ;

	// . estimated # of total hits
	// . this is now an EXACT count... since we read all posdb termlists
	int64_t getNumTotalHits () { return m_msg3a.getNumTotalEstimatedHits(); }

	// . we copy query and coll to our own local buffer
	// . these routines give us back our inputted parameters we saved
	char *getQuery              ( ) { return m_si->m_q.getQuery(); }
	int32_t  getQueryLen           ( ) { return m_si->m_q.getQueryLen(); }

	int32_t  getDocsWanted         ( ) { return m_si->m_docsWanted; }
	int32_t  getFirstResultNum     ( ) { return m_si->m_firstResultNum; }

	int32_t  getNumResults (        ){return m_msg3a.getNumDocIds(); }

	char   getClusterLevel(int32_t i){return m_msg3a.getClusterLevels()[i];}

	int64_t getDocId  ( int32_t i ){return m_msg3a.m_docIds[i]; }
	double  getScore  ( int32_t i ){return m_msg3a.getScores()[i]; }

	class DocIdScore *getScoreInfo(int32_t i){
		if ( ! m_msg3a.m_scoreInfos ) return NULL;
		return m_msg3a.m_scoreInfos[i];
	}

	bool  moreResultsFollow ( )   {return m_moreToCome; }
	time_t getCachedTime ( )      {return m_cachedTime; }

	bool printSearchResult9 ( int32_t ix, int32_t *numPrintedSoFar, class Msg20Reply *mr ) ;

	int32_t m_numMsg20sOut ;
	int32_t m_numMsg20sIn  ;

	int32_t m_omitCount;

	HashTableX m_dedupTable;

	int32_t m_msg3aRecallCnt;

	int32_t       m_docsToGet;
	int32_t       m_docsToGetVisible;

	// incoming parameters 
	void       *m_state;
	void      (* m_callback ) ( void *state );

	// a bunch of msg20's for getting summaries/titles/...
	Msg20    **m_msg20; 
	int32_t       m_numMsg20s;

	char      *m_msg20StartBuf;
	int32_t       m_numToFree;

	int32_t m_numPrinted    ;
	bool m_printedHeader ;
	bool m_printedTail   ;
	int32_t m_sendsOut      ;
	int32_t m_sendsIn       ;
	int32_t m_printi        ;
	int32_t m_numDisplayed  ;
	int32_t m_numPrintedSoFar;
	int32_t m_socketHadError;


	// use msg3a to get docIds
	Msg3a      m_msg3a;

	// count summary replies (msg20 replies) we get
	int32_t       m_numRequests;
	int32_t       m_numReplies;

	// true if more results follow these
	bool       m_moreToCome;

	int32_t m_lastProcessedi;

	bool m_didSummarySkip;

	// a multicast class to send the request
	Multicast  m_mcast;

	// for timing how long to get all summaries
	int64_t  m_startTime;

	// was Msg40 cached? if so, at what time?
	time_t     m_cachedTime;

	int32_t m_tasksRemaining;

	int32_t m_printCount;

	// buffer we deserialize from, allocated by Msg17, but we free it
	char *m_buf;
	int32_t  m_bufMaxSize;

	// for holding the msg20s
	char *m_buf2;
	int32_t  m_bufMaxSize2;

	int32_t  m_errno;

	SearchInput   *m_si;

	bool mergeDocIdsIntoBaseMsg3a();
	int32_t m_numCollsToSearch;
	class Msg3a **m_msg3aPtrs;
	SafeBuf m_msg3aPtrBuf;
	int32_t m_num3aRequests;
	int32_t m_num3aReplies;
	collnum_t m_firstCollnum;

	HashTableT<uint64_t, uint64_t> m_urlTable;
};

#endif // GB_MSG40_H
