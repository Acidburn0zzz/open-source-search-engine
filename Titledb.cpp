#include "gb-include.h"

#include "Titledb.h"
#include "Collectiondb.h"
#include "JobScheduler.h"
#include "Rebalance.h"
#include "Process.h"
#include "Conf.h"
#include "XmlDoc.h"
#include "UrlBlockList.h"

Titledb g_titledb;
Titledb g_titledb2;

// reset rdb
void Titledb::reset() { m_rdb.reset(); }

// init our rdb
bool Titledb::init ( ) {

	// key sanity tests
	int64_t uh48  = 0x1234567887654321LL & 0x0000ffffffffffffLL;
	int64_t docId = 123456789;
	key96_t k = makeKey(docId,uh48,false);
	if ( getDocId(&k) != docId ) { g_process.shutdownAbort(true);}
	if ( getUrlHash48(&k) != uh48 ) { g_process.shutdownAbort(true);}

	const char *url = "http://.ezinemark.com/int32_t-island-child-custody-attorneys-new-york-visitation-lawyers-melville-legal-custody-law-firm-45f00bbed18.html";
	Url uu;
	uu.set(url);
	const char *d1 = uu.getDomain();
	int32_t  dlen1 = uu.getDomainLen();
	int32_t dlen2 = 0;
	const char *d2 = getDomFast ( url , &dlen2 );
	if ( !d1 || !d2 ) { g_process.shutdownAbort(true); }
	if ( dlen1 != dlen2 ) { g_process.shutdownAbort(true); }

	// another one
	url = "http://ok/";
	uu.set(url);
	const char *d1a = uu.getDomain();
	dlen1 = uu.getDomainLen();
	dlen2 = 0;
	const char *d2a = getDomFast ( url , &dlen2 );
	if ( d1a || d2a ) { g_process.shutdownAbort(true); }
	if ( dlen1 != dlen2 ) { g_process.shutdownAbort(true); }

	// . what's max # of tree nodes?
	// . assume avg TitleRec size (compressed html doc) is about 1k we get:
	// . NOTE: overhead is about 32 bytes per node
	int32_t maxTreeNodes  = g_conf.m_titledbMaxTreeMem / (1*1024);

	// initialize our own internal rdb
	return m_rdb.init("titledb",
	                  getFixedDataSize(),
	                  //g_conf.m_titledbMinFilesToMerge ,
	                  // this should not really be changed...
	                  -1,
	                  g_conf.m_titledbMaxTreeMem,
	                  maxTreeNodes,
	                  getUseHalfKeys(),
	                  getKeySize(),
	                  false);         //useIndexFile

	// validate
	//return verify ( );
}

// init the rebuild/secondary rdb, used by PageRepair.cpp
bool Titledb::init2 ( int32_t treeMem ) {
	// . what's max # of tree nodes?
	// . assume avg TitleRec size (compressed html doc) is about 1k we get:
	// . NOTE: overhead is about 32 bytes per node
	int32_t maxTreeNodes  = treeMem / (1*1024);
	// initialize our own internal rdb
	return m_rdb.init("titledbRebuild",
	                  getFixedDataSize(),
	                  240, // MinFilesToMerge
	                  treeMem,
	                  maxTreeNodes,
	                  getUseHalfKeys(),
	                  getKeySize(),
	                  false);         //useIndexFile

	// validate
	//return verify ( );
}
/*
bool Titledb::addColl ( char *coll, bool doVerify ) {
	if ( ! m_rdb.addColl ( coll ) ) return false;
	if ( ! doVerify ) return true;
	// verify
	if ( verify(coll) ) return true;
	// if not allowing scale, return false
	if ( ! g_conf.m_allowScale ) return false;
	// otherwise let it go
	log ( "db: Verify failed, but scaling is allowed, passing." );
	return true;
}
*/
bool Titledb::verify(const char *coll) {
	log ( LOG_DEBUG, "db: Verifying Titledb for coll %s...", coll );

	Msg5 msg5;
	RdbList list;
	key96_t startKey;
	key96_t endKey;
	startKey.setMin();
	endKey.setMax();
	//int32_t minRecSizes = 64000;
	const CollectionRec *cr = g_collectiondb.getRec(coll);

	if ( ! msg5.getList ( RDB_TITLEDB   ,
			      cr->m_collnum       ,
			      &list         ,
			      &startKey     ,
			      &endKey       ,
			      1024*1024     , // minRecSizes   ,
			      true          , // includeTree   ,
			      0             , // max cache age
			      0             , // startFileNum  ,
			      -1            , // numFiles      ,
			      NULL          , // state
			      NULL          , // callback
			      0             , // niceness
			      false         , // err correction?
			      -1            , // maxRetries
			      false         , // isRealMerge
			      true))          // allowPageCache
	{
		log(LOG_DEBUG, "db: HEY! it did not block");
		return false;
	}

	int32_t count = 0;
	int32_t got   = 0;
	for ( list.resetListPtr() ; ! list.isExhausted() ;
	      list.skipCurrentRecord() ) {
		key96_t k = list.getCurrentKey();
		// skip negative keys
		if ( (k.n0 & 0x01) == 0x00 ) continue;
		count++;
		//uint32_t groupId = getGroupId ( RDB_TITLEDB , &k );
		//if ( groupId == g_hostdb.m_groupId ) got++;
		uint32_t shardNum = getShardNum ( RDB_TITLEDB, &k );
		if ( shardNum == getMyShardNum() ) got++;
	}
	if ( got != count ) {
		// tally it up
		g_rebalance.m_numForeignRecs += count - got;
		log ("db: Out of first %" PRId32" records in titledb, "
		     "only %" PRId32" belong to our shard. c=%s",count,got,coll);
		// exit if NONE, we probably got the wrong data
		if ( count > 10 && got == 0 ) 
			log("db: Are you sure you have the right "
				   "data in the right directory? "
			    "coll=%s "
			    "Exiting.",
			    coll);
		// repeat with log
		for ( list.resetListPtr() ; ! list.isExhausted() ;
		      list.skipCurrentRecord() ) {
			key96_t k = list.getCurrentKey();
			//uint32_t groupId = getGroupId ( RDB_TITLEDB,&k);
			//int32_t groupNum = g_hostdb.getGroupNum(groupId);
			int32_t shardNum = getShardNum ( RDB_TITLEDB, &k );
			log("db: docid=%" PRId64" shard=%" PRId32,
			    getDocId(&k),shardNum);
		}
		//if ( g_conf.m_bypassValidation ) return true;
		//if ( g_conf.m_allowScale ) return true;
		// don't exit any more, allow it, but do not delete
		// recs that belong to different shards when we merge now!
		log ( "db: db shards unbalanced. "
		      "Click autoscale in master controls.");
		//return false;
		return true;
	}

	log ( LOG_DEBUG, "db: Titledb passed verification successfully for %" PRId32
			" recs.", count );
	// DONE
	return true;
}

// . get a 38 bit docid
// . lower 38 bits are set
// . we put a domain hash in the docId to ease site clustering
// . returns false and sets errno on error
/*
uint64_t Titledb::getProbableDocId ( char *url ) {
	// just hash the whole collection/url
	int64_t docId ;
	docId = 0; // hash64  ( coll , collLen );
	docId = hash64b ( url , docId );
	// now hash the HOSTNAME of the url as lower middle bits in docId
	//unsigned char h = hash8 ( url->getDomain() , url->getDomainLen() );
	// . now we store h in the middle 8 bits of the docId
	// . if we stored in top 8 bits all titleRecs from the same hostname
	//   would be clumped together
	// . if we stored in lower 8 bits the chaining might change our
	//   lower 8 bits making the docid seems like from a different site
	// . 00000000 00000000 00000000 00dddddd
	// . dddddddd dddddddd hhhhhhhh dddddddd
	//int64_t h2 = (h << 8);
	// . clear all but lower 38 bits, then clear 8 bits for h2
	docId &= DOCID_MASK; 
	//docId |= h2;
	// or in the hostname hash as the low 8 bits
	//	*docId |= h;
	return docId;
}
*/

bool Titledb::isLocal ( int64_t docId ) {
	// shift it up (64 minus 38) bits so we can mask it
	//key96_t key = makeTitleRecKey ( docId , false /*isDelKey?*/ );
	// mask upper bits of the top 4 bytes
	//return ( getGroupIdFromDocId ( docId ) == g_hostdb.m_groupId ) ;
	return ( getShardNumFromDocId(docId) == getMyShardNum() );
}

// . make the key of a TitleRec from a docId
// . remember to set the low bit so it's not a delete
// . hi bits are set in the key
key96_t Titledb::makeKey ( int64_t docId, int64_t uh48, bool isDel ){
	key96_t key ;
	// top bits are the docid so generic getGroupId() works!
	key.n1 = (uint32_t)(docId >> 6); // (NUMDOCIDBITS-32));

	int64_t n0 = (uint64_t)(docId&0x3f);
	// sanity check
	if ( uh48 & 0xffff000000000000LL ) { g_process.shutdownAbort(true); }
	// make room for uh48
	n0 <<= 48;
	n0 |= uh48;
	// 9 bits reserved
	n0 <<= 9;
	// final del bit
	n0 <<= 1;
	if ( ! isDel ) n0 |= 0x01;
	// store it
	key.n0 = n0;
	return key;
};

void Titledb::printKey(const char *k) {
	logf(LOG_TRACE, "k=%s "
		     "docId=%012" PRId64" "
		     "urlHash48=%02" PRId64" "
		     "isDel=%d",
	     KEYSTR(k, sizeof(key96_t)),
	     getDocId((key96_t*)k),
	     getUrlHash48((key96_t*)k),
	     KEYNEG(k));
}

void filterTitledbList(RdbList *list) {
	char *newList = list->getList();
	char *dst = newList;
	char *lastKey = NULL;

	int32_t oldSize = list->getListSize();
	int32_t filteredCount = 0;
	for (list->resetListPtr(); !list->isExhausted();) {
		char *rec = list->getCurrentRec();
		int32_t  recSize = list->getCurrentRecSize();

		// pre skip it (necessary because we manipulate the raw list below)
		list->skipCurrentRecord();

		if (!KEYNEG(rec)) {
			XmlDoc xd;
			if (xd.set2(rec, recSize, "main", NULL, 0)) {
				if (g_urlBlockList.isUrlBlocked(*(xd.getFirstUrl()))) {
					++filteredCount;
					continue;
				}
			}
		}

		lastKey = dst;
		memmove(dst, rec, recSize);
		dst += recSize;
	}

	// sanity check
	if ( dst < list->getList() || dst > list->getListEnd() ) {
		g_process.shutdownAbort(true);
	}

	// and stick our newly filtered list in there
	list->setListSize(dst - newList);
	// set to end i guess
	list->setListEnd(list->getList() + list->getListSize());
	list->setListPtr(dst);
	list->setListPtrHi(NULL);

	log(LOG_DEBUG, "db: filtered %" PRId32" entries of %" PRId32 " bytes out of %" PRId32 " bytes.",
	    filteredCount, oldSize - list->getListSize(), oldSize);

	if( !lastKey ) {
		logError("lastKey is null. Should not happen?");
	} else {
		list->setLastKey(lastKey);
	}
}
