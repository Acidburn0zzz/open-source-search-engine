#include "gb-include.h"

#include "Parms.h"
#include "File.h"
#include "Conf.h"
#include "TcpSocket.h"
#include "HttpRequest.h"
#include "Pages.h"         // g_pages
#include "Tagdb.h"        // g_tagdb
#include "Collectiondb.h"
#include "HttpMime.h"      // atotime()
#include "Indexdb.h" // for MIN_TRUNC
#include "SearchInput.h"
#include "Unicode.h"
#include "Threads.h"
#include "Spider.h" // MAX_SPIDER_PRIORITIES
#include "SpiderColl.h"
#include "SpiderLoop.h"
#include "Statsdb.h"
#include "Sections.h"
#include "Msg17.h"
#include "Process.h"
#include "Repair.h"
#include "PingServer.h"
#include "Proxy.h"
#include "hash.h"
#include "Test.h"
#include "Rebalance.h"
#include "SpiderProxy.h" // buildProxyTable()
#include "PageInject.h" // InjectionRequest

// width of input box in characters for url filter expression
#define REGEX_TXT_MAX 80

Parms g_parms;

#include "Spider.h"
#include "Tagdb.h"
#include "Indexdb.h"
#include "Datedb.h"
#include "Clusterdb.h"
#include "Collectiondb.h"

//
// new functions to extricate info from parm recs
//

int32_t getDataSizeFromParmRec ( char *rec ) {
	return *(int32_t *)(rec+sizeof(key96_t));
}

char *getDataFromParmRec ( char *rec ) {
	return rec+sizeof(key96_t)+4;
}

collnum_t getCollnumFromParmRec ( char *rec ) {
	key96_t *k = (key96_t *)rec;
	return (collnum_t)k->n1;
}

// for parms that are arrays...
int16_t getOccNumFromParmRec ( char *rec ) {
	key96_t *k = (key96_t *)rec;
	return (int16_t)((k->n0>>16));
}

Parm *getParmFromParmRec ( char *rec ) {
	key96_t *k = (key96_t *)rec;
	int32_t cgiHash32 = (k->n0 >> 32);
	return g_parms.getParmFast2 ( cgiHash32 );
}

int32_t getHashFromParmRec ( char *rec ) {
	key96_t *k = (key96_t *)rec;
	int32_t cgiHash32 = (k->n0 >> 32);
	return cgiHash32;
}

// . occNum is index # for parms that are arrays. it is -1 if not used.
// . collnum is -1 for g_conf, which is not a collrec
// . occNUm is -1 for a non-array parm
key96_t makeParmKey ( collnum_t collnum , Parm *m , int16_t occNum ) {
	key96_t k;
	k.n1 = collnum;
	k.n0 = (uint32_t)m->m_cgiHash; // 32 bit
	k.n0 <<= 16;
	k.n0 |= (uint16_t)occNum;
	// blanks
	k.n0 <<= 16;
	// delbit. 1 means positive key
	k.n0 |= 0x01;
	// test
	if ( getCollnumFromParmRec ((char *)&k)!=collnum){char*xx=NULL;*xx=0;}
	if ( getOccNumFromParmRec ((char *)&k)!=occNum){char*xx=NULL;*xx=0;}
	return k;
}

bool printUrlExpressionExamples ( SafeBuf *sb ) ;


//////////////////////////////////////////////
//
// Command Functions. All return false if block... yadayada
//
//////////////////////////////////////////////

////////
//
// . do commands this way now
// . when handleRequest4 receives a special "command" parmdb rec
//   it calls executes the cmd, one of the functions listed below
// . all these Command*() functions are called in updateParm() below
// . they return false if they would block and they'll call your callback
//   specified in you "we" the WaitEntry
// . they return true with g_errno set on error, set to 0 on success
//
////////


// from PageBasic.cpp:
bool updateSiteListBuf(collnum_t collnum,bool addSeeds,char *siteListArg);

bool CommandUpdateSiteList ( char *rec ) {
	// caller must specify collnum
	collnum_t collnum = getCollnumFromParmRec ( rec );
	if ( collnum < 0 ) {
		log("parms: bad collnum for update site list");
		g_errno = ENOCOLLREC;
		return true;
	}
	// sanity
	int32_t dataSize = getDataSizeFromParmRec ( rec );
	if ( dataSize < 0 ) {
		log("parms: bad site list size = %"INT32" bad!",dataSize);
		g_errno = EBADENGINEER;
		return true;
	}
	// need this
	CollectionRec *cr = g_collectiondb.getRec ( collnum );
	if ( ! cr ) {
		log("parms: no cr for collnum %"INT32" to update",(int32_t)collnum);
		return true;
	}
	// get the sitelist
	char *data = getDataFromParmRec ( rec );
	// update the table that maps site to whether we should spider it
	// and also add newly introduced sites in "data" into spiderdb.
	updateSiteListBuf ( collnum ,
			    true , // add NEW seeds?
			    data // entire sitelist
			    );
	// now that we deduped the old site list with the new one for
	// purposes of adding NEW seeds, we can do the final copy
	cr->m_siteListBuf.set ( data );
	return true;
}

// . require user manually execute this to prevent us fucking up the data
//   at first initially because of a bad hosts.conf file!!!
// . maybe put a red 'A' in the hosts table on the web page to indicate
//   we detected records that don't belong to our shard so user knows to
//   rebalance?
// . we'll show it in a special msg box on all admin pages if required
bool CommandRebalance ( char *rec ) {
	g_rebalance.m_userApproved = true;
	// force this to on so it goes through
	g_rebalance.m_numForeignRecs = 1;
	g_rebalance.m_needsRebalanceValid = false;
	return true;
}

bool CommandInsertUrlFiltersRow ( char *rec ) {
	// caller must specify collnum
	collnum_t collnum = getCollnumFromParmRec ( rec );
	if ( collnum < 0 ) {
		log("parms: bad collnum for insert row");
		g_errno = ENOCOLLREC;
		return true;
	}
	// sanity
	int32_t dataSize = getDataSizeFromParmRec ( rec );
	if ( dataSize <= 1 ) {
		log("parms: insert row data size = %"INT32" bad!",dataSize);
		g_errno = EBADENGINEER;
		return true;
	}
	// need this
	CollectionRec *cr = g_collectiondb.getRec ( collnum );
	// get the row #
	char *data = getDataFromParmRec ( rec );
	int32_t rowNum = atol(data);//*(int32_t *)data;
	// scan all parms for url filter parms
	for ( int32_t i = 0 ; i < g_parms.m_numParms ; i++ ) {
		Parm *m = &g_parms.m_parms[i];
		// parm must be a url filters parm
		if ( m->m_page != PAGE_FILTERS ) continue;
		// must be an array!
		if ( ! m->isArray() ) continue;
		// sanity check
		if ( m->m_obj != OBJ_COLL ) { char *xx=NULL;*xx=0; }
		// . add that row
		// . returns false and sets g_errno on error
		if ( ! g_parms.insertParm ( i, rowNum,(char *)cr)) return true;
	}
	return true;
}

bool CommandRemoveUrlFiltersRow ( char *rec ) {
	// caller must specify collnum
	collnum_t collnum = getCollnumFromParmRec ( rec );
	if ( collnum < 0 ) {
		g_errno = ENOCOLLREC;
		log("parms: bad collnum for remove row");
		return true;
	}
	// sanity
	int32_t dataSize = getDataSizeFromParmRec ( rec );
	if ( dataSize <= 1 ) {
		log("parms: insert row data size = %"INT32" bad!",dataSize);
		g_errno = EBADENGINEER;
		return true;
	}
	// need this
	CollectionRec *cr = g_collectiondb.getRec ( collnum );
	// get the row #
	char *data = getDataFromParmRec ( rec );
	int32_t rowNum = atol(data);
	// scan all parms for url filter parms
	for ( int32_t i = 0 ; i < g_parms.m_numParms ; i++ ) {
		Parm *m = &g_parms.m_parms[i];
		// parm must be a url filters parm
		if ( m->m_page != PAGE_FILTERS ) continue;
		// must be an array!
		if ( ! m->isArray() ) continue;
		// sanity check
		if ( m->m_obj != OBJ_COLL ) { char *xx=NULL;*xx=0; }
		// . nuke that parm's element
		// . returns false and sets g_errno on error
		if ( ! g_parms.removeParm ( i,rowNum,(char *)cr)) return true;
	}
	return true;
}

#ifndef PRIVACORE_SAFE_VERSION
// after we add a new coll, or at anytime after we can clone it
bool CommandCloneColl ( char *rec ) {

	// the collnum we want to affect.
	collnum_t dstCollnum = getCollnumFromParmRec ( rec );

	// . data is the collnum in ascii.
	// . from "&restart=467" for example
	char *data = rec + sizeof(key96_t) + 4;
	int32_t dataSize = *(int32_t *)(rec + sizeof(key96_t));
	//if ( dataSize < 1 ) { char *xx=NULL;*xx=0; }
	// copy parm settings from this collection name
	char *srcColl = data;
	
	// return if none to clone from
	if ( dataSize <= 0 ) return true;
	// avoid defaulting to main collection
	if ( ! data[0]  ) return true;

	CollectionRec *srcRec = NULL;
	CollectionRec *dstRec = NULL;
	srcRec = g_collectiondb.getRec ( srcColl    ); // get from name
	dstRec = g_collectiondb.getRec ( dstCollnum ); // get from #
	
	if ( ! srcRec ) 
		return log("parms: invalid coll %s to clone from",
			   srcColl);
	if ( ! dstRec ) 
		return log("parms: invalid collnum %"INT32" to clone to",
			   (int32_t)dstCollnum);

	log ("parms: cloning parms from collection %s to %s",
	      srcRec->m_coll,dstRec->m_coll);

	g_parms.cloneCollRec ( (char *)dstRec , (char *)srcRec );

	return true;
}
#endif


// customCrawl:
// 0 for regular collection
// 1 for custom crawl
// 2 for bulk job
// . returns false if blocks true otherwise
#ifndef PRIVACORE_SAFE_VERSION
bool CommandAddColl ( char *rec , char customCrawl ) {

	// caller must specify collnum
	collnum_t newCollnum = getCollnumFromParmRec ( rec );

	// sanity.
	if ( newCollnum < 0 ) {
		g_errno = ENOCOLLREC;
		log("parms: bad collnum for AddColl");
		return true;
	}

	char *data = rec + sizeof(key96_t) + 4;
	int32_t dataSize = *(int32_t *)(rec + sizeof(key96_t));
	// collection name must be at least 2 bytes (includes \0)
	if ( dataSize <= 1 ) { char *xx=NULL;*xx=0; }

	// then collname, \0 terminated
	char *collName = data;

	if ( gbstrlen(collName) > MAX_COLL_LEN ) {
		log("crawlbot: collection name too long");
		return true;
	}

	// this saves it to disk! returns false and sets g_errno on error.
	if ( ! g_collectiondb.addNewColl ( collName,
					   customCrawl ,
					   NULL ,  // copy from
					   0  , // copy from len
					   true , // save?
					   newCollnum
					   ) )
		// error! g_errno should be set
		return true;

	return true;
}

// all nodes are guaranteed to add the same collnum for the given name
bool CommandAddColl0 ( char *rec ) { // regular collection
	return CommandAddColl ( rec , 0 );
}

bool CommandAddColl1 ( char *rec ) { // custom crawl
	return CommandAddColl ( rec , 1 );
}

bool CommandAddColl2 ( char *rec ) { // bulk job
	return CommandAddColl ( rec , 2 );
}
#endif

bool CommandResetProxyTable ( char *rec ) {
	// from SpiderProxy.h
	return resetProxyStats();
}


#ifndef PRIVACORE_SAFE_VERSION
// . returns true and sets g_errno on error
// . returns false if would block
bool CommandDeleteColl ( char *rec , WaitEntry *we ) {
	collnum_t collnum = getCollnumFromParmRec ( rec );

	// the delete might block because the tree is saving and we can't
	// remove our collnum recs from it while it is doing that
	if ( ! g_collectiondb.deleteRec2 ( collnum ) )
		// we blocked, we->m_callback will be called when done
		return false;
	// delete is successful
	return true;
}

// . returns true and sets g_errno on error
// . returns false if would block
bool CommandDeleteColl2 ( char *rec , WaitEntry *we ) {
	char *data = rec + sizeof(key96_t) + 4;
	char *coll = (char *)data;
	collnum_t collnum = g_collectiondb.getCollnum ( coll );

	if ( collnum < 0 ) {
		g_errno = ENOCOLLREC;
		return true;;
	}
	// the delete might block because the tree is saving and we can't
	// remove our collnum recs from it while it is doing that
	if ( ! g_collectiondb.deleteRec2 ( collnum ) )
		// we blocked, we->m_callback will be called when done
		return false;
	// delete is successful
	return true;
}
#endif



bool CommandForceNextSpiderRound ( char *rec ) {

	// caller must specify collnum
	collnum_t collnum = getCollnumFromParmRec ( rec );
	// need this
	CollectionRec *cr = g_collectiondb.getRec ( collnum );
	if ( ! cr ) {
		g_errno = ENOCOLLREC;
		log("parms: bad collnum %"INT32" for restart spider round",
		    (int32_t)collnum);
		return true;
	}

	// seems like parmlist is an rdblist, so we have a key_t followed
	// by 4 bytes of datasize then the data... which is an ascii string
	// in our case...
	char *data = getDataFromParmRec ( rec );
	uint32_t roundStartTime;
	int32_t newRoundNum;
	// see the HACK: in Parms::convertHttpRequestToParmList() where we
	// construct this data in response to a "roundStart" cmd. we used
	// sprintf() so it's natural to use sscanf() to parse it out.
	sscanf ( data , "%"UINT32",%"INT32"", 
		 &roundStartTime,
		 &newRoundNum);

	cr->m_spiderRoundStartTime = roundStartTime;
	cr->m_spiderRoundNum = newRoundNum;

	// if we don't have this is prints  out "skipping0 ... " for urls
	// we try to spider in Spider.cpp.
	cr->m_spiderStatus = SP_INPROGRESS;

	// reset the round counts. this will log a msg. resetting the
	// round counts will prevent maxToProcess/maxToCrawl from holding
	// us back...
	spiderRoundIncremented ( cr );

	// yeah, if we don't nuke doledb then it doesn't work...
	cr->rebuildUrlFilters();

	return true;
}


#ifndef PRIVACORE_SAFE_VERSION
// . returns true and sets g_errno on error
// . returns false if would block
bool CommandRestartColl ( char *rec , WaitEntry *we ) {

	collnum_t newCollnum = getCollnumFromParmRec ( rec );

	// . data is the collnum in ascii.
	// . from "&restart=467" for example
	char *data = rec + sizeof(key96_t) + 4;
	int32_t dataSize = *(int32_t *)(rec + sizeof(key96_t));
	if ( dataSize < 1 ) { char *xx=NULL;*xx=0; }
	collnum_t oldCollnum = atol(data);

	if ( oldCollnum < 0 || 
	     oldCollnum >= g_collectiondb.m_numRecs ||
	     ! g_collectiondb.m_recs[oldCollnum] ) {
		log("parms: invalid collnum %"INT32" to restart",(int32_t)oldCollnum);
		return true;
	}

	// this can block if tree is saving, it has to wait
	// for tree save to complete before removing old
	// collnum recs from tree
	if ( ! g_collectiondb.resetColl2 ( oldCollnum ,
					   newCollnum ,
					   false ) ) // purgeSeeds?
		// we blocked, we->m_callback will be called when done
		return false;

	// turn on spiders on new collrec. collname is same but collnum
	// will be different.
	CollectionRec *cr = g_collectiondb.getRec ( newCollnum );
	// if reset from crawlbot api page then enable spiders
	// to avoid user confusion
	//if ( cr ) cr->m_spideringEnabled = 1;

	if ( ! cr ) return true;

	//
	// repopulate spiderdb with the same sites
	//

	char *oldSiteList = cr->m_siteListBuf.getBufStart();
	// do not let it have the buf any more
	cr->m_siteListBuf.detachBuf();
	// can't leave it NULL, safebuf parms do not like to be null
	cr->m_siteListBuf.nullTerm();
	// re-add the buf so it re-seeds spiderdb. it will not dedup these
	// urls in "oldSiteList" with "m_siteListBuf" which is now empty.
	// "true" = addSeeds.
	updateSiteListBuf ( newCollnum , true , oldSiteList );
	// now put it back
	if ( oldSiteList ) cr->m_siteListBuf.safeStrcpy ( oldSiteList );

	// all done
	return true;
}
#endif

#ifndef PRIVACORE_SAFE_VERSION
// . returns true and sets g_errno on error
// . returns false if would block
bool CommandResetColl ( char *rec , WaitEntry *we ) {

	collnum_t newCollnum = getCollnumFromParmRec ( rec );

	// . data is the collnum in ascii.
	// . from "&restart=467" for example
	char *data = rec + sizeof(key96_t) + 4;
	int32_t dataSize = *(int32_t *)(rec + sizeof(key96_t));
	if ( dataSize < 1 ) { char *xx=NULL;*xx=0; }
	collnum_t oldCollnum = atol(data);

	if ( oldCollnum < 0 || 
	     oldCollnum >= g_collectiondb.m_numRecs ||
	     ! g_collectiondb.m_recs[oldCollnum] ) {
		log("parms: invalid collnum %"INT32" to reset",(int32_t)oldCollnum);
		return true;
	}

	// this will not go through if tree is saving, it has to wait
	// for tree save to complete before removing old
	// collnum recs from tree. so return false in that case so caller
	// will know to re-call later.
	if ( ! g_collectiondb.resetColl2 ( oldCollnum ,
					   newCollnum ,
					   true ) ) // purgeSeeds?
		// we blocked, we->m_callback will be called when done
		return false;

	// turn on spiders on new collrec. collname is same but collnum
	// will be different.
	CollectionRec *cr = g_collectiondb.getRec ( newCollnum );

	if ( ! cr ) return true;

	//
	// repopulate spiderdb with the same sites
	//

	char *oldSiteList = cr->m_siteListBuf.getBufStart();
	// do not let it have the buf any more
	cr->m_siteListBuf.detachBuf();
	// can't leave it NULL, safebuf parms do not like to be null
	cr->m_siteListBuf.nullTerm();
	// re-add the buf so it re-seeds spiderdb. it will not dedup these
	// urls in "oldSiteList" with "m_siteListBuf" which is now empty.
	// "true" = addSeeds.
	updateSiteListBuf ( newCollnum , true , oldSiteList );
	// now put it back
	if ( oldSiteList ) cr->m_siteListBuf.safeStrcpy ( oldSiteList );

	// turn spiders off
	//if ( cr ) cr->m_spideringEnabled = 0;

	return true;
}
#endif


bool CommandParserTestInit ( char *rec ) {
	// enable testing for all other hosts
	g_conf.m_testParserEnabled = 1;
	// reset all files
	g_test.removeFiles();
	// turn spiders on globally
	g_conf.m_spideringEnabled = 1;
	//g_conf.m_webSpideringEnabled = 1;
	// turn on for test coll too
	CollectionRec *cr = g_collectiondb.getRec("qatest123");
	// turn on spiders
	if ( cr ) cr->m_spideringEnabled = 1;
	// tell spider loop to update active list
	g_spiderLoop.m_activeListValid = false;
	// if we are not host 0, turn on spiders for testing
	if ( g_hostdb.m_myHost->m_hostId != 0 ) return true;
	// start the test loop to inject urls for parsing/spidering
	g_test.initTestRun();
	// done 
	return true;
}

bool CommandSpiderTestInit ( char *rec ) {
	// enable testing for all other hosts
	g_conf.m_testSpiderEnabled = 1;
	// reset all files
	g_test.removeFiles();
	// turn spiders on globally
	g_conf.m_spideringEnabled = 1;
	//g_conf.m_webSpideringEnabled = 1;
	// turn on for test coll too
	CollectionRec *cr = g_collectiondb.getRec("qatest123");
	// turn on spiders
	if ( cr ) cr->m_spideringEnabled = 1;
	// tell spider loop to update active list
	g_spiderLoop.m_activeListValid = false;
	// if we are not host 0, turn on spiders for testing
	if ( g_hostdb.m_myHost->m_hostId != 0 ) return true;
	// start the test loop to inject urls for parsing/spidering
	g_test.initTestRun();
	// done 
	return true;
}

bool CommandSpiderTestCont ( char *rec ) {
	// enable testing for all other hosts
	g_conf.m_testSpiderEnabled = 1;
	// turn spiders on globally
	g_conf.m_spideringEnabled = 1;
	//g_conf.m_webSpideringEnabled = 1;
	// turn on for test coll too
	CollectionRec *cr = g_collectiondb.getRec("qatest123");
	// turn on spiders
	if ( cr ) cr->m_spideringEnabled = 1;
	// tell spider loop to update active list
	g_spiderLoop.m_activeListValid = false;
	// done 
	return true;
}


bool CommandMergePosdb ( char *rec ) {
	forceMergeAll ( RDB_POSDB ,1);
	// set this for each posdb base
	return true;
}


bool CommandMergeTitledb ( char *rec ) {
	forceMergeAll ( RDB_TITLEDB ,1);
	//g_titledb.getRdb()->attemptMerge    (1,true);
	return true;
}


bool CommandMergeSpiderdb ( char *rec ) {
	forceMergeAll ( RDB_SPIDERDB ,1);
	//g_spiderdb.getRdb()->attemptMerge    (1,true);
	return true;
}

bool CommandMergeLinkdb ( char *rec ) {
        forceMergeAll ( RDB_LINKDB ,1);
        //g_spiderdb.getRdb()->attemptMerge    (1,true);
        return true;
}


bool CommandDiskPageCacheOff ( char *rec ) {
	g_process.resetPageCaches();
	return true;
}

bool CommandForceIt ( char *rec ) {
	g_conf.m_forceIt = true;
	return true;
}

bool CommandDiskDump ( char *rec ) {
	g_clusterdb.getRdb()->dumpTree  ( 1 );
	g_tagdb.getRdb()->dumpTree     ( 1 );  
	g_spiderdb.getRdb()->dumpTree   ( 1 );
	g_posdb.getRdb()->dumpTree    ( 1 );
	g_titledb.getRdb()->dumpTree    ( 1 );
	g_statsdb.getRdb()->dumpTree    ( 1 );
	g_linkdb.getRdb() ->dumpTree    ( 1 );
	g_errno = 0;
	return true;
}


bool CommandJustSave ( char *rec ) {
	// returns false if blocked, true otherwise
	g_process.save ();
	// always return true here
	return true;
}

bool CommandSaveAndExit ( char *rec ) {
	// return true if this blocks
	g_process.shutdown ( false , NULL , NULL );
	return true;
}


bool CommandClearKernelError ( char *rec ) {
	g_hostdb.m_myHost->m_pingInfo.m_kernelErrors = 0;
	return true;
}

bool CommandPowerNotice ( int32_t hasPower ) {

	//int32_t hasPower = r->getLong("haspower",-1);
	log("powermo: received haspower=%"INT32"",hasPower);
	if ( hasPower != 0 && hasPower != 1 ) return true;

	// did power state change? if not just return true
	if (   g_process.m_powerIsOn &&   hasPower ) return true;
	if ( ! g_process.m_powerIsOn && ! hasPower ) return true;
	
	if ( hasPower ) {
		log("powermo: power is regained");
		g_process.m_powerIsOn = true;
		return true;
	}

	// if it was on and went off...
	// now it is off
	log("powermo: power was lost");
	// . SpiderLoop.cpp will not launch any more spiders as
	//   int32_t as the power is off
	// . autosave should kick in every 30 seconds
	g_process.m_powerIsOn = false;
	// note the autosave
	log("powermo: disabling spiders, suspending merges, disabling "
	    "tree writes and saving.");
	// tell Process.cpp::save2() to save the blocking caches too!
	//g_process.m_pleaseSaveCaches = true;
	// . save everything now... this may block some when saving the
	//   caches... then do not do ANY writes... 
	// . RdbMerge suspends all merging if power is off
	// . Rdb.cpp does not allow any adds if power is off. it will
	//   send back an ETRYAGAIN...
	// . if a tree is being dumped, this will keep re-calling
	//   Process.cpp::save2()
	g_process.save();

	// also send an email if we are host #0
	if ( g_hostdb.m_myHost->m_hostId != 0 ) return true;
	if ( g_proxy.isProxy() ) return true;

	char tmp[128];
	Host *h0 = g_hostdb.getHost ( 0 );
	int32_t ip0 = 0;
	if ( h0 ) ip0 = h0->m_ip;
	sprintf(tmp,"%s: POWER IS OFF",iptoa(ip0));

	g_pingServer.sendEmail ( NULL  , // Host ptr
				 tmp   , // msg
				 true  , // sendToAdmin
				 false , // oom?
				 false , // kernel error?
				 true  , // parm change?
				 // force it? even if disabled?
				 false  );
	return true;
}


bool CommandPowerOnNotice ( char *rec ) {
	return CommandPowerNotice ( 1 );
}

bool CommandPowerOffNotice ( char *rec ) {
	return CommandPowerNotice ( 0 );
}

bool CommandInSync ( char *rec ) {
	g_parms.m_inSyncWithHost0 = true;
	return true;
}

//////////////////////
//
// end new commands
//
//////////////////////


static bool printDropDown   ( int32_t n , SafeBuf* sb, char *name, 
			      int32_t selet , 
			      bool includeMinusOne ,
			      bool includeMinusTwo ) ;

extern bool closeAll ( void *state, void (* callback)(void *state) );
extern bool allExit ( ) ;

Parms::Parms ( ) {
	m_isDefaultLoaded = false;
	m_inSyncWithHost0 = false;
	m_triedToSync     = false;
}

void Parms::detachSafeBufs ( CollectionRec *cr ) {
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		Parm *m = &m_parms[i];
		if ( m->m_type != TYPE_SAFEBUF ) continue;
		if ( m->m_obj != OBJ_COLL ) continue;
		if ( m->m_off < 0 ) continue;
		int32_t max = 1;
		// this will be zero if not an array.
		// othewise it is the # of elements in the array
		if ( m->m_size > max ) max = m->m_size;
		// an array of safebufs? m->m_size will be > 1 then.
		for ( int32_t j = 0 ; j < max ; j++ ) {
			// get it
			SafeBuf *sb = (SafeBuf *)((char *)cr + m->m_off +
						  j*sizeof(SafeBuf));
			sb->detachBuf();
		}
	}
}

// from Pages.cpp
bool printApiForPage ( SafeBuf *sb , int32_t PAGENUM , CollectionRec *cr ) ;

// returns false and sets g_errno on error
bool Parms::setGigablastRequest ( TcpSocket *socket ,
				  HttpRequest *hrArg ,
				  GigablastRequest *gr ) {
	// get the page from the path... like /sockets --> PAGE_SOCKETS
	int32_t page = g_pages.getDynamicPageNumber ( hrArg );
	// is it a collection?
	char *THIS = (char *)gr;

	// ensure valid
	if ( ! THIS ) {
		// it is null when no collection explicitly specified...
		log("admin: THIS is null for page %"INT32".",page);
		return false;
	}

	// just in case
	memset ( gr , 0 , sizeof(GigablastRequest) );

	gr->m_socket = socket;

	// make a copy of the httprequest because the original is on the stack
	// in HttpServer::requestHandler()
	if ( ! gr->m_hr.copy ( hrArg ) ) {
		log("admin: failed to copy httprequest: %s",
		    mstrerror(g_errno));
		return false;
	}

	// use the one we copied which won't disappear/beFreed on us
	HttpRequest *hr = &gr->m_hr;

	// need this
	int32_t obj = OBJ_GBREQUEST;

	//
	// reset THIS to defaults. use NULL for cr since mostly for SearchInput
	//
	setToDefault ( THIS , obj , NULL);


	// map PAGE_ADDURL to PAGE_ADDURL2 so 
	// /addurl is same as /admin/addurl as far as parms.
	if ( page == PAGE_ADDURL ) 
		page = PAGE_ADDURL2;

	// loop through cgi parms
	for ( int32_t i = 0 ; i < hr->getNumFields() ; i++ ) {
		// get cgi parm name
		char *field = hr->getField    ( i );
		//int32_t  flen  = hr->getFieldLen ( i );
		// find in parms list
		int32_t  j;
		Parm *m;
		for ( j = 0 ; j < m_numParms ; j++ ) {
			// get it
			m = &m_parms[j];
			// must be of this type
			if ( m->m_obj != obj ) continue;
			// page must match
			if ( m->m_page != page ) continue;
			// skip if no cgi parm, may not be configurable now
			if ( ! m->m_cgi ) continue;
			// otherwise, must match the cgi name exactly
			if ( strcmp ( field,m->m_cgi ) == 0 ) break;
		}
		// bail if the cgi field is not in the parms list
		if ( j >= m_numParms ) {
			//log("parms: missing cgi parm %s",field);
			continue;
		}
		// value of cgi parm (null terminated)
		char *v = hr->getValue ( i );
		// . skip if no value was provided
		// . unless it was a string! so we can make them empty.
		if ( v[0] == '\0' && 
		     m->m_type != TYPE_CHARPTR &&
		     m->m_type != TYPE_STRING && 
		     m->m_type != TYPE_STRINGBOX ) continue;
		// skip if offset is negative, that means none
		if ( m->m_off < 0 ) continue;
		// skip if no permission
		//if ( (m->m_perms & user) == 0 ) continue;
		// set it. now our TYPE_CHARPTR will just be set to it directly
		// to save memory...
		setParm ( (char *)THIS , m, j, 0, v, false,//not html enc
			  false ); // true );
		// need to save it
		//if ( THIS != (char *)&g_conf ) 
		//	((CollectionRec *)THIS)->m_needsSave = true;
	}
	
	return true;
}

bool printSitePatternExamples ( SafeBuf *sb , HttpRequest *hr );

// . returns false if blocked, true otherwise
// . sets g_errno on error
// . must ultimately send reply back on "s"
// . called by Pages.cpp's sendDynamicReply() when it calls pg->function()
//   which is called by HttpServer::sendReply(s,r) when it gets an http request
bool Parms::sendPageGeneric ( TcpSocket *s , HttpRequest *r ) {

	char  buf [ 128000 ];
	SafeBuf stackBuf(buf,128000);

	SafeBuf *sb = &stackBuf;

	int32_t page = g_pages.getDynamicPageNumber ( r );

	char format = r->getReplyFormat();

	char guide = r->getLong("guide",0);


	bool isMasterAdmin = g_conf.isMasterAdmin ( s , r );
	bool isCollAdmin = g_conf.isCollAdmin ( s , r );
	if ( ! g_conf.m_allowCloudUsers &&
	     ! isMasterAdmin &&
	     ! isCollAdmin ) {
		char *msg = "NO PERMISSION";
		return g_httpServer.sendDynamicPage (s, msg,gbstrlen(msg));
	}

	//
	// CLOUD SEARCH ENGINE SUPPORT
	//
	char *action = r->getString("action",NULL);
	if ( page == PAGE_BASIC_SETTINGS &&
	     guide &&
	     // this is non-null if handling a submit request
	     action && 
	     format == FORMAT_HTML ) {
		//return g_parms.sendPageGeneric ( s, r, PAGE_BASIC_SETTINGS );
		// just redirect to it
		char *coll = r->getString("c",NULL);
		if ( coll ) {
			sb->safePrintf("<meta http-equiv=Refresh "
				      "content=\"0; URL=/widgets.html"
				      "?guide=1&c=%s\">",
				      coll);
			return g_httpServer.sendDynamicPage (s,
							     sb->getBufStart(),
							     sb->length());
		}
	}


	//
	// some "generic" pages do additional processing on the provided input
	// so we need to call those functions here...
	//

	char *bodyjs = NULL;
	if ( page == PAGE_BASIC_SETTINGS )
		bodyjs =" onload=document.getElementById('tabox').focus();";

	// print standard header
	if ( format != FORMAT_XML && format != FORMAT_JSON )
		g_pages.printAdminTop ( sb , s , r , NULL , bodyjs );

	// xml/json header
	char *res = NULL;
	if ( format == FORMAT_XML )
		res = "<response>\n"
			"\t<statusCode>0</statusCode>\n"
			"\t<statusMsg>Success</statusMsg>\n";
	if ( format == FORMAT_JSON )
		res = "{ \"response:\"{\n"
			"\t\"statusCode\":0,\n"
			"\t\"statusMsg\":\"Success\"\n";
	if ( res )
		sb->safeStrcpy ( res );
	
	// do not show the parms and their current values unless showsettings=1
	// was explicitly given for the xml/json feeds
	int32_t show = 1;
	if ( format != FORMAT_HTML )
	 	show = r->getLong("show",0);
	if ( show )
	 	printParmTable ( sb , s , r );

	// xml/json tail
	if ( format == FORMAT_XML )
		res = "</response>\n";
	if ( format == FORMAT_JSON )
		res = "\t}\n}\n";
	if ( res )
		sb->safeStrcpy ( res );


	bool POSTReply = g_pages.getPage ( page )->m_usePost;

	char *ct = "text/html";
	if ( format == FORMAT_XML ) ct = "text/xml";
	if ( format == FORMAT_JSON ) ct = "application/json";

	return g_httpServer.sendDynamicPage ( s                , 
					      sb->getBufStart() ,
					      sb->length()      , 
					      -1               ,
					      POSTReply        ,
					      ct             , // contType
					      -1               , // httpstatus
					      NULL,//cookie           ,
					      NULL             );// charset
}


bool Parms::printParmTable ( SafeBuf *sb , TcpSocket *s , HttpRequest *r ) {

	int32_t page = g_pages.getDynamicPageNumber ( r );

#ifndef PRIVACORE_SAFE_VERSION
	int32_t  fromIp   = s->m_ip;
#endif

	char format = r->getReplyFormat();

	if ( page == PAGE_COLLPASSWORDS2 )
		page = PAGE_COLLPASSWORDS;

	// print the start of the table
	char *tt = "None";
	if ( page == PAGE_LOG        ) tt = "Log Controls";
	if ( page == PAGE_MASTER     ) tt = "Master Controls";
	if ( page == PAGE_INJECT     ) tt = "Inject Url";
	if ( page == PAGE_MASTERPASSWORDS ) tt = "Master Passwords";
	if ( page == PAGE_ADDURL2    ) tt = "Add Urls";
	if ( page == PAGE_SPIDER     ) tt = "Spider Controls";
	if ( page == PAGE_SEARCH     ) tt = "Search Controls";
	if ( page == PAGE_FILTERS    ) tt = "Url Filters";
	if ( page == PAGE_BASIC_SETTINGS ) tt = "Settings";
	if ( page == PAGE_COLLPASSWORDS ) tt = "Collection Passwords";
#ifndef PRIVACORE_SAFE_VERSION
	if ( page == PAGE_REPAIR     ) tt = "Rebuild Controls";
#endif

	// special messages for spider controls
	char *e1 = "";
	char *e2 = "";
	if ( page == PAGE_SPIDER && ! g_conf.m_spideringEnabled )
		e1 = "<tr><td colspan=20><font color=#ff0000><b><center>"
			"Spidering is temporarily disabled in Master Controls."
			"</font></td></tr>\n";
	if ( page == PAGE_SPIDER && ! g_conf.m_addUrlEnabled )
		e2 = "<tr><td colspan=20><font color=#ff0000><b><center>"
			"Add url is temporarily disabled in Master Controls."
			"</font></td></tr>\n";

	if ( format == FORMAT_XML || format == FORMAT_JSON ) {
		char *coll = g_collectiondb.getDefaultColl(r);
		CollectionRec *cr = g_collectiondb.getRec(coll);//2(r,true);
		bool isMasterAdmin = g_conf.isMasterAdmin ( s , r );
		bool isCollAdmin = g_conf.isCollAdmin ( s , r );
	 	g_parms.printParms2 ( sb ,
				      page ,
	 	 		      cr ,
	 	 		      1 , // int32_t nc , # cols?
	 	 		      1 , // int32_t pd , print desc?
	 	 		      false , // isCrawlbot
	 	 		      format ,
	 	 		      NULL , // TcpSocket *sock
				      isMasterAdmin ,
				      isCollAdmin ); 
		return true;
	}


	// . page repair (PageRepair.cpp) has a status table BEFORE the parms
	//   iff we are doing a repair
	// . only one page for all collections, we have a parm that is
	//   a comma-separated list of the collections to repair. leave blank
	//   to repair all collections.
#ifndef PRIVACORE_SAFE_VERSION
	if ( page == PAGE_REPAIR )
		g_repair.printRepairStatus ( sb , fromIp );
#endif
	
	// start the table
	sb->safePrintf( 
		       "\n"
		       "<table %s "
		       //"style=\"border-radius:15px;"
		       //"border:#6060f0 2px solid;"
		       //"\" "
		       //"width=100%% bgcolor=#%s "
		       //"bgcolor=black "
		       //"cellpadding=4 "
		       //"border=0 "//border=1 "
		       "id=\"parmtable\">"
		       "<tr><td colspan=20>"// bgcolor=#%s>"
		       ,TABLE_STYLE
		       //,DARKER_BLUE
		       //,DARK_BLUE
			);

	sb->safePrintf(//"<div style=\"margin-left:45%%;\">"
		       //"<font size=+1>"
		       "<center>"
		       "<b>%s</b>"
		       //"</font>"
		       "</center>"
		       //"</div>"
		       "</td></tr>%s%s\n",
		       tt,e1,e2);

	//bool isCrawlbot = false;
	//if ( collOveride ) isCrawlbot = true;
		
	// print the table(s) of controls
	//p= g_parms.printParms (p, pend, page, user, THIS, coll, pwd, nc, pd);
	g_parms.printParms ( sb , s , r );

	// end the table
	sb->safePrintf ( "</table>\n" );

	// this must be outside of table, submit button follows
	sb->safePrintf ( "<br>\n" );

	if ( page == PAGE_SPIDERPROXIES ) {
		// wrap up the form, print a submit button
		g_pages.printSubmit ( sb );
		printSpiderProxyTable ( sb );
		// do not print another submit button
		return true;
	}

	// url filter page has a test table
	if ( page == PAGE_FILTERS ) {
		// wrap up the form, print a submit button
		g_pages.printSubmit ( sb );
		printUrlExpressionExamples ( sb );
	}
	else if ( page == PAGE_BASIC_SETTINGS ) {
		// wrap up the form, print a submit button
		g_pages.printSubmit ( sb );
		printSitePatternExamples ( sb , r );
	}
	else if ( page == PAGE_SPIDER ) { // PAGE_SITES
		// wrap up the form, print a submit button
		g_pages.printSubmit ( sb );
		printSitePatternExamples ( sb , r );
	}
	else {
		// wrap up the form, print a submit button
		g_pages.printAdminBottom ( sb );
	}

	return true;
}

bool printDropDown ( int32_t n , SafeBuf* sb, char *name, int32_t select,
		     bool includeMinusOne ,
		     bool includeMinusTwo ) {	// begin the drop down menu
	sb->safePrintf ( "<select name=%s>", name );
	char *s;
	int32_t i = -1;
	if ( includeMinusOne ) i = -1;
	// . by default, minus 2 includes minus 3, the new "FILTERED" priority
	// . it is link "BANNED" but does not mean the url is low quality necessarily
	if ( includeMinusTwo ) i = -3;

	// no more DELETE, etc.
	i = 0;
	if ( select < 0 ) select = 0;

	for ( ; i < n ; i++ ) {
		if ( i == select ) s = " selected";
		else               s = "";
		if      ( i == -3 ) 
			sb->safePrintf ("<option value=%"INT32"%s>DELETE",i,s);
		else if ( i == -2 ) 
			//sb->safePrintf ("<option value=%"INT32"%s>BANNED",i,s);
			continue;
		else if ( i == -1 ) 
			//sb->safePrintf ("<option value=%"INT32"%s>undefined",i,s);
			continue;
		else    
			sb->safePrintf ("<option value=%"INT32"%s>%"INT32"",i,s,i);
	}
	sb->safePrintf ( "</select>" );
	return true;
}

class DropLangs {
public:
	char *m_title;
	char *m_lang;
	char *m_tld;
};

DropLangs g_drops[] = {
	{"custom",NULL,NULL},
	{"web",NULL,NULL},
	{"news",NULL,NULL},
	{"english","en","com,us.gov,org"},
	{"german","de","de"},
	{"french","fr","fr"},
	{"norweigian","nl","nl"},
	{"spanish","es","es"},
	{"italian","it","it"},
	{"romantic","en,de,fr,nl,es,it","com,us.gov,org,de,fr,nl,es,it"}
};

// "url filters profile" values. used to set default crawl rules
// in Collectiondb.cpp's CollectionRec::setUrlFiltersToDefaults(). 
// for instance, UFP_NEWS spiders sites more frequently but less deep in
// order to get "news" pages and articles
bool printDropDownProfile ( SafeBuf* sb, char *name, CollectionRec *cr ) {
	sb->safePrintf ( "<select name=%s>", name );
	// the type of url filters profiles
	//char *items[] = {"custom","web","news","chinese","shallow"};
	int32_t nd = sizeof(g_drops)/sizeof(DropLangs);
	for ( int32_t i = 0 ; i < nd ; i++ ) {
		//if ( i == select ) s = " selected";
		//else               s = "";
		char *x = cr->m_urlFiltersProfile.getBufStart();
		char *s;
		if ( strcmp(g_drops[i].m_title, x) == 0 ) s = " selected";
		else                                      s = "";
		sb->safePrintf ("<option value=%s%s>%s",
				g_drops[i].m_title,
				s,
				g_drops[i].m_title );
	}
	sb->safePrintf ( "</select>");
	return true;
}

bool printCheckBoxes ( int32_t n , SafeBuf* sb, char *name, char *array){
	for ( int32_t i = 0 ; i < n ; i++ ) {
		if ( i > 0 ) 
			sb->safePrintf ("<input type=checkbox value=1 name=%s%"INT32"",
					name,i);
		else
			sb->safePrintf ("<input type=checkbox value=1 name=%s",
				 name);
		if ( array[i] ) {
			sb->safePrintf ( " checked");
		}
		sb->safePrintf ( ">%"INT32" &nbsp;" , i );
		//if i is single digit, add another nbsp so that everything's 
		//aligned
		if ( i < 10 )
			sb->safePrintf("&nbsp;&nbsp;");
			
		if ( i > 0 && (i+1) % 6 == 0 )
			sb->safePrintf("<br>\n");
	}
	return true;
}

bool Parms::printParms (SafeBuf* sb, TcpSocket *s , HttpRequest *r) {
	int32_t  page = g_pages.getDynamicPageNumber ( r );
	int32_t nc = r->getLong("nc",1);
	int32_t pd = r->getLong("pd",1);
	char *coll = g_collectiondb.getDefaultColl(r);
	CollectionRec *cr = g_collectiondb.getRec(coll);//2(r,true);

	bool isMasterAdmin = g_conf.isMasterAdmin ( s , r );
	bool isCollAdmin = g_conf.isCollAdmin ( s , r );

	//char *coll = r->getString ( "c"   );
	//if ( ! coll || ! coll[0] ) coll = "main";
	//CollectionRec *cr = g_collectiondb.getRec ( coll );
	// if "main" collection does not exist, try another
	//if ( ! cr ) cr = getCollRecFromHttpRequest ( r );
	printParms2 ( sb, page, cr, nc, pd,0,0 , s,isMasterAdmin,isCollAdmin);
	return true;
}

static int32_t s_count = 0;

bool Parms::printParms2 ( SafeBuf* sb , 
			  int32_t page , 
			  CollectionRec *cr , 
			  int32_t nc ,
			  int32_t pd , 
			  bool isCrawlbot , 
			  char format , // bool isJSON ,
			  TcpSocket *sock ,
			  bool isMasterAdmin ,
			  bool isCollAdmin ) {
	bool status = true;
	s_count = 0;
	// background color
	char *bg1 = LIGHT_BLUE;
	char *bg2 = DARK_BLUE;
	// background color
	char *bg = NULL;

	char *coll = NULL;
	if ( cr ) coll = cr->m_coll;

	// page aliases
	//if ( page == PAGE_COLLPASSWORDS )
	//	page = PAGE_MASTERPASSWORDS;

	if ( page == PAGE_COLLPASSWORDS2 )
		page = PAGE_COLLPASSWORDS;

	GigablastRequest gr;
	g_parms.setToDefault ( (char *)&gr , OBJ_GBREQUEST , NULL);
    
	InjectionRequest ir;
	g_parms.setToDefault ( (char *)&ir , OBJ_IR , NULL);	

	// Begin "parms":[]
	if (format == FORMAT_JSON ) {     
		sb->safePrintf ("\"parms\":[\n");
	}

	// find in parms list
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		// get it
		Parm *m = &m_parms[i];
		// make sure we got the right parms for what we want
		if ( m->m_page != page ) continue;
		// and same object tpye. but allow OBJ_NONE for
		// PageAddUrl.cpp
		//if ( m->m_obj != parmObj && m->m_obj != OBJ_NONE ) continue;
		// skip if offset is negative, that means none
		// well then use OBJ_NONE now!!!
		//if ( m->m_off < 0 && 
		//     m->m_type != TYPE_MONOD2 &&
		//     m->m_type != TYPE_MONOM2 &&
		//     m->m_type != TYPE_CMD     ) continue;
		// skip if hidden
		if ( m->m_flags & PF_HIDDEN ) continue;

		// or if should not show in html, like the 
		// name of the collection, the "c" parm we do not show
		// generally on the html page even though it is a required parm
		// we have it in a hidden html input tag in Pages.cpp.
		if ( (m->m_flags & PF_NOHTML) && 
		     format != FORMAT_JSON &&
		     format != FORMAT_XML )
			continue;

		// get right ptr
		char *THIS = NULL;
		if ( m->m_obj == OBJ_CONF ) 
			THIS = (char *)&g_conf;
		if ( m->m_obj == OBJ_COLL ) {
			THIS = (char *)cr;
			if ( ! THIS ) continue;
		}
		if ( m->m_obj == OBJ_GBREQUEST )
			THIS = (char *)&gr;
		if ( m->m_obj == OBJ_IR )
			THIS = (char *)&ir;
		// might have an array, do not exceed the array size
		int32_t  jend = m->m_max;
		int32_t  size = jend ;
		char *ss   = ((char *)THIS + m->m_off - 4);
		if ( m->m_type == TYPE_MONOD2 ) ss = NULL;
		if ( m->m_type == TYPE_MONOM2 ) ss = NULL;
		if ( m->m_max > 1 && ss ) size = *(int32_t *)ss;
		if ( size < jend        ) jend = size;
		// toggle background color on group boundaries...
		if ( m->m_group == 1 ) {
			if ( bg == bg1 ) bg = bg2;
			else             bg = bg1;
		}

			//
			// mdw just debug to here ... left off here
			//char *xx=NULL;*xx=0;

		// . do we have an array? if so print title on next row
		//   UNLESS these are priority checkboxes, those can all 
		//   cluster together onto one row
		// . only add if not in a row of controls
		if ( m->m_max > 1 && m->m_type != TYPE_PRIORITY_BOXES &&
		     m->m_rowid == -1 &&
		     format != FORMAT_JSON &&
		     format != FORMAT_XML ) { // ! isJSON ) {
			//
			// make a separate table for array of parms
			sb->safePrintf (
				  //"<table width=100%% bgcolor=#d0d0e0 "
				  //"cellpadding=4 border=1>\n"
				  "<tr><td colspan=20 bgcolor=#%s>"
				  "<center>"
				  //"<font size=+1>"
				  "<b>%s"
				  "</b>"
				  //"</font>"
				  "</td></tr>\n"
				  "<tr><td colspan=20><font size=-1>"
				  ,DARK_BLUE,m->m_title);
			// print the description
			sb->safePrintf ( "%s" , m->m_desc );
			// end the description
			sb->safePrintf("</font></td></tr>\n");

		}

		// arrays always have blank line for adding stuff
		if ( m->m_max > 1 ) {
			size++;
		}

		// if m_rowid of consecutive parms are the same then they
		// are all printed in the same row, otherwise the inner loop
		// has no effect
		int32_t rowid = m_parms[i].m_rowid;
		// if not part of a complex row, just print this array right up
		if ( rowid == -1 ) {
			for ( int32_t j = 0 ; j < size ; j++ )
				status &=printParm ( sb,NULL,&m_parms[i],i,
						     j, jend, (char *)THIS,
						     coll,NULL,
						     bg,nc,pd,
						     false,
						     isCrawlbot,
						     format,
						     isMasterAdmin,
						     isCollAdmin,
						     sock);
			continue;
		}
		// if not first in a row, skip it, we printed it already
		if ( i > 0 && m_parms[i-1].m_rowid == rowid ) continue;

		// otherwise print everything in the row
		for ( int32_t j = 0 ; j < size ; j++ ) {
			// flip j if in this page
			int32_t newj = j;
			//if ( m->m_page == PAGE_PRIORITIES )
			//	newj = size - 1 - j;
			for ( int32_t k = i ; 
			      k < m_numParms && 
				      m_parms[k].m_rowid == rowid;  
			      k++ ) {

				status &=printParm(sb,NULL,&m_parms[k],k,
					    newj,jend,(char *)THIS,coll,NULL,
						   bg,nc,pd, j==size-1,
						   isCrawlbot,format,
						   isMasterAdmin,
						   isCollAdmin,
						   sock);
			}
		}
	}

  // end "parms":[]
  if ( format == FORMAT_JSON ) {
    if ( m_numParms != 0 ) sb->m_length -= 2;
    sb->safePrintf("\n]\n");
  }

	return status;
}


bool Parms::printParm ( SafeBuf* sb,
			//int32_t  user ,
			char *username,
			Parm *m    , 
			int32_t  mm   , // m = &m_parms[mm]
			int32_t  j    ,
			int32_t  jend ,
			char *THIS ,
			char *coll ,
			char *pwd  ,
			char *bg   ,
			int32_t  nc   , // # column?
			int32_t  pd   , // print description
			bool lastRow ,
			bool isCrawlbot ,
			//bool isJSON ) {
			char format ,
			bool isMasterAdmin ,
			bool isCollAdmin ,
			TcpSocket *sock ) {
	bool status = true;
	// priv of 4 means do not print at all
	if ( m->m_priv == 4 ) return true;

	// do not print comments, those are for the xml conf file
	if ( m->m_type == TYPE_COMMENT ) return true;

	if ( m->m_flags & PF_HIDDEN ) return true;

	CollectionRec *cr = NULL;
	collnum_t collnum = -1;
	if ( coll ) {
		cr = g_collectiondb.getRec ( coll );
		if ( cr ) collnum = cr->m_collnum;
	}

	if ( format == FORMAT_XML || format == FORMAT_JSON ) {
		// the upload button has no val, cmds too
		if ( m->m_type == TYPE_FILEUPLOADBUTTON ) return true;
	}

	int32_t page = m->m_page;

	if ( format == FORMAT_XML ) {
		sb->safePrintf ( "\t<parm>\n");
		sb->safePrintf ( "\t\t<title><![CDATA[");
		sb->cdataEncode ( m->m_title );
		sb->safePrintf ( "]]></title>\n");
		sb->safePrintf ( "\t\t<desc><![CDATA[");
		sb->cdataEncode ( m->m_desc );
		sb->safePrintf ( "]]></desc>\n");
		if ( m->m_flags & PF_REQUIRED )
			sb->safePrintf("\t\t<required>1</required>\n");
		sb->safePrintf ( "\t\t<cgi>%s</cgi>\n",m->m_cgi);
		// and default value if it exists
		char *def = m->m_def;
		if ( ! def ) def = "";
		sb->safePrintf ( "\t\t<defaultValue><![CDATA[");
		sb->cdataEncode ( def );
		sb->safePrintf ( "]]></defaultValue>\n");
		if ( page == PAGE_MASTER ||
		     page == PAGE_SEARCH ||
		     page == PAGE_SPIDER ||
		     page == PAGE_SPIDERPROXIES ||
		     page == PAGE_FILTERS ||
		     page == PAGE_MASTERPASSWORDS ||
#ifndef PRIVACORE_SAFE_VERSION
		     page == PAGE_REPAIR ||
#endif
		     page == PAGE_LOG ) {
			sb->safePrintf ( "\t\t<currentValue><![CDATA[");
			SafeBuf xb;
			m->printVal ( &xb , collnum , 0 );//occNum
			sb->cdataEncode ( xb.getBufStart() );
			sb->safePrintf ( "]]></currentValue>\n");
		}
		sb->safePrintf ( "\t</parm>\n");
		return true;
	}

	if ( format == FORMAT_JSON ) {
		sb->safePrintf ( "\t{\n");
		sb->safePrintf ( "\t\t\"title\":\"%s\",\n",m->m_title);
		sb->safePrintf ( "\t\t\"desc\":\"");
		sb->jsonEncode ( m->m_desc );
		sb->safePrintf("\",\n");
		if ( m->m_flags & PF_REQUIRED )
			sb->safePrintf("\t\t\"required\":1,\n");
		sb->safePrintf ( "\t\t\"cgi\":\"%s\",\n",m->m_cgi);
		// and default value if it exists
		char *def = m->m_def;
		if ( ! def ) def = "";
		sb->safePrintf ( "\t\t\"defaultValue\":\"");
		sb->jsonEncode(def);
		sb->safePrintf("\",\n");
		if ( page == PAGE_MASTER ||
		     page == PAGE_SEARCH ||
		     page == PAGE_SPIDER ||
		     page == PAGE_SPIDERPROXIES ||
		     page == PAGE_FILTERS ||
		     page == PAGE_MASTERPASSWORDS ||
#ifndef PRIVACORE_SAFE_VERSION
		     page == PAGE_REPAIR ||
#endif
		     page == PAGE_LOG ) {
			sb->safePrintf ( "\t\t\"currentValue\":\"");
			SafeBuf js;
			m->printVal ( &js , collnum , 0 );//occNum );
			sb->jsonEncode(js.getBufStart());
			sb->safePrintf("\",\n");
		}
    sb->m_length -= 2; // hack of trailing comma
		sb->safePrintf("\n\t},\n");
		return true;
	}

	// what type of parameter?
	char t = m->m_type;
	// point to the data in THIS
	char *s = THIS + m->m_off + m->m_size * j ;

	// if THIS is NULL then it must be GigablastRequest or something
	// and is not really a persistent thing, but a one-shot deal.
	if ( ! THIS ) s = NULL;

	// . if an array, passed our end, this is the blank line at the end
	// . USE THIS EMPTY/DEFAULT LINE TO ADD NEW DATA TO AN ARRAY
	// . make at least as big as a int64_t
	if ( j >= jend ) s = "\0\0\0\0\0\0\0\0";
	// delimit each cgi var if we need to
	if ( m->m_cgi && gbstrlen(m->m_cgi) > 45 ) {
		log(LOG_LOGIC,"admin: Cgi variable is TOO big.");
		char *xx = NULL; *xx = 0;
	}
	char cgi[64];
	if ( m->m_cgi ) {
		if ( j > 0 ) sprintf ( cgi , "%s%"INT32"" , m->m_cgi , j );
		else         sprintf ( cgi , "%s"    , m->m_cgi     );
		// let's try dropping the index # and just doing dup parms
		//sprintf ( cgi , "%s"    , m->m_cgi     );
	}
	// . display title and description of the control/parameter
	// . the input cell of some parameters are colored
	char *color = "";
	if ( t == TYPE_CMD  || t == TYPE_BOOL2 ) 
		color = " bgcolor=#6060ff";
	if ( t == TYPE_BOOL ) {
		if ( *s ) color = " bgcolor=#00ff00";
		else      color = " bgcolor=#ff0000";
	}
	if ( t == TYPE_BOOL || t == TYPE_BOOL2 ) {
		// disable controls not allowed in read only mode
		if ( g_conf.m_readOnlyMode && m->m_rdonly )
			  color = " bgcolor=#ffff00";
	}

	bool firstInRow = false;
	if ( (s_count % nc) == 0 ) firstInRow = true;
	s_count++;

	if ( mm > 0 && m->m_rowid >= 0 && m_parms[mm-1].m_rowid == m->m_rowid )
		firstInRow = false;

	int32_t firstRow = 0;
	//if ( m->m_page==PAGE_PRIORITIES ) firstRow = MAX_PRIORITY_QUEUES - 1;
	// . use a separate table for arrays
	// . make title and description header of that table
	// . do not print all headers if not m_hdrs, a special case for the 
	//   default line in the url filters table
	if ( j == firstRow && m->m_rowid >= 0 && firstInRow && m->m_hdrs ) {
		// print description as big comment
		if ( m->m_desc && pd == 1 ) {
			// url FILTERS table description row
			sb->safePrintf ( "<td colspan=20 bgcolor=#%s>"
					 "<font size=-1>\n" , DARK_BLUE);

			//p = htmlEncode ( p , pend , m->m_desc ,
			//		 m->m_desc + gbstrlen ( m->m_desc ) );
			sb->safePrintf ( "%s" , m->m_desc );
			sb->safePrintf ( "</font></td></tr>"
					 // for "#,expression,harvestlinks.."
					 // header row in url FILTERS table
					 "<tr bgcolor=#%s>\n" ,DARK_BLUE);
		}
		// # column
		// do not show this for PAGE_PRIORITIES it is confusing
		if ( m->m_max > 1 ) {
		     //m->m_page != PAGE_PRIORITIES ) {
			sb->safePrintf (  "<td><b>#</b></td>\n" );
		}
		// print all headers
		for ( int32_t k = mm ; 
		      k<m_numParms && m_parms[k].m_rowid==m->m_rowid; k++ ) {
			// parm shortcut
			Parm *mk = &m_parms[k];
			// not if printing json
			//if ( format != FORMAT_HTML )continue;//isJSON )
			// skip if hidden
			if ( cr && ! cr->m_isCustomCrawl &&
			     (mk->m_flags & PF_DIFFBOT) )
				continue;

			// . hide table column headers that are too advanced
			// . we repeat this logic above for the actual parms
			//char *vt = "";
			//if ( isCrawlbot &&
			//     m->m_page == PAGE_FILTERS &&
			//     (strcmp(mk->m_xml,"spidersEnabled") == 0 ||
			//      //strcmp(mk->m_xml,"maxSpidersPerRule")==0||
			//      //strcmp(mk->m_xml,"maxSpidersPerIp") == 0||
			//      strcmp(mk->m_xml,"spiderIpWait") == 0 ) )
			//	vt = " style=display:none;display:none;";
			//sb->safePrintf ( "<td%s>" , vt );
			sb->safePrintf ( "<td>" );
			// if its of type checkbox in a table make it
			// toggle them all on/off
			if ( mk->m_type == TYPE_CHECKBOX &&
			     mk->m_page == PAGE_FILTERS ) {
				sb->safePrintf("<a href=# "
					       "onclick=\"checkAll(this, "
					       "'id_%s', %"INT32");\">",
					       m_parms[k].m_cgi, m->m_max);
			}
			sb->safePrintf ( "<b>%s</b>", m_parms[k].m_title );
			if ( mk->m_type == TYPE_CHECKBOX &&
			     mk->m_page == PAGE_FILTERS ) 
				sb->safePrintf("</a>");
			sb->safePrintf ("</td>\n");
		}
		//if ( format == FORMAT_HTML ) 
		sb->safePrintf ( "</tr>\n" ); // mdw added
	}

	// skip if hidden. diffbot api url only for custom crawls.
	//if(cr && ! cr->m_isCustomCrawl && (m->m_flags & PF_DIFFBOT) )
	//	return true;

	// print row start for single parm
	if ( m->m_max <= 1 && ! m->m_hdrs ) {
		if ( firstInRow ) {
			sb->safePrintf ( "<tr bgcolor=#%s><td>" , bg );
		}
		sb->safePrintf ( "<td width=%"INT32"%%>" , 100/nc/2 );
	}

	// if parm value is not defaut, use orange!
	char rr[1024];
	SafeBuf val1(rr,1024);
	if ( m->m_type != TYPE_FILEUPLOADBUTTON )
		m->printVal ( &val1 , collnum , j ); // occNum );
	// test it
	if ( m->m_def && 
	     m->m_obj != OBJ_NONE &&
	     m->m_obj != OBJ_IR && // do not do for injectionrequest
	     m->m_obj != OBJ_GBREQUEST && // do not do for GigablastRequest
	     strcmp ( val1.getBufStart() , m->m_def ) )
		// put non-default valued parms in orange!
		bg = "ffa500";


	// print the title/description in current table for non-arrays
	if ( m->m_max <= 1 && m->m_hdrs ) { // j == 0 && m->m_rowid < 0 ) {
		if ( firstInRow )
			sb->safePrintf ( "<tr bgcolor=#%s>",bg);

		if ( t == TYPE_STRINGBOX ) {
			sb->safePrintf ( "<td colspan=2><center>"
				  "<b>%s</b><br><font size=-1>",m->m_title );
			if ( pd ) {
				status &= sb->htmlEncode (m->m_desc,
							  gbstrlen(m->m_desc),
							  false);
				// is it required?
				if ( m->m_flags & PF_REQUIRED )
					sb->safePrintf(" <b><font color=green>"
						       "REQUIRED</font></b>");
			}

			sb->safePrintf ( "</font><br>\n" );
		}
		if ( t != TYPE_STRINGBOX ) {
			// this td will be invisible if isCrawlbot and the
			// parm is too advanced to display
			sb->safePrintf ( "<td " );
			if ( m->m_colspan > 0 )
				sb->safePrintf ( "colspan=%"INT32" ",
						 (int32_t)m->m_colspan);
			sb->safePrintf ( "width=%"INT32"%%>"//"<td width=78%%>
					 "<b>%s</b><br><font size=1>",
					 3*100/nc/2/4, m->m_title );

			// the "site list" parm has html in description
			if ( pd ) {
				status &= sb->safeStrcpy(m->m_desc);
				//status &= sb->htmlEncode (m->m_desc,
				//			  gbstrlen(m->m_desc),
				//			  false);
				// is it required?
				if ( m->m_flags & PF_REQUIRED )
					sb->safePrintf(" <b><font color=green>"
						       "REQUIRED</font></b>");

				// print users current ip if showing the list
				// of "Master IPs" for admin access
				if ( ( m->m_page == PAGE_MASTERPASSWORDS ||
				       m->m_page == PAGE_COLLPASSWORDS ) &&
				     sock &&
				     m->m_title &&
				     strstr(m->m_title,"IP") )
					sb->safePrintf(" <b>Your current IP "
						       "is %s.</b>",
						       iptoa(sock->m_ip));
			}

			// and cgi parm if it exists
			//if ( m->m_def && m->m_scgi )
			//	sb->safePrintf(" CGI override: %s.",m->m_scgi);
			// just let them see the api page for this...
			//sb->safePrintf(" CGI: %s.",m->m_cgi);
			// and default value if it exists
			if ( m->m_def && m->m_def[0] && t != TYPE_CMD ) {
				char *d = m->m_def;
				if ( t == TYPE_BOOL || t == TYPE_CHECKBOX ) {
					if ( d[0]=='0' ) d = "NO";
					else             d = "YES";
					sb->safePrintf ( " <nobr>"
							 "Default: %s."
							 "</nobr>",d);
				} 
				else {
					sb->safePrintf (" Default: ");
					status &= sb->htmlEncode (d,
								  gbstrlen(d),
								  false);
				}
			}
			sb->safePrintf ( "</font></td>\n<td%s width=%"INT32"%%>" , 
				  color , 100/nc/2/4 );
		}
	}

	// . print number in row if array, start at 1 for clarity's sake
	// . used for url filters table, etc.
	if ( m->m_max > 1 ) {
		// bg color alternates
		char *bgc = LIGHT_BLUE;
		if ( j % 2 ) bgc = DARK_BLUE;
		// do not print this if doing json
		//if ( format != FORMAT_HTML );//isJSON ) ;
		// but if it is in same row as previous, do not repeat it
		// for this same row, silly
		if ( firstInRow ) // && m->m_page != PAGE_PRIORITIES ) 
			sb->safePrintf ( "<tr bgcolor=#%s>"
					 "<td>%"INT32"</td>\n<td>", 
					 bgc,
					 j );//j+1
		else if ( firstInRow ) 
			sb->safePrintf ( "<tr><td>" );
		else    
			//sb->safePrintf ( "<td%s>" , vt);
			sb->safePrintf ( "<td>" );
	}

	//int32_t cast = m->m_cast;
	//if ( g_proxy.isProxy() ) cast = 0;

	// print the input box
	if ( t == TYPE_BOOL ) {
		char *tt, *v;
		if ( *s ) { tt = "YES"; v = "0"; }
		else      { tt = "NO" ; v = "1"; }
		if ( g_conf.m_readOnlyMode && m->m_rdonly )
			sb->safePrintf ( "<b>read-only mode</b>" );
		// if cast=1, command IS broadcast to all hosts
		else 
			sb->safePrintf ( "<b><a href=\"/%s?c=%s&"
					 "%s=%s\">" // &cast=%"INT32"\">"
					 "<center>%s</center></a></b>", 
					 g_pages.getPath(m->m_page),coll,
					 cgi,v,//cast,
					 tt);
	}
	else if ( t == TYPE_BOOL2 ) {
		if ( g_conf.m_readOnlyMode && m->m_rdonly )
			sb->safePrintf ( "<b><center>read-only mode"
					 "</center></b>");
		// always use m_def as the value for TYPE_BOOL2
		else
			sb->safePrintf ( "<b><a href=\"/%s?c=%s&%s=%s\">"
					 //"cast=1\">"
					 "<center>%s</center></a></b>", 
					 g_pages.getPath(m->m_page),coll,
					 cgi,m->m_def, m->m_title);
	}
	else if ( t == TYPE_CHECKBOX ) {
		//char *ddd1 = "";
		//char *ddd2 = "";
		//if ( *s ) ddd1 = " checked";
		//else      ddd2 = " checked";
		// just show the parm name and value if printing in json
		// if ( format == FORMAT_JSON ) { // isJSON ) {
		// 	if ( ! lastRow ) {
		// 		int32_t val = 0;
		// 		if ( *s ) val = 1;
		// 		sb->safePrintf("\"%s\":%"INT32",\n",cgi,val);
		// 	}
		// }
		//sb->safePrintf("<center><nobr>");
		sb->safePrintf("<nobr>");
		// this is part of the "HACK" fix below. you have to
		// specify the cgi parm in the POST request, and 
		// unchecked checkboxes are not included in the POST 
		// request.
		//if ( lastRow && m->m_page == PAGE_FILTERS ) 
		//	sb->safePrintf("<input type=hidden ");
		//char *val = "Y";
		//if ( ! *s ) val = "N";
		char *val = "";
		// "s" is invalid of parm has no "object"
		if ( m->m_obj == OBJ_NONE && m->m_def[0] != '0' )
			val = " checked";
		if ( m->m_obj != OBJ_NONE && s && *s ) 
			val = " checked";
		// s is NULL for GigablastRequest parms
		if ( ! s && m->m_def && m->m_def[0]=='1' )
			val = " checked";

		// in case it is not checked, submit that!
		// if it gets checked this should be overridden then
		// BR 20160205: Do not remove this. Otherwise checkboxes with
		// default value 1 does not work when you uncheck the box in the UI.
		sb->safePrintf("<input type=hidden name=%s value=0>", cgi );


		sb->safePrintf("<input type=checkbox value=1 ");
		//"<nobr><input type=button ");
		if ( m->m_page == PAGE_FILTERS)
			sb->safePrintf("id=id_%s ",cgi);
			
		sb->safePrintf("name=%s%s"
			       //" onmouseup=\""
			       //"if ( this.value=='N' ) {"
			       //"this.value='Y';"
			       //"} "
			       //"else if ( this.value=='Y' ) {"
			       //"this.value='N';"
			       //"}"
			       //"\" "
			       ">"
			       ,cgi
			       ,val);//,ddd);
		//
		// repeat for off position
		//
		//if ( ! lastRow || m->m_page != PAGE_FILTERS )  {
		//	sb->safePrintf(" Off:<input type=radio ");
		//	if ( m->m_page == PAGE_FILTERS)
		//		sb->safePrintf("id=id_%s ",cgi);
		//	sb->safePrintf("value=0 name=%s%s>",
		//		       cgi,ddd2);
		//}
		sb->safePrintf("</nobr>"
			       //"</center>"
			       );
	}
	else if ( t == TYPE_CHAR )
		sb->safePrintf ("<input type=text name=%s value=\"%"INT32"\" "
				"size=3>",cgi,(int32_t)(*s));
	/*	else if ( t == TYPE_CHAR2 )
		sprintf (p,"<input type=text name=%s value=\"%"INT32"\" "
		"size=3>",cgi,*(char*)s);*/
	else if ( t == TYPE_PRIORITY ) 
		printDropDown ( MAX_SPIDER_PRIORITIES , sb , cgi , *s , 
				false , false );
	else if ( t == TYPE_PRIORITY2 ) {
		// just show the parm name and value if printing in json
		// if ( format==FORMAT_JSON) // isJSON )
		// 	sb->safePrintf("\"%s\":%"INT32",\n",cgi,(int32_t)*(char *)s);
		// else
		printDropDown ( MAX_SPIDER_PRIORITIES , sb , cgi , *s ,
				true , true );
	}
	// this url filters parm is an array of SAFEBUFs now, so each is
	// a string and that string is the diffbot api url to use. 
	// the string is empty or zero length to indicate none.
	//else if ( t == TYPE_DIFFBOT_DROPDOWN ) {
	//	char *xx=NULL;*xx=0;
	//}
	//else if ( t == TYPE_UFP )
	else if ( t == TYPE_SAFEBUF && 
		  strcmp(m->m_title,"url filters profile")==0)
		// url filters profile drop down "ufp"
		printDropDownProfile ( sb , "ufp" , cr );//*s );

	// do not expose master passwords or IPs to non-root admins
	else if ( ( m->m_flags & PF_PRIVATE ) && 
		  m->m_obj == OBJ_CONF &&
		  ! isMasterAdmin )
		return true;

	// do not expose master passwords or IPs to non-root admins
	else if ( ( m->m_flags & PF_PRIVATE ) && 
		  m->m_obj == OBJ_COLL &&
		  ! isCollAdmin )
		return true;

	else if ( t == TYPE_RETRIES    ) 
		printDropDown ( 4 , sb , cgi , *s , false , false );
	else if ( t == TYPE_FILEUPLOADBUTTON    ) {
		sb->safePrintf("<input type=file name=%s>",cgi);
	}
	else if ( t == TYPE_PRIORITY_BOXES ) {
		// print ALL the checkboxes when we get the first parm
		if ( j != 0 ) return status;
		printCheckBoxes ( MAX_SPIDER_PRIORITIES , sb , cgi , s );
	}
	else if ( t == TYPE_CMD )
		// if cast=0 it will be executed, otherwise it will be
		// broadcasted with cast=1 to all hosts and they will all
		// execute it
		sb->safePrintf ( "<b><a href=\"/%s?c=%s&%s=1\">" // cast=%"INT32"
			  "<center>%s</center></a></b>",
			  g_pages.getPath(m->m_page),coll,
			  cgi,m->m_title);
	else if ( t == TYPE_FLOAT ) {
		// just show the parm name and value if printing in json
		// if ( format == FORMAT_JSON )//isJSON )
		// 	sb->safePrintf("\"%s\":%f,\n",cgi,*(float *)s);
		// else
		sb->safePrintf ("<input type=text name=%s "
				"value=\"%f\" "
				// 3 was ok on firefox but need 6
				// on chrome
				"size=7>",cgi,*(float *)s);
	}
	else if ( t == TYPE_IP ) {
		if ( m->m_max > 0 && j == jend ) 
			sb->safePrintf ("<input type=text name=%s value=\"\" "
					"size=12>",cgi);
		else
			sb->safePrintf ("<input type=text name=%s value=\"%s\" "
					"size=12>",cgi,iptoa(*(int32_t *)s));
	}
	else if ( t == TYPE_LONG ) {
		// just show the parm name and value if printing in json
		// if ( format == FORMAT_JSON ) // isJSON )
		// 	sb->safePrintf("\"%s\":%"INT32",\n",cgi,*(int32_t *)s);
		// else
		sb->safePrintf ("<input type=text name=%s "
				"value=\"%"INT32"\" "
				// 3 was ok on firefox but need 6
				// on chrome
				"size=6>",cgi,*(int32_t *)s);
	}
	else if ( t == TYPE_LONG_CONST ) 
		sb->safePrintf ("%"INT32"",*(int32_t *)s);
	else if ( t == TYPE_LONG_LONG )
		sb->safePrintf ("<input type=text name=%s value=\"%"INT64"\" "
				"size=12>",cgi,*(int64_t *)s);
	else if ( t == TYPE_STRING || t == TYPE_STRINGNONEMPTY ) {
		int32_t size = m->m_size;
		// give regular expression box on url filters page more room
		//if ( m->m_page == PAGE_FILTERS ) {
		//	if ( size > REGEX_TXT_MAX ) size = REGEX_TXT_MAX;
		//}
		//else {
		if ( size > 20 ) size = 20;
		//}
		sb->safePrintf ("<input type=text name=%s size=%"INT32" value=\"",
				cgi,size);

		// if it has PF_DEFAULTCOLL flag set then use the coll
		if ( cr && (m->m_flags & PF_COLLDEFAULT) )
			sb->safePrintf("%s",cr->m_coll);
		else
			sb->dequote ( s , gbstrlen(s) );

		sb->safePrintf ("\">");
	}
	else if ( t == TYPE_CHARPTR ) {
		int32_t size = m->m_size;
		char *sp = NULL;
		if ( s && *s ) sp = *(char **)s;
		if ( ! sp ) sp = "";
		if ( m->m_flags & PF_TEXTAREA ) {
			sb->safePrintf ("<textarea name=%s rows=10 cols=80>",
					cgi);
			if ( m->m_obj != OBJ_NONE )
				sb->htmlEncode(sp,gbstrlen(sp),false);
			sb->safePrintf ("</textarea>");
		}
		else {
			sb->safePrintf ("<input type=text name=%s size=%"INT32" "
					"value=\"",cgi,size);
			// if it has PF_DEFAULTCOLL flag set then use the coll
			if ( cr && (m->m_flags & PF_COLLDEFAULT) )
				sb->safePrintf("%s",cr->m_coll);
			else if ( sp )
				sb->dequote ( sp , gbstrlen(sp) );
			sb->safePrintf ("\">");
		}
	}
	else if ( t == TYPE_SAFEBUF ) {
		int32_t size = m->m_size;
		// give regular expression box on url filters page more room
		if ( m->m_page == PAGE_FILTERS ) {
			//if ( size > REGEX_TXT_MAX ) size = REGEX_TXT_MAX;
			size = 40;
		}
		else {
			if ( size > 20 ) size = 20;
		}
		SafeBuf *sx = (SafeBuf *)s;

		SafeBuf tmp;
		// if printing a parm in a one-shot deal like GigablastRequest
		// then s and sx will always be NULL, so set to default
		if ( ! sx ) {
			sx = &tmp;
			char *def = m->m_def;
			// if it has PF_DEFAULTCOLL flag set then use the coll
			if ( cr && (m->m_flags & PF_COLLDEFAULT) ) 
				def = cr->m_coll;
			tmp.safePrintf("%s",def);
		}

		// just show the parm name and value if printing in json
		// if ( format == FORMAT_JSON ) { // isJSON ) {
		// 	// this can be empty for the empty row i guess
		// 	if ( sx->length() ) {
		// 		// convert diffbot # to string
		// 		sb->safePrintf("\"%s\":\"",cgi);
		// 		if ( m->m_obj != OBJ_NONE )
		// 			sb->safeUtf8ToJSON (sx->getBufStart());
		// 		sb->safePrintf("\",\n");
		// 	}
		// }
		if ( m->m_flags & PF_TEXTAREA ) {
			int rows = 10;
			if ( m->m_flags & PF_SMALLTEXTAREA )
				rows = 4;
			sb->safePrintf ("<textarea id=tabox "
					"name=%s rows=%i cols=80>",
					cgi,rows);
			//sb->dequote ( s , gbstrlen(s) );
			// note it
			//log("hack: %s",sx->getBufStart());
			//sb->dequote ( sx->getBufStart() , sx->length() );
			if ( m->m_obj != OBJ_NONE )
				sb->htmlEncode(sx->getBufStart(),
					       sx->length(),false);
			sb->safePrintf ("</textarea>");
		}
		else {
			sb->safePrintf ("<input type=text name=%s size=%"INT32" "
					"value=\"",
					cgi,size);
			//sb->dequote ( s , gbstrlen(s) );
			// note it
			//log("hack: %s",sx->getBufStart());


			if ( cr && 
			     (m->m_flags & PF_COLLDEFAULT) &&
			     sx &&
			     sx->length() <= 0 ) 
				sb->dequote ( cr->m_coll,gbstrlen(cr->m_coll));

			// if parm is OBJ_NONE there is no stored valued
			else if ( m->m_obj != OBJ_NONE )
				sb->dequote ( sx->getBufStart(), sx->length());

			sb->safePrintf ("\">");
		}
	}
	else if ( t == TYPE_STRINGBOX ) {
		sb->safePrintf("<textarea id=tabox rows=10 cols=64 name=%s>",
			       cgi);
		//p += urlEncode ( p , pend - p , s , gbstrlen(s) );
		//p += htmlDecode ( p , s , gbstrlen(s) );
		sb->htmlEncode ( s , gbstrlen(s), false );
		//sprintf ( p , "%s" , s );
		//p += gbstrlen(p);		
		sb->safePrintf ("</textarea>\n");
	}
	else if ( t == TYPE_CONSTANT ) 
		sb->safePrintf ("%s",m->m_title);
	else if ( t == TYPE_MONOD2 )
		sb->safePrintf ("%"INT32"",j / 2 );
	else if ( t == TYPE_MONOM2 ) {
		/*
		if ( m->m_page == PAGE_PRIORITIES ) {
			if ( j % 2 == 0 ) sb->safePrintf ("old");
			else              sb->safePrintf ("new");
		}
		else
		*/
			sb->safePrintf ("%"INT32"",j % 2 );
	}
	else if ( t == TYPE_RULESET ) ;
		// subscript is already included in "cgi"
		//g_pages.printRulesetDropDown ( sb         ,
		//			       user       ,
		//			       cgi        ,
		//			       *(int32_t *)s ,  // selected  
		//			       -1         ); // subscript
	else if ( t == TYPE_TIME ) {
		//time is stored as a string
		//if time is not stored properly, just write 00:00
		if ( s[2] != ':' )
			strncpy ( s, "00:00", 5 );
		char hr[3];
		char min[3];
		gbmemcpy ( hr, s, 2 );
		gbmemcpy ( min, s + 3, 2 );
		hr[2] = '\0';
		min[2] = '\0';
		// print the time in the input forms
		sb->safePrintf("<input type=text name=%shr size=2 "
			       "value=%s>h " 
			       "<input type=text name=%smin size=2 "
			       "value=%s>m " ,
			       cgi    , 
			       hr ,
			       cgi    , 
			       min  );
	}

	else if ( t == TYPE_DATE || t == TYPE_DATE2 ) {
		// time is stored as int32_t
		int32_t ct = *(int32_t *)s;
		// get the time struct
		struct tm *tp = gmtime ( (time_t *)&ct ) ;
		// set the "selected" month for the drop down
		char *ss[12];
		for ( int32_t i = 0 ; i < 12 ; i++ ) ss[i]="";
		int32_t month = tp->tm_mon;
		if ( month < 0 || month > 11 ) month = 0; // Jan
		ss[month] = " selected";
		// print the date in the input forms
		sb->safePrintf(
			"<input type=text name=%sday "
			"size=2 value=%"INT32"> "
			"<select name=%smon>"
			"<option value=0%s>Jan"
			"<option value=1%s>Feb"
			"<option value=2%s>Mar"
			"<option value=3%s>Apr"
			"<option value=4%s>May"
			"<option value=5%s>Jun"
			"<option value=6%s>Jul"
			"<option value=7%s>Aug"
			"<option value=8%s>Sep"
			"<option value=9%s>Oct"
			"<option value=10%s>Nov"
			"<option value=11%s>Dec"
			"</select>\n"
			"<input type=text name=%syr size=4 value=%"INT32">"
			"<br>"
			"<input type=text name=%shr size=2 "
			"value=%02"INT32">h " 
			"<input type=text name=%smin size=2 "
			"value=%02"INT32">m " 
			"<input type=text name=%ssec size=2 "
			"value=%02"INT32">s" ,
			cgi    ,
			(int32_t)tp->tm_mday ,
			cgi    ,
			ss[0],ss[1],ss[2],ss[3],ss[4],ss[5],ss[6],ss[7],ss[8],
			ss[9],ss[10],ss[11],
			cgi    ,
			(int32_t)tp->tm_year + 1900 ,
			cgi    , 
			(int32_t)tp->tm_hour ,
			cgi    , 
			(int32_t)tp->tm_min  ,
			cgi    ,
			(int32_t)tp->tm_sec  );
		/*
		if ( t == TYPE_DATE2 ) {
			p += gbstrlen ( p );
			// a int32_t after the int32_t is used for this
			int32_t ct = *(int32_t *)(THIS+m->m_off+4);
			char *ss = "";
			if ( ct ) ss = " checked";
			sprintf ( p , "<br><input type=checkbox "
				  "name=%sct value=1%s> use current "
				  "time\n",cgi,ss);
		}
		*/
	}
	else if ( t == TYPE_SITERULE ) {
		// print the siterec rules as a drop down
		char *ss[5];
		for ( int32_t i = 0; i < 5; i++ ) ss[i] = "";
		int32_t v = *(int32_t*)s;
		if ( v < 0 || v > 4 ) v = 0;
		ss[v] = " selected";
		sb->safePrintf ( "<select name=%s>"
				 "<option value=0%s>Hostname"
				 "<option value=1%s>Path Depth 1"
				 "<option value=2%s>Path Depth 2"
				 "<option value=3%s>Path Depth 3"
				 "</select>\n",
				 cgi, ss[0], ss[1], ss[2], ss[3] );
	}


	// end the input cell
	sb->safePrintf ( "</td>\n");

	// "insert above" link? used for arrays only, where order matters
	if ( m->m_addin && j < jend ) {//! isJSON ) {
		sb->safePrintf ( "<td><a href=\"?c=%s&" // cast=1&"
				 //"ins_%s=1\">insert</td>\n",coll,cgi );
				 // insert=<rowNum>
				 // "j" is the row #
				 "insert=%"INT32"\">insert</td>\n",coll,j );
	}

	// does next guy start a new row?
	bool lastInRow = true; // assume yes
	if (mm+1<m_numParms&&m->m_rowid>=0&&m_parms[mm+1].m_rowid==m->m_rowid)
		lastInRow = false;
	if ( ((s_count-1) % nc) != (nc-1) ) lastInRow = false;

	// . display the remove link for arrays if we need to
	// . but don't display if next guy does NOT start a new row
	//if ( m->m_max > 1 && lastInRow && ! isJSON ) {
	if ( m->m_addin && j < jend ) { //! isJSON ) {
	//     m->m_page != PAGE_PRIORITIES ) {
		// show remove link?
		bool show = true;
		//if ( j >= jend )  show = false;
		// get # of rows
		int32_t *nr = (int32_t *)((char *)THIS + m->m_off - 4);
		// are we the last row?
		bool lastRow = false;
		// yes, if this is true
		if ( j == *nr - 1 ) lastRow = true;
		// do not allow removal of last default url filters rule
		//if ( lastRow && !strcmp(m->m_cgi,"fsp")) show = false;
		char *suffix = "";
		if ( m->m_page == PAGE_MASTERPASSWORDS &&
		     m->m_type == TYPE_IP ) 
			suffix = "ip";
		if ( m->m_page == PAGE_MASTERPASSWORDS &&
		     m->m_type == TYPE_STRINGNONEMPTY ) 
			suffix = "pwd";
		if ( show )
			sb->safePrintf ("<td><a href=\"?c=%s&" // cast=1&"
					//"rm_%s=1\">"
					// remove=<rownum>
					"remove%s=%"INT32"\">"
					"remove</a></td>\n",coll,//cgi );
					suffix,
					j); // j is row #
					
		else
			sb->safePrintf ( "<td></td>\n");
	}

	if ( lastInRow ) sb->safePrintf ("</tr>\n");
	return status;
}

// now we use this to set SearchInput and GigablastRequest
bool Parms::setFromRequest ( HttpRequest *r , 
			     TcpSocket* s,
			     CollectionRec *newcr ,
			     char *THIS ,
			     int32_t objType ) {

	// get the page from the path... like /sockets --> PAGE_SOCKETS
	//int32_t page = g_pages.getDynamicPageNumber ( r );

	// use convertHttpRequestToParmList() for these because they
	// are persistent records that are updated on every shard.
	if ( objType == OBJ_COLL ) { char *xx=NULL;*xx=0; }
	if ( objType == OBJ_CONF ) { char *xx=NULL;*xx=0; }

	// ensure valid
	if ( ! THIS ) {
		// it is null when no collection explicitly specified...
		log(LOG_LOGIC,"admin: THIS is null for setFromRequest");
		char *xx=NULL;*xx=0; 
	}

	// loop through cgi parms
	for ( int32_t i = 0 ; i < r->getNumFields() ; i++ ) {
		// get cgi parm name
		char *field = r->getField    ( i );
		// find in parms list
		int32_t  j;
		Parm *m;
		for ( j = 0 ; j < m_numParms ; j++ ) {
			// get it
			m = &m_parms[j];
			// skip if not our type
			if ( m->m_obj != objType ) continue;
			// skip if offset is negative, that means none
			if ( m->m_off < 0 ) continue;
			// skip if no cgi parm, may not be configurable now
			if ( ! m->m_cgi ) continue;
			// otherwise, must match the cgi name exactly
			if ( strcmp ( field,m->m_cgi ) == 0 ) break;
		}
		// bail if the cgi field is not in the parms list
		if ( j >= m_numParms ) continue;
		// get the value of cgi parm (null terminated)
		char *v = r->getValue ( i );
		// empty?
		if ( ! v ) continue;
		// . skip if no value was provided
		// . unless it was a string! so we can make them empty.
		if ( v[0] == '\0' && 
		     m->m_type != TYPE_STRING && 
		     m->m_type != TYPE_STRINGBOX ) continue;
		// set it
		setParm ( (char *)THIS , m, j, 0, v, false,//not html enc
			  false );//true );
	}

	return true;
}


bool Parms::insertParm ( int32_t i , int32_t an ,  char *THIS ) {
	Parm *m = &m_parms[i];
	// . shift everyone above down
	// . first int32_t at offset is always the count
	//   for arrays
	char *pos =  (char *)THIS + m->m_off ;
	int32_t  num = *(int32_t *)(pos - 4);
	// ensure we are valid
	if ( an >= num || an < 0 ) {
		log("admin: Invalid insertion of element "
		    "%"INT32" in array of size %"INT32" for \"%s\".",
		    an,num,m->m_title);
		return false;
	}
	// also ensure that we have space to put the parm in, because in
	// case of URl filters, it is bounded by MAX_FILTERS
	if ( num >= MAX_FILTERS ){
		log("admin: Invalid insert of element %"INT32", array is full "
		    "in size %"INT32" for \"%s\".",an, num, m->m_title);
		return false;
	}
	// point to the place where the element is to be inserted
	char *src = pos + m->m_size * an;

	//point to where it is to be moved
	char *dst = pos + m->m_size * ( an + 1 );

	// how much to move
	int32_t size = ( num - an ) * m->m_size ;
	// move them
	memmove ( dst , src , size );
	// if the src was a TYPE_SAFEBUF clear it so we don't end up doing
	// a double free, etc.!
	memset ( src , 0 , m->m_size );
	// inc the count
	*(int32_t *)(pos-4) = (*(int32_t *)(pos-4)) + 1;
	// put the defaults in the inserted line
	setParm ( (char *)THIS , m , i , an , m->m_def , false ,false );
	return true;
}

bool Parms::removeParm ( int32_t i , int32_t an , char *THIS ) {
	Parm *m = &m_parms[i];
	// . shift everyone above down
	// . first int32_t at offset is always the count
	//   for arrays
	char *pos =  (char *)THIS + m->m_off ;
	int32_t  num = *(int32_t *)(pos - 4);
	// ensure we are valid
	if ( an >= num || an < 0 ) {
		log("admin: Invalid removal of element "
		    "%"INT32" in array of size %"INT32" for \"%s\".",
		    an,num,m->m_title);
		return false;
	}
	// point to the element being removed
	char *dst = pos + m->m_size * an;
	// free memory pointed to by safebuf, if we are safebuf, before
	// overwriting it... prevents a memory leak
	if ( m->m_type == TYPE_SAFEBUF ) {
		SafeBuf *dx = (SafeBuf *)dst;
		dx->purge();
	}
	// then point to the good stuf
	char *src = pos + m->m_size * (an+1);
	// how much to bury it with
	int32_t size = (num - an - 1 ) * m->m_size ;
	// bury it
	gbmemcpy ( dst , src , size );

	// and detach the buf on the tail so it doesn't core in Mem.cpp
	// when it tries to free...
	if ( m->m_type == TYPE_SAFEBUF ) {
		SafeBuf *tail = (SafeBuf *)(pos + m->m_size * (num-1));
		tail->detachBuf();
	}

	// dec the count
	*(int32_t *)(pos-4) = (*(int32_t *)(pos-4)) - 1;
	return true;
}

void Parms::setParm ( char *THIS , Parm *m , int32_t mm , int32_t j , char *s ,
		      bool isHtmlEncoded , bool fromRequest ) {

	if ( fromRequest ) { char *xx=NULL;*xx=0; }

	// . this is just for setting CollectionRecs, so skip if offset < 0
	// . some parms are just for SearchInput (search parms)
	if ( m->m_off < 0 ) return;

	if ( m->m_obj == OBJ_NONE ) return ;

	float oldVal = 0;
	float newVal = 0;

	if ( ! s && 
	     m->m_type != TYPE_CHARPTR && 
	     m->m_type != TYPE_FILEUPLOADBUTTON && 
	     m->m_defOff==-1) {
		s = "0";
		char *tit = m->m_title;
		if ( ! tit || ! tit[0] ) tit = m->m_xml;
		log(LOG_LOGIC,"admin: Parm \"%s\" had NULL default value. "
		    "Forcing to 0.",
		    tit);
		//char *xx = NULL; *xx = 0;
	}

	// sanity check
	if ( &m_parms[mm] != m ) {
		log(LOG_LOGIC,"admin: Not sane parameters.");
		char *xx = NULL; *xx = 0;
	}

	// if attempting to add beyond array max, bail out
	if ( j >= m->m_max && j >= m->m_fixed ) {
		log ( "admin: Attempted to set parm beyond limit. Aborting." );
		return;
	}

	// ensure array count at least j+1
	if ( m->m_max > 1 ) {
		// . is this element we're adding bumping up the count?
		// . array count is 4 bytes before the array
		char *pos =  (char *)THIS + m->m_off - 4 ;
		// set the count to it if it is bigger than current count
		if ( j + 1 > *(int32_t *)pos ) *(int32_t *)pos = j + 1;
	}

	char  t   = m->m_type;

	if      ( t == TYPE_CHAR           || 
		  t == TYPE_CHAR2          || 
		  t == TYPE_CHECKBOX       ||
		  t == TYPE_BOOL           ||
		  t == TYPE_BOOL2          ||
		  t == TYPE_PRIORITY       || 
		  t == TYPE_PRIORITY2      || 
		  //t == TYPE_DIFFBOT_DROPDOWN ||
		  t == TYPE_UFP            ||
		  t == TYPE_PRIORITY_BOXES || 
		  t == TYPE_RETRIES        ||
		  t == TYPE_FILTER           ) {
		if ( fromRequest && *(char *)(THIS + m->m_off + j) == atol(s))
			return;
		if ( fromRequest)oldVal = (float)*(char *)(THIS + m->m_off +j);
		*(char *)(THIS + m->m_off + j) = atol ( s );
 		newVal = (float)*(char *)(THIS + m->m_off + j);
		goto changed; }
	else if ( t == TYPE_CHARPTR ) {
		// "s" might be NULL or m->m_def...
		*(char **)(THIS + m->m_off + j) = s;
	}
	else if ( t == 	TYPE_FILEUPLOADBUTTON ) {
		// "s" might be NULL or m->m_def...
		*(char **)(THIS + m->m_off + j) = s;
	}
	else if ( t == TYPE_CMD ) {
		log(LOG_LOGIC, "conf: Parms: TYPE_CMD is not a cgi var.");
		return;	}
	else if ( t == TYPE_DATE2 || t == TYPE_DATE ) {
		int32_t v = (int32_t)atotime ( s ); 
		if ( fromRequest && *(int32_t *)(THIS + m->m_off + 4*j) == v ) 
			return;
		*(int32_t *)(THIS + m->m_off + 4*j) = v;
		if ( v < 0 ) log("conf: Date for <%s> of \""
				 "%s\" is not in proper format like: "
				 "01 Jan 1980 22:45",m->m_xml,s);
		goto changed; }
	else if ( t == TYPE_FLOAT ) {
		if( fromRequest &&
		    *(float *)(THIS + m->m_off + 4*j) == (float)atof ( s ) ) 
			return;
		// if changed within .00001 that is ok too, do not count
		// as changed, the atof() has roundoff errors
		//float curVal = *(float *)(THIS + m->m_off + 4*j);
		//float newVal = atof(s);
		//if ( newVal < curVal && newVal + .000001 >= curVal ) return;
		//if ( newVal > curVal && newVal - .000001 <= curVal ) return;
		if ( fromRequest ) oldVal = *(float *)(THIS + m->m_off + 4*j);
		*(float *)(THIS + m->m_off + 4*j) = (float)atof ( s );
		newVal = *(float *)(THIS + m->m_off + 4*j);
		goto changed; }
	else if ( t == TYPE_DOUBLE ) {
		if( fromRequest &&
		    *(double *)(THIS + m->m_off + 4*j) == (double)atof ( s ) ) 
			return;
		if ( fromRequest ) oldVal = *(double *)(THIS + m->m_off + 4*j);
		*(double *)(THIS + m->m_off + 4*j) = (double)atof ( s );
		newVal = *(double *)(THIS + m->m_off + 4*j);
		goto changed; }
	else if ( t == TYPE_IP ) {
		if ( fromRequest && *(int32_t *)(THIS + m->m_off + 4*j) == 
		     (int32_t)atoip (s,gbstrlen(s) ) ) 
			return;
		*(int32_t *)(THIS + m->m_off + 4*j) = (int32_t)atoip (s,gbstrlen(s) ); 
		goto changed; }
	else if ( t == TYPE_LONG || t == TYPE_LONG_CONST || t == TYPE_RULESET||
		  t == TYPE_SITERULE ) {
		int32_t v = atol ( s );
		// min is considered valid if >= 0
		if ( m->m_min >= 0 && v < m->m_min ) v = m->m_min;
		if ( fromRequest && *(int32_t *)(THIS + m->m_off + 4*j) == v ) 
			return;
		if ( fromRequest)oldVal=(float)*(int32_t *)(THIS + m->m_off +4*j);
		*(int32_t *)(THIS + m->m_off + 4*j) = v;
		newVal = (float)*(int32_t *)(THIS + m->m_off + 4*j);
		goto changed; }
	else if ( t == TYPE_LONG_LONG ) {
		if ( fromRequest &&
		     *(uint64_t *)(THIS + m->m_off+8*j)==
		     strtoull(s,NULL,10))
			return;
		*(int64_t *)(THIS + m->m_off + 8*j) = strtoull(s,NULL,10);
		goto changed; }
	// like TYPE_STRING but dynamically allocates
	else if ( t == TYPE_SAFEBUF ) {
		int32_t len = gbstrlen(s);
		// no need to truncate since safebuf is dynamic
		//if ( len >= m->m_size ) len = m->m_size - 1; // truncate!!
		//char *dst = THIS + m->m_off + m->m_size*j ;
		// point to the safebuf, in the case of an array of
		// SafeBufs "j" is the # in the array, starting at 0
		SafeBuf *sb = (SafeBuf *)(THIS+m->m_off+(j*sizeof(SafeBuf)) );
		int32_t oldLen = sb->length();
		// why was this commented out??? we need it now that we
		// send email alerts when parms change!
		if ( fromRequest &&
		     ! isHtmlEncoded && oldLen == len &&
		     memcmp ( sb->getBufStart() , s , len ) == 0 ) 
			return;
		// nuke it
		sb->purge();
		// this means that we can not use string POINTERS as parms!!
		if ( ! isHtmlEncoded ) sb->safeMemcpy ( s , len ); 
		else                   len = sb->htmlDecode (s,len,false,0);
		// tag it
		sb->setLabel ( "parm1" );
		// ensure null terminated
		sb->nullTerm();
		// note it
		//log("hack: %s",s);

		// null term it all
		//dst[len] = '\0';
		//sb->reserve ( 1 );
		// null terminate but do not include as m_length so the
		// memcmp() above still works right
		//sb->m_buf[sb->m_length] = '\0';
		// . might have to set length
		// . used for CollectionRec::m_htmlHeadLen and m_htmlTailLen
		//if ( m->m_plen >= 0 ) 
		//	*(int32_t *)(THIS + m->m_plen) = len ;
		goto changed; 
	}
	else if ( t == TYPE_STRING         || 
		  t == TYPE_STRINGBOX      || 
		  t == TYPE_STRINGNONEMPTY ||
		  t == TYPE_TIME            ) {
		int32_t len = gbstrlen(s);
		if ( len >= m->m_size ) len = m->m_size - 1; // truncate!!
		char *dst = THIS + m->m_off + m->m_size*j ;
		// why was this commented out??? we need it now that we
		// send email alerts when parms change!
		if ( fromRequest &&
		     ! isHtmlEncoded && (int32_t)gbstrlen(dst) == len &&
		     memcmp ( dst , s , len ) == 0 ) {
			return;
		}

		// this means that we can not use string POINTERS as parms!!
		if ( !isHtmlEncoded ) {
			gbmemcpy( dst, s, len );
		} else {
			len = htmlDecode( dst, s, len, false, 0 );
		}

		dst[len] = '\0';
		// . might have to set length
		// . used for CollectionRec::m_htmlHeadLen and m_htmlTailLen
		if ( m->m_plen >= 0 ) 
			*(int32_t *)(THIS + m->m_plen) = len ;
		goto changed; }
 changed:
	// tell gigablast the value is EXPLICITLY given -- no longer based
	// on default.conf
	//if ( m->m_obj == OBJ_COLL ) ((CollectionRec *)THIS)->m_orig[mm] = 2;

	// we do not recognize timezones corectly when this is serialized
	// into coll.conf, it says UTC, which is ignored in HttpMime.cpp's
	// atotime() function. and when we submit it i think we use the
	// local time zone, so the values end up changing every time we 
	// submit!!! i think it might read it in as UTC then write it out
	// as local time, or vice versa.
	if ( t == TYPE_DATE || t == TYPE_DATE2 ) return;

	// do not send if setting from startup
	if ( ! fromRequest ) return;

	// note it in the log
	log("admin: parm \"%s\" changed value",m->m_title);

	int64_t nowms = gettimeofdayInMillisecondsLocal();

	// . note it in statsdb
	// . record what parm change and from/to what value
	g_statsdb.addStat ( 0, // niceness ,
			    "parm_change" ,
			    nowms,
			    nowms,
			    0         , // value
			    m->m_hash , // parmHash
			    oldVal,
			    newVal);

	// if they turn spiders on or off then tell spiderloop to update
	// the active list
	//if ( strcmp(m->m_cgi,"cse") )
	//	g_spiderLoop.m_activeListValid = false;

	// only send email alerts if we are host 0 since everyone syncs up
	// with host #0 anyway
	if ( g_hostdb.m_hostId != 0 ) return;

	// send an email alert notifying the admins that this parm was changed
	// BUT ALWAYS send it if email alerts were just TURNED OFF 
	// ("sea" = Send Email Alerts)
	if ( ! g_conf.m_sendEmailAlerts && strcmp(m->m_cgi,"sea") != 0 )
		return;

	// if spiders we turned on, do not send an email alert, cuz we
	// turn them on when we restart the cluster
	if ( strcmp(m->m_cgi,"se")==0 && g_conf.m_spideringEnabled )
		return;


	char tmp[1024];
	Host *h0 = g_hostdb.getHost ( 0 );
	int32_t ip0 = 0;
	if ( h0 ) ip0 = h0->m_ip;
	sprintf(tmp,"%s: parm \"%s\" changed value",iptoa(ip0),m->m_title);
	g_pingServer.sendEmail ( NULL  , // Host ptr
				 tmp   , // msg
				 true  , // sendToAdmin
				 false , // oom?
				 false , // kernel error?
				 true  , // parm change?
				 true  );// force it? even if disabled?

	// now the spider collection can just check the collection rec
	//int64_t nowms = gettimeofdayInMilliseconds();
	//((CollectionRec *)THIS)->m_lastUpdateTime = nowms;

	return;
}

Parm *Parms::getParmFromParmHash ( int32_t parmHash ) {
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		Parm *m = &m_parms[i];
		if ( m->m_hash != parmHash ) continue;
		return m;
	}
	return NULL;
}


void Parms::setToDefault ( char *THIS , char objType , CollectionRec *argcr ) {
	// init if we should
	init();

	// . clear out any coll rec to get the diffbotApiNum dropdowns
	// . this is a backwards-compatibility hack since this new parm
	//   will not be in old coll.conf files and will not be properly
	//   initialize when displaying a url filter row.
	//if ( THIS != (char *)&g_conf ) {
	//	CollectionRec *cr = (CollectionRec *)THIS;
	//	memset ( cr->m_spiderDiffbotApiNum , 0 , MAX_FILTERS);
	//}

	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		Parm *m = &m_parms[i];
		if ( m->m_obj != objType ) continue;
		if ( m->m_obj == OBJ_NONE ) continue;
		if ( m->m_type == TYPE_COMMENT ) continue;
		// no, we gotta set GigablastRequest::m_contentFile to NULL
		//if ( m->m_type == TYPE_FILEUPLOADBUTTON ) 
		//	continue;
		if ( m->m_type == TYPE_MONOD2  ) continue;
		if ( m->m_type == TYPE_MONOM2  ) continue;
		if ( m->m_type == TYPE_CMD     ) continue;
		if (THIS == (char *)&g_conf && m->m_obj != OBJ_CONF ) continue;
		if (THIS != (char *)&g_conf && m->m_obj == OBJ_CONF ) continue;
		// what is this?
		//if ( m->m_obj == OBJ_COLL ) {
		//	CollectionRec *cr = (CollectionRec *)THIS;
		//	if ( cr->m_bases[1] ) { char *xx=NULL;*xx=0; }
		//}
		// sanity check, make sure it does not overflow
		if ( m->m_obj == OBJ_COLL &&
		     m->m_off > (int32_t)sizeof(CollectionRec)){
			log(LOG_LOGIC,"admin: Parm in Parms.cpp should use "
			    "OBJ_COLL not OBJ_CONF");
			char *xx = NULL; *xx = 0;
		}
		//if ( m->m_page == PAGE_PRIORITIES )
		//	log("hey");
		// or 
		if ( m->m_page > PAGE_API && // CGIPARMS &&
		     m->m_page != PAGE_NONE &&
		     m->m_obj == OBJ_CONF ) {
			log(LOG_LOGIC,"admin: Page can not reference "
			    "g_conf and be declared AFTER PAGE_CGIPARMS in "
			    "Pages.h. Title=%s",m->m_title);
			char *xx = NULL; *xx = 0;
		}
		// if defOff >= 0 get from cr like for searchInput vals
		// whose default is from the collectionRec...
		if ( m->m_defOff >= 0 && argcr ) {
			if ( ! argcr ) { char *xx=NULL;*xx=0; }
			char *def = m->m_defOff+(char *)argcr;
			char *dst = (char *)THIS + m->m_off;
			gbmemcpy ( dst , def , m->m_size );
			continue;
		}
		// leave arrays empty, set everything else to default
		if ( m->m_max <= 1 ) {
			//if ( i == 282 )  // "query" parm
			//	log("hey");
			//if ( ! m->m_def ) { char *xx=NULL;*xx=0; }
			setParm ( THIS , m, i, 0, m->m_def, false/*not enc.*/,
				  false );
			//((CollectionRec *)THIS)->m_orig[i] = 1;
			//m->m_orig = 0; // set in setToDefaults()
		}
		// these are special, fixed size arrays
		if ( m->m_fixed > 0 ) {
			for ( int32_t k = 0 ; k < m->m_fixed ; k++ ) {
				setParm(THIS,m,i,k,m->m_def,false/*not enc.*/,
					false);
				//m->m_orig = 0; // set in setToDefaults()
				//((CollectionRec *)THIS)->m_orig[i] = 1;
			}
			continue;
		}
		// make array sizes 0
		if ( m->m_max <= 1 ) continue;
		// otherwise, array is not fixed size
		char *s = THIS + m->m_off ;
		// set count to 1 if a default is present
		//if (   m->m_def[0] ) *(int32_t *)(s-4) = 1;
		//else                 *(int32_t *)(s-4) = 0;
		*(int32_t *)(s-4) = 0;
	}
}

// . returns false and sets g_errno on error
// . you should set your "THIS" to its defaults before calling this
bool Parms::setFromFile ( void *THIS        , 
			  char *filename    , 
			  char *filenameDef ,
			  char  objType ) {
	// make sure we're init'd
	init();
	// let em know
	//if ( THIS == &g_conf) log (LOG_INIT,"conf: Reading %s." , filename );
	// . let the log know what we are doing
	// . filename is NULL if a call from CollectionRec::setToDefaults()
	Xml xml;
	//char buf [ MAX_XML_CONF ];
	SafeBuf sb;
	if ( filename&&!setXmlFromFile(&xml,filename,&sb)){//buf,MAX_XML_CONF))
		log("parms: error setting from file %s: %s",filename,
		    mstrerror(g_errno));
		return false;
	}

	int32_t  vlen;
	char *v ;
	//char  c ;
	int32_t numNodes  = xml.getNumNodes();
	int32_t numNodes2 = m_xml2.getNumNodes();

	// now set THIS based on the parameters in the xml file
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		// get it
		Parm *m = &m_parms[i];
		if ( m->m_obj != objType ) continue;
		if ( m->m_obj == OBJ_NONE ) continue;
		//log(LOG_DEBUG, "Parms: %s: parm: %s", filename, m->m_xml);
		// . there are 2 object types, coll recs and g_conf, aka
		//   OBJ_COLL and OBJ_CONF.
		// . make sure we got the right parms for what we want
		if ( THIS == &g_conf && m->m_obj != OBJ_CONF ) continue;
		if ( THIS != &g_conf && m->m_obj == OBJ_CONF ) continue;
		// skip comments and command
		if ( m->m_type == TYPE_COMMENT  ) continue;
		if ( m->m_type == TYPE_FILEUPLOADBUTTON ) continue;
		if ( m->m_type == TYPE_MONOD2   ) continue;
		if ( m->m_type == TYPE_MONOM2   ) continue;
		if ( m->m_type == TYPE_CMD      ) continue;
		if ( m->m_type == TYPE_CONSTANT ) continue;
		// these are special commands really
		if ( m->m_type == TYPE_BOOL2    ) continue;
		//if ( strcmp ( m->m_xml , "forceDeleteUrls" ) == 0 )
		//	log("got it");
		// we did not get one from first xml file yet
		bool first = true;
		// array count
		int32_t j = 0;
		// node number
		int32_t nn = 0;
		// a tmp thingy
		char tt[1];
		int32_t nb;
		int32_t newnn;
	loop:
		if ( m->m_obj == OBJ_NONE ) { char *xx=NULL;*xx=0; }
		// get xml node number of m->m_xml in the "xml" file
		newnn = xml.getNodeNum(nn,1000000,m->m_xml,gbstrlen(m->m_xml));

		// debug
		//log("%s --> %"INT32"",m->m_xml,nn);
		// try default xml file if none, but only if first try
		if ( newnn < 0 && first ) goto try2; 
		// it is valid, use it
		nn = newnn;
		// set the flag, we've committed the array to the first file
		first = false;
		// otherwise, we had some in this file, but now we're out
		if ( nn < 0 ) continue;
		// . next node is the value of this tag
		// . skip if none there
		if ( nn + 1 >= numNodes ) continue;
		// point to it
		v    = xml.getNode    ( nn + 1 );
		vlen = xml.getNodeLen ( nn + 1 );
		// if a back tag... set the value to the empty string
		if ( v[0] == '<' && v[1] == '/' ) vlen = 0;
		// now, extricate from the <![CDATA[ ... ]]> tag if we need to
		if ( m->m_type == TYPE_STRING         || 
		     m->m_type == TYPE_STRINGBOX      ||
		     m->m_type == TYPE_SAFEBUF        ||
		     m->m_type == TYPE_STRINGNONEMPTY   ) {
			char *oldv    = v;
			int32_t  oldvlen = vlen;
			// if next guy is NOT a tag node, try the next one
			if ( v[0] != '<' && nn + 2 < numNodes ) {
				v    = xml.getNode    ( nn + 2 );
				vlen = xml.getNodeLen ( nn + 2 );
			}
			// should be a <![CDATA[...]]>
			if ( vlen<12 || strncasecmp(v,"<![CDATA[",9)!=0 ) {
				log("conf: No <![CDATA[...]]> tag found "
				    "for \"<%s>\" tag. Trying without CDATA.",
				    m->m_xml);
				v    = oldv;
				vlen = oldvlen;
			}
			// point to the nugget
			else {
				v    += 9;
				vlen -= 12;
			}
		}
		// get the value
		//v = xml.getString ( nn , nn+2 , m->m_xml , &vlen );
		// this only happens when tag is there, but without a value
		if ( ! v || vlen == 0 ) { vlen = 0; v = tt; }
		//c = v[vlen];
		v[vlen]='\0';
		if ( vlen == 0 ){ 
			// . this is generally ok
			// . this is spamming the log so i am commenting out! (MDW)
			//log(LOG_INFO, "parms: %s: Empty value.", m->m_xml);
			// Allow an empty string
			//continue;
		}

		// now use proper cdata
		// we can't do this and be backwards compatible right now
		//nb = cdataDecode ( v , v , 0 );//, vlen , false ,0);

		// now decode it into itself
		nb = htmlDecode ( v , v , vlen , false ,0);
		v[nb] = '\0';

		// set our parm
		setParm( (char *)THIS, m, i, j, v, false, false );

		// we were set from the explicit file
		//((CollectionRec *)THIS)->m_orig[i] = 2;
		// go back
		//v[vlen] = c;
		// do not repeat same node
		nn++;
		// try to get the next node if we're an array
		if ( ++j < m->m_max || j < m->m_fixed ) { goto loop; }
		// otherwise, if not an array, go to next parm
		continue;
	try2:
		// get xml node number of m->m_xml in the "m_xml" file
		nn = m_xml2.getNodeNum(nn,1000000,m->m_xml,gbstrlen(m->m_xml));
		// otherwise, we had one in file, but now we're out
		if ( nn < 0 ) {
			// if it was ONLY a search input parm, with no
			// default value that can be changed in the 
			// CollectionRec then skip it
			// if ( m->m_soff  != -1 && 
			//      m->m_off   == -1 &&
			//      m->m_smaxc == -1 ) 
			// 	continue;
			// . if it is a string, like <adminIp> and default is 
			//   NULL then don't worry about reporting it
			// . no, just make the default "" then
			//if ( m->m_type==TYPE_STRING && ! m->m_def) continue;
			// bitch that it was not found
			//if ( ! m->m_def[0] ) 
			//	log("conf: %s does not have <%s> tag. "
			//	    "Ommitting.",filename,m->m_xml);
			//else 
			/*
			if ( ! m->m_def ) //m->m_def[0] )
				log("conf: %s does not have <%s> tag. Using "
				    "default value of \"%s\".", filename,
				    m->m_xml,m->m_def);
			*/
			continue;
		}
		// . next node is the value of this tag
		// . skip if none there
		if ( nn + 1 >= numNodes2 ) continue;
		// point to it
		v    = m_xml2.getNode    ( nn + 1 );
		vlen = m_xml2.getNodeLen ( nn + 1 );
		// if a back tag... set the value to the empty string
		if ( v[0] == '<' && v[1] == '/' ) vlen = 0;
		// now, extricate from the <![CDATA[ ... ]]> tag if we need to
		if ( m->m_type == TYPE_STRING         || 
		     m->m_type == TYPE_STRINGBOX      ||
		     m->m_type == TYPE_STRINGNONEMPTY   ) {
			char *oldv    = v;
			int32_t  oldvlen = vlen;
			// reset if not a tag node
			if ( v[0] != '<' && nn + 2 < numNodes2 ) {
				v    = m_xml2.getNode    ( nn + 2 );
				vlen = m_xml2.getNodeLen ( nn + 2 );
			}
			// should be a <![CDATA[...]]>
			if ( vlen<12 || strncasecmp(v,"<![CDATA[",9)!=0 ) {
				log("conf: No <![CDATA[...]]> tag found "
				    "for \"<%s>\" tag. Trying without CDATA.",
				    m->m_xml);
				v    = oldv;
				vlen = oldvlen;
			}
			// point to the nugget
			else {
				v    += 9;
				vlen -= 12;
			}
		}

		// get the value
		//v = m_xml2.getString ( nn , nn+2 , m->m_xml , &vlen );

		// this only happens when tag is there, but without a value
		if ( !v || vlen == 0 ) {
			vlen = 0;
			v = tt;
		}

		//c = v[vlen];
		v[vlen]='\0';

		// now decode it into itself
		nb = htmlDecode ( v , v , vlen , false,0);
		v[nb] = '\0';

		// set our parm
		setParm( (char *)THIS, m, i, j, v, false /*is html encoded?*/, false );

		// we were set from the backup default file
		//((CollectionRec *)THIS)->m_orig[i] = 1;
		// go back
		//v[vlen] = c;
		// do not repeat same node
		nn++;
		// try to get the next node if we're an array
		if ( ++j < m->m_max || j < m->m_fixed ) { goto loop; }
		// otherwise, if not an array, go to next parm
		continue;
	}

	// backwards compatible hack for old <masterPassword> tags
	for ( int32_t i = 1 ; i < numNodes ; i++ ) {
		if ( objType != OBJ_CONF ) break;
		XmlNode *pn = xml.getNodePtr(i-1);
		XmlNode *xn = xml.getNodePtr(i);
		// look for <masterPassword>
		if ( pn->m_nodeId != TAG_XMLTAG) continue;
		if ( xn->m_nodeId != TAG_CDATA) continue;
		if ( pn->m_tagNameLen != 14 ) continue;
		if ( xn->m_tagNameLen != 8 ) continue;
		// if it is not the OLD supported tag then skip
		if ( strncmp ( pn->m_tagName,"masterPassword",14 ) ) continue;
		if ( strncmp ( xn->m_tagName,"![CDATA[",8 ) ) continue;
		// otherwise append to buf
		char *text = xn->m_node + 9;
		int32_t  tlen = xn->m_nodeLen - 12;
		g_conf.m_masterPwds.safeMemcpy(text,tlen);
		// a \n
		g_conf.m_masterPwds.pushChar('\n');
		g_conf.m_masterPwds.nullTerm();
	}
	// another backwards compatible hack for old masterIp tags
	for ( int32_t i = 1 ; i < numNodes ; i++ ) {
		if ( objType != OBJ_CONF ) break;
		XmlNode *xn = xml.getNodePtr(i);
		XmlNode *pn = xml.getNodePtr(i-1);
		// look for <masterPassword>
		if ( pn->m_nodeId != TAG_XMLTAG) continue;
		if ( xn->m_nodeId != TAG_CDATA) continue;
		if ( pn->m_tagNameLen != 8 ) continue;
		if ( xn->m_tagNameLen != 8 ) continue;
		// if it is not the OLD supported tag then skip
		if ( strncmp ( pn->m_tagName,"masterIp",8 ) ) continue;
		if ( strncmp ( xn->m_tagName,"![CDATA[",8 ) ) continue;
		// otherwise append to buf
		char *text = xn->m_node + 9;
		int32_t  tlen = xn->m_nodeLen - 12;
		// otherwise append to buf
		g_conf.m_connectIps.safeMemcpy(text,tlen);
		// a \n
		g_conf.m_connectIps.pushChar('\n');
		g_conf.m_connectIps.nullTerm();
	}

	return true;
}

// returns false and sets g_errno on error
bool Parms::setXmlFromFile(Xml *xml, char *filename, SafeBuf *sb ) {
	sb->load ( filename );
	char *buf = sb->getBufStart();
	if ( ! buf )
		return log ("conf: Could not read %s : %s.",
			    filename,mstrerror(g_errno));
		
	// . remove all comments in case they contain tags
	// . if you have a # as part of your string, it must be html encoded,
	//   just like you encode < and >
	char *s = buf;
	char *d = buf;
	while ( *s ) {
		// . skip comments
		// . watch out for html encoded pound signs though
		if ( *s == '#' ) {
			if (s>buf && *(s-1)=='&' && is_digit(*(s+1))) goto ok;
			while ( *s && *s != '\n' ) s++; 
			continue; 
		}
		// otherwise, transcribe over
	ok:
		*d++ = *s++;
	}
	*d = '\0';
	int32_t bufSize = d - buf;
	// . set to xml
	// . use version of 0
	return xml->set( buf, bufSize, 0, 0, CT_XML );
}

//#define MAX_CONF_SIZE 200000

// returns false and sets g_errno on error
bool Parms::saveToXml ( char *THIS , char *f , char objType ) {
	if ( g_conf.m_readOnlyMode ) return true;
	// print into buffer
	// "seeds" can be pretty big so go with safebuf now
	// fix so if we core in malloc/free we can still save conf
	char  tmpbuf[200000];
	SafeBuf sb(tmpbuf,200000);
	//char *p    = buf;
	//char *pend = buf + MAX_CONF_SIZE;
	int32_t  len ;
	//int32_t  n   ;
	File  ff  ;
	int32_t  j   ;
	int32_t  count;
	char *s;
	CollectionRec *cr = NULL;
	if ( THIS != (char *)&g_conf ) cr = (CollectionRec *)THIS;
	// now set THIS based on the parameters in the xml file
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		// get it
		Parm *m = &m_parms[i];
		if ( m->m_obj != objType ) continue;
		// . there are 2 object types, coll recs and g_conf, aka
		//   OBJ_COLL and OBJ_CONF.
		// . make sure we got the right parms for what we want
		if ( m->m_obj == OBJ_NONE ) continue;
		// skip dups
		if ( m->m_flags & PF_DUP ) continue;
		// do not allow searchinput parms through
		if ( m->m_obj == OBJ_SI ) continue;
		if ( THIS == (char *)&g_conf && m->m_obj != OBJ_CONF) continue;
		if ( THIS != (char *)&g_conf && m->m_obj == OBJ_CONF) continue;
		if ( m->m_type == TYPE_MONOD2  ) continue;
		if ( m->m_type == TYPE_MONOM2  ) continue;
		if ( m->m_type == TYPE_CMD ) continue;
		if ( m->m_type == TYPE_BOOL2 ) continue;
		if ( m->m_type == TYPE_FILEUPLOADBUTTON ) continue;
		// ignore if hidden as well! no, have to keep those separate
		// since spiderroundnum/starttime is hidden but should be saved
		if ( m->m_flags & PF_NOSAVE ) continue;
		// ignore if diffbot and we are not a diffbot/custom crawl
		if ( cr && 
		     ! cr->m_isCustomCrawl &&
		     (m->m_flags & PF_DIFFBOT) ) continue;
		// skip if we should not save to xml
		if ( ! m->m_save ) continue;
		// allow comments though
		if ( m->m_type == TYPE_COMMENT ) goto skip2;
		// skip if offset is negative, that means none
		s = (char *)THIS + m->m_off ;
		// if array, count can be 0 or more than 1
		count = 1;
		if ( m->m_max   > 1 ) count = *(int32_t *)(s-4);
		if ( m->m_fixed > 0 ) count = m->m_fixed;
		// sanity check
		if ( count > 100000 ) {
			log(LOG_LOGIC,"admin: Outrageous array size in for "
			    "parameter %s. Does the array max size int32_t "
			    "preceed it in the conf class?",m->m_title);
			exit(-1);
		}
skip2:
		// description, do not wrap words around lines
		char *d = m->m_desc;
		// if empty array mod description to include the tag name
		char tmp [10*1024];
		if ( m->m_max > 1 && count == 0 && gbstrlen(d) < 9000 &&
		     m->m_xml && m->m_xml[0] ) {
			char *cc = "";
			if ( d && d[0] ) cc = "\n";
			sprintf ( tmp , "%s%sUse <%s> tag.",d,cc,m->m_xml);
			d = tmp;
		}
		char *END  = d + gbstrlen(d);
		char *dend;
		char *last;
		char *start;
		// just print tag if it has no description
		if ( ! *d ) goto skip;
		//if ( p + gbstrlen(d)+5 >= pend ) goto hadError;
		//if ( p > buf ) *p++='\n';
		if ( sb.length() ) sb.pushChar('\n');
	loop:
		dend  = d + 77;
		if ( dend > END ) dend = END;
		last  = d;
		start = d;
		while ( *d && d < dend ) { 
			if ( *d == ' '  ) last = d; 
			if ( *d == '\n' ) { last = d; break; }
			d++; 
		}
		if ( ! *d ) last = d;
		//gbmemcpy ( p , "# " , 2 ); 
		//p += 2;
		sb.safeMemcpy("# ",2);
		//gbmemcpy ( p , start , last - start );
		//p += last - start;
		sb.safeMemcpy(start,last-start);
		//*p++='\n';
		sb.pushChar('\n');
		d = last + 1;
		if ( d < END && *d ) goto loop;
		// bail if comment
		if ( m->m_type == TYPE_COMMENT ) {
			//sprintf ( p , "\n" );
			//p += gbstrlen ( p );
			continue;
		}
		if ( m->m_type == TYPE_MONOD2  ) continue;
		if ( m->m_type == TYPE_MONOM2  ) continue;

	skip:
		// loop over all in this potential array
		for ( j = 0 ; j < count ; j++ ) {
			// the xml
			//if ( p + gbstrlen(m->m_xml) >= pend ) goto hadError;
			if ( g_errno ) goto hadError;
			//sprintf ( p , "<%s>" , m->m_xml );
			//p += gbstrlen ( p );
			sb.safePrintf("<%s>" , m->m_xml );
			// print CDATA if string
			if ( m->m_type == TYPE_STRING         || 
			     m->m_type == TYPE_STRINGBOX      ||
			     m->m_type == TYPE_SAFEBUF        ||
			     m->m_type == TYPE_STRINGNONEMPTY   ) {
				//sprintf ( p , "<![CDATA[" );
				//p += gbstrlen ( p );
				sb.safeStrcpy( "<![CDATA[" );
			}
			// break point
			//if (strcmp ( m->m_xml , "filterRulesetDefault")==0)
			//	log("got it");
			// . represent it in ascii form
			// . this escapes out <'s and >'s
			// . this ALSO encodes #'s (xml comment indicators)
			//p = getParmHtmlEncoded(p,pend,m,s);
			getParmHtmlEncoded(&sb,m,s);
			// print CDATA if string
			if ( m->m_type == TYPE_STRING         || 
			     m->m_type == TYPE_STRINGBOX      ||
			     m->m_type == TYPE_SAFEBUF        ||
			     m->m_type == TYPE_STRINGNONEMPTY   ) {
				//sprintf ( p , "]]>" );
				//p += gbstrlen ( p );
				sb.safeStrcpy("]]>" );
			}
			// this is NULL if it ran out of room
			//if ( ! p ) goto hadError;
			if ( g_errno ) goto hadError;
			// advance to next element in array, if it is one
			s = s + m->m_size;
			// close the xml tag
			//if ( p + 4 >= pend ) goto hadError;
			//sprintf ( p , "</>\n" );
			//p += gbstrlen ( p );
			sb.safeStrcpy("</>\n" );
			if ( g_errno ) goto hadError;
		}
	}
	//*p = '\0';
	sb.nullTerm();

	//ff.set ( f );
	//if ( ! ff.open ( O_RDWR | O_CREAT | O_TRUNC ) )
	//	return log("db: Could not open %s : %s",
	//		   ff.getFilename(),mstrerror(g_errno));

	// save the parm to the file
	//len = gbstrlen(buf);
	len = sb.length();
	// use -1 for offset so we do not use pwrite() so it will not leave
	// garbage at end of file
	//n    = ff.write ( buf , len , -1 );
	//n    = ff.write ( sb.getBufStart() , len , -1 );
	//ff.close();
	//if ( n == len ) return true;

	// save to filename "f". returns # of bytes written. -1 on error.
	if ( sb.safeSave ( f ) >= 0 )
		return true;

	return log("admin: Could not write to file %s.",f);
 hadError:
	return log("admin: Error writing to %s: %s",f,mstrerror(g_errno));

	//File bigger than %"INT32" bytes."
	//	   "  Please increase #define in Parms.cpp.",
	//	   (int32_t)MAX_CONF_SIZE);
}

bool Parms::getParmHtmlEncoded ( SafeBuf *sb , Parm *m , char *s ) {
	// do not breech the buffer
	//if ( p + 100 >= pend ) return p;
	// print it out
	char t = m->m_type;
	if ( t == TYPE_CHAR           || t == TYPE_BOOL           ||
	     t == TYPE_CHECKBOX       ||
	     t == TYPE_PRIORITY       || t == TYPE_PRIORITY2      || 
	     //t == TYPE_DIFFBOT_DROPDOWN ||
	     t == TYPE_UFP            ||
	     t == TYPE_PRIORITY_BOXES || t == TYPE_RETRIES        ||
	     t == TYPE_RETRIES        || t == TYPE_FILTER         ||
	     t == TYPE_BOOL2          || t == TYPE_CHAR2           ) 
		sb->safePrintf("%"INT32"",(int32_t)*s);
	else if ( t == TYPE_FLOAT )
		sb->safePrintf("%f",*(float *)s);
	else if ( t == TYPE_IP ) 
		sb->safePrintf("%s",iptoa(*(int32_t *)s));
	else if ( t == TYPE_LONG || t == TYPE_LONG_CONST || t == TYPE_RULESET||
		  t == TYPE_SITERULE ) 
		sb->safePrintf("%"INT32"",*(int32_t *)s);
	else if ( t == TYPE_LONG_LONG )
		sb->safePrintf("%"INT64"",*(int64_t *)s);
	else if ( t == TYPE_SAFEBUF ) {
		SafeBuf *sb2 = (SafeBuf *)s;
		char *buf = sb2->getBufStart();
		//int32_t blen = 0;
		//if ( buf ) blen = gbstrlen(buf);
		//p = htmlEncode ( p , pend , buf , buf + blen , true ); // #?*
		// we can't do proper cdata and be backwards compatible
		//sb->cdataEncode ( buf );//, blen );//, true ); // #?*
		if ( buf ) sb->htmlEncode ( buf );
	}
	else if ( t == TYPE_STRING         || 
		  t == TYPE_STRINGBOX      ||
		  t == TYPE_STRINGNONEMPTY ||
		  t == TYPE_TIME) {
		//int32_t slen = gbstrlen ( s );
		// this returns the length of what was written, it may
		// not have converted everything if pend-p was too small...
		//p += saftenTags2 ( p , pend - p , s , len );
		//p = htmlEncode ( p , pend , s , s + slen , true /*#?*/);
		// we can't do proper cdata and be backwards compatible
		//sb->cdataEncode ( s );//, slen );//, true /*#?*/);
		sb->htmlEncode ( s );
	}
	else if ( t == TYPE_DATE || t == TYPE_DATE2 ) {
		// time is stored as int32_t
		int32_t ct = *(int32_t *)s;
		// get the time struct
		struct tm *tp = localtime ( (time_t *)&ct ) ;
		// set the "selected" month for the drop down
		char tmp[100];
		strftime ( tmp , 100 , "%d %b %Y %H:%M UTC" , tp );
		sb->safeStrcpy ( tmp );
		sb->setLabel("parm3");
	}
	//p += gbstrlen ( p );
	//return p;
	return true;
}

void Parms::init ( ) {
	// initialize the Parms class if we need to, only do it once
	static bool s_init = false ;
	if ( s_init ) return;
	s_init = true ;

	// default all
	for ( int32_t i = 0 ; i < MAX_PARMS ; i++ ) {
		m_parms[i].m_parmNum= i;
		m_parms[i].m_hash   = 0         ;
		m_parms[i].m_title  = ""         ; // for detecting if not set
		m_parms[i].m_desc   = ""         ; // for detecting if not set
		m_parms[i].m_cgi    = NULL       ; // for detecting if not set
		m_parms[i].m_off    = -1         ; // for detecting if not set
		// for PAGE_FILTERS url filters for printing the url
		// filter profile parm above the url filters table rows.
		m_parms[i].m_colspan= -1; 
		m_parms[i].m_def    = NULL       ; // for detecting if not set
		m_parms[i].m_defOff = -1; // if default pts to collrec parm
		m_parms[i].m_type   = TYPE_NONE  ; // for detecting if not set
		m_parms[i].m_page   = -1         ; // for detecting if not set
		m_parms[i].m_obj    = -1         ; // for detecting if not set
		m_parms[i].m_max    =  1         ; // max elements in array
		m_parms[i].m_fixed  =  0         ; // size of fixed size array
		m_parms[i].m_size   =  0         ; // max string size
		m_parms[i].m_cast   =  1 ; // send to all hosts?
		m_parms[i].m_rowid  = -1 ; // rowid of -1 means not in row
		m_parms[i].m_addin  =  0 ; // add insert row command?
		m_parms[i].m_rdonly =  0 ; // is command off in read-only mode?
		m_parms[i].m_hdrs   =  1 ; // assume to always print headers
		m_parms[i].m_perms  =  0 ; // same as containing WebPages perms
		m_parms[i].m_plen   = -1 ; // offset for strings length
		m_parms[i].m_group  =  1 ; // start of a new group of controls?
		m_parms[i].m_priv   =  0 ; // is it private?
		m_parms[i].m_save   =  1 ; // save to xml file?
		m_parms[i].m_min    = -1 ; // min value (for int32_t parms)
		m_parms[i].m_flags  = 0;
		m_parms[i].m_icon   = NULL;
		m_parms[i].m_class  = NULL;
		m_parms[i].m_qterm  = NULL;
		m_parms[i].m_subMenu= 0;
		m_parms[i].m_spriv  = 0;
		m_parms[i].m_sminc  = -1;  // min in collection rec
		m_parms[i].m_smaxc  = -1;  // max in collection rec
		m_parms[i].m_smin   = 0x80000000; // 0xffffffff;
		m_parms[i].m_smax   = 0x7fffffff;
		m_parms[i].m_sprpg  =  1; // propagate to other pages via GET
		m_parms[i].m_sprpp  =  1; // propagate to other pages via POST
		m_parms[i].m_sync   = true;
	}

	Parm *m = &m_parms [ 0 ];

	CollectionRec cr;
	SearchInput   si;

	///////////////////////////////////////////
	// CAN ONLY BE CHANGED IN CONF AT STARTUP (no cgi field)
	///////////////////////////////////////////

	char *g = (char *)&g_conf;
	char *x = (char *)&cr;
	char *y = (char *)&si;


	//////////////
	// 
	// now for Pages.cpp printApiForPage() we need these
	//
	//////////////


	GigablastRequest gr;

	InjectionRequest ir;

#ifndef PRIVACORE_SAFE_VERSION
	m->m_title = "collection";
	m->m_desc  = "Clone settings INTO this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_CLONECOLL;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	m->m_flags = PF_API | PF_REQUIRED;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;
#endif

	m->m_title = "collection";
	m->m_desc  = "Use this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_BASIC_STATUS;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	m->m_flags = PF_API | PF_REQUIRED;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Use this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	// do not show in html controls
	m->m_flags = PF_API | PF_REQUIRED | PF_NOHTML;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Use this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	// do not show in html controls
	m->m_flags = PF_API | PF_REQUIRED | PF_NOHTML;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Use this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_SPIDERDB;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	// do not show in html controls
	m->m_flags = PF_API | PF_REQUIRED | PF_NOHTML;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Use this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_SITEDB;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	// do not show in html controls
	m->m_flags = PF_API | PF_REQUIRED | PF_NOHTML;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Inject into this collection.";
	m->m_cgi   = "c";
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	// PF_COLLDEFAULT: so it gets set to default coll on html page
	m->m_flags = PF_API|PF_REQUIRED|PF_NOHTML; 
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	////////////
	//
	// end stuff for printApiForPage()
	//
	////////////

	// just a comment in the conf file
	m->m_desc  = 
		"All <, >, \" and # characters that are values for a field "
		"contained herein must be represented as "
		"&lt;, &gt;, &#34; and &#035; respectively.";
	m->m_type  = TYPE_COMMENT;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	// just a comment in the conf file
	m->m_desc  = 
    "Below the various Gigablast databases are configured.\n"
    "<*dbMaxTreeMem>          - mem used for holding new recs\n"
    "<*dbMaxDiskPageCacheMem> - disk page cache mem for this db\n"
    "<*dbMaxCacheMem>         - cache mem for holding single recs\n"
//"<*dbMinFilesToMerge>     - required # files to trigger merge\n"
    "<*dbSaveCache>           - save the rec cache on exit?\n"
    "<*dbMaxCacheAge>         - max age (seconds) for recs in rec cache\n"
		"See that Stats page for record counts and stats.\n";
	m->m_type  = TYPE_COMMENT;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns max cache mem";
	m->m_desc  = "How many bytes should be used for caching DNS replies?";
	m->m_off   = (char *)&g_conf.m_dnsMaxCacheMem - g;
	m->m_def   = "128000";
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "tagdb max tree mem";
	m->m_desc  = "A tagdb record "
		"assigns a url or site to a ruleset. Each tagdb record is "
		"about 100 bytes or so.";
	m->m_off   = (char *)&g_conf.m_tagdbMaxTreeMem - g;
	m->m_def   = "1028000"; 
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "clusterdb max tree mem";
	m->m_desc  = "Clusterdb caches small records for site clustering "
		"and deduping.";
	m->m_off   = (char *)&g_conf.m_clusterdbMaxTreeMem - g;
	m->m_def   = "1000000";
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	// this is overridden by collection
	m->m_title = "clusterdb min files to merge";
	m->m_desc  = "";
	m->m_cgi   = "cmftm";
	m->m_off   = (char *)&g_conf.m_clusterdbMinFilesToMerge - g;
	m->m_def   = "-1"; // -1 means to use collection rec
	m->m_type  = TYPE_LONG;
	m->m_save  = 0;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "clusterdb save cache";
	m->m_desc  = "";
	m->m_cgi   = "cdbsc";
	m->m_off   = (char *)&g_conf.m_clusterdbSaveCache - g;
	m->m_def   = "0"; 
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "max vector cache mem";
	m->m_desc  = "Max memory for dup vector cache.";
	m->m_off   = (char *)&g_conf.m_maxVectorCacheMem - g;
	m->m_def   = "10000000";
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "robotdb max cache mem";
	m->m_desc  = "Robotdb caches robot.txt files.";
	m->m_off   = (char *)&g_conf.m_robotdbMaxCacheMem - g;
	m->m_def   = "128000"; 
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "robotdb save cache";
	m->m_cgi   = "rdbsc";
	m->m_desc  = "";
	m->m_off   = (char *)&g_conf.m_robotdbSaveCache - g;
	m->m_def   = "0"; 
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "statsdb max tree mem";
	m->m_desc  = "";
	m->m_off   = (char *)&g_conf.m_statsdbMaxTreeMem - g;
	m->m_def   = "5000000";
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "statsdb max cache mem";
	m->m_desc  = "";
	m->m_off   = (char *)&g_conf.m_statsdbMaxCacheMem - g;
	m->m_def   = "0";
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "http max send buf size";
	m->m_desc  = "Maximum bytes of a doc that can be sent before having "
		"to read more from disk";
	m->m_cgi   = "hmsbs";
	m->m_off   = (char *)&g_conf.m_httpMaxSendBufSize - g;
	m->m_def   = "128000"; 
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "search results max cache mem";
	m->m_desc  = "Bytes to use for caching search result pages.";
	m->m_off   = (char *)&g_conf.m_searchResultsMaxCacheMem - g;
	m->m_def   = "100000"; 
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_NOSYNC|PF_NOAPI;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "read only mode";
	m->m_desc  = "Read only mode does not allow spidering.";
	m->m_cgi   = "readonlymode";
	m->m_off   = (char *)&g_conf.m_readOnlyMode - g;
	m->m_def   = "0"; 
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_NOAPI;
	m++;

	///////////////////////////////////////////
	// BASIC SETTINGS
	///////////////////////////////////////////

	m->m_title = "spidering enabled";
	m->m_desc  = "Pause and resumes spidering for this collection.";
	m->m_cgi   = "bcse";
	m->m_off   = (char *)&cr.m_spideringEnabled - x;
	m->m_page  = PAGE_BASIC_SETTINGS;
	m->m_obj   = OBJ_COLL;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_DUP|PF_CLONE;
	m++;

	m->m_title = "site list";
	m->m_xml   = "siteList";
	m->m_desc  = "List of sites to spider, one per line. "
		"See <a href=#examples>example site list</a> below. "
		"<br>"
		"<br>"
		"Example #1: <b>mysite.com myothersite.com</b>"
		"<br>"
		"<i>This will spider just those two sites.</i>"
		"<br>"
		"<br>"
		"Example #2: <b>seed:dmoz.org</b>"
		"<br>"
		"<i>This will spider the whole web starting with the website "
		"dmoz.org</i>"
		"<br><br>"
		"Gigablast uses the "
		"<a href=/admin/filters#insitelist>insitelist</a> "
		"directive on "
		"the <a href=/admin/filters>url filters</a> "
		"page to make sure that the spider only indexes urls "
		"that match the site patterns you specify here, other than "
		"urls you add individually via the add urls or inject url "
		"tools. "
		"Limit list to 300MB. If you have a lot of INDIVIDUAL urls "
		"to add then consider using the <a href=/admin/addurl>add "
		"urls</a> interface.";
	m->m_cgi   = "sitelist";
	m->m_off   = (char *)&cr.m_siteListBuf - x;
	m->m_page  = PAGE_BASIC_SETTINGS;
	m->m_obj   = OBJ_COLL;
	m->m_type  = TYPE_SAFEBUF;
	m->m_func  = CommandUpdateSiteList;
	m->m_def   = "";
	// rebuild urlfilters now will nuke doledb and call updateSiteList()
	m->m_flags = PF_TEXTAREA | PF_DUP | PF_REBUILDURLFILTERS;
	m++;

#ifndef PRIVACORE_SAFE_VERSION
	m->m_title = "restart collection";
	m->m_desc  = "Remove all documents from the collection and re-add "
		"seed urls from site list.";
	// If you do this accidentally there "
	//"is a <a href=/faq.html#recover>recovery procedure</a> to "
	//	"get back the trashed data.";
	m->m_cgi   = "restart";
	m->m_page  = PAGE_BASIC_SETTINGS;
	m->m_obj   = OBJ_COLL;
	m->m_type  = TYPE_CMD;
	m->m_func2 = CommandRestartColl;
	m++;
#endif

	/////////////////////
	//
	// DIFFBOT CRAWLBOT PARMS
	//
	//////////////////////

	///////////
	//
	// DO NOT INSERT parms above here, unless you set
	// m_obj = OBJ_COLL !!! otherwise it thinks it belongs to
	// OBJ_CONF as used in the above parms.
	//
	///////////

	m->m_cgi   = "dbtoken";
	m->m_xml   = "diffbotToken";
	m->m_off   = (char *)&cr.m_diffbotToken - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_DIFFBOT;
	m++;

	m->m_cgi   = "createdtime";
	m->m_xml   = "collectionCreatedTime";
	m->m_desc  = "Time when this collection was created, or time of "
		"the last reset or restart.";
	m->m_off   = (char *)&cr.m_diffbotCrawlStartTime - x;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "0";
	m->m_flags = PF_NOAPI;//PF_DIFFBOT; no i want to saveToXml
	m++;

	m->m_cgi   = "spiderendtime";
	m->m_xml   = "crawlEndTime";
	m->m_desc  = "If spider is done, when did it finish.";
	m->m_off   = (char *)&cr.m_diffbotCrawlEndTime - x;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "0";
	m->m_flags = PF_NOAPI;//PF_DIFFBOT; no i want to saveToXml
	m++;

	m->m_cgi   = "dbcrawlname";
	m->m_xml   = "diffbotCrawlName";
	m->m_off   = (char *)&cr.m_diffbotCrawlName - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_DIFFBOT;
	m++;

	m->m_cgi   = "notifyEmail";
	m->m_title = "notify email";
	m->m_xml   = "notifyEmail";
	m->m_off   = (char *)&cr.m_notifyEmail - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_DIFFBOT;
	m++;

	m->m_cgi   = "notifyWebhook";
	m->m_xml   = "notifyWebhook";
	m->m_title = "notify webhook";
	m->m_off   = (char *)&cr.m_notifyUrl - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_DIFFBOT;
	m++;

	// collective respider frequency (for pagecrawlbot.cpp)
	m->m_title = "collective respider frequency (days)";
	m->m_cgi   = "repeat";
	m->m_xml   = "collectiveRespiderFrequency";
	m->m_off   = (char *)&cr.m_collectiveRespiderFrequency - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.0"; // 0.0
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_units = "days";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_title = "collective crawl delay (seconds)";
	m->m_cgi   = "crawlDelay";
	m->m_xml   = "collectiveCrawlDelay";
	m->m_off   = (char *)&cr.m_collectiveCrawlDelay - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = ".250"; // 250 ms
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m->m_units = "seconds";
	m++;

	m->m_cgi   = "urlCrawlPattern";
	m->m_xml   = "diffbotUrlCrawlPattern";
	m->m_title = "url crawl pattern";
	m->m_off   = (char *)&cr.m_diffbotUrlCrawlPattern - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_cgi   = "urlProcessPattern";
	m->m_xml   = "diffbotUrlProcessPattern";
	m->m_title = "url process pattern";
	m->m_off   = (char *)&cr.m_diffbotUrlProcessPattern - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_cgi   = "pageProcessPattern";
	m->m_xml   = "diffbotPageProcessPattern";
	m->m_title = "page process pattern";
	m->m_off   = (char *)&cr.m_diffbotPageProcessPattern - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_cgi   = "urlCrawlRegEx";
	m->m_xml   = "diffbotUrlCrawlRegEx";
	m->m_title = "url crawl regex";
	m->m_off   = (char *)&cr.m_diffbotUrlCrawlRegEx - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_cgi   = "urlProcessRegEx";
	m->m_xml   = "diffbotUrlProcessRegEx";
	m->m_title = "url process regex";
	m->m_off   = (char *)&cr.m_diffbotUrlProcessRegEx - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;
  
	m->m_cgi   = "maxHops";
	m->m_xml   = "diffbotHopcount";
	m->m_title = "diffbot max hopcount";
	m->m_off   = (char *)&cr.m_diffbotMaxHops - x;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "-1";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_cgi   = "onlyProcessIfNew";
	m->m_xml   = "diffbotOnlyProcessIfNew";
	m->m_title = "onlyProcessIfNew";
	m->m_off   = (char *)&cr.m_diffbotOnlyProcessIfNewUrl - x;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "1";
	m->m_flags = PF_REBUILDURLFILTERS | PF_DIFFBOT;
	m++;

	m->m_cgi   = "seeds";
	m->m_xml   = "diffbotSeeds";
	m->m_off   = (char *)&cr.m_diffbotSeeds - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_DIFFBOT;
	m->m_def   = "";
	m++;

	m->m_xml   = "isCustomCrawl";
	m->m_off   = (char *)&cr.m_isCustomCrawl - x;
	m->m_type  = TYPE_CHAR;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_cgi   = "isCustomCrawl";
	m->m_def   = "0";
	m->m_flags = PF_DIFFBOT;
	m++;

	m->m_cgi   = "maxToCrawl";
	m->m_title = "max to crawl";
	m->m_xml   = "maxToCrawl";
	m->m_off   = (char *)&cr.m_maxToCrawl - x;
	m->m_type  = TYPE_LONG_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "100000";
	m->m_flags = PF_DIFFBOT;
	m++;

	m->m_cgi   = "maxToProcess";
	m->m_title = "max to process";
	m->m_xml   = "maxToProcess";
	m->m_off   = (char *)&cr.m_maxToProcess - x;
	m->m_type  = TYPE_LONG_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "-1";
	m->m_flags = PF_DIFFBOT;
	m++;

	m->m_cgi   = "maxRounds";
	m->m_title = "max crawl rounds";
	m->m_xml   = "maxCrawlRounds";
	m->m_off   = (char *)&cr.m_maxCrawlRounds - x;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "-1";
	m->m_flags = PF_DIFFBOT;
	m++;

	/////////////////////
	//
	// new cmd parms
	//
	/////////////////////


	m->m_title = "insert parm row";
	m->m_desc  = "insert a row into a parm";
	m->m_cgi   = "insert";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandInsertUrlFiltersRow;
	m->m_cast  = 1;
	m->m_flags = PF_REBUILDURLFILTERS;
	m++;

	m->m_title = "remove parm row";
	m->m_desc  = "remove a row from a parm";
	m->m_cgi   = "remove";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandRemoveUrlFiltersRow;
	m->m_cast  = 1;
	m->m_flags = PF_REBUILDURLFILTERS;
	m++;

#ifndef PRIVACORE_SAFE_VERSION
	m->m_title = "delete collection";
	m->m_desc  = "delete a collection";
	m->m_cgi   = "delete";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func2 = CommandDeleteColl;
	m->m_cast  = 1;
	m++;

	m->m_title = "delete collection 2";
	m->m_desc  = "delete the specified collection";
	m->m_cgi   = "delColl";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func2 = CommandDeleteColl2;
	m->m_cast  = 1;
	m++;

	m->m_title = "delete collection";
	m->m_desc  = "Delete the specified collection. You can specify "
		"multiple &delcoll= parms in a single request to delete "
		"multiple collections at once.";
	// lowercase as opposed to camelcase above
	m->m_cgi   = "delcoll";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_DELCOLL;
	m->m_obj   = OBJ_COLL;
	m->m_func2 = CommandDeleteColl2;
	m->m_cast  = 1;
	m->m_flags = PF_API | PF_REQUIRED;
	m++;

	// arg is the collection # to clone from
	m->m_title = "clone collection";
	m->m_desc  = "Clone collection settings FROM this collection.";
	m->m_cgi   = "clonecoll";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_CLONECOLL;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandCloneColl;
	m->m_cast  = 1;
	m->m_flags = PF_API | PF_REQUIRED;
	m++;

	m->m_title = "add collection";
	m->m_desc  = "add a new collection";
	// camelcase support
	m->m_cgi   = "addColl";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandAddColl0;
	m->m_cast  = 1;
	m++;

	m->m_title = "add collection";
	m->m_desc  = "Add a new collection with this name. No spaces "
		"allowed or strange characters allowed. Max of 64 characters.";
	// lower case support
	m->m_cgi   = "addcoll";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_ADDCOLL;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandAddColl0;
	m->m_cast  = 1;
	m->m_flags = PF_API | PF_REQUIRED;
	m++;
#endif


#ifndef PRIVACORE_SAFE_VERSION
	//
	// CLOUD SEARCH ENGINE SUPPORT
	//

	m->m_title = "add custom crawl";
	m->m_desc  = "add custom crawl";
	m->m_cgi   = "addCrawl";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandAddColl1;
	m->m_cast  = 1;
	m++;

	m->m_title = "add bulk job";
	m->m_desc  = "add bulk job";
	m->m_cgi   = "addBulk";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandAddColl2;
	m->m_cast  = 1;
	m++;
#endif

	m->m_title = "in sync";
	m->m_desc  = "signify in sync with host 0";
	m->m_cgi   = "insync";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_COLL;
	m->m_func  = CommandInSync;
	m->m_cast  = 1;
	m++;



	///////////////////////////////////////////
	// SEARCH CONTROLS
	///////////////////////////////////////////

	m->m_title = "read from cache by default";
	m->m_desc  = "Should we read search results from the cache? Set "
		"to false to fix dmoz bug.";
	m->m_cgi   = "rcd";
	m->m_off   = (char *)&cr.m_rcache - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "fast results";
	m->m_desc  = "Use &fast=1 to obtain seach results from the much "
		"faster Gigablast index, although the results are not "
		"searched as thoroughly.";
	m->m_obj   = OBJ_SI;
	m->m_page  = PAGE_RESULTS;
	m->m_off   = (char *)&si.m_query - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_def   = "0";
	m->m_cgi   = "fast";
	m->m_flags = PF_COOKIE | PF_WIDGET_PARM | PF_API;
	m++;


	m->m_title = "query";
	m->m_desc  = "The query to perform. See <a href=/help.html>help</a>. "
		"See the <a href=#qops>query operators</a> below for "
		"more info.";
	m->m_obj   = OBJ_SI;
	m->m_page  = PAGE_RESULTS;
	m->m_off   = (char *)&si.m_query - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "q";
	m->m_flags = PF_REQUIRED | PF_COOKIE | PF_WIDGET_PARM | PF_API;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Search this collection. Use multiple collection names "
		"separated by a whitespace to search multiple collections at "
		"once.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	m->m_flags = PF_API | PF_REQUIRED;
	m->m_off   = (char *)&si.m_coll - y;
	m++;

	m->m_title = "number of results per query";
	m->m_desc  = "The number of results returned per page.";
	// make it 25 not 50 since we only have like 26 balloons
	m->m_def   = "10";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_off   = (char *)&si.m_docsWanted - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "n";
	m->m_flags = PF_WIDGET_PARM | PF_API;
	m->m_smin  = 0;
	m++;


	m->m_title = "first result num";
	m->m_desc  = "Start displaying at search result #X. Starts at 0.";
	m->m_def   = "0";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_off   = (char *)&si.m_firstResultNum - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "s";
	m->m_smin  = 0;
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_flags = PF_REDBOX;
	m++;

	m->m_title = "show errors";
	m->m_desc  = "Show errors from generating search result summaries "
		"rather than just hide the docid. Useful for debugging.";
	m->m_cgi   = "showerrors";
	m->m_off   = (char *)&si.m_showErrors - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "site cluster";
	m->m_desc  = "Should search results be site clustered? This "
		"limits each site to appearing at most twice in the "
		"search results. Sites are subdomains for the most part, "
		"like abc.xyz.com.";
	m->m_cgi   = "sc";
	m->m_off   = (char *)&si.m_doSiteClustering - y;
	m->m_defOff= (char *)&cr.m_siteClusterByDefault - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "hide all clustered results";
	m->m_desc  = "Only display at most one result per site.";
	m->m_cgi   = "hacr";
	m->m_off   = (char *)&si.m_hideAllClustered - y;
	m->m_defOff= (char *)&cr.m_hideAllClustered - x;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_API;
	m++;


	m->m_title = "dedup results";
	m->m_desc  = "Should duplicate search results be removed? This is "
		"based on a content hash of the entire document. "
		"So documents must be exactly the same for the most part.";
	m->m_cgi   = "dr"; // dedupResultsByDefault";
	m->m_off   = (char *)&si.m_doDupContentRemoval - y;
	m->m_defOff= (char *)&cr.m_dedupResultsByDefault - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 1;
	m->m_cgi   = "dr";
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "percent similar dedup summary";
	m->m_desc  = "If document summary (and title) are "
		"this percent similar "
		"to a document summary above it, then remove it from the "
		"search results. 100 means only to remove if exactly the "
		"same. 0 means no summary deduping. You must also supply "
		"dr=1 for this to work.";
	m->m_cgi   = "pss";
	m->m_off   = (char *)&si.m_percentSimilarSummary - y;
	m->m_defOff= (char *)&cr.m_percentSimilarSummary - x;
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_smin  = 0;
	m->m_smax  = 100;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;       


	m->m_title = "dedup URLs";
	m->m_desc  = "Should we dedup URLs with case insensitivity? This is "
                     "mainly to correct duplicate wiki pages.";
	m->m_cgi   = "ddu";
	m->m_off   = (char *)&si.m_dedupURL - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "do spell checking";
	m->m_desc  = "If enabled while using the XML feed, "
		"when Gigablast finds a spelling recommendation it will be "
		"included in the XML <spell> tag. Default is 0 if using an "
		"XML feed, 1 otherwise. Will be availble again soon.";
	m->m_cgi   = "spell";
	m->m_off   = (char *)&si.m_spellCheck - y;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = "1";
	m->m_flags = PF_API;
	m++;

	m->m_title = "stream search results";
	m->m_desc  = "Stream search results back on socket as they arrive. "
		"Useful when thousands/millions of search results are "
		"requested. Required when doing such things otherwise "
		"Gigablast could run out of memory. Only supported for "
		"JSON and XML formats, not HTML.";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_off   = (char *)&si.m_streamResults - y;
	m->m_type  = TYPE_CHAR;
	m->m_def   = "0";
	m->m_cgi   = "stream";
	m->m_flags = PF_API;
	m->m_sprpg = 0; // propagate to next 10
	m->m_sprpp = 0;
	m++;

	m->m_title = "seconds back";
	m->m_desc  = "Limit results to pages spidered this many seconds ago. "
		"Use 0 to disable.";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_off   = (char *)&si.m_secsBack - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_cgi   = "secsback";
	m->m_flags = PF_API;
	m++;

	m->m_title = "sort by";
	m->m_desc  = "Use 0 to sort results by relevance, 1 to sort by "
		"most recent spider date down, and 2 to sort by oldest "
		"spidered results first.";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_off   = (char *)&si.m_sortBy - y;
	m->m_type  = TYPE_CHAR;
	m->m_def   = "0"; // this means relevance
	m->m_cgi   = "sortby";
	m->m_flags = PF_API;
	m++;

	m->m_title = "filetype";
	m->m_desc  = "Restrict results to this filetype. Supported "
		"filetypes are pdf, doc, html xml, json, xls.";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_off   = (char *)&si.m_filetype - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = "";
	m->m_cgi   = "filetype";
	m->m_flags = PF_API;
	m++;

	m->m_title = "get scoring info";
	m->m_desc  = "Get scoring information for each result so you "
		"can see how each result is scored. You must explicitly "
		"request this using &scores=1 for the XML feed because it "
		"is not included by default.";
	m->m_cgi   = "scores"; // dedupResultsByDefault";
	m->m_off   = (char *)&si.m_getDocIdScoringInfo - y;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = NULL;
	m->m_flags = PF_API;
	// get default from collectionrec item
	m->m_defOff= (char *)&cr.m_getDocIdScoringInfo - x;
	m++;



	m->m_title = "do query expansion";
	m->m_desc  = "If enabled, query expansion will expand your query "
		"to include the various forms and "
		"synonyms of the query terms.";
	m->m_off   = (char *)&si.m_queryExpansion - y;
	m->m_defOff= (char *)&cr.m_queryExpansion - x;
	m->m_type  = TYPE_BOOL;
	m->m_cgi  = "qe";
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	// limit to this # of the top term pairs from inlink text whose
	// score is accumulated
	m->m_title = "real max top";
	m->m_desc  = "Only score up to this many inlink text term pairs";
	m->m_off   = (char *)&si.m_realMaxTop - y;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = "10";
	m->m_cgi   = "rmt";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m++;

	m->m_title = "do max score algo";
	m->m_desc  = "Quickly eliminated docids using max score algo";
	m->m_off   = (char *)&si.m_doMaxScoreAlgo - y;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = "1";
	m->m_cgi   = "dmsa";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m++;

	m->m_title = "max number of facets to return";
	m->m_desc  = "Max number of facets to return";
	m->m_off   = (char *)&si.m_maxFacets - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "50";
	m->m_group = 1;
	m->m_cgi   = "nf";
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "language weight";
	m->m_desc  = "Defalt language weight if document matches quer "
		"language. Use this to give results that match the specified "
		"the speicified &qlang higher ranking, or docs whose language "
		"is unnknown. Can be override with "
		"&langw in the query url.";
	m->m_cgi   = "langweight";
	m->m_off   = (char *)&cr.m_sameLangWeight - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "20.000000";
	m->m_group = 1;
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "use language weights";
	m->m_desc  = "Use Language weights to sort query results. "
		"This will give results that match the specified &qlang "
		"higher ranking.";
	m->m_cgi   = "lsort";
	m->m_off   = (char *)&cr.m_enableLanguageSorting - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 1;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "sort language preference";
	m->m_desc  = "Default language to use for ranking results. "
		//"This should only be used on limited collections. "
		"Value should be any language abbreviation, for example "
		"\"en\" for English. Use <i>xx</i> to give ranking "
		"boosts to no language in particular. See the language "
		"abbreviations at the bottom of the "
		"<a href=/admin/filters>url filters</a> page.";
	m->m_cgi   = "qlang";
	m->m_off   = (char *)&si.m_defaultSortLang - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = "";//"xx";//_US";
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "language weight";
	m->m_desc  = "Use this to override the default language weight "
		"for this collection. The default language weight can be "
		"set in the search controls and is usually something like "
		"20.0. Which means that we multiply a result's score by 20 "
		"if from the same language as the query or the language is "
		"unknown.";
	m->m_off   = (char *)&si.m_sameLangWeight - y;
	m->m_defOff= (char *)&cr.m_sameLangWeight - x;
	m->m_type  = TYPE_FLOAT;
	m->m_cgi  = "langw";
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "sort country preference";
	m->m_desc  = "Default country to use for ranking results. "
		//"This should only be used on limited collections. "
		"Value should be any country code abbreviation, for example "
		"\"us\" for United States. This is currently not working.";
	m->m_cgi   = "qcountry";
	m->m_off   = (char *)&si.m_defaultSortCountry - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_size  = 2+1;
	m->m_def   = "us";
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "docs to check for post query";
	m->m_desc  = "How many search results should we "
		"scan for post query demotion? "
		"0 disables all post query reranking. ";
	m->m_cgi   = "pqrds";
	m->m_off   = (char *)&si.m_docsToScanForReranking - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 1;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "demotion for foreign languages";
	m->m_desc  = "Demotion factor of non-relevant languages.  Score "
		"will be penalized by this factor as a percent if "
		"it's language is foreign. "
		"A safe value is probably anywhere from 0.5 to 1. ";
	m->m_cgi   = "pqrlang";
	m->m_off   = (char *)&cr.m_languageWeightFactor - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.999";
	m->m_group = 0;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for unknown languages";
	m->m_desc  = "Demotion factor for unknown languages. "
		"Page's score will be penalized by this factor as a percent "
		"if it's language is not known. "
		"A safe value is 0, as these pages will be reranked by "
		"country (see below). "
		"0 means no demotion.";
	m->m_cgi   = "pqrlangunk";
	m->m_off   = (char *)&cr.m_languageUnknownWeight- x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.0";
	m->m_group = 0;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for pages where the country of the page writes "
		"in the same language as the country of the query";
	m->m_desc  = "Demotion for pages where the country of the page writes "
		"in the same language as the country of the query. "
		"If query language is the same as the language of the page, "
		"then if a language written in the country of the page matches "
		"a language written by the country of the query, then page's "
		"score will be demoted by this factor as a percent. "
		"A safe range is between 0.5 and 1. ";
	m->m_cgi   = "pqrcntry";
	m->m_off   = (char *)&cr.m_pqr_demFactCountry - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.98";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for query terms or gigabits in url";
	m->m_desc  = "Demotion factor for query terms or gigabits "
		"in a result's url. "
		"Score will be penalized by this factor times the number "
		"of query terms or gigabits in the url divided by "
		"the max value below such that fewer "
		"query terms or gigabits in the url causes the result "
		"to be demoted more heavily, depending on the factor. "
		"Higher factors demote more per query term or gigabit "
		"in the page's url. "
		"Generally, a page may not be demoted more than this "
		"factor as a percent. Also, how it is demoted is "
		"dependant on the max value. For example, "
		"a factor of 0.2 will demote the page 20% if it has no "
		"query terms or gigabits in its url. And if the max value is "
		"10, then a page with 5 query terms or gigabits in its "
		"url will be demoted 10%; and 10 or more query terms or "
		"gigabits in the url will not be demoted at all. "
		"0 means no demotion. "
		"A safe range is from 0 to 0.35. ";
	m->m_cgi   = "pqrqttiu";
	m->m_off   = (char *)&cr.m_pqr_demFactQTTopicsInUrl - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max value for pages with query terms or gigabits "
		"in url";
	m->m_desc  = "Max number of query terms or gigabits in a url. "
		"Pages with a number of query terms or gigabits in their "
		"urls greater than or equal to this value will not be "
		"demoted. "
		"This controls the range of values expected to represent "
		"the number of query terms or gigabits in a url. It should "
		"be set to or near the estimated max number of query terms "
		"or topics that can be in a url. Setting to a lower value "
		"increases the penalty per query term or gigabit that is "
		"not in a url, but decreases the range of values that "
		"will be demoted.";
	m->m_cgi   = "pqrqttium";
	m->m_off   = (char *)&cr.m_pqr_maxValQTTopicsInUrl - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for pages that are not "
		"root or have many paths in the url";
	m->m_desc  = "Demotion factor each path in the url. "
		"Score will be demoted by this factor as a percent "
		"multiplied by the number of paths in the url divided "
		"by the max value below. "
		"Generally, the page will not be demoted more than this "
		"value as a percent. "
		"0 means no demotion. "
		"A safe range is from 0 to 0.75. ";
	m->m_cgi   = "pqrpaths";
	m->m_off   = (char *)&cr.m_pqr_demFactPaths - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max value for pages that have many paths in the url";
	m->m_desc  = "Max number of paths in a url. "
		"This should be set to a value representing a very high "
		"number of paths for a url. Lower values increase the "
		"difference between how much each additional path demotes. ";
	m->m_cgi   = "pqrpathsm";
	m->m_off   = (char *)&cr.m_pqr_maxValPaths - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "16";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for larger pages";
	m->m_desc  = "Demotion factor for larger pages. "
		"Page will be penalized by its size times this factor "
		"divided by the max page size below. "
		"Generally, a page will not be demoted more than this "
		"factor as a percent. "
		"0 means no demotion. "
		"A safe range is between 0 and 0.25. ";
	m->m_cgi   = "pqrpgsz";
	m->m_off   = (char *)&cr.m_pqr_demFactPageSize - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max value for larger pages";
	m->m_desc  = "Max page size. "
		"Pages with a size greater than or equal to this will be "
		"demoted by the max amount (the factor above as a percent). ";
	m->m_cgi   = "pqrpgszm";
	m->m_off   = (char *)&cr.m_pqr_maxValPageSize - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "524288";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for non-location specific queries "
		"with a location specific title";
	m->m_desc  = "Demotion factor for non-location specific queries "
		"with a location specific title. "
		"Pages which contain a location in their title which is "
		"not in the query or the gigabits will be demoted by their "
		"population multiplied by this factor divided by the max "
		"place population specified below. "
		"Generally, a page will not be demoted more than this "
		"value as a percent. "
		"0 means no demotion. ";
	m->m_cgi   = "pqrloct";
	m->m_off   = (char *)&cr.m_pqr_demFactLocTitle - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.99";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for non-location specific queries "
		"with a location specific summary";
	m->m_desc  = "Demotion factor for non-location specific queries "
		"with a location specific summary. "
		"Pages which contain a location in their summary which is "
		"not in the query or the gigabits will be demoted by their "
		"population multiplied by this factor divided by the max "
		"place population specified below. "
		"Generally, a page will not be demoted more than this "
		"value as a percent. "
		"0 means no demotion. ";
	m->m_cgi   = "pqrlocs";
	m->m_off   = (char *)&cr.m_pqr_demFactLocSummary - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.95";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demote locations that appear in gigabits";
	m->m_desc  = "Demote locations that appear in gigabits.";
	m->m_cgi   = "pqrlocg";
	m->m_off   = (char *)&cr.m_pqr_demInTopics - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max value for non-location specific queries "
		"with location specific results";
	m->m_desc  = "Max place population. "
		"Places with a population greater than or equal to this "
		"will be demoted to the maximum amount given by the "
		"factor above as a percent. ";
	m->m_cgi   = "pqrlocm";
	m->m_off   = (char *)&cr.m_pqr_maxValLoc - x;
	m->m_type  = TYPE_LONG;
	// charlottesville was getting missed when this was 1M
	m->m_def   = "100000";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for non-html";
	m->m_desc  = "Demotion factor for content type that is non-html. "
		"Pages which do not have an html content type will be "
		"demoted by this factor as a percent. "
		"0 means no demotion. "
		"A safe range is between 0 and 0.35. ";
	m->m_cgi   = "pqrhtml";
	m->m_off   = (char *)&cr.m_pqr_demFactNonHtml - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for xml";
	m->m_desc  = "Demotion factor for content type that is xml. "
		"Pages which have an xml content type will be "
		"demoted by this factor as a percent. "
		"0 means no demotion. "
		"Any value between 0 and 1 is safe if demotion for non-html "
		"is set to 0. Otherwise, 0 should probably be used. ";
	m->m_cgi   = "pqrxml";
	m->m_off   = (char *)&cr.m_pqr_demFactXml - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.95";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for pages with other pages from same "
		"hostname";
	m->m_desc  = "Demotion factor for pages with fewer other pages from "
		"same hostname. "
		"Pages with results from the same host will be "
		"demoted by this factor times each fewer host than the max "
		"value given below, divided by the max value. "
		"Generally, a page will not be demoted more than this "
		"factor as a percent. "
		"0 means no demotion. "
		"A safe range is between 0 and 0.35. ";
	m->m_cgi   = "pqrfsd";
	m->m_off   = (char *)&cr.m_pqr_demFactOthFromHost - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max value for pages with other pages from same "
		"domain";
	m->m_desc  = "Max number of pages from same domain. "
		"Pages which have this many or more pages from the same "
		"domain will not be demoted. "; 
	m->m_cgi   = "pqrfsdm";
	m->m_off   = (char *)&cr.m_pqr_maxValOthFromHost - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "12";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for pages based on datedb date";
	m->m_desc  = "Demotion factor for pages based on datedb date. "
		"Pages will be penalized for being published earlier than the "
		"max date given below. "
		"The older the page, the more it will be penalized based on "
		"the time difference between the page's date and the max date, "
		"divided by the max date. "
		"Generally, a page will not be demoted more than this "
		"value as a percent. "
		"0 means no demotion. "
		"A safe range is between 0 and 0.4. ";
	m->m_cgi   = "pqrdate";
	m->m_off   = (char *)&cr.m_pqr_demFactDatedbDate - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;
	
	m->m_title = "min value for demotion based on datedb date ";
	m->m_desc  = "Pages with a publish date equal to or earlier than "
		"this date will be demoted to the max (the factor above as "
		"a percent). "
		"Use this parm in conjunction with the max value below "
		"to specify the range of dates where demotion occurs. "
		"If you set this parm near the estimated earliest publish "
		"date that occurs somewhat frequently, this method can better "
		"control the additional demotion per publish day. "
		"This number is given as seconds since the epoch, January 1st, "
		"1970 divided by 1000. "
		"0 means use the epoch. ";
	m->m_cgi   = "pqrdatei";
	m->m_off   = (char *)&cr.m_pqr_minValDatedbDate - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "631177"; // Jan 01, 1990 
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max value for demotion based on datedb date ";
	m->m_desc  = "Pages with a publish date greater than or equal to "
		"this value divided by 1000 will not be demoted. "
		"Use this parm in conjunction with the min value above "
		"to specify the range of dates where demotion occurs. "
		"This number is given as seconds before the current date "
		"and time taken from the system clock divided by 1000. "
		"0 means use the current time of the current day. ";
	m->m_cgi   = "pqrdatem";
	m->m_off   = (char *)&cr.m_pqr_maxValDatedbDate - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0"; 
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for pages based on proximity";
	m->m_desc  = "Demotion factor for proximity of query terms in "
		"a document.  The closer together terms occur in a "
		"document, the higher it will score."
		"0 means no demotion. ";
	m->m_cgi   = "pqrprox";
	m->m_off   = (char *)&cr.m_pqr_demFactProximity - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion for pages based on query terms section";
	m->m_desc  = "Demotion factor for where the query terms occur "
		"in the document.  If the terms only occur in a menu, "
		"a link, or a list, the document will be punished."
		"0 means no demotion. ";
	m->m_cgi   = "pqrinsec";
	m->m_off   = (char *)&cr.m_pqr_demFactInSection - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "weight of indexed score on pqr";
	m->m_desc  = "The proportion that the original score affects "
		"its rerank position. A factor of 1 will maintain "
		"the original score, 0 will only use the indexed "
		"score to break ties.";
	m->m_cgi   = "pqrorig";
	m->m_off   = (char *)&cr.m_pqr_demFactOrigScore - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;



	m->m_title = "max value for demotion for pages based on proximity";
	m->m_desc  = "Max summary score where no more demotion occurs above. "
		"Pages with a summary score greater than or equal to this "
		"value will not be demoted. ";
	m->m_cgi   = "pqrproxm";
	m->m_off   = (char *)&cr.m_pqr_maxValProximity - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100000"; 
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "demotion for query being exclusivly in a subphrase";
	m->m_desc  = "Search result which contains the query terms only"
		" as a subphrase of a larger phrase will have its score "
		" reduced by this percent.";
	m->m_cgi   = "pqrspd";
	m->m_off   = (char *)&cr.m_pqr_demFactSubPhrase - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "demotion based on common inlinks";
	m->m_desc  = "Based on the number of inlinks a search results has "
		"which are in common with another search result.";
	m->m_cgi   = "pqrcid";
	m->m_off   = (char *)&cr.m_pqr_demFactCommonInlinks - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = ".5";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of document calls multiplier";
	m->m_desc  = "Allows more results to be gathered in the case of "
		"an index having a high rate of duplicate results.  Generally"
		" expressed as 1.2";
	m->m_cgi   = "ndm";
	m->m_off   = (char *)&cr.m_numDocsMultiplier - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "1.2";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max real time inlinks";
	m->m_desc  = "Limit number of linksdb inlinks requested per result.";
	m->m_cgi   = "mrti";
	m->m_off   = (char *)&cr.m_maxRealTimeInlinks - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10000";
	m->m_group = 0;
	m->m_smin  = 0;
	m->m_smax  = 100000;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "percent topic similar default";
	m->m_desc  = "Like above, but used for deciding when to cluster "
		"results by topic for the news collection.";
	m->m_cgi   = "ptcd";
	m->m_off   = (char *)&cr.m_topicSimilarCutoffDefault - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "50";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;	
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max query terms";
	m->m_desc  = "Do not allow more than this many query terms. Helps "
		"prevent big queries from resource hogging.";
	m->m_cgi   = "mqt";
	m->m_off   = (char *)&cr.m_maxQueryTerms - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "999999"; // now we got synonyms... etc
	m->m_group = 0;
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE; 
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// REFERENCE PAGES CONTROLS
	m->m_title = "number of reference pages to generate";
	m->m_desc  = "What is the number of "
		"reference pages to generate per query? Set to 0 to save "
		"CPU time.";
	m->m_cgi   = "nrp";
	m->m_off   = (char *)&cr.m_refs_numToGenerate - x;
	//m->m_soff  = (char *)&si.m_refs_numToGenerate - y;
	m->m_smaxc = (char *)&cr.m_refs_numToGenerateCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_priv  = 0;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of reference pages to generate";
	m->m_desc  = "What is the number of "
		"reference pages to generate per query? Set to 0 to save "
		"CPU time.";
	m->m_cgi   = "snrp";
	m->m_off  = (char *)&si.m_refs_numToGenerate - y;
	m->m_type  = TYPE_LONG;
	m->m_defOff =(char *)&cr.m_refs_numToGenerate - x;
	m->m_priv  = 0;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "number of reference pages to display";
	m->m_desc  = "What is the number of "
		"reference pages to display per query?";
	m->m_cgi   = "nrpdd";
	m->m_off   = (char *)&cr.m_refs_numToDisplay - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_priv  = 0; // allow the (more) link
	m->m_sprpg = 0; // do not propagate
        m->m_sprpp = 0; // do not propagate
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "docs to scan for reference pages";
	m->m_desc  = "How many search results should we "
		"scan for reference pages per query?";
	m->m_cgi   = "dsrp";
	m->m_off   = (char *)&cr.m_refs_docsToScan - x;
	m->m_smaxc = (char *)&cr.m_refs_docsToScanCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "30";
	m->m_group = 0;
	m->m_priv  = 0;
	m->m_smin  = 0;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m++;

	m->m_title = "min references quality";
	m->m_desc  = "References with page quality below this "
		"will be excluded.  (set to 101 to disable references while "
		"still generating related pages.";
	m->m_cgi   = "mrpq";
	m->m_off   = (char *)&cr.m_refs_minQuality - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "min links per references";
	m->m_desc  = "References need this many links to results to "
		"be included.";
	m->m_cgi   = "mlpr";
	m->m_off   = (char *)&cr.m_refs_minLinksPerReference - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "2";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max linkers to consider for references per page";
	m->m_desc  = "Stop processing referencing pages after hitting this "
		"limit.";
	m->m_cgi   = "mrpl";
	m->m_off   = (char *)&cr.m_refs_maxLinkers - x;
	m->m_smaxc = (char *)&cr.m_refs_maxLinkersCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "500";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "page fetch multiplier for references";
	m->m_desc  = "Use this multiplier to fetch more than the required "
                "number of reference pages.  fetches N * (this parm) "
		"references and displays the top scoring N.";
	m->m_cgi   = "ptrfr";
	m->m_off   = (char *)&cr.m_refs_additionalTRFetch - x;
	m->m_smaxc = (char *)&cr.m_refs_additionalTRFetchCeiling - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "1.5";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of links coefficient";
	m->m_desc  = "A in A * numLinks + B * quality + C * "
		"numLinks/totalLinks.";
        m->m_cgi   = "nlc";
	m->m_off   = (char *)&cr.m_refs_numLinksCoefficient - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "quality coefficient";
	m->m_desc  = "B in A * numLinks + B * quality + C * "
		"numLinks/totalLinks.";
	m->m_cgi   = "qc";
	m->m_off   = (char *)&cr.m_refs_qualityCoefficient - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "link density coefficient";
	m->m_desc  = "C in A * numLinks + B * quality + C * "
		"numLinks/totalLinks.";
	m->m_cgi   = "ldc";
	m->m_off   = (char *)&cr.m_refs_linkDensityCoefficient - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1000";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "add or multipy quality times link density";
	m->m_desc  = "[+|*] in A * numLinks + B * quality [+|*]"
		" C * numLinks/totalLinks.";
	m->m_cgi   = "mrs";
	m->m_off   = (char *)&cr.m_refs_multiplyRefScore - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// reference pages ceiling parameters
	m->m_title = "maximum allowed value for "
		"numReferences parameter";
	m->m_desc  = "maximum allowed value for "
		"numReferences parameter";
	m->m_cgi   = "nrpc";
	m->m_off   = (char *)&cr.m_refs_numToGenerateCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "maximum allowed value for "
		"docsToScanForReferences parameter";
	m->m_desc  = "maximum allowed value for "
		"docsToScanForReferences parameter";
	m->m_cgi   = "dsrpc";
	m->m_off   = (char *)&cr.m_refs_docsToScanCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "maximum allowed value for "
		"maxLinkers parameter";
	m->m_desc  = "maximum allowed value for "
		"maxLinkers parameter";
	m->m_cgi   = "mrplc";
	m->m_off   = (char *)&cr.m_refs_maxLinkersCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "5000";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "maximum allowed value for "
		"additionalTRFetch";
	m->m_desc  = "maximum allowed value for "
		"additionalTRFetch parameter";
	m->m_cgi   = "ptrfrc";
	m->m_off   = (char *)&cr.m_refs_additionalTRFetchCeiling - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "10";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// related pages parameters
	m->m_title = "number of related pages to generate";
	m->m_desc  = "number of related pages to generate.";
	m->m_cgi   = "nrpg";
	m->m_off   = (char *)&cr.m_rp_numToGenerate - x;
	m->m_smaxc = (char *)&cr.m_rp_numToGenerateCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_priv  = 0;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of related pages to display";
	m->m_desc  = "number of related pages to display.";
	m->m_cgi   = "nrpd";
	m->m_off   = (char *)&cr.m_rp_numToDisplay - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_priv  = 0; // allow the (more) link
	m->m_sprpg = 0; // do not propagate
        m->m_sprpp = 0; // do not propagate
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of links to scan for related pages";
	m->m_desc  = "number of links per reference page to scan for related "
		"pages.";
	m->m_cgi   = "nlpd";
	m->m_off   = (char *)&cr.m_rp_numLinksPerDoc - x;
	m->m_smaxc = (char *)&cr.m_rp_numLinksPerDocCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1024";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "min related page quality";
	m->m_desc  = "related pages with a quality lower than this will be "
		"ignored.";
	m->m_cgi   = "merpq";
	m->m_off   = (char *)&cr.m_rp_minQuality - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "30";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "min related page score";
	m->m_desc  = "related pages with an adjusted score lower than this "
		"will be ignored.";
	m->m_cgi   = "merps";
	m->m_off   = (char *)&cr.m_rp_minScore - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "min related page links";
	m->m_desc  = "related pages with less than this number of links"
		" will be ignored.";
	m->m_cgi   = "merpl";
	m->m_off   = (char *)&cr.m_rp_minLinks - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "2";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "coefficient for number of links in related pages score "
		"calculation";
	m->m_desc  = "A in A * numLinks + B * avgLnkrQlty + C * PgQlty"
		" + D * numSRPLinks.";
	m->m_cgi   = "nrplc";
	m->m_off   = (char *)&cr.m_rp_numLinksCoeff - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "coefficient for average linker quality in related pages "
		"score calculation";
	m->m_desc  = "B in A * numLinks + B * avgLnkrQlty + C * PgQlty"
		" + D * numSRPLinks.";
	m->m_cgi   = "arplqc";
	m->m_off   = (char *)&cr.m_rp_avgLnkrQualCoeff - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "coefficient for page quality in related pages "
		"score calculation";
	m->m_desc  = "C in A * numLinks + B * avgLnkrQlty + C * PgQlty"
		" + D * numSRPLinks";
	m->m_cgi   = "qrpc";
	m->m_off   = (char *)&cr.m_rp_qualCoeff - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "coefficient for search result links in related pages "
		"score calculation";
	m->m_desc  = "D in A * numLinks + B * avgLnkrQlty + C * PgQlty"
		" + D * numSRPLinks.";
	m->m_cgi   = "srprpc";
	m->m_off   = (char *)&cr.m_rp_srpLinkCoeff - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of related page summary excerpts";
	m->m_desc  = "What is the maximum number of "
		"excerpts displayed in the summary of a related page?";
	m->m_cgi   = "nrps";
	m->m_off   = (char *)&cr.m_rp_numSummaryLines - x;
	m->m_smaxc = (char *)&cr.m_rp_numSummaryLinesCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_smin  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "highlight query terms in related pages summary";
	m->m_desc  = "Highlight query terms in related pages summary.";
	m->m_cgi   = "hqtirps"; 
	m->m_off   = (char *)&cr.m_rp_doRelatedPageSumHighlight - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "number of characters to display in title before "
		"truncating";
	m->m_desc  = "Truncates a related page title after this many "
		"charaters and adds ...";
	m->m_cgi   = "ttl";
	m->m_off   = (char *)&cr.m_rp_titleTruncateLimit - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "50";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "use results pages as references";
	m->m_desc  = "Use the search results' links in order to generate "
		"related pages.";
	m->m_cgi   = "urar"; 
	m->m_off   = (char *)&cr.m_rp_useResultsAsReferences - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "get related pages from other cluster";
	m->m_desc  = "Say yes here to make Gigablast check another Gigablast "
		"cluster for title rec for related pages. Gigablast will "
		"use the hosts2.conf file in the working directory to "
		"tell it what hosts belong to the other cluster.";
	m->m_cgi   = "erp"; // external related pages
	m->m_off   = (char *)&cr.m_rp_getExternalPages - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "collection for other related pages cluster";
	m->m_desc  = "Gigablast will fetch the related pages title record "
		"from this collection in the other cluster.";
	m->m_cgi   = "erpc"; // external related pages collection
	m->m_off   = (char *)&cr.m_rp_externalColl - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_COLL_LEN;
	m->m_def   = "main";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// relate pages ceiling parameters
	m->m_title = "maximum allowed value for numToGenerate parameter";
	m->m_desc  = "maximum allowed value for numToGenerate parameter";
	m->m_cgi   = "nrpgc";
	m->m_off   = (char *)&cr.m_rp_numToGenerateCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "maximum allowed value for numRPLinksPerDoc parameter";
	m->m_desc  = "maximum allowed value for numRPLinksPerDoc parameter";
	m->m_cgi   = "nlpdc";
	m->m_off   = (char *)&cr.m_rp_numLinksPerDocCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "5000";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "maximum allowed value for numSummaryLines parameter";
	m->m_desc  = "maximum allowed value for numSummaryLines parameter";
	m->m_cgi   = "nrpsc";
	m->m_off   = (char *)&cr.m_rp_numSummaryLinesCeiling - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// import search results controls
	m->m_title = "how many imported results should we insert";
	m->m_desc  = "Gigablast will import X search results from the "
		"external cluster given by hosts2.conf and merge those "
		"search results into the current set of search results. "
		"Set to 0 to disable.";
	m->m_cgi   = "imp";
	m->m_off   = (char *)&cr.m_numResultsToImport - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "imported score weight";
	m->m_desc  = "The score of all imported results will be multiplied "
		"by this number. Since results are mostly imported from "
		"a large collection they will usually have higher scores "
		"because of having more link texts or whatever, so tone it "
		"down a bit to put it on par with the integrating collection.";
	m->m_cgi   = "impw";
	m->m_off   = (char *)&cr.m_importWeight - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = ".80";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "how many linkers must each imported result have";
	m->m_desc  = "The urls of imported search results must be linked to "
		"by at least this many documents in the primary collection.";
	m->m_cgi   = "impl";
	m->m_off   = (char *)&cr.m_minLinkersPerImportedResult - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "3";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "num linkers weight";
	m->m_desc  = "The number of linkers an imported result has from "
		"the base collection is multiplied by this weight and then "
		"added to the final score. The higher this is the more an "
		"imported result with a lot of linkers will be boosted. "
		"Currently, 100 is the max number of linkers permitted.";
	m->m_cgi   = "impnlw";
	m->m_off   = (char *)&cr.m_numLinkerWeight - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "50";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "the name of the collection to import from";
	m->m_desc  = "Gigablast will import X search results from this "
		"external collection and merge them into the current search "
		"results.";
	m->m_cgi   = "impc";
	m->m_off   = (char *)&cr.m_importColl - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_COLL_LEN;
	m->m_def   = "main";
	m->m_group = 0;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max similar results for cluster by topic";
	m->m_desc  = "Max similar results to show when clustering by topic.";
	m->m_cgi   = "ncbt";
	m->m_off   = (char *)&cr.m_maxClusterByTopicResults - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of extra results to get for cluster by topic";
	m->m_desc  = "number of extra results to get for cluster by topic";
	m->m_cgi   = "ntwo";
	m->m_off   = (char *)&cr.m_numExtraClusterByTopicResults - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "Minimum number of in linkers required to consider getting"
		" the title from in linkers";
	m->m_desc  = "Minimum number of in linkers required to consider getting"
	       "	the title from in linkers";
	m->m_cgi   = "mininlinkers";
	m->m_off   = (char *)&cr.m_minTitleInLinkers - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "Max number of in linkers to consider";
	m->m_desc  = "Max number of in linkers to consider for getting in "
		"linkers titles.";
	m->m_cgi   = "maxinlinkers";
	m->m_off   = (char *)&cr.m_maxTitleInLinkers - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "128";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max title len";
	m->m_desc  = "What is the maximum number of "
		"characters allowed in titles displayed in the search "
		"results?";
	m->m_cgi   = "tml";
	m->m_defOff= (char *)&cr.m_titleMaxLen - x;
	m->m_off   = (char *)&si.m_titleMaxLen - y;
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "summary mode";
	m->m_desc  = "0 = old compatibility mode, 1 = UTF-8 mode, "
		"2 = fast ASCII mode, "
		"3 = Ascii Proximity Summary, "
		"4 = Utf8 Proximity Summary, "
		"5 = Ascii Pre Proximity Summary, "
		"6 = Utf8 Pre Proximity Summary:";
	m->m_cgi   = "smd";
	m->m_off   = (char *)&cr.m_summaryMode - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "number of summary excerpts";
	m->m_desc  = "How many summary excerpts to display per search result?";
	m->m_cgi   = "ns";
	m->m_type  = TYPE_LONG;
	m->m_defOff= (char *)&cr.m_summaryMaxNumLines - x;
	m->m_group = 0;
	m->m_off   = (char *)&si.m_numLinesInSummary - y;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "max summary line width";
	m->m_desc  = "&lt;br&gt; tags are inserted to keep the number "
		"of chars in the summary per line at or below this width. "
		"Also affects title. "
		"Strings without spaces that exceed this "
		"width are not split. Has no affect on xml or json feed, "
		"only works on html.";
	m->m_cgi   = "sw";
	m->m_off   = (char *)&si.m_summaryMaxWidth - y;
	m->m_defOff= (char *)&cr.m_summaryMaxWidth - x;
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "max summary excerpt length";
	m->m_desc = "What is the maximum number of "
		"characters allowed per summary excerpt?";
	m->m_cgi   = "smxcpl";
	m->m_off   = (char *)&si.m_summaryMaxNumCharsPerLine - y;
	m->m_defOff= (char *)&cr.m_summaryMaxNumCharsPerLine - x;
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "ip restriction for gigabits";
	m->m_desc  = "Should Gigablast only get one document per IP domain "
		"and per domain for gigabits (related topics) generation?";
	m->m_cgi   = "ipr";
	m->m_off   = (char *)&si.m_ipRestrictForTopics - y;
	m->m_defOff= (char *)&cr.m_ipRestrict - x;
	m->m_type  = TYPE_BOOL;
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "min topics score";
	m->m_desc  = "Gigabits (related topics) with scores below this "
		"will be excluded. Scores range from 0% to over 100%.";
	m->m_cgi   = "mts";
	m->m_defOff= (char *)&cr.m_minTopicScore - x;
	m->m_off   = (char *)&si.m_minTopicScore - y;
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;



	m->m_title = "min gigabit doc count by default";
	m->m_desc  = "How many documents must contain the gigabit "
		"(related topic) in order for it to be displayed.";
	m->m_cgi   = "mdc";
	m->m_defOff= (char *)&cr.m_minDocCount - x;
	m->m_off   = (char *)&si.m_minDocCount - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "2";
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;



	m->m_title = "dedup doc percent for gigabits (related topics)";
	m->m_desc  = "If a document is this percent similar to another "
		"document with a higher score, then it will not contribute "
		"to the gigabit generation.";
	m->m_cgi   = "dsp";
	m->m_defOff= (char *)&cr.m_dedupSamplePercent - x;
	m->m_off   = (char *)&si.m_dedupSamplePercent - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "80";
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	///////////////////////////////////////////
	//  SPIDER PROXY CONTROLS
	//  
	///////////////////////////////////////////

	m->m_title = "always use spider proxies for all collections";
	m->m_desc  = "ALWAYS Use the spider proxies listed below for "
		"spidering. If none are "
		"listed then gb will not use any. Applies to all collections. "
		"If you want to regulate this on a per collection basis then "
		"set this to <b>NO</b> here and adjust the "
		"proxy controls on the "
		"<b>spider controls</b> page. If the list of proxy IPs below "
		"is empty, then of course, no proxies will be used.";
	m->m_cgi   = "useproxyips";
	m->m_xml   = "useSpiderProxies";
	m->m_off   = (char *)&g_conf.m_useProxyIps - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	// hide this for now. just make it a per collection parm.
	m->m_flags = PF_HIDDEN;
	m->m_page  = PAGE_SPIDERPROXIES;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "automatically use spider proxies for all collections";
	m->m_desc  = "AUTOMATICALLY use the spider proxies listed below for "
		"spidering. If none are "
		"listed then gb will not use any. Applies to all collections. "
		"If you want to regulate this on a per collection basis then "
		"set this to <b>NO</b> here and adjust the "
		"proxy controls on the "
		"<b>spider controls</b> page. If the list of proxy IPs below "
		"is empty, then of course, no proxies will be used.";
	m->m_cgi   = "autouseproxyips";
	m->m_xml   = "automaticallyUseSpiderProxies";
	m->m_off   = (char *)&g_conf.m_automaticallyUseProxyIps - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	// hide this for now. just make it a per collection parm.
	m->m_flags = PF_HIDDEN;
	m->m_page  = PAGE_SPIDERPROXIES;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "spider proxy ips";
	m->m_desc  = "List of white space-separated spider proxy IPs. Put "
		"in IP:port format. Example <i>192.0.2.1:80 198.51.100.2:99</i>. "
		"You can also use <i>username:password@192.0.2.1:80</i>. "
		"If a proxy itself times out when downloading through it "
		"it will be perceived as a normal download timeout and the "
		"page will be retried according to the url filters table, so "
		"you  might want to modify the url filters to retry network "
		"errors more aggressively. Search for 'private proxies' on "
		"google to find proxy providers. Try to ensure all your "
		"proxies are on different class C IPs if possible. "
		"That is, the first 3 numbers in the IP addresses are all "
		"different.";
	m->m_cgi   = "proxyips";
	m->m_xml   = "proxyIps";
	m->m_off   = (char *)&g_conf.m_proxyIps - g;
	m->m_type  = TYPE_SAFEBUF; // TYPE_IP;
	m->m_def   = "";
	m->m_flags = PF_TEXTAREA | PF_REBUILDPROXYTABLE;
	m->m_page  = PAGE_SPIDERPROXIES;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "spider proxy test url";
	m->m_desc  = "Download this url every minute through each proxy "
		"listed above to ensure they are up. Typically you should "
		"make this a URL you own so you do not aggravate another "
		"webmaster.";
	m->m_xml   = "proxyTestUrl";
	m->m_cgi   = "proxytesturl";
	m->m_off   = (char *)&g_conf.m_proxyTestUrl - g;
	m->m_type  = TYPE_SAFEBUF;
	m->m_def   = "http://www.gigablast.com/";
	m->m_flags = 0;
	m->m_page  = PAGE_SPIDERPROXIES;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "reset proxy table";
	m->m_desc  = "Reset the proxy statistics in the table below. Makes "
		"all your proxies treated like new again.";
	m->m_cgi   = "resetproxytable";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandResetProxyTable;
	m->m_cast  = 1;
	m->m_page  = PAGE_SPIDERPROXIES;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "squid proxy authorized users";
	m->m_desc  = "Gigablast can also simulate a squid proxy, "
		"complete with "
		"caching. It will forward your request to the proxies you "
		"list above, if any. This list consists of space-separated "
		"<i>username:password</i> items. Leave this list empty "
		"to disable squid caching behaviour. The default cache "
		"size for this is 10MB per shard. Use item *:* to allow "
		"anyone access.";
	m->m_xml   = "proxyAuth";
	m->m_cgi   = "proxyAuth";
	m->m_off   = (char *)&g_conf.m_proxyAuth - g;
	m->m_type  = TYPE_SAFEBUF;
	m->m_def   = "";
	m->m_flags = PF_TEXTAREA;
	m->m_page  = PAGE_SPIDERPROXIES;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max words per gigabit (related topic) by default";
	m->m_desc  = "Maximum number of words a gigabit (related topic) "
		"can have. Affects xml feeds, too.";
	m->m_cgi   = "mwpt";
	m->m_defOff= (char *)&cr.m_maxWordsPerTopic - x;
	m->m_off   = (char *)&si.m_maxWordsPerTopic - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "6";
	m->m_group = 0;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "show images";
	m->m_desc  = "Should we return or show the thumbnail images in the "
		"search results?";
	m->m_cgi   = "showimages";
	m->m_off   = (char *)&si.m_showImages - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_flags = PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "use cache";
	m->m_desc  = "Use 0 if Gigablast should not read or write from "
		"any caches at any level.";
	m->m_def   = "-1";
	m->m_off   = (char *)&si.m_useCache - y;
	m->m_type  = TYPE_CHAR;
	m->m_cgi   = "usecache";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "read from cache";
	m->m_desc  = "Should we read search results from the cache? Set "
		"to false to fix dmoz bug.";
	m->m_cgi   = "rcache";
	m->m_off   = (char *)&si.m_rcache - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_flags = PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "write to cache";
	m->m_desc  = "Use 0 if Gigablast should not write to "
		"any caches at any level.";
	m->m_def   = "-1";
	m->m_off   = (char *)&si.m_wcache - y;
	m->m_type  = TYPE_CHAR;
	m->m_cgi   = "wcache";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "max serp docid";
	m->m_desc  = "Start displaying results after this score/docid pair. "
		"Used by widget to append results to end when index is "
		"volatile.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_minSerpDocId - y;
	m->m_type  = TYPE_LONG_LONG;
	m->m_cgi   = "minserpdocid";
	m->m_flags = PF_API;
	m->m_smin  = 0;
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "max serp score";
	m->m_desc  = "Start displaying results after this score/docid pair. "
		"Used by widget to append results to end when index is "
		"volatile.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_maxSerpScore - y;
	m->m_type  = TYPE_DOUBLE;
	m->m_cgi   = "maxserpscore";
	m->m_flags = PF_API;
	m->m_smin  = 0;
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "restrict search to this url";
	m->m_desc  = "Does a url: query.";
	m->m_off   = (char *)&si.m_url - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "url";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "restrict search to pages that link to this url";
	m->m_desc  = "The url which the pages must link to.";
	m->m_off   = (char *)&si.m_link - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "link";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "search for this phrase quoted";
	m->m_desc  = "The phrase which will be quoted in the query. From the "
		"advanced search page, adv.html.";
	m->m_off   = (char *)&si.m_quote1 - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "quotea";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "search for this second phrase quoted";
	m->m_desc  = "The phrase which will be quoted in the query. From the "
		"advanced search page, adv.html.";
	m->m_off   = (char *)&si.m_quote2 - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "quoteb";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "restrict results to these sites";
	m->m_desc  = "Returned results will have URLs from these "
		"space-separated list of sites. Can have up to 200 sites. "
		"A site can include sub folders. This is allows you to build "
		"a <a href=\"/cts.html\">Custom Topic Search Engine</a>.";
	m->m_off   = (char *)&si.m_sites - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_cgi   = "sites";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_sprpg = 1;
	m->m_sprpp = 1;
	m++;

	m->m_title = "require these query terms";
	m->m_desc  = "Returned results will have all the words in X. "
		"From the advanced search page, adv.html.";
	m->m_off   = (char *)&si.m_plus - y;
	m->m_def   = NULL;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "plus";
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "avoid these query terms";
	m->m_desc  = "Returned results will NOT have any of the words in X. "
		"From the advanced search page, adv.html.";
	m->m_off   = (char *)&si.m_minus - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "minus";
	//m->m_size  = 500;
	m->m_sprpg = 0;
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "format of the returned search results";
	m->m_desc  = "Can be html, xml or json to get results back in that "
		"format.";
	m->m_def   = "html";
	m->m_off   = (char *)&si.m_formatStr - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_cgi   = "format";
	m->m_flags = PF_NOAPI; // alread in the api, so don't repeat
	m++;

	m->m_title = "family filter";
	m->m_desc  = "Remove objectionable results if this is enabled.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_familyFilter - y;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_cgi   = "ff";
	m++;


	m->m_title = "highlight query terms in summaries";
	m->m_desc  = "Use to disable or enable "
		"highlighting of the query terms in the summaries.";
	m->m_def   = "1";
	m->m_off   = (char *)&si.m_doQueryHighlighting - y;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "qh";
	m->m_smin  = 0;
	m->m_smax  = 8;
	m->m_sprpg = 1; // turn off for now
	m->m_sprpp = 1;
	m->m_flags = PF_API;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;


	m->m_title = "cached page highlight query";
	m->m_desc  = "Highlight the terms in this query instead.";
	m->m_def   = NULL;
	m->m_off   = (char *)&si.m_highlightQuery - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_cgi   = "hq";
	m->m_sprpg = 0; // no need to propagate this one
	m->m_sprpp = 0;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "Query match offsets";
	m->m_desc  = "Return a list of the offsets of each query word "
		"actually matched in the document.  1 means byte offset, "
		"and 2 means word offset.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_queryMatchOffsets - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "qmo";
	m->m_smin  = 0;
	m->m_smax  = 2;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "boolean status";
	m->m_desc  = "Can be 0 or 1 or 2. 0 means the query is NOT boolean, "
		"1 means the query is boolean and 2 means to auto-detect.";
	m->m_def   = "2";
	m->m_off   = (char *)&si.m_boolFlag - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "bq";
	m->m_smin  = 0;
	m->m_smax  = 2;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "meta tags to display";
	m->m_desc  = "A space-separated string of <b>meta tag names</b>. "
		"Do not forget to url-encode the spaces to +'s or %%20's. "
		"Gigablast will extract the contents of these specified meta "
		"tags out of the pages listed in the search results and "
		"display that content after each summary. i.e. "
		"<i>&dt=description</i> will display the meta description of "
		"each search result. <i>&dt=description:32+keywords:64</i> "
		"will display the meta description and meta keywords of each "
		"search result and limit the fields to 32 and 64 characters "
		"respectively. When used in an XML feed the <i>&lt;display "
		"name=\"meta_tag_name\"&gt;meta_tag_content&lt;/&gt;</i> XML "
		"tag will be used to convey each requested meta tag's "
		"content.";
	m->m_off   = (char *)&si.m_displayMetas - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_cgi   = "dt";
	//m->m_size  = 3000;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;
	
	m->m_title = "niceness";
	m->m_desc  = "Can be 0 or 1. 0 is usually a faster, high-priority "
		"query, 1 is a slower, lower-priority query.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_niceness - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "niceness";
	m->m_smin  = 0;
	m->m_smax  = 1;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "debug flag";
	m->m_desc  = "Is 1 to log debug information, 0 otherwise.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_debug - y;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "debug";
	//m->m_priv  = 1;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "return number of docs per topic";
	m->m_desc  = "Use 1 if you want Gigablast to return the number of "
		"documents in the search results that contained each topic "
		"(gigabit).";
	m->m_def   = "1";
	m->m_off   = (char *)&si.m_returnDocIdCount - y;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "rdc";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "return docids per topic";
	m->m_desc  = "Use 1 if you want Gigablast to return the list of "
		"docIds from the search results that contained each topic "
		"(gigabit).";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_returnDocIds - y;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "rd";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "return popularity per topic";
	m->m_desc  = "Use 1 if you want Gigablast to return the popularity "
		"of each topic (gigabit).";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_returnPops - y;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "rp";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "return docids only";
	m->m_desc  = "Is 1 to return only docids as query results.";
	m->m_def   = "0";
	m->m_off   = (char *)&si.m_docIdsOnly - y;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "dio";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "image url";
	m->m_desc  = "The url of an image to co-brand on the search "
		"results page.";
	m->m_off   = (char *)&si.m_imgUrl - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_def   = NULL;
	//m->m_size  = 512;
	m->m_cgi   = "iu";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "image link";
	m->m_desc  = "The hyperlink to use on the image to co-brand on "
		"the search results page.";
	m->m_off   = (char *)&si.m_imgLink - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_def   = NULL;
	m->m_cgi   = "ix";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "image width";
	m->m_desc  = "The width of the image on the search results page.";
	m->m_off   = (char *)&si.m_imgWidth - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "iw";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = "200";
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "image height";
	m->m_desc  = "The height of the image on the search results "
		"page.";
	m->m_off   = (char *)&si.m_imgHeight - y;
	m->m_type  = TYPE_LONG;
	m->m_cgi   = "ih";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_def   = "200";
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "admin override";
	m->m_desc  = "admin override";
	m->m_off   = (char *)&si.m_isMasterAdmin - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_cgi   = "admin";
	m->m_sprpg = 1; // propagate on GET request
        m->m_sprpp = 1; // propagate on POST request
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	// prepend to query
	m->m_title = "prepend";
	m->m_desc  = "prepend this to the supplied query followed by a |.";
	m->m_off   = (char *)&si.m_prepend - y;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_cgi   = "prepend";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "GB Country";
	m->m_desc  = "Country code to restrict search";
	m->m_off   = (char *)&si.m_gbcountry - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_def   = NULL;
	m->m_cgi   = "gbcountry";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "show banned pages";
	m->m_desc  = "show banned pages";
	m->m_off   = (char *)&si.m_showBanned - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_cgi   = "sb";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "queryCharset";
	m->m_desc  = "Charset in which the query is encoded";
	m->m_off   = (char *)&si.m_queryCharset - y;
	m->m_type  = TYPE_CHARPTR;//STRING;
	m->m_def   = "utf-8";
	m->m_cgi   = "qcs";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	// buzz
	m->m_title = "display inlinks";
	m->m_desc  = "Display all inlinks of each result.";
	m->m_off   = (char *)&si.m_displayInlinks - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_cgi   = "inlinks";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	// buzz
	m->m_title = "display outlinks";
	m->m_desc  = "Display all outlinks of each result. outlinks=1 "
		"displays only external outlinks. outlinks=2 displays "
		"external and internal outlinks.";
	m->m_off   = (char *)&si.m_displayOutlinks - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_cgi   = "outlinks";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_NOAPI;
	m++;

	// buzz
	m->m_title = "display term frequencies";
	m->m_desc  = "Display Terms and Frequencies in results.";
	m->m_off   = (char *)&si.m_displayTermFreqs - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_cgi   = "tf";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	// buzz
	m->m_title = "spider results";
	m->m_desc  = "Results of this query will be forced into the spider "
		"queue for reindexing.";
	m->m_off   = (char *)&si.m_spiderResults - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_cgi   = "spiderresults";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	// buzz
	m->m_title = "spider result roots";
	m->m_desc  = "Root urls of the results of this query will be forced "
		"into the spider queue for reindexing.";
	m->m_off   = (char *)&si.m_spiderResultRoots - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_cgi   = "spiderresultroots";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	// buzz
	m->m_title = "just mark clusterlevels";
	m->m_desc  = "Check for deduping, but just mark the cluster levels "
		"and the doc deduped against, don't remove the result.";
	m->m_off   = (char *)&si.m_justMarkClusterLevels - y;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_cgi   = "jmcl";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m++;

	m->m_title = "include cached copy of page";
	m->m_desc  = "Will cause a cached copy of content to be returned "
		"instead of summary.";
	m->m_off   = (char *)&si.m_includeCachedCopy - y;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_cgi   = "icc";
	m->m_page  = PAGE_RESULTS;
	m->m_obj   = OBJ_SI;
	m->m_flags = PF_API;
	m++;

	//////////////
	// END /search
	//////////////


	//////////
	// PAGE GET (cached web pages)
	///////////
	m->m_title = "docId";
	m->m_desc  = "The docid of the cached page to view.";
	m->m_off   = (char *)&gr.m_docId - (char *)&gr;
	m->m_type  = TYPE_LONG_LONG;
	m->m_page  = PAGE_GET;
	m->m_obj   = OBJ_GBREQUEST; // generic request class
	m->m_def   = "0";
	m->m_cgi   = "d";
	m->m_flags = PF_API | PF_REQUIRED;
	m++;


	m->m_title = "url";
	m->m_desc  = "Instead of specifying a docid, you can get the "
		"cached webpage by url as well.";
	m->m_off   = (char *)&gr.m_url - (char *)&gr;
	m->m_type  = TYPE_CHARPTR; // reference into the HttpRequest
	m->m_page  = PAGE_GET;
	m->m_obj   = OBJ_GBREQUEST; // generic request class
	m->m_def   = NULL;
	m->m_cgi   = "url";
	m->m_flags = PF_API | PF_REQUIRED;
	m++;

	m->m_title = "collection";
	m->m_desc  = "Get the cached page from this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_GET;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m->m_type  = TYPE_CHARPTR;//SAFEBUF;
	m->m_def   = NULL;
	m->m_flags = PF_REQUIRED | PF_API;
	m++;

	m->m_title = "strip";
	m->m_desc  = "Is 1 or 2 two strip various tags from the "
		"cached content.";
	m->m_off   = (char *)&gr.m_strip - (char *)&gr;
	m->m_page  = PAGE_GET;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_cgi   = "strip";
	m->m_def   = "0";
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_API;
	m++;

	m->m_title = "include header";
	m->m_desc  = "Is 1 to include the Gigablast header at the top of "
		"the cached page, 0 to exclude the header.";
	m->m_def   = "1";
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_GET;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_cgi   = "ih";
	m->m_off   = (char *)&gr.m_includeHeader - (char *)&gr;
	m->m_flags = PF_API;
	m++;

	m->m_title = "query";
	m->m_desc  = "Highlight this query in the page.";
	m->m_def   = "";
	m->m_type  = TYPE_CHARPTR;
	m->m_page  = PAGE_GET;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_cgi   = "q";
	m->m_off   = (char *)&gr.m_query - (char *)&gr;
	m->m_flags = PF_API;
	m++;

	// Process.cpp calls Msg28::massConfig with &haspower=[0|1] to 
	// indicate power loss or coming back on from a power loss
	m->m_title = "power on status notificiation";
	m->m_desc  = "Indicates power is back on.";
	m->m_cgi   = "poweron";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandPowerOnNotice;
	m->m_cast  = 0;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "power off status notificiation";
	m->m_desc  = "Indicates power is off.";
	m->m_cgi   = "poweroff";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandPowerOffNotice;
	m->m_cast  = 0;
	m->m_page  = PAGE_NONE;
	m->m_obj   = OBJ_CONF;
	m++;

	//////////////
	// END PAGE_GET
	//////////////


	///////////////////////////////////////////
	// MASTER CONTROLS
	///////////////////////////////////////////

	m->m_title = "spidering enabled";
	m->m_desc  = "Controls all spidering for all collections";
	m->m_cgi   = "se";
	m->m_off   = (char *)&g_conf.m_spideringEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "injections enabled";
	m->m_desc  = "Controls injecting for all collections";
	m->m_cgi   = "injen";
	m->m_off   = (char *)&g_conf.m_injectionsEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "querying enabled";
	m->m_desc  = "Controls querying for all collections";
	m->m_cgi   = "qryen";
	m->m_off   = (char *)&g_conf.m_queryingEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "return results even if a shard is down";
	m->m_desc  = "If you turn this off then Gigablast will return "
		"an error message if a shard was down and did not return "
		"results for a query. The XML and JSON feed let's you know "
		"when a shard is down and will give you the results back "
		"any way, but if you would rather have just and error message "
		"and no results, then set then set this to 'NO'.";
	m->m_cgi   = "rra";
	m->m_off   = (char *)&g_conf.m_returnResultsAnyway - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max mem";
	m->m_desc  = "Mem available to this process. May be exceeded due "
		"to fragmentation.";
	m->m_cgi   = "maxmem";
	m->m_off   = (char *)&g_conf.m_maxMem - g;
	m->m_def   = "8000000000";
	m->m_obj   = OBJ_CONF;
	m->m_page  = PAGE_MASTER; // PAGE_NONE;
	m->m_type  = TYPE_LONG_LONG;
	m++;

	
	m->m_title = "max total spiders";
	m->m_desc  = "What is the maximum number of web "
		"pages the spider is allowed to download "
		"simultaneously for ALL collections PER HOST? Caution: "
		"raising this too high could result in some Out of Memory "
		"(OOM) errors. The hard limit is currently 300. Each "
		"collection has its own limit in the <i>spider controls</i> "
		"that you may have to increase as well.";
	m->m_cgi   = "mtsp";
	m->m_off   = (char *)&g_conf.m_maxTotalSpiders - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "add url enabled";
	m->m_desc  = "Can people use the add url interface to add urls "
		"to the index?";
	m->m_cgi   = "ae";
	m->m_off   = (char *)&g_conf.m_addUrlEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "use collection passwords";
	m->m_desc  = "Should collections have individual password settings "
		"so different users can administrer different collections? "
		"If not the only the master passwords and IPs will be able "
		"to administer any collection.";
	m->m_cgi   = "ucp";
	m->m_off   = (char *)&g_conf.m_useCollectionPasswords - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

#ifndef PRIVACORE_SAFE_VERSION
	m->m_title = "allow cloud users";
	m->m_desc  = "Can guest users create and administer "
		"a collection? Limit: 1 "
		"collection per IP address. This is mainly for doing "
		"demos on the gigablast.com domain.";
	m->m_cgi   = "acu";
	m->m_off   = (char *)&g_conf.m_allowCloudUsers - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;
#endif

	m->m_title = "auto save frequency";
	m->m_desc  = "Save data in memory to disk after this many minutes "
		"have passed without the data having been dumped or saved "
		"to disk. Use 0 to disable.";
	m->m_cgi   = "asf";
	m->m_off   = (char *)&g_conf.m_autoSaveFrequency - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "5";
	m->m_units = "mins";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max http sockets";
	m->m_desc  = "Maximum sockets available to serve incoming HTTP "
		"requests. Too many outstanding requests will increase "
		"query latency. Excess requests will simply have their "
		"sockets closed.";
	m->m_cgi   = "ms";
	m->m_off   = (char *)&g_conf.m_httpMaxSockets - g;
	m->m_type  = TYPE_LONG;
	// up this some, am seeing sockets closed because of using gb
	// as a cache...
	m->m_def   = "300";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max https sockets";
	m->m_desc  = "Maximum sockets available to serve incoming HTTPS "
		"requests. Like max http sockets, but for secure sockets.";
	m->m_cgi   = "mss";
	m->m_off   = (char *)&g_conf.m_httpsMaxSockets - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "spider user agent";
	m->m_desc  = "Identification seen by web servers when "
		"the Gigablast spider downloads their web pages. "
		"It is polite to insert a contact email address here so "
		"webmasters that experience problems from the Gigablast "
		"spider have somewhere to vent.";
	m->m_cgi   = "sua";
	m->m_off   = (char *)&g_conf.m_spiderUserAgent - g;
	m->m_type  = TYPE_STRING;
	m->m_size  = USERAGENTMAXSIZE;
	m->m_def   = "GigablastOpenSource/1.0";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "bot name";
	m->m_desc  = "Bot name used when checking robots.txt and metatags for specific allow/deny rules.";
	m->m_cgi   = "botname";
	m->m_off   = (char *)&g_conf.m_spiderBotName - g;
	m->m_type  = TYPE_STRING;
	m->m_size  = USERAGENTMAXSIZE;
	m->m_def   = "gigablastopensource";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

        m->m_title = "use temporary cluster";
        m->m_desc  = "Used by proxy to point to a temporary cluster while the "
		"original cluster is updated with a new binary. The "
		"temporary cluster is the same as the original cluster but "
		"the ports are all incremented by one from what is in "
		"the hosts.conf. This should ONLY be used for the proxy.";
        m->m_cgi   = "aotp";
        m->m_off   = (char *)&g_conf.m_useTmpCluster - g;
        m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
        m++;

	m->m_title = "init QA tests";
	m->m_desc  = "If initiated gb performs some integrity tests "
		"to ensure injecting, spidering and searching works "
		"properly. Uses ./test/ subdirectory. Injects "
		"urls in ./test/inject.txt. Spiders urls "
		"in ./test/spider.txt. "
		"Each of those two files is essentially a simple format of "
		"a url followed by the http reply received from the server "
		"for that url. "
		// TODO: generate these files
		;
	m->m_cgi   = "qasptei";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandSpiderTestInit;
	m->m_def   = "1";
	m->m_cast  = 1;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "init parser test run";
	m->m_desc  = "If enabled gb injects the urls in the "
		"./test-parser/urls.txt "
		"file and outputs ./test-parser/qa.html";
	m->m_cgi   = "qaptei";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandParserTestInit;
	m->m_def   = "1";
	m->m_cast  = 1;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "init spider test run";
	m->m_desc  = "If enabled gb injects the urls in "
		"./test-spider/spider.txt "
		"and spiders links.";
	m->m_cgi   = "qasptei";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandSpiderTestInit;
	m->m_def   = "1";
	m->m_cast  = 1;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "continue spider test run";
	m->m_desc  = "Resumes the test.";
	m->m_cgi   = "qaspter";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandSpiderTestCont;
	m->m_def   = "1";
	m->m_cast  = 1;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "qa search test enabled";
	m->m_desc  = "If enabled gb does the search queries in "
		"./test-search/queries.txt and compares to the last run and "
		"outputs the diffs for inspection and validation.";
	m->m_cgi   = "qasste";
	m->m_off   = (char *)&g_conf.m_testSearchEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	//m->m_cast  = 0;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "save";
	m->m_desc  = "Saves in-memory data for ALL hosts. Does Not exit.";
	m->m_cgi   = "js";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandJustSave;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "save & exit";
	m->m_desc  = "Saves the data and exits for ALL hosts.";
	m->m_cgi   = "save";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandSaveAndExit;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

#ifndef PRIVACORE_SAFE_VERSION
	m->m_title = "rebalance shards";
	m->m_desc  = "Tell all hosts to scan all records in all databases, "
		"and move "
		"records to the shard they belong to. You only need to run "
		"this if Gigablast tells you to, when you are changing "
		"hosts.conf to add or remove more nodes/hosts.";
	m->m_cgi   = "rebalance";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandRebalance;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;
#endif

	m->m_title = "dump to disk";
	m->m_desc  = "Flushes all records in memory to the disk on all hosts.";
	m->m_cgi   = "dump";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandDiskDump;
	m->m_cast  = 1;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "force reclaim";
	m->m_desc  = "Force reclaim of doledb mem.";
	m->m_cgi   = "forceit";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandForceIt;
	m->m_cast  = 1;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m++;

	m->m_title = "tight merge posdb";
	m->m_desc  = "Merges all outstanding posdb (index) files.";
	m->m_cgi   = "pmerge";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandMergePosdb;
	m->m_cast  = 1;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "tight merge titledb";
	m->m_desc  = "Merges all outstanding titledb (web page cache) files.";
	m->m_cgi   = "tmerge";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandMergeTitledb;
	m->m_cast  = 1;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "tight merge spiderdb";
	m->m_desc  = "Merges all outstanding spiderdb files.";
	m->m_cgi   = "spmerge";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandMergeSpiderdb;
	m->m_cast  = 1;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


        m->m_title = "tight merge linkdb";
        m->m_desc  = "Merges all outstanding linkdb files.";
        m->m_cgi   = "lmerge";
        m->m_type  = TYPE_CMD;
        m->m_func  = CommandMergeLinkdb;
        m->m_cast  = 1;
        m->m_group = 0;
        m->m_page  = PAGE_MASTER;
        m->m_obj   = OBJ_CONF;
        m++;



	m->m_title = "clear kernel error message";
	m->m_desc  = "Clears the kernel error message. You must do this "
		"to stop getting email alerts for a kernel ring buffer "
		"error alert.";
	m->m_cgi   = "clrkrnerr";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandClearKernelError;
	m->m_cast  = 1;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "disk page cache off";
	m->m_desc  = "Disable all disk page caches to save mem for "
		"tmp cluster. Run "
		"gb cacheoff to do for all hosts.";
	m->m_cgi   = "dpco";
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandDiskPageCacheOff;
	m->m_cast  = 1;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "do stripe balancing";
	m->m_desc  = "Stripe #n contains twin #n from each group. Doing "
		"stripe balancing helps prevent too many query requests "
		"coming into one host. This parm is only for the proxy. "
		"Stripe balancing is done by default unless the parm is "
		"disabled on the proxy in which case it appends a "
		"&dsb=0 to the query url it sends to the host. The proxy "
		"alternates to which host it forwards the incoming query "
		"based on the stripe. It takes the number of query terms in "
		"the query into account to make a more even balance.";
	m->m_cgi   = "dsb";
	m->m_off   = (char *)&g_conf.m_doStripeBalancing - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "is live cluster";
	m->m_desc  = "Is this cluster part of a live production cluster? "
		"If this is true we make sure that elvtune is being "
		"set properly for best performance, otherwise, gb will "
		"not startup.";
	m->m_cgi   = "live";
	m->m_off   = (char *)&g_conf.m_isLive - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	// we use wikipedia cluster for quick categorization
	m->m_title = "is wikipedia cluster";
	m->m_desc  = "Is this cluster just used for indexing wikipedia pages?";
	m->m_cgi   = "iswiki";
	m->m_off   = (char *)&g_conf.m_isWikipedia - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


        m->m_title = "ask for gzipped docs when downloading";
        m->m_desc  = "If this is true, gb will send Accept-Encoding: gzip "
		"to web servers when doing http downloads. It does have "
		"a tendency to cause out-of-memory errors when you enable "
		"this, so until that is fixed better, it's probably a good "
		"idea to leave this disabled.";
        m->m_cgi   = "afgdwd";
        m->m_off   = (char *)&g_conf.m_gzipDownloads - g;
        m->m_type  = TYPE_BOOL;
	// keep this default off because it seems some pages are huge
	// uncomressed causing OOM errors and possibly corrupting stuff?
	// not sure exactly, but i don't like going OOM. so maybe until
	// that is fixed leave this off.
        m->m_def   = "0";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
        m++;
	
	m->m_title = "search results cache max age";
	m->m_desc = "How many seconds should we cache a search results "
		"page for?";
	m->m_cgi  = "srcma";
	m->m_off  = (char *)&g_conf.m_searchResultsMaxCacheAge - g;
	m->m_def  = "10800"; // 3 hrs
	m->m_type = TYPE_LONG;
	m->m_units = "seconds";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max heartbeat delay in milliseconds";
	m->m_desc  = "If a heartbeat is delayed this many milliseconds "
		"dump a core so we can see where the CPU was. "
		"Logs 'db: missed heartbeat by %"INT64" ms'. "
		"Use 0 or less to disable.";
	m->m_cgi   = "mhdms";
	m->m_off   = (char *)&g_conf.m_maxHeartbeatDelay - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_flags = PF_CLONE; // PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max delay before logging a callback or handler";
	m->m_desc  = "If a call to a message callback or message handler "
		"in the udp server takes more than this many milliseconds, "
		"then log it. "
		"Logs 'udp: Took %"INT64" ms to call callback for msgType="
		"0x%hhx niceness=%"INT32"'. "
		"Use -1 or less to disable the logging.";
	m->m_cgi   = "mdch";
	m->m_off   = (char *)&g_conf.m_maxCallbackDelay - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "-1";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "sendmail IP";
	m->m_desc  = "We send crawlbot notification emails to this sendmail "
		"server which forwards them to the specified email address.";
		m->m_cgi   = "smip";
	m->m_off   = (char *)&g_conf.m_sendmailIp - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "";
	m->m_size  = MAX_MX_LEN;
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send email alerts";
	m->m_desc  = "Sends emails to admin if a host goes down.";
	m->m_cgi   = "sea";
	m->m_off   = (char *)&g_conf.m_sendEmailAlerts - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "delay non critical email alerts";
	m->m_desc  = "Do not send email alerts about dead hosts to "
		"anyone except sysadmin@gigablast.com between the times "
		"given below unless all the twins of the dead host are "
		"also dead. Instead, wait till after if the host "
		"is still dead. ";
	m->m_cgi   = "dnca";
	m->m_off   = (char *)&g_conf.m_delayNonCriticalEmailAlerts - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "cluster name";
	m->m_desc  = "Email alerts will include the cluster name";
	m->m_cgi   = "cn";
	m->m_off   = (char *)&g_conf.m_clusterName - g;
	m->m_type  = TYPE_STRING;
	m->m_size  = 32;
	m->m_def   = "unspecified";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;

	m->m_title = "spider round start time";
	m->m_desc  = "When the next spider round starts. If you force this to "
		"zero it sets it to the current time. That way you can "
		"respider all the urls that were already spidered, and urls "
		"that were not yet spidered in the round will still be "
		"spidered.";
	m->m_cgi   = "spiderRoundStart";
	m->m_size  = 0;
	m->m_off   = (char *)&cr.m_spiderRoundStartTime - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_HIDDEN | PF_REBUILDURLFILTERS ;
	m++;

	// DIFFBOT:
	// this http parm actually ads the "forceround" parm to the parmlist
	// below with the appropriate args.
	m->m_title = "manually restart a spider round";
	m->m_desc  = "Updates round number and resets local processed "
		"and crawled counts to 0.";
	m->m_cgi   = "roundStart";
	m->m_type  = TYPE_CMD;
	m->m_func  = NULL;
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_HIDDEN;
	m++;

	// DIFFBOT:
	// . this is sent to each shard by issuing a "&roundStart=1" cmd
	// . similar to the "addcoll" cmd we add args to it and make it
	//   the "forceround" cmd parm and add THAT to the parmlist.
	//   so "roundStart=1" is really an alias for us.
	m->m_title = "manually restart a spider round on shard";
	m->m_desc  = "Updates round number and resets local processed "
		"and crawled counts to 0.";
	m->m_cgi   = "forceround";
	//m->m_off   = (char *)&cr.m_spiderRoundStartTime - x;
	m->m_type  = TYPE_CMD;
	m->m_func  = CommandForceNextSpiderRound;
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_HIDDEN | PF_REBUILDURLFILTERS ;
	m++;

	m->m_title = "spider round num";
	m->m_desc  = "The spider round number.";
	m->m_cgi   = "spiderRoundNum";
	m->m_off   = (char *)&cr.m_spiderRoundNum - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_HIDDEN ;
	m++;

	m->m_title = "send email alerts to sysadmin";
	m->m_desc  = "Sends to sysadmin@gigablast.com.";
	m->m_cgi   = "seatsa";
	m->m_off   = (char *)&g_conf.m_sendEmailAlertsToSysadmin - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dead host timeout";
	m->m_desc  = "Consider a host in the Gigablast network to be dead if "
		"it does not respond to successive pings for this number of "
		"seconds. Gigablast does not send requests to dead hosts. "
		"Outstanding requests may be re-routed to a twin.";
	m->m_cgi   = "dht";
	m->m_off   = (char *)&g_conf.m_deadHostTimeout - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "4000";
	m->m_units = "milliseconds";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send email timeout";
	m->m_desc  = "Send an email after a host has not responded to "
		"successive pings for this many milliseconds.";
	m->m_cgi   = "set";
	m->m_off   = (char *)&g_conf.m_sendEmailTimeout - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "62000";
	m->m_priv  = 2;
	m->m_units = "milliseconds";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "ping spacer";
	m->m_desc  = "Wait this many milliseconds before pinging the next "
		"host. Each host pings all other hosts in the network.";
	m->m_cgi   = "ps";
	m->m_off   = (char *)&g_conf.m_pingSpacer - g;
	m->m_min   = 50; // i've seen values of 0 hammer the cpu
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_units = "milliseconds";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "query success rate threshold";
	m->m_desc  = "Send email alerts when query success rate goes below "
		"this threshold. (percent rate between 0.0 and 1.0)";
	m->m_cgi   = "qsrt";
	m->m_off   = (char *)&g_conf.m_querySuccessThreshold - g;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.850000";
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "average query latency threshold";
	m->m_desc  = "Send email alerts when average query latency goes above "
		"this threshold. (in seconds)";
	m->m_cgi   = "aqpst";
	m->m_off   = (char *)&g_conf.m_avgQueryTimeThreshold - g;
	m->m_type  = TYPE_FLOAT;
	// a titlerec fetch times out after 2 seconds and is re-routed
	m->m_def   = "2.000000";
	m->m_priv  = 2;
	m->m_units = "seconds";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "number of query times in average";
	m->m_desc  = "Record this number of query times before calculating "
		"average query latency.";
	m->m_cgi   = "nqt";
	m->m_off   = (char *)&g_conf.m_numQueryTimes - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "300";
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "max corrupt index lists";
	m->m_desc  = "If we reach this many corrupt index lists, send "
		"an admin email.  Set to -1 to disable.";
	m->m_cgi   = "mcil";
	m->m_off   = (char *)&g_conf.m_maxCorruptLists - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "5";
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_flags = PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max hard drive temperature";
	m->m_desc  = "At what temperature in Celsius should we send "
		"an email alert if a hard drive reaches it?";
	m->m_cgi   = "mhdt";
	m->m_off   = (char *)&g_conf.m_maxHardDriveTemp - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "45";
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "error string 1";
	m->m_desc  = "Look for this string in the kernel buffer for sending "
		"email alert. Useful for detecting some strange "
		"hard drive failures that really slow performance.";
	m->m_cgi   = "errstrone";
	m->m_off   = (char *)&g_conf.m_errstr1 - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "I/O error";
	m->m_size  = MAX_URL_LEN;
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "error string 2";
	m->m_desc  = "Look for this string in the kernel buffer for sending "
		"email alert. Useful for detecting some strange "
		"hard drive failures that really slow performance.";
	m->m_cgi   = "errstrtwo";
	m->m_off   = (char *)&g_conf.m_errstr2 - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "";
	m->m_size  = MAX_URL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "error string 3";
	m->m_desc  = "Look for this string in the kernel buffer for sending "
		"email alert. Useful for detecting some strange "
		"hard drive failures that really slow performance.";
	m->m_cgi   = "errstrthree";
	m->m_off   = (char *)&g_conf.m_errstr3 - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "";
	m->m_size  = MAX_URL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send email alerts to email 1";
	m->m_desc  = "Sends to email address 1 through email server 1.";
	m->m_cgi   = "seatone";
	m->m_off   = (char *)&g_conf.m_sendEmailAlertsToEmail1 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send parm change email alerts to email 1";
	m->m_desc  = "Sends to email address 1 through email server 1 if "
		"any parm is changed.";
	m->m_cgi   = "seatonep";
	m->m_off   = (char *)&g_conf.m_sendParmChangeAlertsToEmail1 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email server 1";
	m->m_desc  = "Connects to this IP or hostname "
		"directly when sending email 1. "
		"Use <i>apt-get install sendmail</i> to install sendmail "
		"on that IP or hostname. Add <i>From:10.5 RELAY</i> to "
		"/etc/mail/access to allow sendmail to forward email it "
		"receives from gigablast if gigablast hosts are on the "
		"10.5.*.* IPs. Then run <i>/etc/init.d/sendmail restart</i> "
		"as root to pick up those changes so sendmail will forward "
		"Gigablast's email to the email address you give below.";
	m->m_cgi   = "esrvone";
	m->m_off   = (char *)&g_conf.m_email1MX - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "127.0.0.1";
	m->m_size  = MAX_MX_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email address 1";
	m->m_desc  = "Sends to this address when sending email 1 ";
	m->m_cgi   = "eaddrone";
	m->m_off   = (char *)&g_conf.m_email1Addr - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "4081234567@vtext.com";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "from email address 1";
	m->m_desc  = "The from field when sending email 1 ";
	m->m_cgi   = "efaddrone";
	m->m_off   = (char *)&g_conf.m_email1From - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "sysadmin@mydomain.com";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send email alerts to email 2";
	m->m_desc  = "Sends to email address 2 through email server 2.";
	m->m_cgi   = "seattwo";
	m->m_off   = (char *)&g_conf.m_sendEmailAlertsToEmail2 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send parm change email alerts to email 2";
	m->m_desc  = "Sends to email address 2 through email server 2 if "
		"any parm is changed.";
	m->m_cgi   = "seattwop";
	m->m_off   = (char *)&g_conf.m_sendParmChangeAlertsToEmail2 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email server 2";
	m->m_desc  = "Connects to this server directly when sending email 2 ";
	m->m_cgi   = "esrvtwo";
	m->m_off   = (char *)&g_conf.m_email2MX - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "mail.mydomain.com";
	m->m_size  = MAX_MX_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email address 2";
	m->m_desc  = "Sends to this address when sending email 2 ";
	m->m_cgi   = "eaddrtwo";
	m->m_off   = (char *)&g_conf.m_email2Addr - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "from email address 2";
	m->m_desc  = "The from field when sending email 2 ";
	m->m_cgi   = "efaddrtwo";
	m->m_off   = (char *)&g_conf.m_email2From - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "sysadmin@mydomain.com";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send email alerts to email 3";
	m->m_desc  = "Sends to email address 3 through email server 3.";
	m->m_cgi   = "seatthree";
	m->m_off   = (char *)&g_conf.m_sendEmailAlertsToEmail3 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send parm change email alerts to email 3";
	m->m_desc  = "Sends to email address 3 through email server 3 if "
		"any parm is changed.";
	m->m_cgi   = "seatthreep";
	m->m_off   = (char *)&g_conf.m_sendParmChangeAlertsToEmail3 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email server 3";
	m->m_desc  = "Connects to this server directly when sending email 3 ";
	m->m_cgi   = "esrvthree";
	m->m_off   = (char *)&g_conf.m_email3MX - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "mail.mydomain.com";
	m->m_size  = MAX_MX_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email address 3";
	m->m_desc  = "Sends to this address when sending email 3 ";
	m->m_cgi   = "eaddrthree";
	m->m_off   = (char *)&g_conf.m_email3Addr - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "from email address 3";
	m->m_desc  = "The from field when sending email 3 ";
	m->m_cgi   = "efaddrthree";
	m->m_off   = (char *)&g_conf.m_email3From - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "sysadmin@mydomain.com";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send email alerts to email 4";
	m->m_desc  = "Sends to email address 4 through email server 4.";
	m->m_cgi   = "seatfour";
	m->m_off   = (char *)&g_conf.m_sendEmailAlertsToEmail4 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "send parm change email alerts to email 4";
	m->m_desc  = "Sends to email address 4 through email server 4 if "
		"any parm is changed.";
	m->m_cgi   = "seatfourp";
	m->m_off   = (char *)&g_conf.m_sendParmChangeAlertsToEmail4 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email server 4";
	m->m_desc  = "Connects to this server directly when sending email 4 ";
	m->m_cgi   = "esrvfour";
	m->m_off   = (char *)&g_conf.m_email4MX - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "mail.mydomain.com";
	m->m_size  = MAX_MX_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "email address 4";
	m->m_desc  = "Sends to this address when sending email 4 ";
	m->m_cgi   = "eaddrfour";
	m->m_off   = (char *)&g_conf.m_email4Addr - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "from email address 4";
	m->m_desc  = "The from field when sending email 4 ";
	m->m_cgi   = "efaddrfour";
	m->m_off   = (char *)&g_conf.m_email4From - g;
	m->m_type  = TYPE_STRING;
	m->m_def   = "sysadmin@mydomain.com";
	m->m_size  = MAX_EMAIL_LEN;
	m->m_priv  = 2;
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "prefer local reads";
	m->m_desc  = "If you have scsi drives or a slow network, say yes here "
		"to minimize data fetches across the network.";
	m->m_cgi   = "plr";
	m->m_off   = (char *)&g_conf.m_preferLocalReads - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	// this is ifdef'd out in Msg3.cpp for performance reasons,
	// so do it here, too
#ifdef GBSANITYCHECK
	m->m_title = "max corrupted read retries";
	m->m_desc  = "How many times to retry disk reads that had corrupted "
		"data before requesting the list from a twin, and, if that "
		"fails, removing the bad data.";
	m->m_cgi   = "crr";
	m->m_off   = (char *)&g_conf.m_corruptRetries - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "100";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;
#endif

	m->m_title = "do incremental updating";
	m->m_desc  = "When reindexing a document, do not re-add data "
		"that should already be in index or clusterdb "
		"since the last time the document was indexed. Otherwise, "
		"re-add the data regardless.";
	m->m_cgi   = "oic";
	//m->m_off   = (char *)&g_conf.m_onlyAddUnchangedTermIds - g;
	m->m_off   = (char *)&g_conf.m_doIncrementalUpdating - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "use etc hosts";
	m->m_desc  = "Use /etc/hosts file to resolve hostnames? the "
		"/etc/host file is reloaded every minute, so if you make "
		"a change to it you might have to wait one minute for the "
		"change to take affect.";
	m->m_cgi   = "ueh";
	m->m_off   = (char *)&g_conf.m_useEtcHosts - g;
	m->m_def   = "1"; 
	m->m_type  = TYPE_BOOL;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "twins are split";
	m->m_desc  = "If enabled, Gigablast assumes the first half of "
		"machines in hosts.conf "
		"are on a different network switch than the second half, "
		"and minimizes transmits between the switches.";
	m->m_cgi   = "stw";
	m->m_off   = (char *)&g_conf.m_splitTwins - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "do out of memory testing";
	m->m_desc  = "When enabled Gigablast will randomly fail at "
		"allocating memory. Used for testing stability.";
	m->m_cgi   = "dot";
	m->m_off   = (char *)&g_conf.m_testMem - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "do consistency testing";
	m->m_desc  = "When enabled Gigablast will make sure it reparses "
		"the document exactly the same way. It does this every "
		"1000th document anyway, but enabling this makes it do it "
		"for every document.";
	m->m_cgi   = "dct";
	m->m_off   = (char *)&g_conf.m_doConsistencyTesting - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "use shotgun";
	m->m_desc  = "If enabled, all servers must have two gigabit "
		"ethernet ports hooked up and Gigablast will round robin "
		"packets between both ethernet ports when sending to another "
		"host. Can speed up network transmissions as much as 2x.";
	m->m_cgi   = "usht";
	m->m_off   = (char *)&g_conf.m_useShotgun - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "use quickpoll";
	m->m_desc  = "If enabled, Gigablast will use quickpoll. Significantly "
		"improves performance. Only turn this off for testing.";
	m->m_cgi   = "uqp";
	m->m_off   = (char *)&g_conf.m_useQuickpoll - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	// . this will leak the shared mem if the process is Ctrl+C'd
	// . that is expected behavior
	// . you can clean up the leaks using 'gb freecache 20000000'
	//   and use 'ipcs -m' to see what leaks you got
	// . generally, only the main gb should use shared mem, so
	//   keep this off for teting
	m->m_title = "use shared mem";
	m->m_desc  = "If enabled, Gigablast will use shared memory. "
		"Should really only be used on the live cluster, "
		"keep this on the testing cluster since it can "
		"leak easily.";
	m->m_cgi   = "ushm";
	m->m_off   = (char *)&g_conf.m_useSHM - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "posdb disk cache size";
	m->m_desc  = "How much file cache size to use in bytes? Posdb is "
		"the index.";
	m->m_cgi   = "dpcsp";
	m->m_off   = (char *)&g_conf.m_posdbFileCacheSize - g;
	m->m_type  = TYPE_LONG_LONG;
	m->m_def   = "30000000";
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "tagdb disk cache size";
	m->m_desc  = "How much file cache size to use in bytes? Tagdb is "
		"consulted at spider time and query time to determine "
		"if a url or outlink is banned or what its siterank is, etc.";
	m->m_cgi   = "dpcst";
	m->m_off   = (char *)&g_conf.m_tagdbFileCacheSize - g;
	m->m_type  = TYPE_LONG_LONG;
	m->m_def   = "30000000";
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "clusterdb disk cache size";
	m->m_desc  = "How much file cache size to use in bytes? "
	        "Gigablast does a "
		"lookup in clusterdb for each search result at query time to "
		"get its site information for site clustering. If you "
		"disable site clustering in the search controls then "
		"clusterdb will not be consulted.";
	m->m_cgi   = "dpcsc";
	m->m_off   = (char *)&g_conf.m_clusterdbFileCacheSize - g;
	m->m_type  = TYPE_LONG_LONG;
	m->m_def   = "30000000";
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "titledb disk cache size";
	m->m_desc  = "How much file cache size to use in bytes? Titledb "
		"holds the cached web pages, compressed. Gigablast consults "
		"it to generate a summary for a search result, or to see if "
		"a url Gigablast is spidering is already in the index.";
	m->m_cgi   = "dpcsx";
	m->m_off   = (char *)&g_conf.m_titledbFileCacheSize - g;
	m->m_type  = TYPE_LONG_LONG;
	m->m_def   = "30000000";
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "spiderdb disk cache size";
	m->m_desc  = "How much file cache size to use in bytes? Titledb "
		"holds the cached web pages, compressed. Gigablast consults "
		"it to generate a summary for a search result, or to see if "
		"a url Gigablast is spidering is already in the index.";
	m->m_cgi   = "dpcsy";
	m->m_off   = (char *)&g_conf.m_spiderdbFileCacheSize - g;
	m->m_type  = TYPE_LONG_LONG;
	m->m_def   = "30000000";
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "scan all if not found";
	m->m_desc  = "Scan all titledb files if rec not found. You should "
		"keep this on to avoid corruption. Do not turn it off unless "
		"you are Matt Wells.";
	m->m_cgi   = "sainf";
	m->m_off   = (char *)&g_conf.m_scanAllIfNotFound - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "interface machine";
	m->m_desc  = "for specifying if this is an interface machine"
		     "messages are rerouted from this machine to the main"
		     "cluster set in the hosts.conf.";
	m->m_cgi   = "intmch";
	m->m_off   = (char *)&g_conf.m_interfaceMachine - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "generate vector at query time";
	m->m_desc  = "At query time, should Gigablast generate content "
		"vectors for title records lacking them? This is an "
		"expensive operation, so is really just for testing purposes.";
	m->m_cgi   = "gv";
	m->m_off   = (char *)&g_conf.m_generateVectorAtQueryTime - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;



        m->m_title = "redirect non-raw traffic";
        m->m_desc  = "If this is non empty, http traffic will be redirected "
		"to the specified address.";
        m->m_cgi   = "redir";
        m->m_off   = (char *)&g_conf.m_redirect - g;
        m->m_type  = TYPE_STRING;
	m->m_size  = MAX_URL_LEN;
        m->m_def   = "";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
        m++;

        m->m_title = "send requests to compression proxy";
        m->m_desc  = "If this is true, gb will route download requests for"
		" web pages to proxies in hosts.conf.  Proxies will"
		" download and compress docs before sending back. ";
        m->m_cgi   = "srtcp";
        m->m_off   = (char *)&g_conf.m_useCompressionProxy - g;
        m->m_type  = TYPE_BOOL;
        m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
        m++;

        m->m_title = "synchronize proxy to cluster time";
        m->m_desc  = "Enable/disable the ability to synchronize time between "
                "the cluster and the proxy";
        m->m_cgi   = "sptct";
        m->m_off   = (char *)&g_conf.m_timeSyncProxy - g;
        m->m_type  = TYPE_BOOL;
        m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
        m++;

	m->m_title = "allow scaling of hosts";
	m->m_desc  = "Allows scaling up of hosts by deleting recs not in "
		"the correct group.  This should only happen why copying "
		"a set of servers to the new hosts. Otherwise corrupted "
		"data will cause a halt.";
	m->m_cgi   = "asoh";
	m->m_off   = (char *)&g_conf.m_allowScale - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "allow bypass of db validation";
	m->m_desc  = "Allows bypass of db validation so gigablast will not "
		"halt if a corrupt db is discovered durring load.  Use this "
		"when attempting to load with a collection that has known "
		"corruption.";
	m->m_cgi   = "abov";
	m->m_off   = (char *)&g_conf.m_bypassValidation - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 0";
	m->m_desc  = "IP address of the primary DNS server. Assumes UDP "
		"port 53. REQUIRED FOR SPIDERING! Use <company>'s "
		"public DNS " PUBLICLY_AVAILABLE_DNS1 " as default.";
	m->m_cgi   = "pdns";
	m->m_off   = (char *)&g_conf.m_dnsIps[0] - g;
	m->m_type  = TYPE_IP;
	// default to google public dns #1
	m->m_def   = (char*)PUBLICLY_AVAILABLE_DNS1;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 1";
	m->m_desc  = "IP address of the secondary DNS server. Assumes UDP "
	"port 53. Will be accessed in conjunction with the primary "
	"dns, so make sure this is always up. An ip of 0 means "
	"disabled. <company>'s secondary public DNS is " PUBLICLY_AVAILABLE_DNS2 ".";
	m->m_cgi   = "sdns";
	m->m_off   = (char *)&g_conf.m_dnsIps[1] - g;
	m->m_type  = TYPE_IP;
	// default to google public dns #2
	m->m_def   = (char*)PUBLICLY_AVAILABLE_DNS2;
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 2";
	m->m_desc  = "All hosts send to these DNSes based on hash "
		"of the subdomain to try to split DNS load evenly.";
	m->m_cgi   = "sdnsa";
	m->m_off   = (char *)&g_conf.m_dnsIps[2] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 3";
	m->m_desc  = "";
	m->m_cgi   = "sdnsb";
	m->m_off   = (char *)&g_conf.m_dnsIps[3] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 4";
	m->m_desc  = "";
	m->m_cgi   = "sdnsc";
	m->m_off   = (char *)&g_conf.m_dnsIps[4] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 5";
	m->m_desc  = "";
	m->m_cgi   = "sdnsd";
	m->m_off   = (char *)&g_conf.m_dnsIps[5] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 6";
	m->m_desc  = "";
	m->m_cgi   = "sdnse";
	m->m_off   = (char *)&g_conf.m_dnsIps[6] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 7";
	m->m_desc  = "";
	m->m_cgi   = "sdnsf";
	m->m_off   = (char *)&g_conf.m_dnsIps[7] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 8";
	m->m_desc  = "";
	m->m_cgi   = "sdnsg";
	m->m_off   = (char *)&g_conf.m_dnsIps[8] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 9";
	m->m_desc  = "";
	m->m_cgi   = "sdnsh";
	m->m_off   = (char *)&g_conf.m_dnsIps[9] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 10";
	m->m_desc  = "";
	m->m_cgi   = "sdnsi";
	m->m_off   = (char *)&g_conf.m_dnsIps[10] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 11";
	m->m_desc  = "";
	m->m_cgi   = "sdnsj";
	m->m_off   = (char *)&g_conf.m_dnsIps[11] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 12";
	m->m_desc  = "";
	m->m_cgi   = "sdnsk";
	m->m_off   = (char *)&g_conf.m_dnsIps[12] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 13";
	m->m_desc  = "";
	m->m_cgi   = "sdnsl";
	m->m_off   = (char *)&g_conf.m_dnsIps[13] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 14";
	m->m_desc  = "";
	m->m_cgi   = "sdnsm";
	m->m_off   = (char *)&g_conf.m_dnsIps[14] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dns 15";
	m->m_desc  = "";
	m->m_cgi   = "sdnsn";
	m->m_off   = (char *)&g_conf.m_dnsIps[15] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "geocoder IP #1";
	m->m_desc  = "";
	m->m_cgi   = "gca";
	m->m_off   = (char *)&g_conf.m_geocoderIps[0] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0"; // sp1
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "geocoder IP #2";
	m->m_desc  = "";
	m->m_cgi   = "gcb";
	m->m_off   = (char *)&g_conf.m_geocoderIps[1] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "geocoder IP #3";
	m->m_desc  = "";
	m->m_cgi   = "gcc";
	m->m_off   = (char *)&g_conf.m_geocoderIps[2] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "geocoder IP #4";
	m->m_desc  = "";
	m->m_cgi   = "gcd";
	m->m_off   = (char *)&g_conf.m_geocoderIps[3] - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "wiki proxy ip";
	m->m_desc  = "Access the wiki coll through this proxy ip";
	m->m_cgi   = "wpi";
	m->m_off   = (char *)&g_conf.m_wikiProxyIp - g;
	m->m_type  = TYPE_IP;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "wiki proxy port";
	m->m_desc  = "Access the wiki coll through this proxy port";
	m->m_cgi   = "wpp";
	m->m_off   = (char *)&g_conf.m_wikiProxyPort - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "default collection";
	m->m_desc  = "When no collection is explicitly specified, assume "
		"this collection name.";
	m->m_cgi   = "dcn";
	m->m_off   = (char *)&g_conf.m_defaultColl - g;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_COLL_LEN+1;
	m->m_def   = "";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "directory collection";
	m->m_desc  = "Collection to be used for directory searching and "
		"display of directory topic pages.";
	m->m_cgi   = "dircn";
	m->m_off   = (char *)&g_conf.m_dirColl - g;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_COLL_LEN+1;
	m->m_def   = "main";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "directory hostname";
	m->m_desc  = "Hostname of the server providing the directory. "
		     "Leave empty to use this host.";
	m->m_cgi   = "dirhn";
	m->m_off   = (char *)&g_conf.m_dirHost - g;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_URL_LEN;
	m->m_def   = "";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max incoming bandwidth for spider";
	m->m_desc  = "Total incoming bandwidth used by all spiders should "
		"not exceed this many kilobits per second. ";
	m->m_cgi   = "mkbps";
	m->m_off   = (char *)&g_conf.m_maxIncomingKbps - g;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "999999.0";
	m->m_units = "Kbps";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max 1-minute sliding-window loadavg";
	m->m_desc  = "Spiders will shed load when their host exceeds this "
		"value for the 1-minute load average in /proc/loadavg. "
		"The value 0.0 disables this feature.";
	m->m_cgi   = "mswl";
	m->m_off   = (char *)&g_conf.m_maxLoadAvg - g;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "0.0";
	m->m_units = "";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "max pages per second";
	m->m_desc  = "Maximum number of pages to index or delete from index "
		"per second for all hosts combined.";
	m->m_cgi   = "mpps";
	m->m_off   = (char *)&g_conf.m_maxPagesPerSecond - g;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "999999.0";
	m->m_units = "pages/second";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "use threads";
	m->m_desc  = "If enabled, Gigablast will use threads.";
	m->m_cgi   = "ut";
	m->m_off   = (char *)&g_conf.m_useThreads - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	// turn off for now. after seeing how SLOOOOOW brian's merge op was
	// when all 16 shards on a 16-core machine were merging (even w/ SSDs)
	// i turned threads off and it was over 100x faster. so until we have
	// pooling or something turn these off
	m->m_title = "use threads for disk";
	m->m_desc  = "If enabled, Gigablast will use threads for disk ops. "
		"Now that Gigablast uses pthreads more effectively, "
		"leave this enabled for optimal performance in all cases.";
	//"Until pthreads is any good leave this off. If you have "
	//"SSDs performance can be as much as 100x better.";
	m->m_cgi   = "utfd";
	m->m_off   = (char *)&g_conf.m_useThreadsForDisk - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = 0;//PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "use threads for intersects and merges";
	m->m_desc  = "If enabled, Gigablast will use threads for these ops. "
		"Default is now on in the event you have simultaneous queries "
		"so one query does not hold back the other. There seems "
		"to be a bug so leave this ON for now.";
	        //"Until pthreads is any good leave this off.";
	m->m_cgi   = "utfio";
	m->m_off   = (char *)&g_conf.m_useThreadsForIndexOps - g;
	m->m_type  = TYPE_BOOL;
	// enable this in the event of multiple cores available and
	// large simultaneous queries coming in
	m->m_def   = "1";
	m->m_flags = 0;//PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "use threads for system calls";
	m->m_desc  = "Gigablast does not make too many system calls so "
		"leave this on in case the system call is slow.";
	m->m_cgi   = "utfsc";
	m->m_off   = (char *)&g_conf.m_useThreadsForSystemCalls - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = 0;//PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;


	m->m_title = "max cpu threads";
	m->m_desc  = "Maximum number of threads to use per Gigablast process "
		"for intersecting docid lists.";
	m->m_cgi   = "mct";
	m->m_off   = (char *)&g_conf.m_maxCpuThreads - g;
	m->m_type  = TYPE_LONG;
	// make it 3 for new gb in case one query takes way longer 
	// than the others
	m->m_def   = "6"; // "2";
	m->m_units = "threads";
	m->m_min   = 1;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "max cpu merge threads";
	m->m_desc  = "Maximum number of threads to use per Gigablast process "
		"for merging lists read from disk.";
	m->m_cgi   = "mcmt";
	m->m_off   = (char *)&g_conf.m_maxCpuMergeThreads - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_units = "threads";
	m->m_min   = 1;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "max write threads";
	m->m_desc  = "Maximum number of threads to use per Gigablast process "
		"for writing data to the disk. "
		"Keep low to reduce file interlace effects and impact "
		"on query response time.";
	m->m_cgi   = "mwt";
	m->m_off   = (char *)&g_conf.m_maxWriteThreads - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_units = "threads";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "flush disk writes";
	m->m_desc  = "If enabled then all writes will be flushed to disk. "
		"If not enabled, then gb uses the Linux disk write cache.";
	m->m_cgi   = "fw";
	m->m_off   = (char *)&g_conf.m_flushWrites - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_API;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "verify disk writes";
	m->m_desc  = "Read what was written in a verification step. Decreases "
		"performance, but may help fight disk corruption mostly on "
		"Maxtors and Western Digitals.";
	m->m_cgi   = "vdw";
	m->m_off   = (char *)&g_conf.m_verifyWrites - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "max spider read threads";
	m->m_desc  = "Maximum number of threads to use per Gigablast process "
		"for accessing the disk "
		"for index-building purposes. Keep low to reduce impact "
		"on query response time. Increase for fast disks or when "
		"preferring build speed over lower query latencies";
	m->m_cgi   = "smdt";
	m->m_off   = (char *)&g_conf.m_spiderMaxDiskThreads - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "20";
	m->m_units = "threads";
	m->m_flags = 0;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m++;

	m->m_title = "separate disk reads";
	m->m_desc  = "If enabled then we will not launch a low priority "
		"disk read or write while a high priority is outstanding. "
		"Help improve query response time at the expense of "
		"spider performance.";
	m->m_cgi   = "sdt";
	m->m_off   = (char *)&g_conf.m_separateDiskReads - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = 0;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "min popularity for speller";
	m->m_desc  = "Word or phrase must be present in this percent "
		"of documents in order to qualify as a spelling "
		"recommendation.";
	m->m_cgi   = "mps";
	m->m_off   = (char *)&g_conf.m_minPopForSpeller - g;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = ".01";
	m->m_units = "%%";
	m->m_priv  = 2;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "phrase weight";
	m->m_desc  = "Percent to weight phrases in queries.";
	m->m_cgi   = "qp";
	m->m_off   = (char *)&g_conf.m_queryPhraseWeight - g;
	m->m_type  = TYPE_FLOAT;
	// was 350, but 'new mexico tourism' and 'boots uk'
	// emphasized the phrase terms too much!!
	m->m_def   = "100"; 
	m->m_units = "%%";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "weights.cpp slider parm (tmp)";
	m->m_desc  = "Percent of how much to use words to phrase ratio weights.";
	m->m_cgi   = "wsp";
	m->m_off   = (char *)&g_conf.m_sliderParm - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "90";
	m->m_units = "%%";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "maximum serialized query size";
	m->m_desc  = "When passing queries around the network, send the raw "
		"string instead of the serialized query if the required "
		"buffer is bigger than this. Smaller values decrease network "
		"traffic for large queries at the expense of processing time.";
	m->m_cgi   = "msqs";
	m->m_off   = (char *)&g_conf.m_maxSerializedQuerySize - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "8192";
	m->m_units = "bytes";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "merge buf size";
	m->m_desc  = "Read and write this many bytes at a time when merging "
		"files.  Smaller values are kinder to query performance, "
		" but the merge takes longer. Use at least 1000000 for "
		"fast merging.";
	m->m_cgi   = "mbs";
	m->m_off   = (char *)&g_conf.m_mergeBufSize - g;
	m->m_type  = TYPE_LONG;
	// keep this way smaller than that 800k we had in here, 100k seems
	// to be way better performance for qps
	m->m_def   = "500000";
	m->m_units = "bytes";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "doc count adjustment";
	m->m_desc  = "Add this number to the total document count in the "
		"index. Just used for displaying on the homepage.";
	m->m_cgi   = "dca";
	m->m_off   = (char *)&g_conf.m_docCountAdjustment - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "dynamic performance graph";
	m->m_desc  = "Generates profiling data for callbacks on page "
		"performance";
	m->m_cgi   = "dpg";
	m->m_off   = (char *)&g_conf.m_dynamicPerfGraph - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "enable profiling";
	m->m_desc  = "Enable profiler to do accounting of time taken by "
		"functions. ";
	m->m_cgi   = "enp";
	m->m_off   = (char *)&g_conf.m_profilingEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "minimum profiling threshold";
	m->m_desc  = "Profiler will not show functions which take less "
		"than this many milliseconds "
		"in the log or  on the perfomance graph.";
	m->m_cgi   = "mpt";
	m->m_off   = (char *)&g_conf.m_minProfThreshold - g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "10";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "sequential profiling.";
	m->m_desc  = "Produce a LOG_TIMING log message for each "
		"callback called, along with the time it took.  "
		"Profiler must be enabled.";
	m->m_cgi   = "ensp";
	m->m_off   = (char *)&g_conf.m_sequentialProfiling - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;
	
	m->m_title = "use statsdb";
	m->m_desc  = "Archive system statistics information in Statsdb.";
	m->m_cgi   = "usdb"; 
	m->m_off   = (char *)&g_conf.m_useStatsdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_MASTER;
	m->m_obj   = OBJ_CONF;
	m++;

	//////
	// END MASTER CONTROLS
	//////


	///////////////////////////////////////////
	// URL FILTERS
	///////////////////////////////////////////

	m->m_cgi   = "ufp";
	m->m_title = "url filters profile";
	m->m_xml   = "urlFiltersProfile";
	m->m_desc  = "Rather than editing the table below, you can select "
		"a predefined set of url instructions in this drop down menu "
		"that will update the table for you. Selecting <i>custom</i> "
		"allows you to make custom changes to the table. "
		"Selecting <i>web</i> configures the table for spidering "
		"the web in general. "
		"Selcting <i>news</i> configures the table for spidering "
		"new sites. "
		"Selecting <i>chinese</i> makes the spider prioritize the "
		"spidering of chinese pages, etc. "
		"Selecting <i>shallow</i> makes the spider go deep on "
		"all sites unless they are tagged <i>shallow</i> in the "
		"site list. "
		"Important: "
		"If you select a profile other than <i>custom</i> "
		"then your changes "
		"to the table will be lost.";
	m->m_off   = (char *)&cr.m_urlFiltersProfile - x;
	m->m_colspan = 3;
	m->m_type  = TYPE_SAFEBUF;//UFP;// 1 byte dropdown menu
	m->m_def   = "web"; // UFP_WEB
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "expression";
	m->m_desc  = "Before downloading the contents of a URL, Gigablast "
		"first chains down this "
		"list of "
		"expressions</a>, "
		"starting with expression #0.  "
		//"This table is also consulted "
		//"for every outlink added to spiderdb. "
		"The first expression it matches is the ONE AND ONLY "
		"matching row for that url. "
		"It then uses the "
		//"<a href=/overview.html#spiderfreq>"
		"respider frequency, "
		//"<a href=/overview.html#spiderpriority>"
		"spider priority, etc. on the MATCHING ROW when spidering "
		//"and <a href=/overview.html#ruleset>ruleset</a> to "
		"that URL. "
		"If you specify the <i>expression</i> as "
		"<i><b>default</b></i> then that MATCHES ALL URLs. "
		"URLs with high spider priorities take spidering "
		"precedence over "
		"URLs with lower spider priorities. "
		"The respider frequency dictates how often a URL will "
		"be respidered. "

		"See the help table below for examples of all the supported "
		"expressions. "
		"Use the <i>&&</i> operator to string multiple expressions "
		"together in the same expression text box. "
		"If you check the <i>delete</i> checkbox then urls matching "
		"that row will be deleted if already indexed, otherwise, "
		"they just won't be indexed."
		"<br><br>";
		
	m->m_cgi   = "fe";
	m->m_xml   = "filterExpression";
	m->m_max   = MAX_FILTERS;
	// array of safebufs i guess...
	m->m_off   = (char *)cr.m_regExs - x;
	// this is a safebuf, dynamically allocated string really
	m->m_type  = TYPE_SAFEBUF;//STRINGNONEMPTY
	// the size of each element in the array:
	m->m_size  = sizeof(SafeBuf);//MAX_REGEX_LEN+1;
	m->m_page  = PAGE_FILTERS;
	m->m_rowid = 1; // if we START a new row
	m->m_def   = "";
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "harvest links";
	m->m_cgi   = "hspl";
	m->m_xml   = "harvestLinks";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_harvestLinks - x;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "1";
	m->m_page  = PAGE_FILTERS;
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m->m_obj   = OBJ_COLL;
	m++;

	/*
	m->m_title = "spidering enabled";
	m->m_cgi   = "cspe";
	m->m_xml   = "spidersEnabled";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_spidersEnabled - x;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "1";
	m->m_page  = PAGE_FILTERS;
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS;
	m++;
	*/

	m->m_title = "respider frequency (days)";
	m->m_cgi   = "fsf";
	m->m_xml   = "filterFrequency";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_spiderFreqs - x;
	m->m_type  = TYPE_FLOAT;
	// why was this default 0 days?
	m->m_def   = "30.0"; // 0.0
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m->m_units = "days";
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;

	m->m_title = "max spiders";
	m->m_desc  = "Do not allow more than this many outstanding spiders "
		"for all urls in this priority."; // was "per rule"
	m->m_cgi   = "mspr";
	m->m_xml   = "maxSpidersPerRule";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_maxSpidersPerRule - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "99";
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;

	m->m_title = "max spiders per ip";
	m->m_desc  = "Allow this many spiders per IP.";
	m->m_cgi   = "mspi";
	m->m_xml   = "maxSpidersPerIp";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_spiderIpMaxSpiders - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "7";
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;

	m->m_title = "same ip wait (ms)";
	m->m_desc  = "Wait at least this int32_t before downloading urls from "
		"the same IP address.";
	m->m_cgi   = "xg";
	m->m_xml   = "spiderIpWait";
	m->m_max   = MAX_FILTERS;
	//m->m_fixed = MAX_PRIORITY_QUEUES;
	m->m_off   = (char *)cr.m_spiderIpWaits - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1000";
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m->m_units = "milliseconds";
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;

	m->m_title = "delete";
	m->m_cgi   = "fdu";
	m->m_xml   = "forceDeleteUrls";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_forceDelete - x;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_page  = PAGE_FILTERS;
	m->m_rowid = 1;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "spider priority";
	m->m_cgi   = "fsp";
	m->m_xml   = "filterPriority";
	m->m_max   = MAX_FILTERS;
	m->m_off   = (char *)cr.m_spiderPriorities - x;
	m->m_type  = TYPE_PRIORITY2; // includes UNDEFINED priority in dropdown
	m->m_page  = PAGE_FILTERS;
	m->m_obj   = OBJ_COLL;
	m->m_rowid = 1;
	m->m_def   = "50";
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m->m_addin = 1; // "insert" follows?
	m++;

	///////////////////////////////////////////
	//  SEARCH URL CONTROLS
	//  these are only specified in the search url when doing a search
	///////////////////////////////////////////


	/////
	//
	// OLDER SEARCH INPUTS
	//
	////

	// IMPORT PARMS
	m->m_title = "enable document importation";
	m->m_desc  = "Import documents into this collection.";
	m->m_cgi   = "import";
	m->m_page  = PAGE_IMPORT;
	m->m_obj   = OBJ_COLL;
	m->m_off   = (char *)&cr.m_importEnabled - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_API; 
	m++;

	m->m_title = "directory containing titledb files";
	m->m_desc  = "Import documents contained in titledb files in this "
		"directory. This is an ABSOLUTE directory path.";
	m->m_cgi   = "importdir";
	m->m_xml   = "importDir";
	m->m_page  = PAGE_IMPORT;
	m->m_obj   = OBJ_COLL;
	m->m_off   = (char *)&cr.m_importDir - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_def   = "";
	m->m_flags = PF_API;
	m++;

	m->m_title = "number of simultaneous injections";
	m->m_desc  = "Typically try one or two injections per host in "
		"your cluster.";
	m->m_cgi   = "numimportinjects";
	m->m_xml   = "numImportInjects";
	m->m_page  = PAGE_IMPORT;
	m->m_obj   = OBJ_COLL;
	m->m_off   = (char *)&cr.m_numImportInjects - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "2";
	m->m_flags = PF_API;
	m++;



	///////////
	//
	// ADD URL PARMS
	//
	///////////

	m->m_title = "collection";
	m->m_desc  = "Add urls into this collection.";
	m->m_cgi   = "c";
	m->m_page  = PAGE_ADDURL2;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	// PF_COLLDEFAULT: so it gets set to default coll on html page
	m->m_flags = PF_API|PF_REQUIRED|PF_NOHTML; 
	m++;

	m->m_title = "urls to add";
	m->m_desc  = "List of urls to index. One per line or space separated. "
		"If your url does not index as you expect you "
		"can check it's spider history by doing a url: search on it. "
		"Added urls will have a "
		"<a href=/admin/filters#hopcount>hopcount</a> of 0. "
		"Added urls will match the <i><a href=/admin/filters#isaddurl>"
		"isaddurl</a></i> directive on "
		"the url filters page. "
		"The add url api is described on the "
		"<a href=/admin/api>api</a> page.";
	m->m_cgi   = "urls";
	m->m_page  = PAGE_ADDURL2;
	m->m_obj   = OBJ_GBREQUEST; // do not store in g_conf or collectionrec
	m->m_off   = (char *)&gr.m_urlsBuf - (char *)&gr;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_TEXTAREA | PF_NOSAVE | PF_API|PF_REQUIRED;
	m++;

	m->m_title = "strip sessionids";
	m->m_desc  = "Strip added urls of their session ids.";
	m->m_cgi   = "strip";
	m->m_page  = PAGE_ADDURL2;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_off   = (char *)&gr.m_stripBox - (char *)&gr;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m++;

	m->m_title = "harvest links";
	m->m_desc  = "Harvest links of added urls so we can spider them?.";
	m->m_cgi   = "spiderlinks";
	m->m_page  = PAGE_ADDURL2;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_off   = (char *)&gr.m_harvestLinks - (char *)&gr;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m++;

	/*
	m->m_title = "force respider";
	m->m_desc  = "Force an immediate respider even if the url "
		"is already indexed.";
	m->m_cgi   = "force";
	m->m_page  = PAGE_ADDURL2;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_off   = (char *)&gr.m_forceRespiderBox - (char *)&gr;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m++;
	*/



	////////
	//
	// now the new injection parms
	//
	////////

	m->m_title = "url";
	m->m_desc  = "Specify the URL that will be immediately crawled "
		"and indexed in real time while you wait. The browser "
		"will return the "
		"final index status code. Alternatively, "
		"use the <a href=/admin/addurl>add url</a> page "
		"to add urls individually or in bulk "
		"without having to wait for the pages to be "
		"actually indexed in realtime. "

		"By default, injected urls "
		"take precedence over the \"insitelist\" expression in the "
		"<a href=/admin/filters>url filters</a> "
		"so injected urls need not match the patterns in your "
		"<a href=/admin/sites>site list</a>. You can "
		"change that behavior in the <a href=/admin/filters>url "
		"filters</a> if you want. "
		"Injected urls will have a "
		"<a href=/admin/filters#hopcount>hopcount</a> of 0. "
		"The injection api is described on the "
		"<a href=/admin/api>api</a> page. "
		"Make up a fake url if you are injecting content that "
		"does not have one."
		"<br>"
		"<br>"
		"If the url ends in .warc or .arc or .warc.gz or .arc.gz "
		"Gigablast will index the contained documents as individual "
		"documents, using the appropriate dates and other meta "
		"information contained in the containing archive file."
		;
	m->m_cgi   = "url";
	//m->m_cgi2  = "u";
	//m->m_cgi3  = "seed"; // pagerawlbot
	//m->m_cgi4  = "injecturl";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_API | PF_REQUIRED;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_url - (char *)&ir;
	m++;

	// alias #1
	m->m_title = "url";
	m->m_cgi   = "u";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_HIDDEN;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_url - (char *)&ir;
	m++;

	// alias #2
	m->m_title = "url";
	m->m_cgi   = "seed";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_HIDDEN | PF_DIFFBOT;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_url - (char *)&ir;
	m++;

	// alias #3
	m->m_title = "url";
	m->m_cgi   = "injecturl";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_HIDDEN;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_url - (char *)&ir;
	m++;

	m->m_title = "inject links";
	m->m_desc  = "Should we inject the links found in the injected "
		"content as well?";
	m->m_cgi   = "injectlinks";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_injectLinks - (char *)&ir;
	m++;


	m->m_title = "spider links";
	m->m_desc  = "Add the outlinks of the injected content into spiderdb "
		"for spidering?";
	m->m_cgi   = "spiderlinks";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	// leave off because could start spidering whole web unintentionally
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_spiderLinks - (char *)&ir;
	m++;
	
	m->m_title = "short reply";
	m->m_desc  = "Should the injection response be short and simple?";
	m->m_cgi   = "quick";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_shortReply - (char *)&ir;
	m++;

	m->m_title = "only inject content if new";
	m->m_desc  = "If the specified url is already in the index then "
		"skip the injection.";
	m->m_cgi   = "newonly";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_newOnly - (char *)&ir;
	m++;

	m->m_title = "delete from index";
	m->m_desc  = "Delete the specified url from the index.";
	m->m_cgi   = "deleteurl";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_deleteUrl - (char *)&ir;
	m++;

	m->m_title = "recycle content";
	m->m_desc  = "If the url is already in the index, then do not "
		"re-download the content, just use the content that was "
		"stored in the cache from last time.";
	m->m_cgi   = "recycle";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_recycle - (char *)&ir;
	m++;

	m->m_title = "dedup url";
	m->m_desc  = "Do not index the url if there is already another "
		"url in the index with the same content.";
	m->m_cgi   = "dedup";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_dedup - (char *)&ir;
	m++;

	m->m_title = "do consistency checking";
	m->m_desc  = "Turn this on for debugging.";
	m->m_cgi   = "consist";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN; // | PF_API
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_doConsistencyTesting - (char *)&ir;
	m++;

	m->m_title = "hop count";
	m->m_desc  = "Use this hop count when injecting the page.";
	m->m_cgi   = "hopcount";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN; // | PF_API
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_hopCount - (char *)&ir;
	m++;

	m->m_title = "url IP";
	m->m_desc  = "Use this IP when injecting the document. Do not use or "
		"set to 0.0.0.0, if unknown. If provided, it will save an IP "
		"lookup.";
	m->m_cgi   = "urlip";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_IP;
	m->m_def   = "0.0.0.0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_injectDocIp - (char *)&ir;
	m++;

	m->m_title = "last spider time";
	m->m_desc  = "Override last time spidered";
	m->m_cgi   = "lastspidered";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN; // | PF_API
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_lastSpidered - (char *)&ir;
	m++;

	m->m_title = "first indexed";
	m->m_desc  = "Override first indexed time";
	m->m_cgi   = "firstindexed";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN; // | PF_API
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_firstIndexed - (char *)&ir;
	m++;


	m->m_title = "content has mime";
	m->m_desc  = "If the content of the url is provided below, does "
		"it begin with an HTTP mime header?";
	m->m_cgi   = "hasmime";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_hasMime - (char *)&ir;
	m++;

	m->m_title = "content delimeter";
	m->m_desc  = "If the content of the url is provided below, then "
		"it consist of multiple documents separated by this "
		"delimeter. Each such item will be injected as an "
		"independent document. Some possible delimeters: "
		"<i>========</i> or <i>&lt;doc&gt;</i>. If you set "
		"<i>hasmime</i> above to true then Gigablast will check "
		"for a url after the delimeter and use that url as the "
		"injected url. Otherwise it will append numbers to the "
		"url you provide above.";
	m->m_cgi   = "delim";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_contentDelim - (char *)&ir;
	m++;


	m->m_title = "content type";
	m->m_desc  = "If you supply content in the text box below without "
		"an HTTP mime header, "
		"then you need to enter the content type. "
		"Possible values: <b>text/html text/plain text/xml "
		"application/json</b>";
	m->m_cgi   = "contenttype";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR; //text/html application/json application/xml
	m->m_def   = "text/html";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_contentTypeStr - (char *)&ir;
	m++;

	m->m_title = "content charset";
	m->m_desc  = "A number representing the charset of the content "
		"if provided below and no HTTP mime header "
		"is given. Defaults to utf8 "
		"which is 106. "
		"See iana_charset.h for the numeric values.";
	m->m_cgi   = "charset";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_LONG;
	m->m_def   = "106";
	m->m_flags = PF_API;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_charset - (char *)&ir;
	m++;

	m->m_title = "upload content file";
	m->m_desc  = "Instead of specifying the content to be injected in "
		"the text box below, upload this file for it.";
	m->m_cgi   = "file";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_FILEUPLOADBUTTON;
	m->m_def   = NULL;
	m->m_flags = PF_NOAPI;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_contentFile - (char *)&ir;
	m++;

	m->m_title = "content";
	m->m_desc = "If you want to supply the URL's content "
		"rather than have Gigablast download it, then "
		"enter the content here. "
		"Enter MIME header "
		"first if \"content has mime\" is set to true above. "
		"Separate MIME from actual content with two returns. "
		"At least put a single space in here if you want to "
		"inject empty content, otherwise the content will "
		"be downloaded from the url. This is because the "
		"page injection form always submits the content text area "
		"even if it is empty, which should signify that the "
		"content should be downloaded.";
	m->m_cgi   = "content";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_API|PF_TEXTAREA;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_content - (char *)&ir;
	m++;

	m->m_title = "metadata";
	m->m_desc = "Json encoded metadata to be indexed with the document.";
	m->m_cgi   = "metadata";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	m->m_flags = PF_API|PF_TEXTAREA;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.ptr_metadata - (char *)&ir;
	m++;


	m->m_title = "get sectiondb voting info";
	m->m_desc = "Return section information of injected content for "
		"the injected subdomain. ";
	m->m_cgi   = "sections";
	m->m_obj   = OBJ_IR;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_API|PF_NOHTML;
	m->m_page  = PAGE_INJECT;
	m->m_off   = (char *)&ir.m_getSections - (char *)&ir;
	m++;


	///////////////////
	//
	// QUERY REINDEX
	//
	///////////////////

	m->m_title = "collection";
	m->m_desc  = "query reindex in this collection.";
	m->m_cgi   = "c";
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHARPTR;
	m->m_def   = NULL;
	// PF_COLLDEFAULT: so it gets set to default coll on html page
	m->m_flags = PF_API|PF_REQUIRED|PF_NOHTML; 
	m->m_page  = PAGE_REINDEX;
	m->m_off   = (char *)&gr.m_coll - (char *)&gr;
	m++;

	m->m_title = "query to reindex or delete";
	m->m_desc  = "We either reindex or delete the search results of "
		"this query. Reindexing them will redownload them and "
		"possible update the siterank, which is based on the "
		"number of links to the site. This will add the url "
		"requests to "
		"the spider queue so ensure your spiders are enabled.";
	m->m_cgi   = "q";
	m->m_off   = (char *)&gr.m_query - (char *)&gr;
	m->m_type  = TYPE_CHARPTR;
	m->m_page  = PAGE_REINDEX;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_def   = NULL;
	m->m_flags = PF_API |PF_REQUIRED;
	m++;

	m->m_title = "start result number";
	m->m_desc  = "Starting with this result #. Starts at 0.";
	m->m_cgi   = "srn";
	m->m_off   = (char *)&gr.m_srn - (char *)&gr;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_REINDEX;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_def   = "0";
	m->m_flags = PF_API ;
	m++;

	m->m_title = "end result number";
	m->m_desc  = "Ending with this result #. 0 is the first result #.";
	m->m_cgi   = "ern";
	m->m_off   = (char *)&gr.m_ern - (char *)&gr;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_REINDEX;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_def   = "99999999";
	m->m_flags = PF_API ;
	m++;

	m->m_title = "query language";
	m->m_desc  = "The language the query is in. Used to rank results. "
		"Just use xx to indicate no language in particular. But "
		"you should use the same qlang value you used for doing "
		"the query if you want consistency.";
	m->m_cgi   = "qlang";
	m->m_off   = (char *)&gr.m_qlang - (char *)&gr;
	m->m_type  = TYPE_CHARPTR;
	m->m_page  = PAGE_REINDEX;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_def   = "en";
	m->m_flags = PF_API ;
	m++;


	m->m_title = "recycle content";
	m->m_desc  = "If you check this box then Gigablast will not "
		"re-download the content, but use the content that was "
		"stored in the cache from last time. Useful for rebuilding "
		"the index to pick up new inlink text or fresher "
		"sitenuminlinks counts which influence ranking.";
	m->m_cgi   = "qrecycle";
	m->m_obj   = OBJ_GBREQUEST;
	m->m_type  = TYPE_CHECKBOX;
	m->m_def   = "0";
	m->m_flags = PF_API;
	m->m_page  = PAGE_REINDEX;
	m->m_off   = (char *)&gr.m_recycleContent - (char *)&gr;
	m++;


	m->m_title = "FORCE DELETE";
	m->m_desc  = "Check this checkbox to delete the results, not just "
		"reindex them.";
	m->m_cgi   = "forcedel";
	m->m_off   = (char *)&gr.m_forceDel - (char *)&gr;
	m->m_type  = TYPE_CHECKBOX;
	m->m_page  = PAGE_REINDEX;
	m->m_obj   = OBJ_GBREQUEST;
	m->m_def   = "0";
	m->m_flags = PF_API ;
	m++;


	///////////////////
	//
	// SEARCH CONTROLS
	//
	///////////////////


	m->m_title = "do spell checking by default";
	m->m_desc  = "If enabled while using the XML feed, "
		"when Gigablast finds a spelling recommendation it will be "
		"included in the XML <spell> tag. Default is 0 if using an "
		"XML feed, 1 otherwise.";
	m->m_cgi   = "spell";
	m->m_off   = (char *)&cr.m_spellCheck - x;
	//m->m_soff  = (char *)&si.m_spellCheck - y;
	//m->m_sparm = 1;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "1";
	m->m_flags = PF_API | PF_NOSAVE | PF_CLONE;
	m++;

	m->m_title = "get scoring info by default";
	m->m_desc  = "Get scoring information for each result so you "
		"can see how each result is scored. You must explicitly "
		"request this using &scores=1 for the XML feed because it "
		"is not included by default.";
	m->m_cgi   = "scores"; // dedupResultsByDefault";
	m->m_off   = (char *)&cr.m_getDocIdScoringInfo - x;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "1";
	m->m_flags = PF_API | PF_CLONE;
	m++;

	m->m_title = "do query expansion by default";
	m->m_desc  = "If enabled, query expansion will expand your query "
		"to include the various forms and "
		"synonyms of the query terms.";
	m->m_def   = "1";
	m->m_off   = (char *)&cr.m_queryExpansion - x;
	m->m_type  = TYPE_BOOL;
	m->m_cgi  = "qe";
	m->m_page  = PAGE_SEARCH;
	m->m_flags = PF_API | PF_CLONE;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "highlight query terms in summaries by default";
	m->m_desc  = "Use to disable or enable "
		"highlighting of the query terms in the summaries.";
	m->m_def   = "1";
	m->m_off   = (char *)&cr.m_doQueryHighlighting - x;
	m->m_type  = TYPE_BOOL;
	m->m_cgi   = "qh";
	m->m_smin  = 0;
	m->m_smax  = 8;
	m->m_sprpg = 1; // turn off for now
	m->m_sprpp = 1;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max title len";
	m->m_desc  = "What is the maximum number of "
		"characters allowed in titles displayed in the search "
		"results?";
	m->m_cgi   = "tml";
	m->m_off   = (char *)&cr.m_titleMaxLen - x;
	m->m_type  = TYPE_LONG;
	m->m_flags = PF_API | PF_CLONE;
	m->m_def   = "80";
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "site cluster by default";
	m->m_desc  = "Should search results be site clustered? This "
		"limits each site to appearing at most twice in the "
		"search results. Sites are subdomains for the most part, "
		"like abc.xyz.com.";
	m->m_cgi   = "scd";
	m->m_off   = (char *)&cr.m_siteClusterByDefault - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// buzz
	m->m_title = "hide all clustered results";
	m->m_desc  = "Only display at most one result per site.";
	m->m_cgi   = "hacr";
	m->m_off   = (char *)&cr.m_hideAllClustered - x;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m++;

	m->m_title = "dedup results by default";
	m->m_desc  = "Should duplicate search results be removed? This is "
		"based on a content hash of the entire document. "
		"So documents must be exactly the same for the most part.";
	m->m_cgi   = "drd"; // dedupResultsByDefault";
	m->m_off   = (char *)&cr.m_dedupResultsByDefault - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 1;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "do tagdb lookups for queries";
	m->m_desc  = "For each search result a tagdb lookup is made, "
		"usually across the network on distributed clusters, to "
		"see if the URL's site has been manually banned in tagdb. "
		"If you don't manually ban sites then turn this off for "
		"extra speed.";
	m->m_cgi   = "stgdbl";
	m->m_off   = (char *)&cr.m_doTagdbLookups - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 1;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "percent similar dedup summary default value";
	m->m_desc  = "If document summary (and title) are "
		"this percent similar "
		"to a document summary above it, then remove it from the "
		"search results. 100 means only to remove if exactly the "
		"same. 0 means no summary deduping.";
	m->m_cgi   = "psds";
	m->m_off   = (char *)&cr.m_percentSimilarSummary - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "90";
	m->m_group = 0;
	m->m_smin  = 0;
	m->m_smax  = 100;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;       

	m->m_title = "number of lines to use in summary to dedup";
	m->m_desc  = "Sets the number of lines to generate for summary "
		"deduping. This is to help the deduping process not throw "
		"out valid summaries when normally displayed summaries are "
		"smaller values. Requires percent similar dedup summary to "
		"be non-zero.";
	m->m_cgi   = "msld";
	m->m_off   = (char *)&cr.m_summDedupNumLines - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "4";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;       
	

	m->m_title = "dedup URLs by default";
	m->m_desc  = "Should we dedup URLs with case insensitivity? This is "
                     "mainly to correct duplicate wiki pages.";
	m->m_cgi   = "ddu";
	m->m_off   = (char *)&cr.m_dedupURLDefault - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "sort language preference default";
	m->m_desc  = "Default language to use for ranking results. "
		//"This should only be used on limited collections. "
		"Value should be any language abbreviation, for example "
		"\"en\" for English. Use <i>xx</i> to give ranking "
		"boosts to no language in particular. See the language "
		"abbreviations at the bottom of the "
		"<a href=/admin/filters>url filters</a> page.";
	m->m_cgi   = "defqlang";
	m->m_off   = (char *)&cr.m_defaultSortLanguage2 - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = 6; // up to 5 chars + NULL, e.g. "en_US"
	m->m_def   = "xx";//_US";
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "sort country preference default";
	m->m_desc  = "Default country to use for ranking results. "
		//"This should only be used on limited collections. "
		"Value should be any country code abbreviation, for example "
		"\"us\" for United States. This is currently not working.";
	m->m_cgi   = "qcountry";
	m->m_off   = (char *)&cr.m_defaultSortCountry - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = 2+1;
	m->m_def   = "us";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	// for post query reranking 
	m->m_title = "docs to check for post query demotion by default";
	m->m_desc  = "How many search results should we "
		"scan for post query demotion? "
		"0 disables all post query reranking. ";
	m->m_cgi   = "pqrds";
	m->m_off   = (char *)&cr.m_pqr_docsToScan - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 1;
	//m->m_scgi  = "pqrds";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max summary len";
	m->m_desc  = "What is the maximum number of "
		"characters displayed in a summary for a search result?";
	m->m_cgi   = "sml";
	m->m_off   = (char *)&cr.m_summaryMaxLen - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "180";
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max summary excerpts";
	m->m_desc  = "What is the maximum number of "
		"excerpts displayed in the summary of a search result?";
	m->m_cgi   = "smnl";
	m->m_off   = (char *)&cr.m_summaryMaxNumLines - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max summary excerpt length";
	m->m_desc = "What is the maximum number of "
		"characters allowed per summary excerpt?";
	m->m_cgi   = "smxcpl";
	m->m_off   = (char *)&cr.m_summaryMaxNumCharsPerLine - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "180";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max summary line width by default";
	m->m_desc  = "&lt;br&gt; tags are inserted to keep the number "
		"of chars in the summary per line at or below this width. "
		"Also affects title. "
		"Strings without spaces that exceed this "
		"width are not split. Has no affect on xml or json feed, "
		"only works on html.";
	m->m_cgi   = "smw";
	m->m_off   = (char *)&cr.m_summaryMaxWidth - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "80";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "bytes of doc to scan for summary generation";
	m->m_desc  = "Truncating this will miss out on good summaries, but "
		"performance will increase.";
	m->m_cgi   = "clmfs";
	m->m_off   = (char *)&cr.m_contentLenMaxForSummary - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "70000";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;       

	m->m_title = "front highlight tag";
	m->m_desc  = "Front html tag used for highlightig query terms in the "
		"summaries displated in the search results.";
	m->m_cgi   = "sfht";
	m->m_off   = (char *)cr.m_summaryFrontHighlightTag - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = SUMMARYHIGHLIGHTTAGMAXSIZE ;
	m->m_def   = "<b style=\"color:black;background-color:#ffff66\">";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "back highlight tag";
	m->m_desc  = "Front html tag used for highlightig query terms in the "
		"summaries displated in the search results.";
	m->m_cgi   = "sbht";
	m->m_off   = (char *)cr.m_summaryBackHighlightTag - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = SUMMARYHIGHLIGHTTAGMAXSIZE ;
	m->m_def   = "</b>";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "ip restriction for gigabits by default";
	m->m_desc  = "Should Gigablast only get one document per IP domain "
		"and per domain for gigabits (related topics) generation?";
	m->m_cgi   = "ipr";
	m->m_off   = (char *)&cr.m_ipRestrict - x;
	m->m_type  = TYPE_BOOL;
	// default to 0 since newspaperarchive only has docs from same IP dom
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "remove overlapping topics";
	m->m_desc  = "Should Gigablast remove overlapping topics (gigabits)?";
	m->m_cgi   = "rot";
	m->m_off   = (char *)&cr.m_topicRemoveOverlaps - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "min gigabit score by default";
	m->m_desc  = "Gigabits (related topics) with scores below this "
		"will be excluded. Scores range from 0% to over 100%.";
	m->m_cgi   = "mts";
	m->m_off   = (char *)&cr.m_minTopicScore - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "5";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "min gigabit doc count by default";
	m->m_desc  = "How many documents must contain the gigabit "
		"(related topic) in order for it to be displayed.";
	m->m_cgi   = "mdc";
	m->m_off   = (char *)&cr.m_minDocCount - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "2";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "dedup doc percent for gigabits (related topics)";
	m->m_desc  = "If a document is this percent similar to another "
		"document with a higher score, then it will not contribute "
		"to the gigabit generation.";
	m->m_cgi   = "dsp";
	m->m_off   = (char *)&cr.m_dedupSamplePercent - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "80";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max words per gigabit (related topic) by default";
	m->m_desc  = "Maximum number of words a gigabit (related topic) "
		"can have. Affects xml feeds, too.";
	m->m_cgi   = "mwpt";
	m->m_off   = (char *)&cr.m_maxWordsPerTopic - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "6";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;


	m->m_title = "gigabit max sample size";
	m->m_desc  = "Max chars to sample from each doc for gigabits "
		"(related topics).";
	m->m_cgi   = "tmss";
	m->m_off   = (char *)&cr.m_topicSampleSize - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "4096";
	m->m_group = 0;
	m->m_flags = PF_API | PF_CLONE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "gigabit max punct len";
	m->m_desc  = "Max sequential punct chars allowed in a gigabit "
		"(related topic). "
		" Set to 1 for speed, 5 or more for best topics but twice as "
		"slow.";
	m->m_cgi   = "tmpl";
	m->m_off   = (char *)&cr.m_topicMaxPunctLen - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "display indexed date";
	m->m_desc  = "Display the indexed date along with results.";
	m->m_cgi   = "didt";
	m->m_off   = (char *)&cr.m_displayIndexedDate - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "display last modified date";
	m->m_desc  = "Display the last modified date along with results.";
	m->m_cgi   = "dlmdt";
	m->m_off   = (char *)&cr.m_displayLastModDate - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "display published date";
	m->m_desc  = "Display the published date along with results.";
	m->m_cgi   = "dipt";
	m->m_off   = (char *)&cr.m_displayPublishDate - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "enable click 'n' scroll";
	m->m_desc  = "The [cached] link on results pages loads click n "
		"scroll.";
	m->m_cgi   = "ecns";
	m->m_off   = (char *)&cr.m_clickNScrollEnabled - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m++;

        m->m_title = "use data feed account server";
        m->m_desc  = "Enable/disable the use of a remote account verification "
                "for Data Feed Customers.";
        m->m_cgi   = "dfuas";
        m->m_off   = (char *)&cr.m_useDFAcctServer - x;
        m->m_type  = TYPE_BOOL;
        m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
        m++;

        m->m_title = "data feed server ip";
        m->m_desc  = "The ip address of the Gigablast data feed server to "
                "retrieve customer account information from.";
        m->m_cgi   = "dfip";
        m->m_off   = (char *)&cr.m_dfAcctIp - x;
        m->m_type  = TYPE_IP;
        m->m_def   = "2130706433";
        m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
        m++;

        m->m_title = "data feed server port";
        m->m_desc  = "The port of the Gigablast data feed server to retrieve "
                "customer account information from.";
        m->m_cgi   = "dfport";
        m->m_off   = (char *)&cr.m_dfAcctPort - x;
        m->m_type  = TYPE_LONG;
        m->m_def   = "8040";
        m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
        m++;

	m->m_title = "home page";
	static SafeBuf s_tmpBuf;
	s_tmpBuf.setLabel("stmpb1");
	s_tmpBuf.safePrintf (
			  "Html to display for the home page. "
			  "Leave empty for default home page. "
			  "Use %%N for total "
			  "number of pages indexed. Use %%n for number of "
			  "pages indexed "
			  "for the current collection. "
			  //"Use %%H so Gigablast knows where to insert "
			  //"the hidden form input tags, which must be there. "
			  "Use %%c to insert the current collection name. "
			  //"Use %T to display the standard footer. "
			  "Use %%q to display the query in "
			  "a text box. "
			  "Use %%t to display the directory TOP. "
			  "Example to paste into textbox: "
			  "<br><i>"
			  );
	s_tmpBuf.htmlEncode (
			      "<html>"
			      "<title>My Gigablast Search Engine</title>"
			      "<script>\n"
			      //"<!--"
			      "function x(){document.f.q.focus();}"
			      //"// -->"
			      "\n</script>"
			      "<body onload=\"x()\">"
			      "<br><br>"
			      "<center>"
			      "<a href=/>"
			      "<img border=0 width=500 height=122 "
			      "src=/logo-med.jpg></a>"
			      "<br><br>"
			      "<b>My Search Engine</b>"
			      "<br><br>"
			      // "<br><br><br>"
			      // "<b>web</b> "
			      // "&nbsp;&nbsp;&nbsp;&nbsp; "
			      // "<a href=\"/Top\">directory</a> "
			      // "&nbsp;&nbsp;&nbsp;&nbsp; "
			      // "<a href=/adv.html>advanced search</a> "
			      // "&nbsp;&nbsp;&nbsp;&nbsp; "
			      // "<a href=/addurl "
			      // "title=\"Instantly add your url to "
			      //"the index\">"
			      // "add url</a>"
			      // "<br><br>"
			      "<form method=get action=/search name=f>"
			      "<input type=hidden name=c value=\"%c\">"
			      "<input name=q type=text size=60 value=\"\">"
			      "&nbsp;"
			      "<input type=\"submit\" value=\"Search\">"
			      "</form>"
			      "<br>"
			      "<center>"
			      "Searching the <b>%c</b> collection of %n "
			      "documents."
			      "</center>"
			      "<br>"
			      "</body></html>")  ;
	s_tmpBuf.safePrintf("</i>");
	m->m_desc = s_tmpBuf.getBufStart();
	m->m_xml  = "homePageHtml";
	m->m_cgi   = "hp";
	m->m_off   = (char *)&cr.m_htmlRoot - x;
	//m->m_plen  = (char *)&cr.m_htmlRootLen - x; // length of string
	m->m_type  = TYPE_SAFEBUF;//STRINGBOX;
	//m->m_size  = MAX_HTML_LEN + 1;
	m->m_def   = "";
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_TEXTAREA | PF_CLONE;
	m++;


	m->m_title = "html head";
        static SafeBuf s_tmpBuf2;
	s_tmpBuf2.setLabel("stmpb2");
	s_tmpBuf2.safePrintf("Html to display before the search results. ");
	char *fff = "Leave empty for default. "
		"Convenient "
		"for changing colors and displaying logos. Use "
		"the variable, "
		"%q, to represent the query to display in a "
		"text box. "
		"Use %e to print the url encoded query.  "
		//"Use %e to print the page encoding. "
		// i guess this is out for now
		//"Use %D to "
		//"print a drop down "
		//"menu for the number of search results to return. "
		"Use %S "
		"to print sort by date or relevance link. Use "
		"%L to "
		"display the logo. Use %R to display radio "
		"buttons for site "
		"search. Use %F to begin the form. and use %H to "
		"insert "
		"hidden text "
		"boxes of parameters like the current search result "
		"page number. "
		"BOTH %F and %H are necessary for the html head, but do "
		"not duplicate them in the html tail. "
		"Use %f to display "
		"the family filter radio buttons. "
		// take this out for now
		//"Directory: Use %s to display the directory "
		//"search type options. "
		//"Use %l to specify the "
		//"location of "
		//"dir=rtl in the body tag for RTL pages. "
		//"Use %where and %when to substitute the where "
		//"and when of "
		//"the query. "
		//"These values may be set based on the cookie "
		//"if "
		//"none was explicitly given. "
		//"IMPORTANT: In the xml configuration file, "
		//"this html "
		//"must be encoded (less thans mapped to &lt;, "
		//"etc.).";
		"Example to paste into textbox: <br><i>";
	s_tmpBuf2.safeStrcpy(fff);
	s_tmpBuf2.htmlEncode(
		"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 "
		"Transitional//EN\">\n"
		"<html>\n"
		"<head>\n"
		"<title>My Gigablast Search Results</title>\n"
		"<meta http-equiv=\"Content-Type\" "
		"content=\"text/html; charset=utf-8\">\n"
		"</head>\n"
		"<body%l>\n"

		//"<form method=\"get\" action=\"/search\" name=\"f\">\n"
		// . %F prints the <form method=...> tag
		// . method will be GET or POST depending on the size of the
		//   input data. MSIE can't handle sending large GETs requests
		//   that are more than like 1k or so, which happens a lot with
		//   our CTS technology (the sites= cgi parm can be very large)
		"%F"
		"<table cellpadding=\"2\" cellspacing=\"0\" border=\"0\">\n"
		"<tr>\n"
		"<td valign=top>"
		// this prints the Logo
		"%L"
		//"<a href=\"/\">"
		//"<img src=\"logo2.gif\" alt=\"Gigablast Logo\" "
		//"width=\"210\" height=\"25\" border=\"0\" valign=\"top\">"
		//"</a>"
		"</td>\n"

		"<td valign=top>\n"
		"<nobr>\n"
		"<input type=\"text\" name=\"q\" size=\"60\" value=\"\%q\"> " 
		// %D is the number of results drop down menu
		"\%D" 
		"<input type=\"submit\" value=\"Blast It!\" border=\"0\">\n"
		"</nobr>\n"
		// family filter
		// %R radio button for site(s) search
		"<br>%f %R\n"
		// directory search options
		// MDW: i guess this is out for now
		//"</td><td>%s</td>\n"
		"</tr>\n"
		"</table>\n"
		// %H prints the hidden for vars. Print them *after* the input 
		// text boxes, radio buttons, etc. so these hidden vars can be 
		// overriden as they should be.
		"%H"); 
	s_tmpBuf2.safePrintf("</i>");
	m->m_desc  = s_tmpBuf2.getBufStart();
	m->m_xml   = "htmlHead";
	m->m_cgi   = "hh";
	m->m_off   = (char *)&cr.m_htmlHead - x;
	m->m_type  = TYPE_SAFEBUF;//STRINGBOX;
	m->m_def   = "";
	//m->m_sparm = 1;
	//m->m_soff  = (char *)&si.m_htmlHead - y;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_TEXTAREA | PF_CLONE;
	m++;


	m->m_title = "html tail";
        static SafeBuf s_tmpBuf3;
	s_tmpBuf3.setLabel("stmpb3");
	s_tmpBuf3.safePrintf("Html to display after the search results. ");
	s_tmpBuf3.safeStrcpy(fff);
	s_tmpBuf3.htmlEncode (
		"<br>\n"
		//"%F"
		"<table cellpadding=2 cellspacing=0 border=0>\n"
		"<tr><td></td>\n"
		//"<td valign=top align=center>\n"
		// this old query is overriding a newer query above so
		// i commented out. mfd 6/2014
		//"<nobr>"
		//"<input type=text name=q size=60 value=\"%q\"> %D\n"
		//"<input type=submit value=\"Blast It!\" border=0>\n"
		//"</nobr>"
		// family filter
		//"<br>%f %R\n"
		//"<br>"
		//"%R\n"
		//"</td>"
		"<td>%s</td>\n"
		"</tr>\n"
		"</table>\n"
		"Try your search on  \n"
		"<a href=http://www.google.com/search?q=%e>google</a> &nbsp;\n"
		"<a href=http://search.yahoo.com/bin/search?p=%e>yahoo</a> "
		"&nbsp;\n"
		//"<a href=http://www.alltheweb.com/search?query=%e>alltheweb"
		//"</a>\n"
		"<a href=http://search.dmoz.org/cgi-bin/search?search=%e>"
		"dmoz</a> &nbsp;\n"
		//"<a href=http://search01.altavista.com/web/results?q=%e>"
		//"alta vista</a>\n"
		//"<a href=http://s.teoma.com/search?q=%e>teoma</a> &nbsp;\n"
		//"<a href=http://wisenut.com/search/query.dll?q=%e>wisenut"
		//"</a>\n"
		"</font></body>\n");
	s_tmpBuf3.safePrintf("</i>");
	m->m_desc  = s_tmpBuf3.getBufStart();
	m->m_xml   = "htmlTail";
	m->m_cgi   = "ht";
	m->m_off   = (char *)&cr.m_htmlTail - x;
	m->m_type  = TYPE_SAFEBUF;//STRINGBOX;
	m->m_def   = "";
	//m->m_sparm = 1;
	//m->m_soff  = (char *)&si.m_htmlHead - y;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_TEXTAREA | PF_CLONE;
	m++;


	m->m_title = "msg40->39 timeout";
	m->m_desc  = "Timeout for Msg40/Msg3a to collect candidate docids with Msg39. In milliseconds";
	m->m_cgi   = "msgfourty_msgthirtynine_timeout";
	m->m_off   = offsetof(Conf,m_msg40_msg39_timeout);
	m->m_xml   = "msg40_msg39_timeout";
	m->m_type  = TYPE_LONG_LONG;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "5000";
	m->m_flags = 0;
	m++;


	m->m_title = "msg3a->39 network overhead";
	m->m_desc  = "Additional overhead/latecny for msg39 request+response over the network";
	m->m_cgi   = "msgthreea_msgthirtynine_network_overhead";
	m->m_off   = offsetof(Conf,m_msg3a_msg39_network_overhead);
	m->m_xml   = "msg3a_msg39_network_overhead";
	m->m_type  = TYPE_LONG_LONG;
	m->m_page  = PAGE_SEARCH;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "250";
	m->m_flags = 0;
	m++;

	///////////////////////////////////////////
	// PAGE SPIDER CONTROLS
	///////////////////////////////////////////

	// just a comment in the conf file
	m->m_desc  = 
		"All <, >, \" and # characters that are values for a field "
		"contained herein must be represented as "
		"&lt;, &gt;, &#34; and &#035; respectively.";
	m->m_type  = TYPE_COMMENT;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "spidering enabled";
	m->m_desc  = "Controls just the spiders for this collection.";
	m->m_cgi   = "cse";
	m->m_off   = (char *)&cr.m_spideringEnabled - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	// this linked list of colls is in Spider.cpp and used to only
	// poll the active spider colls for spidering. so if coll
	// gets paused/unpaused we have to update it.
	m->m_flags = PF_CLONE | PF_REBUILDACTIVELIST;
	m++;

	m->m_title = "site list";
	m->m_xml   = "siteList";
	m->m_desc  = "List of sites to spider, one per line. "
		"See <a href=#examples>example site list</a> below. "
		"Gigablast uses the "
		"<a href=/admin/filters#insitelist>insitelist</a> "
		"directive on "
		"the <a href=/admin/filters>url filters</a> "
		"page to make sure that the spider only indexes urls "
		"that match the site patterns you specify here, other than "
		"urls you add individually via the add urls or inject url "
		"tools. "
		"Limit list to 300MB. If you have a lot of INDIVIDUAL urls "
		"to add then consider using the <a href=/admin/addurl>addurl"
		"</a> interface.";
	m->m_cgi   = "sitelist";
	m->m_off   = (char *)&cr.m_siteListBuf - x;
	m->m_page  = PAGE_SPIDER;// PAGE_SITES;
	m->m_obj   = OBJ_COLL;
	m->m_type  = TYPE_SAFEBUF;
	m->m_func  = CommandUpdateSiteList;
	m->m_def   = "";
	// rebuild urlfilters now will nuke doledb and call updateSiteList()
	m->m_flags = PF_TEXTAREA | PF_REBUILDURLFILTERS | PF_CLONE;
	m++;


#ifndef PRIVACORE_SAFE_VERSION
	m->m_title = "reset collection";
	m->m_desc  = "Remove all documents from the collection and turn "
		"spiders off.";
	m->m_cgi   = "reset";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_func2 = CommandResetColl;
	m->m_cast  = 1;
	m->m_flags = PF_HIDDEN;
	m++;

	m->m_title = "restart collection";
	m->m_desc  = "Remove all documents from the collection and re-add "
		"seed urls from site list.";
	m->m_cgi   = "restart";
	m->m_type  = TYPE_CMD;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_func2 = CommandRestartColl;
	m->m_cast  = 1;
	m++;
#endif

	m->m_title = "max spiders";
	m->m_desc  = "What is the maximum number of web "
		"pages the spider is allowed to download "
		"simultaneously PER HOST for THIS collection? The "
		"maximum number of spiders over all collections is "
		"controlled in the <i>master controls</i>.";
	m->m_cgi   = "mns";
	m->m_off   = (char *)&cr.m_maxNumSpiders - x;
	m->m_type  = TYPE_LONG;
	// make it the hard max so control is really in the master controls
	m->m_def   = "300";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "spider delay in milliseconds";
	m->m_desc  = "make each spider wait this many milliseconds before "
		"getting the ip and downloading the page.";
	m->m_cgi  = "sdms";
	m->m_off   = (char *)&cr.m_spiderDelayInMilliseconds - x; 
	m->m_type  = TYPE_LONG;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++; 

	m->m_title = "obey robots.txt";
	m->m_xml   = "useRobotstxt";
	m->m_desc  = "If this is true Gigablast will respect "
		"the robots.txt convention and rel no follow meta tags.";
	m->m_cgi   = "obeyRobots";
	m->m_off   = (char *)&cr.m_useRobotsTxt - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "obey rel no follow links";
	m->m_desc  = "If this is true Gigablast will respect "
		"the rel no follow link attribute.";
	m->m_cgi   = "obeyRelNoFollow";
	m->m_off   = (char *)&cr.m_obeyRelNoFollowLinks - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "max robots.txt cache age";
	m->m_desc  = "How many seconds to cache a robots.txt file for. "
		"86400 is 1 day. 0 means Gigablast will not read from the "
		"cache at all and will download the robots.txt before every "
		"page if robots.txt use is enabled above. However, if this is "
		"0 then Gigablast will still store robots.txt files in the "
		"cache.";
	m->m_cgi   = "mrca";
	m->m_off   = (char *)&cr.m_maxRobotsCacheAge - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "86400"; // 24*60*60 = 1day
	m->m_units = "seconds";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;


	m->m_title = "always use spider proxies";
	m->m_desc  = "If this is true Gigablast will ALWAYS use the proxies "
		"listed on the <a href=/admin/proxies>proxies</a> "
		"page for "
		"spidering for "
		"this collection."
		//"regardless whether the proxies are enabled "
		//"on the <a href=/admin/proxies>proxies</a> page."
		;
	m->m_cgi   = "useproxies";
	m->m_off   = (char *)&cr.m_forceUseFloaters - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "automatically use spider proxies";
	m->m_desc  = "Use the spider proxies listed on the proxies page "
		"if gb detects that "
		"a webserver is throttling the spiders. This way we can "
		"learn the webserver's spidering policy so that our spiders "
		"can be more polite. If no proxies are listed on the "
		"proxies page then this parameter will have no effect.";
	m->m_cgi   = "automaticallyuseproxies";
	m->m_off   = (char *)&cr.m_automaticallyUseProxies - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;



	m->m_title = "automatically back off";
	m->m_desc  = "Set the crawl delay to 5 seconds if gb detects "
		"that an IP is throttling or banning gigabot from crawling "
		"it. The crawl delay just applies to that IP. "
		"Such throttling will be logged.";
	m->m_cgi   = "automaticallybackoff";
	m->m_xml   = "automaticallyBackOff";
	m->m_off   = (char *)&cr.m_automaticallyBackOff - x;
	m->m_type  = TYPE_BOOL;
	// a lot of pages have recaptcha links but they have valid content
	// so leave this off for now... they have it in a hidden div which
	// popups to email the article link or whatever to someone.
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "use time axis";
	m->m_desc  = "If this is true Gigablast will index the same "
		"url multiple times if its content varies over time, "
		"rather than overwriting the older version in the index. "
		"Useful for archive web pages as they change over time.";
	m->m_cgi   = "usetimeaxis";
	m->m_off   = (char *)&cr.m_useTimeAxis - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "daily merge time";
	m->m_desc  = "Do a tight merge on posdb and titledb at this time "
		"every day. This is expressed in MINUTES past midnight UTC. "
		"UTC is 5 hours ahead "
		"of EST and 7 hours ahead of MST. Leave this as -1 to "
		"NOT perform a daily merge. To merge at midnight EST use "
		"60*5=300 and midnight MST use 60*7=420.";
	m->m_cgi   = "dmt";
	m->m_off   = (char *)&cr.m_dailyMergeTrigger - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "-1";
	m->m_units = "minutes";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "daily merge days";
	m->m_desc  = "Comma separated list of days to merge on. Use "
		"0 for Sunday, 1 for Monday, ... 6 for Saturday. Leaving "
		"this parmaeter empty or without any numbers will make the "
		"daily merge happen every day";
	m->m_cgi   = "dmdl";
	m->m_off   = (char *)&cr.m_dailyMergeDOWList - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = 48;
	// make sunday the default
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "daily merge last started";
	m->m_desc  = "When the daily merge was last kicked off. Expressed in "
		"UTC in seconds since the epoch.";
	m->m_cgi   = "dmls";
	m->m_off   = (char *)&cr.m_dailyMergeStarted - x;
	m->m_type  = TYPE_LONG_CONST;
	m->m_def   = "-1";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_NOAPI;
	m++;

	m->m_title = "max add urls";
	m->m_desc = "Maximum number of urls that can be "
		"submitted via the addurl interface, per IP domain, per "
		"24 hour period. A value less than or equal to zero "
		"implies no limit.";
	m->m_cgi = "mau";
	m->m_off = (char *)&cr.m_maxAddUrlsPerIpDomPerDay - x;
	m->m_type = TYPE_LONG;
	m->m_def = "0";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "do not re-add old outlinks more than this many days";
	m->m_desc  = "If less than this many days have elapsed since the "
		"last time we added the outlinks to spiderdb, do not re-add "
		"them to spiderdb. Saves resources.";
	m->m_cgi   = "slrf";
	m->m_off   = (char *)&cr.m_outlinksRecycleFrequencyDays - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "30";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "deduping enabled";
	m->m_desc  = "When enabled, the spider will "
		"discard web pages which are identical to other web pages "
		"that are already in the index. "//AND that are from the same "
		//"hostname. 
		//"An example of a hostname is www1.ibm.com. "
		"However, root urls, urls that have no path, are never "
		"discarded. It most likely has to hit disk to do these "
		"checks so it does cause some slow down. Only use it if you "
		"need it.";
	m->m_cgi   = "de";
	m->m_off   = (char *)&cr.m_dedupingEnabled - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "deduping enabled for www";
	m->m_desc  = "When enabled, the spider will "
		"discard web pages which, when a www is prepended to the "
		"page's url, result in a url already in the index.";
	m->m_cgi   = "dew";
	m->m_off   = (char *)&cr.m_dupCheckWWW - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "detect custom error pages";
	m->m_desc  = "Detect and do not index pages which have a 200 status"
		" code, but are likely to be error pages.";
	m->m_cgi   = "dcep";
	m->m_off   = (char *)&cr.m_detectCustomErrorPages - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "delete 404s";
	m->m_desc  = "Should pages be removed from the index if they are no "
		"longer accessible on the web?";
	m->m_cgi   = "dnf";
	m->m_off   = (char *)&cr.m_delete404s - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_HIDDEN;
	m++;

	m->m_title = "delete timed out docs";
	m->m_desc  = "Should documents be deleted from the index "
		"if they have been retried them enough times and the "
		"last received error is a time out? "
		"If your internet connection is flaky you may say "
		"no here to ensure you do not lose important docs.";
	m->m_cgi   = "dtod";
	m->m_off   = (char *)&cr.m_deleteTimeouts - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "use simplified redirects";
	m->m_desc  = "If this is true, the spider, when a url redirects "
		"to a \"simpler\" url, will add that simpler url into "
		"the spider queue and abandon the spidering of the current "
		"url.";
	m->m_cgi   = "usr";
	m->m_off   = (char *)&cr.m_useSimplifiedRedirects - x;
	m->m_type  = TYPE_BOOL;
	// turn off for now. spider time deduping should help any issues
	// by disabling this.
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "use canonical redirects";
	m->m_desc  = "If page has a <link canonical> on it then treat it "
		"as a redirect, add it to spiderdb for spidering "
		"and abandon the indexing of the current url.";
	m->m_cgi   = "ucr";
	m->m_off   = (char *)&cr.m_useCanonicalRedirects - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m->m_group = 0;
	m++;

	m->m_title = "use ifModifiedSince";
	m->m_desc  = "If this is true, the spider, when "
		"updating a web page that is already in the index, will "
		"not even download the whole page if it hasn't been "
		"updated since the last time Gigablast spidered it. "
		"This is primarily a bandwidth saving feature. It relies on "
		"the remote webserver's returned Last-Modified-Since field "
		"being accurate.";
	m->m_cgi   = "uims";
	m->m_off   = (char *)&cr.m_useIfModifiedSince - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "build similarity vector from content only";
	m->m_desc  = "If this is true, the spider, when checking the page "
		     "if it has changed enough to reindex or update the "
		     "published date, it will build the vector only from "
		     "the content located on that page.";
	m->m_cgi   = "bvfc";
	m->m_off   = (char *)&cr.m_buildVecFromCont - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "use content similarity to index publish date";
	m->m_desc  = "This requires build similarity from content only to be "
		     "on.  This indexes the publish date (only if the content "
		     "has changed enough) to be between the last two spider "
		     "dates.";
	m->m_cgi   = "uspd";
	m->m_off   = (char *)&cr.m_useSimilarityPublishDate - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "max percentage similar to update publish date";
	m->m_desc  = "This requires build similarity from content only and "
		     "use content similarity to index publish date to be "
		     "on.  This percentage is the maximum similarity that can "
		     "exist between an old document and new before the publish "
		     "date will be updated.";
	m->m_cgi   = "mpspd";
	m->m_off   = (char *)&cr.m_maxPercentSimilarPublishDate - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "80";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "do url sporn checking";
	m->m_desc  = "If this is true and the spider finds "
		"lewd words in the hostname of a url it will throw "
		"that url away. It will also throw away urls that have 5 or "
		"more hyphens in their hostname.";
	m->m_cgi   = "dusc";
	m->m_off   = (char *)&cr.m_doUrlSpamCheck - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "linkdb min files needed to trigger to merge";
	m->m_desc  = "Merge is triggered when this many linkdb data files "
		"are on disk. Raise this when initially growing an index "
		"in order to keep merging down.";
	m->m_cgi   = "mlkftm";
	m->m_off   = (char *)&cr.m_linkdbMinFilesToMerge - x;
	m->m_def   = "6"; 
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_flags = PF_CLONE;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "tagdb min files to merge";
	m->m_desc  = "Merge is triggered when this many linkdb data files "
		"are on disk.";
	m->m_cgi   = "mtftgm";
	m->m_off   = (char *)&cr.m_tagdbMinFilesToMerge - x;
	m->m_def   = "2"; 
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_flags = PF_CLONE;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	// this is overridden by collection
	m->m_title = "titledb min files needed to trigger to merge";
	m->m_desc  = "Merge is triggered when this many titledb data files "
		"are on disk.";
	m->m_cgi   = "mtftm";
	m->m_off   = (char *)&cr.m_titledbMinFilesToMerge - x;
	m->m_def   = "6"; 
	m->m_type  = TYPE_LONG;
	//m->m_save  = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "posdb min files needed to trigger to merge";
	m->m_desc  = "Merge is triggered when this many posdb data files "
		"are on disk. Raise this while doing massive injections "
		"and not doing much querying. Then when done injecting "
		"keep this low to make queries fast.";
	m->m_cgi   = "mpftm";
	m->m_off   = (char *)&cr.m_posdbMinFilesToMerge - x;
	m->m_def   = "6"; 
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_flags = PF_CLONE;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "recycle content";
	m->m_desc   = "Rather than downloading the content again when "
		"indexing old urls, use the stored content. Useful for "
		"reindexing documents under a different ruleset or for "
		"rebuilding an index. You usually "
		"should turn off the 'use robots.txt' switch. "
		"And turn on the 'use old ips' and "
		"'recycle link votes' switches for speed. If rebuilding an "
		"index then you should turn off the 'only index changes' "
		"switches.";
	m->m_cgi   = "rc";
	m->m_off   = (char *)&cr.m_recycleContent - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "enable link voting";
	m->m_desc  = "If this is true Gigablast will "
		"index hyper-link text and use hyper-link "
		"structures to boost the quality of indexed documents. "
		"You can disable this when doing a ton of injections to "
		"keep things fast. Then do a posdb (index) rebuild "
		"after re-enabling this when you are done injecting. Or "
		"if you simply do not want link voting this will speed up"
		"your injections and spidering a bit.";
	m->m_cgi   = "glt";
	m->m_off   = (char *)&cr.m_getLinkInfo - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_CLONE|PF_API;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "compute inlinks to sites";
	m->m_desc  = "If this is true Gigablast will "
		"compute the number of site inlinks for the sites it "
		"indexes. This is a measure of the sites popularity and is "
		"used for ranking and some times spidering prioritzation. "
		"It will cache the site information in tagdb. "
		"The greater the number of inlinks, the longer the cached "
		"time, because the site is considered more stable. If this "
		"is NOT true then Gigablast will use the included file, "
		"sitelinks.txt, which stores the site inlinks of millions "
		"of the most popular sites. This is the fastest way. If you "
		"notice a lot of <i>getting link info</i> requests in the "
		"<i>sockets table</i> you may want to disable this "
		"parm.";
	m->m_cgi   = "csni";
	m->m_off   = (char *)&cr.m_computeSiteNumInlinks - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_CLONE|PF_API;//PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "do link spam checking";
	m->m_desc  = "If this is true, do not allow spammy inlinks to vote. "
		"This check is "
		"too aggressive for some collections, i.e.  it "
		"does not allow pages with cgi in their urls to vote.";
	m->m_cgi   = "dlsc";
	m->m_off   = (char *)&cr.m_doLinkSpamCheck - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "restrict link voting by ip";
	m->m_desc  = "If this is true Gigablast will "
		"only allow one vote per the top 2 significant bytes "
		"of the IP address. Otherwise, multiple pages "
		"from the same top IP can contribute to the link text and "
		"link-based quality ratings of a particular URL. "
		"Furthermore, no votes will be accepted from IPs that have "
		"the same top 2 significant bytes as the IP of the page "
		"being indexed.";
	m->m_cgi   = "ovpid";
	m->m_off   = (char *)&cr.m_oneVotePerIpDom - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "use new link algo";
	m->m_desc  = "Use the links: termlists instead of link:. Also "
		"allows pages linking from the same domain or IP to all "
		"count as a single link from a different IP. This is also "
		"required for incorporating RSS and Atom feed information "
		"when indexing a document.";
	m->m_cgi   = "na";
	m->m_off   = (char *)&cr.m_newAlgo - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "update link info frequency";
	m->m_desc   = "How often should Gigablast recompute the "
		"link info for a url. "
		"Also applies to getting the quality of a site "
		"or root url, which is based on the link info. "
		"In days. Can use decimals. 0 means to update "
		"the link info every time the url's content is re-indexed. "
		"If the content is not reindexed because it is unchanged "
		"then the link info will not be updated. When getting the "
		"link info or quality of the root url from an "
		"external cluster, Gigablast will tell the external cluster "
		"to recompute it if its age is this or higher.";
	m->m_cgi   = "uvf";
	m->m_off   = (char *)&cr.m_updateVotesFreq - x;
	m->m_type  = TYPE_FLOAT;
	m->m_def   = "60.000000";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "index inlink neighborhoods";
	m->m_desc  = "If this is true Gigablast will "
		"index the plain text surrounding the hyper-link text. The "
		"score will be x times that of the hyper-link text, where x "
		"is the scalar below.";
	m->m_cgi   = "iin";
	m->m_off   = (char *)&cr.m_indexInlinkNeighborhoods - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "tagdb collection name";
	m->m_desc  = "Sometimes you want the spiders to use the tagdb of "
		"another collection, like the <i>main</i> collection. "
		"If this is empty it defaults to the current collection.";
	m->m_cgi   = "tdbc";
	m->m_off   = (char *)&cr.m_tagdbColl - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_COLL_LEN+1;
	m->m_def   = "";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "allow adult docs";
	m->m_desc  = "If this is disabled the spider "
		"will not allow any docs which contain adult content "
		"into the index (overides tagdb).";
	m->m_cgi   = "aprnd";
	m->m_off   = (char *)&cr.m_allowAdultDocs - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group =  0 ;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "do serp detection";
	m->m_desc  = "If this is eabled the spider "
		"will not allow any docs which are determined to "
		"be serps.";
	m->m_cgi   = "dsd";
	m->m_off   = (char *)&cr.m_doSerpDetection - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "do IP lookup";
	m->m_desc  = "If this is disabled and the proxy "
		"IP below is not zero then Gigablast will assume "
		"all spidered URLs have an IP address of 1.2.3.4.";
	m->m_cgi   = "dil";
	m->m_off   = (char *)&cr.m_doIpLookups - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "use old IPs";
	m->m_desc = "Should the stored IP "
		"of documents we are reindexing be used? Useful for "
		"pages banned by IP address and then reindexed with "
		"the reindexer tool.";
	m->m_cgi = "useOldIps";
	m->m_off = (char *)&cr.m_useOldIps - x;
	m->m_type = TYPE_BOOL;
	m->m_def = "0";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "remove banned pages";
	m->m_desc  = "Remove banned pages from the index. Pages can be "
		"banned using tagdb or the Url Filters table.";
	m->m_cgi   = "rbp";
	m->m_off   = (char *)&cr.m_removeBannedPages - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "allow HTTPS pages using SSL";
	m->m_desc  = "If this is true, spiders will read "
		     "HTTPS pages using SSL Protocols.";
	m->m_cgi   = "ahttps";
	m->m_off   = (char *)&cr.m_allowHttps - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "apply filter to text pages";
	m->m_desc  = "If this is false then the filter "
		"will not be used on html or text pages.";
	m->m_cgi   = "aft";
	m->m_off   = (char *)&cr.m_applyFilterToText - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "filter name";
	m->m_desc  = "Program to spawn to filter all HTTP "
		"replies the spider receives. Leave blank for none.";
	m->m_cgi   = "filter";
	m->m_def   = "";
	m->m_off   = (char *)&cr.m_filter - x;
	m->m_type  = TYPE_STRING;
	m->m_size  = MAX_FILTER_LEN+1;
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "filter timeout";
	m->m_desc  = "Kill filter shell after this many seconds. Assume it "
		"stalled permanently.";
	m->m_cgi   = "fto";
	m->m_def   = "40";
	m->m_off   = (char *)&cr.m_filterTimeout - x;
	m->m_type  = TYPE_LONG;
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "make image thumbnails";
	m->m_desc  = "Try to find the best image on each page and "
		"store it as a thumbnail for presenting in the search "
		"results.";
	m->m_cgi   = "mit";
	m->m_off   = (char *)&cr.m_makeImageThumbnails - x;
	m->m_type  = TYPE_BOOL;
	// default to off since it slows things down to do this
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "max thumbnail width or height";
	m->m_desc  = "This is in pixels and limits the size of the thumbnail. "
		"Gigablast tries to make at least the width or the height "
		"equal to this maximum, but, unless the thumbnail is sqaure, "
		"one side will be longer than the other.";
	m->m_cgi   = "mtwh";
	m->m_off   = (char *)&cr.m_thumbnailMaxWidthHeight - x;
	m->m_type  = TYPE_LONG;
	m->m_def   = "250";
	m->m_group = 0;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	m->m_title = "index spider status documents";
	m->m_desc  = "Index a spider status \"document\" "
		"for every url the spider "
		"attempts to spider. Search for them using special "
		"query operators like type:status or gberrorstr:success or "
		"stats:gberrornum to get a histogram. "
		"See <a href=/syntax.html>syntax</a> page for more examples. "
		"They will not otherwise "
		"show up in the search results.";
	//      "This will not work for "
	// 	"diffbot crawlbot collections yet until it has proven "
	// 	"more stable.";
	m->m_cgi   = "isr";
	m->m_off   = (char *)&cr.m_indexSpiderReplies - x;
	m->m_type  = TYPE_BOOL;
	// default off for now until we fix it better. 5/26/14 mdw
	// turn back on 6/21 now that we do not index plain text terms
	// and we add gbdocspidertime and gbdocindextime terms so you
	// can use those to sort regular docs and not have spider reply
	// status docs in the serps.
	// back on 4/21/2015 seems pretty stable.
	// but it uses disk space so turn off for now again. 6/16/2015
	m->m_def   = "0";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE;
	m++;

	// i put this in here so i can save disk space for my global
	// diffbot json index
	m->m_title = "index body";
	m->m_desc  = "Index the body of the documents so you can search it. "
		"Required for searching that. You wil pretty much always "
		"want to keep this enabled. Does not apply to JSON "
		"documents.";
	m->m_cgi   = "ib";
	m->m_off   = (char *)&cr.m_indexBody - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_flags = PF_CLONE ;//| PF_HIDDEN; 
	m++;

	m->m_cgi   = "apiUrl";
	m->m_desc  = "Send every spidered url to this url and index "
		"the reply in addition to the normal indexing process. "
		"Example: by specifying http://api.diffbot.com/v3/"
		"analyze?mode=high-precision&token=<yourDiffbotToken> here "
		"you can index the structured JSON replies from diffbot for "
		"every url that is spidered. "
		"Gigablast will automatically "
		"append a &url=<urlBeingSpidered> to this url "
		"before sending it to diffbot.";
	m->m_xml   = "diffbotApiUrl";
	m->m_title = "diffbot api url";
	m->m_off   = (char *)&cr.m_diffbotApiUrl - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_SPIDER;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m->m_def   = "";
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_cgi   = "urlProcessPatternTwo";
	m->m_desc  = "Only send urls that match this simple substring "
		"pattern to Diffbot. Separate substrings with two pipe "
		"operators, ||. Leave empty for no restrictions.";
	m->m_xml   = "diffbotUrlProcessPattern";
	m->m_title = "diffbot url process pattern";
	m->m_off   = (char *)&cr.m_diffbotUrlProcessPattern - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_group = 0;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;

	m->m_cgi   = "urlProcessRegExTwo";
	m->m_desc  = "Only send urls that match this regular expression "
		"to Diffbot. "
		"Leave empty for no restrictions.";
	m->m_xml   = "diffbotUrlProcessRegEx";
	m->m_title = "diffbot url process regex";
	m->m_off   = (char *)&cr.m_diffbotUrlProcessRegEx - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_group = 0;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;

	m->m_cgi   = "pageProcessPatternTwo";
	m->m_desc  = "Only send urls whose content matches this simple "
		"substring "
		"pattern to Diffbot. Separate substrings with two pipe "
		"operators, ||. "
		"Leave empty for no restrictions.";
	m->m_xml   = "diffbotPageProcessPattern";
	m->m_title = "diffbot page process pattern";
	m->m_off   = (char *)&cr.m_diffbotPageProcessPattern - x;
	m->m_type  = TYPE_SAFEBUF;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m->m_def   = "";
	m->m_group = 0;
	m->m_flags = PF_REBUILDURLFILTERS | PF_CLONE;
	m++;



	m->m_title = "spider start time";
	m->m_desc  = "Only spider URLs scheduled to be spidered "
		"at this time or after. In UTC.";
	m->m_cgi   = "sta";
	m->m_off   = (char *)&cr.m_spiderTimeMin - x;
	m->m_type  = TYPE_DATE; // date format -- very special
	m->m_def   = "01 Jan 1970";
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "spider end time";
	m->m_desc  = "Only spider URLs scheduled to be spidered "
		"at this time or before. If \"use current time\" is true "
		"then the current local time is used for this value instead. "
		"in UTC.";
	m->m_cgi   = "stb";
	m->m_off   = (char *)&cr.m_spiderTimeMax - x;
	m->m_type  = TYPE_DATE2;
	m->m_def   = "01 Jan 2010";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	m->m_title = "use current time";
	m->m_desc  = "Use the current time as the spider end time?";
	m->m_cgi   = "uct";
	m->m_off   = (char *)&cr.m_useCurrentTime - x;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_group = 0;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_SPIDER;
	m->m_obj   = OBJ_COLL;
	m++;

	////////////////
	// END PAGE SPIDER CONTROLS
	////////////////


	///////////////////////////////////////////
	//  PAGE REPAIR CONTROLS
	///////////////////////////////////////////
#ifndef PRIVACORE_SAFE_VERSION

	m->m_title = "rebuild mode enabled";
	m->m_desc  = "If enabled, gigablast will rebuild the rdbs as "
		"specified by the parameters below. When a particular "
		"collection is in rebuild mode, it can not spider or merge "
		"titledb files.";
	m->m_cgi   = "rme";
	m->m_off   = (char *)&g_conf.m_repairingEnabled - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_sync  = false;  // do not sync this parm
	m++;

	m->m_title = "collection to rebuild";
	m->m_xml   = "collectionToRebuild";
	m->m_desc  = "Name of collection to rebuild.";
	// m->m_desc  = "Comma or space separated list of the collections "
	// 	"to rebuild.";
	m->m_cgi   = "rctr"; // repair collections to repair
	m->m_off   = (char *)&g_conf.m_collsToRepair - g;
	m->m_type  = TYPE_SAFEBUF;//STRING;
	//m->m_size  = 1024;
	m->m_def   = "";
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_group = 0;
	m->m_flags = PF_REQUIRED;// | PF_COLLDEFAULT;//| PF_NOHTML;
	m++;

	m->m_title = "rebuild ALL collections";
	m->m_desc  = "If enabled, gigablast will rebuild all collections.";
	m->m_cgi   = "rac";
	m->m_off   = (char *)&g_conf.m_rebuildAllCollections - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_group = 0;
	m++;


	m->m_title = "memory to use for rebuild";
	m->m_desc  = "In bytes.";
	m->m_cgi   = "rmtu"; // repair mem to use
	m->m_off   = (char *)&g_conf.m_repairMem - g;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "200000000";
	m->m_units = "bytes";
	m->m_group = 0;
	m++;

	m->m_title = "max rebuild injections";
	m->m_desc  = "Maximum number of outstanding injections for "
		"rebuild.";
	m->m_cgi   = "mrps";
	m->m_off   = (char *)&g_conf.m_maxRepairSpiders - g;
	m->m_type  = TYPE_LONG;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "2";
	m->m_group = 0;
	m++;

	m->m_title = "full rebuild";
	m->m_desc  = "If enabled, gigablast will reinject the content of "
		"all title recs into a secondary rdb system. That will "
		"the primary rdb system when complete.";
	m->m_cgi   = "rfr"; // repair full rebuild
	m->m_off   = (char *)&g_conf.m_fullRebuild - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "1";
	m->m_group = 0;
	m++;

	m->m_title = "add spiderdb recs of non indexed urls";
	m->m_desc  = "If enabled, gigablast will add the spiderdb "
		"records of unindexed urls "
		"when doing the full rebuild or the spiderdb "
		"rebuild. Otherwise, only the indexed urls will get "
		"spiderdb records in spiderdb. This can be faster because "
		"Gigablast does not have to do an IP lookup on every url "
		"if its IP address is not in tagdb already.";
	m->m_cgi   = "rfrknsx";
	m->m_off   = (char *)&g_conf.m_rebuildAddOutlinks - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_group = 0;
	m++;

	m->m_title = "recycle link text";
	m->m_desc  = "If enabled, gigablast will recycle the link text "
		"when rebuilding titledb. "
		"The siterank, which is determined by the "
		"number of inlinks to a site, is stored/cached in tagdb "
		"so that is a separate item. If you want to pick up new "
		"link text you will want to set this to <i>NO</i> and "
		"make sure to rebuild titledb, since that stores the "
		"link text.";
	m->m_cgi   = "rrli"; // repair full rebuild
	m->m_off   = (char *)&g_conf.m_rebuildRecycleLinkInfo - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "1";
	m->m_group = 0;
	m++;

	m->m_title = "rebuild titledb";
	m->m_desc  = "If enabled, gigablast will rebuild this rdb";
	m->m_cgi   = "rrt"; // repair rebuild titledb
	m->m_off   = (char *)&g_conf.m_rebuildTitledb - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m++;

	m->m_title = "rebuild posdb";
	m->m_desc  = "If enabled, gigablast will rebuild this rdb";
	m->m_cgi   = "rri";
	m->m_off   = (char *)&g_conf.m_rebuildPosdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_group = 0;
	m++;

	m->m_title = "rebuild clusterdb";
	m->m_desc  = "If enabled, gigablast will rebuild this rdb";
	m->m_cgi   = "rrcl";
	m->m_off   = (char *)&g_conf.m_rebuildClusterdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_group = 0;
	m++;

	m->m_title = "rebuild spiderdb";
	m->m_desc  = "If enabled, gigablast will rebuild this rdb";
	m->m_cgi   = "rrsp";
	m->m_off   = (char *)&g_conf.m_rebuildSpiderdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_group = 0;
	m++;

	m->m_title = "rebuild linkdb";
	m->m_desc  = "If enabled, gigablast will rebuild this rdb";
	m->m_cgi   = "rrld";
	m->m_off   = (char *)&g_conf.m_rebuildLinkdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "0";
	m->m_group = 0;
	m++;

	m->m_title = "rebuild root urls";
	m->m_desc  = "If disabled, gigablast will skip root urls.";
	m->m_cgi   = "ruru";
	m->m_off   = (char *)&g_conf.m_rebuildRoots - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "1";
	m++;

	m->m_title = "rebuild non-root urls";
	m->m_desc  = "If disabled, gigablast will skip non-root urls.";
	m->m_cgi   = "runru";
	m->m_off   = (char *)&g_conf.m_rebuildNonRoots - g;
	m->m_type  = TYPE_BOOL;
	m->m_page  = PAGE_REPAIR;
	m->m_obj   = OBJ_CONF;
	m->m_def   = "1";
	m->m_group = 0;
	m++;

#endif
	///////////////////////////////////////////
	//          END PAGE REPAIR              //
	///////////////////////////////////////////


	///////////////////////////////////////////
	// ROOT PASSWORDS page
	///////////////////////////////////////////


	m->m_title = "Master Passwords";
	m->m_desc  = "Whitespace separated list of passwords. "
		"Any matching password will have administrative access "
		"to Gigablast and all collections.";
		//"If no Admin Password or Admin IP is specified then "
		//"Gigablast will only allow local IPs to connect to it "
		//"as the master admin.";
	m->m_cgi   = "masterpwds";
	m->m_xml   = "masterPasswords";
	m->m_def   = "";
	m->m_obj   = OBJ_CONF;
	m->m_off   = (char *)&g_conf.m_masterPwds - g;
	m->m_type  = TYPE_SAFEBUF; // STRINGNONEMPTY;
	m->m_page  = PAGE_MASTERPASSWORDS;
	//m->m_max   = MAX_MASTER_PASSWORDS;
	//m->m_size  = PASSWORD_MAX_LEN+1;
	//m->m_addin = 1; // "insert" follows?
	m->m_flags = PF_PRIVATE | PF_TEXTAREA | PF_SMALLTEXTAREA;
	m++;


	m->m_title = "Master IPs";
	//m->m_desc = "Allow UDP requests from this list of IPs. Any datagram "
	//	"received not coming from one of these IPs, or an IP in "
	//	"hosts.conf, is dropped. If another cluster is accessing this "
	//	"cluster for getting link text or whatever, you will need to "
	//	"list the IPs of the accessing machines here. These IPs are "
	//	"also used to allow access to the HTTP server even if it "
	//	"was disabled in the Master Controls. IPs that have 0 has "
	//	"their Least Significant Byte are treated as wildcards for "
	//	"IP blocks. That is, 192.0.2.0 means 1.2.3.*.";
	m->m_desc  = "Whitespace separated list of Ips. "
		"Any IPs in this list will have administrative access "
		"to Gigablast and all collections.";
	m->m_cgi   = "masterips";
	m->m_xml   = "masterIps";
	m->m_page  = PAGE_MASTERPASSWORDS;
	m->m_off   = (char *)&g_conf.m_connectIps - g;
	m->m_type  = TYPE_SAFEBUF;//IP;
	m->m_def   = "";
	m->m_obj   = OBJ_CONF;
	m->m_flags = PF_PRIVATE | PF_TEXTAREA | PF_SMALLTEXTAREA;
	m++;

	m->m_title = "Collection Passwords";
	m->m_desc  = "Whitespace separated list of passwords. "
		"Any matching password will have administrative access "
		"to the controls for just this collection. The master "
		"password and IPs are controled through the "
		"<i>master passwords</i> link under the ADVANCED controls "
		"tab. The master passwords or IPs have administrative "
		"access to all collections.";
	m->m_cgi   = "collpwd";
	m->m_xml   = "collectionPasswords";
	m->m_obj   = OBJ_COLL;
	m->m_off   = (char *)&cr.m_collectionPasswords - x;
	m->m_def   = "";
	m->m_type  = TYPE_SAFEBUF; // STRINGNONEMPTY;
	m->m_page  = PAGE_COLLPASSWORDS;
	m->m_flags = PF_PRIVATE | PF_TEXTAREA | PF_SMALLTEXTAREA;
	m++;

	m->m_title = "Collection IPs";
	m->m_desc  = "Whitespace separated list of IPs. "
		"Any matching IP will have administrative access "
		"to the controls for just this collection.";
	m->m_cgi   = "collips";
	m->m_xml   = "collectionIps";
	m->m_obj   = OBJ_COLL;
	m->m_off   = (char *)&cr.m_collectionIps - x;
	m->m_def   = "";
	m->m_type  = TYPE_SAFEBUF; // STRINGNONEMPTY;
	m->m_page  = PAGE_COLLPASSWORDS;
	m->m_flags = PF_PRIVATE | PF_TEXTAREA | PF_SMALLTEXTAREA;
	m++;


	//////
	// END SECURITY CONTROLS
	//////


	///////////////////////////////////////////
	// LOG CONTROLS
	///////////////////////////////////////////

	m->m_title = "log http requests";
	m->m_desc  = "Log GET and POST requests received from the "
		"http server?";
	m->m_cgi   = "hr";
	m->m_off   = (char *)&g_conf.m_logHttpRequests - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log autobanned queries";
	m->m_desc  = "Should we log queries that are autobanned? "
		"They can really fill up the log.";
	m->m_cgi   = "laq";
	m->m_off   = (char *)&g_conf.m_logAutobannedQueries - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log query time threshold";
	m->m_desc  = "If query took this many millliseconds or longer, then log the "
		"query and the time it took to process.";
	m->m_cgi   = "lqtt";
	m->m_off   = (char *)&g_conf.m_logQueryTimeThreshold- g;
	m->m_type  = TYPE_LONG;
	m->m_def   = "5000";
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log query reply";
	m->m_desc  = "Log query reply in proxy, but only for those queries "
		"above the time threshold above.";
	m->m_cgi   = "lqr";
	m->m_off   = (char *)&g_conf.m_logQueryReply - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_group = 0;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log spidered urls";
	m->m_desc  = "Log status of spidered or injected urls?";
	m->m_cgi   = "lsu";
	m->m_off   = (char *)&g_conf.m_logSpideredUrls - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log network congestion";
	m->m_desc  = "Log messages if Gigablast runs out of udp sockets?";
	m->m_cgi   = "lnc";
	m->m_off   = (char *)&g_conf.m_logNetCongestion - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log informational messages";
	m->m_desc  = "Log messages not related to an error condition, "
		"but meant more to give an idea of the state of "
		"the gigablast process. These can be useful when "
		"diagnosing problems.";
	m->m_cgi   = "li";
	m->m_off   = (char *)&g_conf.m_logInfo - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "1";
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log limit breeches";
	m->m_desc  = "Log it when document not added due to quota "
		"breech. Log it when url is too long and it gets "
		"truncated.";
	m->m_cgi   = "ll";
	m->m_off   = (char *)&g_conf.m_logLimits - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug admin messages";
	m->m_desc  = "Log various debug messages.";
	m->m_cgi   = "lda";
	m->m_off   = (char *)&g_conf.m_logDebugAdmin - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug build messages";
	m->m_cgi   = "ldb";
	m->m_off   = (char *)&g_conf.m_logDebugBuild - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug build time messages";
	m->m_cgi   = "ldbt";
	m->m_off   = (char *)&g_conf.m_logDebugBuildTime - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug database messages";
	m->m_cgi   = "ldd";
	m->m_off   = (char *)&g_conf.m_logDebugDb - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug dirty messages";
	m->m_cgi   = "lddm";
	m->m_off   = (char *)&g_conf.m_logDebugDirty - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug disk messages";
	m->m_cgi   = "lddi";
	m->m_off   = (char *)&g_conf.m_logDebugDisk - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug disk page cache";
	m->m_cgi   = "ldpc";
	m->m_off   = (char *)&g_conf.m_logDebugDiskPageCache - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug dns messages";
	m->m_cgi   = "lddns";
	m->m_off   = (char *)&g_conf.m_logDebugDns - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug http messages";
	m->m_cgi   = "ldh";
	m->m_off   = (char *)&g_conf.m_logDebugHttp - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug image messages";
	m->m_cgi   = "ldi";
	m->m_off   = (char *)&g_conf.m_logDebugImage - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug loop messages";
	m->m_cgi   = "ldl";
	m->m_off   = (char *)&g_conf.m_logDebugLoop - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug language detection messages";
	m->m_cgi   = "ldg";
	m->m_off   = (char *)&g_conf.m_logDebugLang - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug link info";
	m->m_cgi   = "ldli";
	m->m_off   = (char *)&g_conf.m_logDebugLinkInfo - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug mem messages";
	m->m_cgi   = "ldm";
	m->m_off   = (char *)&g_conf.m_logDebugMem - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug mem usage messages";
	m->m_cgi   = "ldmu";
	m->m_off   = (char *)&g_conf.m_logDebugMemUsage - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug net messages";
	m->m_cgi   = "ldn";
	m->m_off   = (char *)&g_conf.m_logDebugNet - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug post query rerank messages";
	m->m_cgi   = "ldpqr";
	m->m_off   = (char *)&g_conf.m_logDebugPQR - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_flags = PF_HIDDEN | PF_NOSAVE;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug query messages";
	m->m_cgi   = "ldq";
	m->m_off   = (char *)&g_conf.m_logDebugQuery - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug quota messages";
	m->m_cgi   = "ldqta";
	m->m_off   = (char *)&g_conf.m_logDebugQuota - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug robots messages";
	m->m_cgi   = "ldr";
	m->m_off   = (char *)&g_conf.m_logDebugRobots - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug spider cache messages";
	m->m_cgi   = "lds";
	m->m_off   = (char *)&g_conf.m_logDebugSpcache - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug speller messages";
	m->m_cgi   = "ldsp";
	m->m_off   = (char *)&g_conf.m_logDebugSpeller - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug sections messages";
	m->m_cgi   = "ldscc";
	m->m_off   = (char *)&g_conf.m_logDebugSections - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug seo insert messages";
	m->m_cgi   = "ldsi";
	m->m_off   = (char *)&g_conf.m_logDebugSEOInserts - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug seo messages";
	m->m_cgi   = "ldseo";
	m->m_off   = (char *)&g_conf.m_logDebugSEO - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug stats messages";
	m->m_cgi   = "ldst";
	m->m_off   = (char *)&g_conf.m_logDebugStats - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug summary messages";
	m->m_cgi   = "ldsu";
	m->m_off   = (char *)&g_conf.m_logDebugSummary - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug spider messages";
	m->m_cgi   = "ldspid";
	m->m_off   = (char *)&g_conf.m_logDebugSpider - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug msg13 messages";
	m->m_cgi   = "ldspmth";
	m->m_off   = (char *)&g_conf.m_logDebugMsg13 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug spider proxies";
	m->m_cgi   = "ldspr";
	m->m_off   = (char *)&g_conf.m_logDebugProxies - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug url attempts";
	m->m_cgi   = "ldspua";
	m->m_off   = (char *)&g_conf.m_logDebugUrlAttempts - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug spider downloads";
	m->m_cgi   = "ldsd";
	m->m_off   = (char *)&g_conf.m_logDebugDownloads - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug facebook";
	m->m_cgi   = "ldfb";
	m->m_off   = (char *)&g_conf.m_logDebugFacebook - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug tagdb messages";
	m->m_cgi   = "ldtm";
	m->m_off   = (char *)&g_conf.m_logDebugTagdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug tcp messages";
	m->m_cgi   = "ldt";
	m->m_off   = (char *)&g_conf.m_logDebugTcp - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug tcp buffer messages";
	m->m_cgi   = "ldtb";
	m->m_off   = (char *)&g_conf.m_logDebugTcpBuf - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug thread messages";
	m->m_cgi   = "ldth";
	m->m_off   = (char *)&g_conf.m_logDebugThread - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug title messages";
	m->m_cgi   = "ldti";
	m->m_off   = (char *)&g_conf.m_logDebugTitle - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug timedb messages";
	m->m_cgi   = "ldtim";
	m->m_off   = (char *)&g_conf.m_logDebugTimedb - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug topic messages";
	m->m_cgi   = "ldto";
	m->m_off   = (char *)&g_conf.m_logDebugTopics - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug topDoc messages";
	m->m_cgi   = "ldtopd";
	m->m_off   = (char *)&g_conf.m_logDebugTopDocs - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug udp messages";
	m->m_cgi   = "ldu";
	m->m_off   = (char *)&g_conf.m_logDebugUdp - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug unicode messages";
	m->m_cgi   = "ldun";
	m->m_off   = (char *)&g_conf.m_logDebugUnicode - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug repair messages";
	m->m_cgi   = "ldre";
	m->m_off   = (char *)&g_conf.m_logDebugRepair - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log debug pub date extraction messages";
	m->m_cgi   = "ldpd";
	m->m_off   = (char *)&g_conf.m_logDebugDate - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log very detailed debug information";
	m->m_cgi   = "lvdd";
	m->m_off   = (char *)&g_conf.m_logDebugDetailed - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "log trace info for BigFile";
	m->m_cgi   = "ltrc_bf";
	m->m_off   = (char *)&g_conf.m_logTraceBigFile - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for Posdb";
	m->m_cgi   = "ltrc_posdb";
	m->m_off   = (char *)&g_conf.m_logTracePosdb - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for RdbBase";
	m->m_cgi   = "ltrc_rb";
	m->m_off   = (char *)&g_conf.m_logTraceRdbBase - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for RdbMap";
	m->m_cgi   = "ltrc_rm";
	m->m_off   = (char *)&g_conf.m_logTraceRdbMap - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for Repairs";
	m->m_cgi   = "ltrc_rp";
	m->m_off   = (char *)&g_conf.m_logTraceRepairs - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for Spider";
	m->m_cgi   = "ltrc_sp";
	m->m_off   = (char *)&g_conf.m_logTraceSpider - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for Msg0";
	m->m_cgi   = "ltrc_msgzero";
	m->m_off   = (char *)&g_conf.m_logTraceMsg0 - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log trace info for XmlDoc";
	m->m_cgi   = "ltrc_xmldoc";
	m->m_off   = (char *)&g_conf.m_logTraceXmlDoc - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "log trace info for network messages (excessive!)";
	m->m_cgi   = "trcmsg";
	m->m_off   = (char *)&g_conf.m_logTraceMsg - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;


	m->m_title = "log timing messages for build";
	m->m_desc  = "Log various timing related messages.";
	m->m_cgi   = "ltb";
	m->m_off   = (char *)&g_conf.m_logTimingBuild - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log timing messages for admin";
	m->m_desc  = "Log various timing related messages.";
	m->m_cgi   = "ltadm";
	m->m_off   = (char *)&g_conf.m_logTimingAdmin - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log timing messages for database";
	m->m_cgi   = "ltd";
	m->m_off   = (char *)&g_conf.m_logTimingDb - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log timing messages for network layer";
	m->m_cgi   = "ltn";
	m->m_off   = (char *)&g_conf.m_logTimingNet - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log timing messages for query";
	m->m_cgi   = "ltq";
	m->m_off   = (char *)&g_conf.m_logTimingQuery - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log timing messages for spcache";
	m->m_desc  = "Log various timing related messages.";
	m->m_cgi   = "ltspc";
	m->m_off   = (char *)&g_conf.m_logTimingSpcache - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log timing messages for related topics";
	m->m_cgi   = "ltt";
	m->m_off   = (char *)&g_conf.m_logTimingTopics - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	m->m_title = "log reminder messages";
	m->m_desc  = "Log reminders to the programmer. You do not need this.";
	m->m_cgi   = "lr";
	m->m_off   = (char *)&g_conf.m_logReminders - g;
	m->m_type  = TYPE_BOOL;
	m->m_def   = "0";
	m->m_priv  = 1;
	m->m_page  = PAGE_LOG;
	m->m_obj   = OBJ_CONF;
	m++;

	/////
	// END PAGE LOG CONTROLS
	/////


	// END PARMS PARM END PARMS END


	m_numParms = m - m_parms;

	// sanity check
	if ( m_numParms >= MAX_PARMS ) {
		log("admin: Boost MAX_PARMS.");
		exit(-1);
	}
	
	// make xml tag names and store in here
	static char s_tbuf [ 18000 ];
	char *p    = s_tbuf;
	char *pend = s_tbuf + 18000;
	int32_t  size;
	char  t;

	// . set hashes of title
	// . used by Statsdb.cpp for identifying a parm
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		if ( ! m_parms[i].m_title ) continue;
		m_parms[i].m_hash = hash32n ( m_parms[i].m_title );
	}

	// cgi hashes
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		if ( ! m_parms[i].m_cgi ) continue;
		m_parms[i].m_cgiHash = hash32n ( m_parms[i].m_cgi );
	}

	// sanity check: ensure all cgi parms are different
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
	for ( int32_t j = 0 ; j < m_numParms ; j++ ) {
		if ( j == i             ) continue;
		if ( m_parms[i].m_type == TYPE_BOOL2 ) continue;
		if ( m_parms[j].m_type == TYPE_BOOL2 ) continue;
		if ( m_parms[i].m_type == TYPE_CMD   ) continue;
		if ( m_parms[j].m_type == TYPE_CMD   ) continue;
		if ( m_parms[i].m_type == TYPE_FILEUPLOADBUTTON ) continue;
		if ( m_parms[j].m_type == TYPE_FILEUPLOADBUTTON ) continue;
		if ( m_parms[i].m_obj == OBJ_NONE ) continue;
		if ( m_parms[j].m_obj == OBJ_NONE ) continue;
		if ( m_parms[i].m_flags & PF_DUP ) continue;
		if ( m_parms[j].m_flags & PF_DUP ) continue;
		// hack to allow "c" for search, inject, addurls
		if ( m_parms[j].m_page != m_parms[i].m_page &&
		     m_parms[i].m_obj != OBJ_COLL &&
		     m_parms[i].m_obj != OBJ_CONF ) 
			continue;
		if ( ! m_parms[i].m_cgi ) continue;
		if ( ! m_parms[j].m_cgi ) continue;
		// gotta be on same page now i guess
		int32_t obj1 = m_parms[i].m_obj;
		int32_t obj2 = m_parms[j].m_obj;
		if ( obj1 != OBJ_COLL && obj1 != OBJ_CONF ) continue;
		if ( obj2 != OBJ_COLL && obj2 != OBJ_CONF ) continue;
		//if ( m_parms[i].m_page != m_parms[j].m_page ) continue;
		// a different m_scmd means a different cgi parm really...
		//if ( m_parms[i].m_sparm && m_parms[j].m_sparm &&
		//     strcmp ( m_parms[i].m_scmd, m_parms[j].m_scmd) != 0 )
		//	continue;
		if ( strcmp ( m_parms[i].m_cgi , m_parms[j].m_cgi ) != 0 &&
		     // ensure cgi hashes are different as well!
		     m_parms[i].m_cgiHash != m_parms[j].m_cgiHash )
			continue;
		// upload file buttons are always dup of another parm
		if ( m_parms[j].m_type == TYPE_FILEUPLOADBUTTON )
			continue;
		log(LOG_LOGIC,"conf: Cgi parm for #%"INT32" \"%s\" "
		    "matches #%"INT32" \"%s\". Exiting.",
		    i,m_parms[i].m_cgi,j,m_parms[j].m_cgi);
		exit(-1);
	}
	}

	int32_t mm = (int32_t)sizeof(CollectionRec);
	if ( (int32_t)sizeof(Conf)        > mm ) mm = (int32_t)sizeof(Conf);
	if ( (int32_t)sizeof(SearchInput) > mm ) mm = (int32_t)sizeof(SearchInput);
	// . set size of each parm based on its type
	// . also do page and obj inheritance
	// . also do sanity checking
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		// sanity check
		if ( m_parms[i].m_off   > mm ||
		     //m_parms[i].m_soff  > mm ||
		     m_parms[i].m_smaxc > mm   ) {
			log(LOG_LOGIC,"conf: Bad offset in parm #%"INT32" %s."
			    " (%"INT32",%"INT32",%"INT32"). Did you FORGET to include "
			    "an & before the cr.myVariable when setting "
			    "m_off for this parm? Or subtract  'x' instead "
			    "of 'g' or vice versa.",
			    i,m_parms[i].m_title,
			    mm,
			    m_parms[i].m_off,
			    //m_parms[i].m_soff,
			    m_parms[i].m_smaxc);
			exit(-1);
		}
		// do not allow numbers in cgi parms, they are used for
		// denoting array indices
		int32_t j = 0;
		for ( ; m_parms[i].m_cgi && m_parms[i].m_cgi[j] ; j++ ) {
			if ( is_digit ( m_parms[i].m_cgi[j] ) ) {
				log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has "
				    "number in cgi name.", 
				    i,m_parms[i].m_title);
				exit(-1);
			}
		}
		// these inheriting cause too many problems when moving
		// parms around in the array
		// inherit page
		//if ( i > 0 && m_parms[i].m_page == -1 ) 
		//	m_parms[i].m_page = m_parms[i-1].m_page;
		// inherit obj
		//if ( i > 0 && m_parms[i].m_obj == -1 ) 
		//	m_parms[i].m_obj = m_parms[i-1].m_obj;
		// sanity now
		if ( m_parms[i].m_page == -1 ) { 
			log("parms: bad page \"%s\"",m_parms[i].m_title);
			char *xx=NULL;*xx=0; }
		if ( m_parms[i].m_obj == -1 ) { 
			log("parms: bad obj \"%s\"",m_parms[i].m_title);
			char *xx=NULL;*xx=0; }

		// if its a fixed size then make sure m_size is not set
		if ( m_parms[i].m_fixed > 0 ) {
			if ( m_parms[i].m_size != 0 ) {
				log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" is "
				    "fixed but size is not 0.", 
				    i,m_parms[i].m_title);
				exit(-1);
			}
		}
		// skip if already set
		if ( m_parms[i].m_size ) goto skipSize;
		// string sizes should already be set!
		size = 0;
		t = m_parms[i].m_type;
		if ( t == -1 ) {
			log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has no type.",
			    i,m_parms[i].m_title);
			exit(-1);
		}
		if ( t == TYPE_CHAR           ) size = 1;
		if ( t == TYPE_CHAR2          ) size = 1;
		if ( t == TYPE_BOOL           ) size = 1;
		if ( t == TYPE_BOOL2          ) size = 1;
		if ( t == TYPE_CHECKBOX       ) size = 1;
		if ( t == TYPE_PRIORITY       ) size = 1;
		if ( t == TYPE_PRIORITY2      ) size = 1;
		//if ( t ==TYPE_DIFFBOT_DROPDOWN) size = 1;
		if ( t == TYPE_UFP            ) size = 1;
		if ( t == TYPE_PRIORITY_BOXES ) size = 1;
		if ( t == TYPE_RETRIES        ) size = 1;
		if ( t == TYPE_TIME           ) size = 6;
		if ( t == TYPE_DATE2          ) size = 4;
		if ( t == TYPE_DATE           ) size = 4;
		if ( t == TYPE_FLOAT          ) size = 4;
		if ( t == TYPE_DOUBLE         ) size = 8;
		if ( t == TYPE_IP             ) size = 4;
		if ( t == TYPE_RULESET        ) size = 4;
		if ( t == TYPE_LONG           ) size = 4;
		if ( t == TYPE_LONG_CONST     ) size = 4;
		if ( t == TYPE_LONG_LONG      ) size = 8;
		if ( t == TYPE_STRING         ) size = m_parms[i].m_size;
		if ( t == TYPE_STRINGBOX      ) size = m_parms[i].m_size;
		if ( t == TYPE_STRINGNONEMPTY ) size = m_parms[i].m_size;
		if ( t == TYPE_SITERULE       ) size = 4;

		// comments and commands do not control underlying variables
		if ( size == 0 && t != TYPE_COMMENT && t != TYPE_CMD &&
		     t != TYPE_SAFEBUF  &&
		     t != TYPE_FILEUPLOADBUTTON &&
		     t != TYPE_CONSTANT &&
		     t != TYPE_CHARPTR &&
		     t != TYPE_MONOD2   &&
		     t != TYPE_MONOM2     ) {
			log(LOG_LOGIC,"conf: Size of parm #%"INT32" \"%s\" "
			    "not set.", i,m_parms[i].m_title);
			exit(-1);
		}
		m_parms[i].m_size = size;
	skipSize:
		// check offset
		if ( m_parms[i].m_obj == OBJ_NONE ) continue;
		if ( t == TYPE_COMMENT  ) continue;
		if ( t == TYPE_FILEUPLOADBUTTON ) continue;
		if ( t == TYPE_CMD      ) continue;
		if ( t == TYPE_CONSTANT ) continue;
		if ( t == TYPE_MONOD2   ) continue;
		if ( t == TYPE_MONOM2   ) continue;
		if ( t == TYPE_SAFEBUF  ) continue;
		// search parms do not need an offset
		if ( m_parms[i].m_off == -1 ){//&& m_parms[i].m_sparm == 0 ) {
			log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has no offset.",
			    i,m_parms[i].m_title);
			exit(-1);
		}
		if ( m_parms[i].m_off < -1 ) {
			log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has bad offset "
			    "of %"INT32".", i,m_parms[i].m_title,m_parms[i].m_off);
			exit(-1);
		}
		if ( m->m_obj == OBJ_CONF && m->m_off >= (int32_t)sizeof(Conf) ) {
			log("admin: Parm %s has bad m_off value.",m->m_title);
			char *xx = NULL; *xx = 0;
		}
		if (m->m_obj==OBJ_COLL&&m->m_off>=(int32_t)sizeof(CollectionRec)){
			log("admin: Parm %s has bad m_off value.",m->m_title);
			char *xx = NULL; *xx = 0;
		}
		if ( m->m_off >= 0 && 
		     m->m_obj == OBJ_SI && 
		     m->m_off >= (int32_t)sizeof(SearchInput)){
			log("admin: Parm %s has bad m_off value.",m->m_title);
			char *xx = NULL; *xx = 0;
		}

		if ( m_parms[i].m_page == -1 ) {
			log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has no page.",
			    i,m_parms[i].m_title);
			exit(-1);
		}
		if ( m_parms[i].m_obj == -1 ) {
			log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has no object.",
			    i,m_parms[i].m_title);
			exit(-1);
		}
		//if ( ! m_parms[i].m_title[0] ) {
		//	log(LOG_LOGIC,"conf: Parm #%"INT32" \"%s\" has no title.",
		//	    i,m_parms[i].m_cgi);
		//	exit(-1);
		//}
		// continue if already have the xml name
		if ( m_parms[i].m_xml ) continue;
		// set xml based on title
		char *tt = m_parms[i].m_title;
		if ( p + gbstrlen(tt) >= pend ) {
			log(LOG_LOGIC,"conf: Not enough room to store xml "
			    "tag name in buffer.");
			exit(-1);
		}
		m_parms[i].m_xml = p;
		for ( int32_t k = 0 ; tt[k] ; k++ ) {
			if ( ! is_alnum_a(tt[k]) ) continue;
			if ( k > 0 && tt[k-1]==' ') *p++ = to_upper_a(tt[k]);
			else                        *p++ = tt[k];
		}
		*p++ = '\0';
	}

	// set m_searchParms
	int32_t n = 0;
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		//if ( ! m_parms[i].m_sparm ) continue;
		if ( m_parms[i].m_obj != OBJ_SI ) continue;
		m_searchParms[n++] = &m_parms[i];
		// sanity check
		if ( m_parms[i].m_off == -1 ) {
			log(LOG_LOGIC,"conf: SEARCH Parm #%"INT32" \"%s\" has "
			    "m_off < 0 (offset into SearchInput).",
			    i,m_parms[i].m_title);
			exit(-1);
		}
	}
	m_numSearchParms = n;

	// . sanity check
	// . we should have it all covered!
	si.test();

	//
	// parm overlap detector
	//
	// . fill in each parm's buffer with byte #b
	// . inc b for each parm
	overlapTest(+1);
	overlapTest(-1);
}

void Parms::overlapTest ( char step ) {

	int32_t start = 0;
	if ( step == -1 ) start = m_numParms - 1;

	//log("conf: Using step=%"INT32"",(int32_t)step);

	SearchInput   tmpsi;
	GigablastRequest tmpgr;
	InjectionRequest tmpir;
	CollectionRec tmpcr;
	Conf          tmpconf;
	char          b;
	char *p1 , *p2;
	int32_t i;
	// sanity check: ensure parms do not overlap
	for ( i = start ; i < m_numParms && i >= 0 ; i += step ) {

		// skip comments
		if ( m_parms[i].m_type == TYPE_COMMENT ) continue;
		if ( m_parms[i].m_type == TYPE_FILEUPLOADBUTTON ) continue;
		// skip if it is a broadcast switch, like "all spiders on"
		// because that modifies another parm, "spidering enabled"
		if ( m_parms[i].m_type == TYPE_BOOL2 ) continue;

		if ( m_parms[i].m_type == TYPE_SAFEBUF ) continue;

		// we use cr->m_spideringEnabled for PAGE_BASIC_SETTINGS too!
		if ( m_parms[i].m_flags & PF_DUP ) continue;

		p1 = NULL;
		if ( m_parms[i].m_obj == OBJ_COLL ) p1 = (char *)&tmpcr;
		if ( m_parms[i].m_obj == OBJ_CONF ) p1 = (char *)&tmpconf;
		if ( m_parms[i].m_obj == OBJ_SI   ) p1 = (char *)&tmpsi;
		if ( m_parms[i].m_obj == OBJ_GBREQUEST   ) p1 = (char *)&tmpgr;
		if ( m_parms[i].m_obj == OBJ_IR   ) p1 = (char *)&tmpir;
		if ( p1 ) p1 += m_parms[i].m_off;
		p2 = NULL;
		int32_t size = m_parms[i].m_size;
		// use i now
		b = (char)i;
		// string box type is a pointer!!
		if ( p1 ) memset ( p1 , b , size );
		//log("conf: setting %"INT32" bytes for %s at 0x%"XINT32" char=0x%hhx",
		//    size,m_parms[i].m_title,(int32_t)p1,b);
		// search input uses character ptrs!!
		if ( m_parms[i].m_type == TYPE_STRINGBOX ) size = 4;
		if ( m_parms[i].m_type == TYPE_STRING    ) size = 4;
		if ( m_parms[i].m_fixed > 0 ) size *= m_parms[i].m_fixed ;
		if ( p2 ) memset ( p2 , b , size );
		//log("conf: setting %"INT32" bytes for %s at 0x%"XINT32" char=0x%hhx "
		//    "i=%"INT32"",   size,m_parms[i].m_title,(int32_t)p2,b,i);
	}

	//
	// now make sure they are the same
	//
	if ( step == -1 ) b--;
	else              b = 0;
	char *objStr = "none";
	int32_t  obj;
	char  infringerB;
	int32_t  j;
	int32_t savedi = -1;

	for ( i = 0 ; i < m_numParms ; i++ ) {

		// skip comments
		if ( m_parms[i].m_type == TYPE_COMMENT ) continue;
		if ( m_parms[i].m_type == TYPE_FILEUPLOADBUTTON ) continue;
		// skip if it is a broadcast switch, like "all spiders on"
		// because that modifies another parm, "spidering enabled"
		if ( m_parms[i].m_type == TYPE_BOOL2 ) continue;

		if ( m_parms[i].m_type == TYPE_SAFEBUF ) continue;

		// we use cr->m_spideringEnabled for PAGE_BASIC_SETTINGS too!
		if ( m_parms[i].m_flags & PF_DUP ) continue;

		p1 = NULL;
		if ( m_parms[i].m_obj == OBJ_COLL ) p1 = (char *)&tmpcr;
		if ( m_parms[i].m_obj == OBJ_CONF ) p1 = (char *)&tmpconf;
		if ( m_parms[i].m_obj == OBJ_SI   ) p1 = (char *)&tmpsi;
		if ( m_parms[i].m_obj == OBJ_GBREQUEST ) p1 = (char *)&tmpgr;
		if ( m_parms[i].m_obj == OBJ_IR   ) p1 = (char *)&tmpir;
		if ( p1 ) p1 += m_parms[i].m_off;
		p2 = NULL;
		int32_t size = m_parms[i].m_size;
		b = (char) i;
		// save it
		obj = m_parms[i].m_obj;

		//log("conf: testing %"INT32" bytes for %s at 0x%"XINT32" char=0x%hhx "
		//    "i=%"INT32"", size,m_parms[i].m_title,(int32_t)p1,b,i);

		for ( j = 0 ; p1 && j < size ; j++ ) {
			if ( p1[j] == b ) continue;
			// this has multiple parms pointing to it!
			//if ( m_parms[i].m_type == TYPE_BOOL2 ) continue;
			// or special cases...
			//if ( p1 == (char *)&tmpconf.m_spideringEnabled ) 
			//	continue;
			// set object type
			objStr = "??????";
			if ( m_parms[i].m_obj == OBJ_COLL )
				objStr = "CollectionRec.h";
			if ( m_parms[i].m_obj == OBJ_CONF )
				objStr = "Conf.h";
			if ( m_parms[i].m_obj == OBJ_SI )
				objStr = "SearchInput.h";
			if ( m_parms[i].m_obj == OBJ_GBREQUEST )
				objStr = "GigablastRequest/Parms.h";
			if ( m_parms[i].m_obj == OBJ_IR )
				objStr = "InjectionRequest/PageInject.h";
			// save it
			infringerB = p1[j];
			savedi = i;
			goto error;
		}
		// search input uses character ptrs!!
		if ( m_parms[i].m_type == TYPE_STRINGBOX ) size = 4;
		if ( m_parms[i].m_type == TYPE_STRING    ) size = 4;
		if ( m_parms[i].m_fixed > 0 ) size *= m_parms[i].m_fixed ;
		objStr = "SearchInput.h";

		//log("conf: testing %"INT32" bytes for %s at 0x%"XINT32" char=0x%hhx "
		//    "i=%"INT32"",  size,m_parms[i].m_title,(int32_t)p2,b,i);

		for ( j = 0 ; p2 && j < size ; j++ ) {
			if ( p2[j] == b ) continue;
			// save it
			infringerB = p2[j];
			savedi = i;
			log("conf: got b=0x%hhx when it should have been "
			    "b=0x%hhx",p2[j],b);
			goto error;
		}
	}

	return;

 error:
	log("conf: Had a parm value collision. Parm #%"INT32" "
	    "\"%s\" (size=%"INT32") in %s has overlapped with another parm. "
	    "Your TYPE_* for this parm or a neighbor of it "
	    "does not agree with what you have declared it as in the *.h "
	    "file.",i,m_parms[i].m_title,m_parms[i].m_size,objStr);
	if ( step == -1 ) b--;
	else              b = 0;
	// show possible parms that could have overwritten it!
	for ( i = start ; i < m_numParms && i >= 0 ; i += step ) {
		//char *p1 = NULL;
		//if ( m_parms[i].m_obj == OBJ_COLL ) p1 = (char *)&tmpcr;
		//if ( m_parms[i].m_obj == OBJ_CONF ) p1 = (char *)&tmpconf;
		// skip if comment
		if ( m_parms[i].m_type == TYPE_COMMENT ) continue;
		if ( m_parms[i].m_type == TYPE_FILEUPLOADBUTTON ) continue;
		if ( m_parms[i].m_flags & PF_DUP ) continue;
		if ( m_parms[i].m_obj != m_parms[savedi].m_obj ) continue;
		// skip if no match
		//bool match = false;
		//if ( m_parms[i].m_obj == obj ) match = true;
		//if ( m_parms[i].m_sparm && 
		// NOTE: these need to be fixed!!!
		b = (char) i;
		if ( b == infringerB )
			log("conf: possible overlap with parm #%"INT32" in %s "
			    "\"%s\" (size=%"INT32") "
			    "xml=%s "
			    "desc=\"%s\"",
			    i,objStr,m_parms[i].m_title,
			    m_parms[i].m_size,
			    m_parms[i].m_xml,
			    m_parms[i].m_desc);
	}

	log("conf: try including \"m->m_obj = OBJ_COLL;\" or "
	    "\"m->m_obj   = OBJ_CONF;\" in your parm definitions");
	log("conf: failed overlap test. exiting.");
	exit(-1);

}


bool Parm::getValueAsBool ( SearchInput *si ) {
	if ( m_obj != OBJ_SI ) { char *xx=NULL;*xx=0; }
	char *p = (char *)si + m_off;
	return *(bool *)p;
}

int32_t Parm::getValueAsLong ( SearchInput *si ) {
	if ( m_obj != OBJ_SI ) { char *xx=NULL;*xx=0; }
	char *p = (char *)si + m_off;
	return *(int32_t *)p;
}

char *Parm::getValueAsString ( SearchInput *si ) {
	if ( m_obj != OBJ_SI ) { char *xx=NULL;*xx=0; }
	char *p = (char *)si + m_off;
	return *(char **)p;
}

/////////
//
// new functions
//
/////////

bool Parms::addNewParmToList1 ( SafeBuf *parmList ,
				collnum_t collnum ,
				char *parmValString ,
				int32_t  occNum ,
				char *parmName ) {
	// get the parm descriptor
	Parm *m = getParmFast1 ( parmName , NULL );
	if ( ! m ) return log("parms: got bogus parm2 %s",parmName );
	return addNewParmToList2 ( parmList,collnum,parmValString,occNum,m );
}

// . make a parm rec using the prodivded string
// . used to convert http requests into a parmlist
// . string could be a float or int32_t or int64_t in ascii, as well as a string
// . returns false w/ g_errno set on error
bool Parms::addNewParmToList2 ( SafeBuf *parmList ,
				collnum_t collnum , 
				char *parmValString ,
				int32_t occNum ,
				Parm *m ) {
	// get value
	char *val = NULL;
	int32_t valSize = 0;

	//char buf[2+MAX_COLL_LEN];

	int32_t val32;
	int64_t val64;
	char val8;
	float valf;

	/*
	char *obj = NULL;
	// we might be adding a collnum if a collection that is being
	// added via the CommandAddColl0() "addColl" or "addCrawl" or
	// "addBulk" commands. they will reserve the collnum, so it might
	// not be ready yet.
	if ( collnum != -1 ) {
		CollectionRec *cr = g_collectiondb.getRec ( collnum );
		if ( cr ) obj = (char *)cr;
		//	log("parms: no coll rec for %"INT32"",(int32_t)collnum);
		//	return false;
		//}
		//obj = (char *)cr;
	}
	else {
		obj = (char *)&g_conf;
	}
	*/


	if ( m->m_type == TYPE_STRING || 
	     m->m_type == TYPE_STRINGBOX || 
	     m->m_type == TYPE_SAFEBUF ||
	     m->m_type == TYPE_STRINGNONEMPTY ) {
		// point to string
		//val = obj + m->m_off;
		// Parm::m_size is the max string size
		//if ( occNum > 0 ) val += occNum * m->m_size;
		// stringlength + 1. no just make it the whole string in
		// case it does not use the \0 protocol
		//valSize = m->m_max;
		val = parmValString;
		// include \0
		valSize = gbstrlen(val)+1;
		// sanity
		if ( val[valSize-1] != '\0' ) { char *xx=NULL;*xx=0; }
	}
	else if ( m->m_type == TYPE_LONG ) {
		// watch out for unsigned 32-bit numbers, so use atoLL()
		val64 = atoll(parmValString);
		val = (char *)&val64;
		valSize = 4;
	}
	else if ( m->m_type == TYPE_FLOAT ) {
		valf = atof(parmValString);
		val = (char *)&valf;
		valSize = 4;
	}
	else if ( m->m_type == TYPE_LONG_LONG ) {
		val64 = atoll(parmValString);
		val = (char *)&val64;
		valSize = 8;
	}
	else if ( m->m_type == TYPE_BOOL ||
		  m->m_type == TYPE_BOOL2 ||
		  m->m_type == TYPE_CHECKBOX ||
		  m->m_type == TYPE_PRIORITY2 ||
		  m->m_type == TYPE_UFP ||
		  m->m_type == TYPE_CHAR ) {
		val8 = atol(parmValString);
		//if ( parmValString && to_lower_a(parmValString[0]) == 'y' )
		//	val8 = 1;
		//if ( parmValString && to_lower_a(parmValString[0]) == 'n' )
		//	val8 = 0;
		val = (char *)&val8;
		valSize = 1;
	}
	// for resetting or restarting a coll i think the ascii arg is
	// the NEW reserved collnum, but for other commands then parmValString
	// will be NULL
	else if ( m->m_type == TYPE_CMD ) {
		val = parmValString;
		if ( val ) valSize = gbstrlen(val)+1;
		// . addcoll collection can not be too long
		// . TODO: supply a Parm::m_checkValFunc to ensure val is
		//   legitimate, and set g_errno on error
		if ( strcmp(m->m_cgi,"addcoll") == 0 &&valSize-1>MAX_COLL_LEN){
			log("admin: addcoll coll too long");
			g_errno = ECOLLTOOBIG;
			return false;
		}
		// scan for holes if we hit the limit
		//if ( g_collectiondb.m_numRecs >= 1LL>>sizeof(collnum_t) )
	}
	else if ( m->m_type == TYPE_IP ) {
		// point to string
		//val = obj + m->m_off;
		// Parm::m_size is the max string size
		//if ( occNum > 0 ) val += occNum * m->m_size;
		// stringlength + 1. no just make it the whole string in
		// case it does not use the \0 protocol
		val32 = atoip(parmValString);
		// store ip in binary format
		val = (char *)&val32;
		valSize = 4;
	}
	else {
		log("parms: shit unsupported parm type");
		char *xx=NULL;*xx=0;
	}

	key96_t key = makeParmKey ( collnum , m ,  occNum );

	// then key
	if ( ! parmList->safeMemcpy ( &key , sizeof(key) ) )
		return false;

	// datasize
	if ( ! parmList->pushLong ( valSize ) )
		return false;

	// and data
	if ( val && valSize && ! parmList->safeMemcpy ( val , valSize ) )
		return false;

	return true;
}

// . use the current value of the parm to make this record
// . parm class itself already helps us reference the binary parm value
bool Parms::addCurrentParmToList2 ( SafeBuf *parmList ,
				    collnum_t collnum , 
				    int32_t occNum ,
				    Parm *m ) {

	char *obj = NULL;

	if ( collnum != -1 ) {
		CollectionRec *cr = g_collectiondb.getRec ( collnum );
		if ( ! cr ) return false;
		obj = (char *)cr;
	}
	else {
		obj = (char *)&g_conf;
	}

	char *data = obj + m->m_off;
	// Parm::m_size is the max string size
	int32_t dataSize = m->m_size;
	if ( occNum > 0 ) data += occNum * m->m_size;

	if ( m->m_type == TYPE_STRING || 
	     m->m_type == TYPE_STRINGBOX || 
	     m->m_type == TYPE_SAFEBUF ||
	     m->m_type == TYPE_STRINGNONEMPTY )
		// include \0 in string
		dataSize = gbstrlen(data) + 1;

	// if a safebuf, point to the string within
	if ( m->m_type == TYPE_SAFEBUF ) {
		SafeBuf *sb = (SafeBuf *)data;
		data = sb->getBufStart();
		dataSize = sb->length();
		// sanity
		if ( dataSize > 0 && !data[dataSize-1]){char *xx=NULL;*xx=0;}
		// include the \0 since we do it for strings above
		if ( dataSize > 0 ) dataSize++;
		// empty? make it \0 then to be like strings i guess
		if ( dataSize == 0 ) {
			data = "\0";
			dataSize = 1;
		}
		// sanity check
		if ( dataSize > 0 && data[dataSize-1] ) {char *xx=NULL;*xx=0;}
		// if just a \0 then make it empty
		//if ( dataSize && !data[0] ) {
		//	data = NULL;
		//	dataSize = 0;
		//}
	}

	//int32_t occNum = -1;
	key96_t key = makeParmKey ( collnum , m ,  occNum );
	/*
	// debug it
	log("parms: adding parm collnum=%i title=%s "
	    "key=%s datasize=%i data=%s hash=%"UINT32
	    ,(int)collnum
	    ,m->m_title
	    ,KEYSTR(&key,sizeof(key))
	    ,(int)dataSize
	    ,data
	    ,(uint32_t)hash32(data,dataSize));
	*/
	// then key
	if ( ! parmList->safeMemcpy ( &key , sizeof(key) ) )
		return false;

	// size
	if ( ! parmList->pushLong ( dataSize ) )
		return false;

	// and data
	if ( dataSize && ! parmList->safeMemcpy ( data , dataSize ) )
		return false;

	return true;
}

// returns false and sets g_errno on error
bool Parms::convertHttpRequestToParmList (HttpRequest *hr, SafeBuf *parmList,
					  int32_t page , TcpSocket *sock ) {

	// false = useDefaultRec?
	CollectionRec *cr = g_collectiondb.getRec ( hr , false );

	//if ( c ) {
	//	cr = g_collectiondb.getRec ( hr );
	//	if ( ! cr ) log("parms: coll not found");
	//}

	bool isMasterAdmin = g_conf.isMasterAdmin ( sock , hr );
	
	// does this user have permission to update the parms?
	bool isCollAdmin = g_conf.isCollAdmin ( sock , hr ) ;


	// might be g_conf specific, not coll specific
	//bool hasPerm = false;
	// just knowing the collection name of a custom crawl means you
	// know the token, so you have permission
	//if ( cr && cr->m_isCustomCrawl ) hasPerm = true;
	//if ( hr->isLocal() ) hasPerm = true;

	// fix jenkins "GET /v2/crawl?token=crawlbottesting" request
	char *name  = hr->getString("name");
	char *token = hr->getString("token");
	//if ( ! cr && token ) hasPerm = true;

	//if ( ! hasPerm ) {
	//	//log("parms: no permission to set parms");
	//	//g_errno = ENOPERM;
	//	//return false;
	//	// just leave the parm list empty and fail silently
	//	return true;
	//}

	// we set the parms in this collnum
	collnum_t parmCollnum = -1;
	if ( cr ) parmCollnum = cr->m_collnum;

	// turn the collnum into an ascii string for providing as args
	// when &reset=1 &restart=1 &delete=1 is given along with a
	// &c= or a &name=/&token= pair.
	char oldCollName[MAX_COLL_LEN+1];
	oldCollName[0] = '\0';
	if ( cr ) sprintf(oldCollName,"%"INT32"",(int32_t)cr->m_collnum);


	////////
	//
	// HACK: if crawlbot user supplies a token, name, and seeds, and the
	// corresponding collection does not exist then assume it is an add
	//
	////////
	char customCrawl = 0;
	char *path = hr->getPath();
	// i think /crawlbot is only used by me to see PageCrawlBot.cpp
	// so don't bother...
	if ( strncmp(path,"/crawlbot",9) == 0 ) customCrawl = 0;
	if ( strncmp(path,"/v2/crawl",9) == 0 ) customCrawl = 1;
	if ( strncmp(path,"/v2/bulk" ,8) == 0 ) customCrawl = 2;
	if ( strncmp(path,"/v3/crawl",9) == 0 ) customCrawl = 1;
	if ( strncmp(path,"/v3/bulk" ,8) == 0 ) customCrawl = 2;

        // throw error if collection record custom crawl type doesn't equal 
	// the crawl type of current request
	if (cr && customCrawl && customCrawl != cr->m_isCustomCrawl ) {
		g_errno = ECUSTOMCRAWLMISMATCH;
		return false;
        }

	bool hasAddCrawl = hr->hasField("addCrawl");
	bool hasAddBulk  = hr->hasField("addBulk");
	bool hasAddColl  = hr->hasField("addColl");
	// sometimes they try to delete a collection that is not there so do
	// not apply this logic in that case!
	bool hasDelete   = hr->hasField("delete");
	bool hasRestart  = hr->hasField("restart");
	bool hasReset    = hr->hasField("reset");
	bool hasSeeds    = hr->hasField("seeds");
	// check for bulk jobs as well
	if ( ! hasSeeds ) hasSeeds = hr->hasField("urls");
	if ( ! cr          && 
	     token         && 
	     name          && 
	     customCrawl   &&
	     hasSeeds      &&
	     ! hasDelete   &&
	     ! hasRestart  &&
	     ! hasReset    &&
	     ! hasAddCrawl && 
	     ! hasAddBulk  && 
	     ! hasAddColl     ) {
		// reserve a new collnum for adding this crawl
		parmCollnum = g_collectiondb.reserveCollNum();
		// must be there!
		if ( parmCollnum == -1 ) {
			g_errno = EBADENGINEER;
			return false;
		}
		// log it for now
		log("parms: trying to add custom crawl (%"INT32")",
		    (int32_t)parmCollnum);
		// formulate name
		char newName[MAX_COLL_LEN+1];
		snprintf(newName,MAX_COLL_LEN,"%s-%s",token,name);
		char *cmdStr = "addCrawl";
		if ( customCrawl == 2 ) cmdStr = "addBulk";
		// add to parm list
		if ( ! addNewParmToList1 ( parmList ,
					   parmCollnum ,
					   newName ,
					   -1 , // occNum
					   cmdStr ) )
			return false;
	}

	// loop through cgi parms
	for ( int32_t i = 0 ; i < hr->getNumFields() ; i++ ) {
		// get cgi parm name
		char *field = hr->getField    ( i );
		// get value of the cgi field
		char *val  = hr->getValue   (i);
		// convert field to parm
		int32_t occNum;
		// parm names can be shared across pages, like "c"
		// for search, addurl, inject, etc.
		Parm *m = getParmFast1 ( field , &occNum );
		if ( ! m ) continue;

		// skip if not a command parm, like "addcoll"
		if ( m->m_type != TYPE_CMD ) continue;

		if ( m->m_obj != OBJ_CONF && m->m_obj != OBJ_COLL )
			continue;

		//
		// HACK
		//
		// if its a resetcoll/restartcoll/addcoll we have to
		// get the next available collnum and use that for setting
		// any additional parms. that is the coll it will act on.
		if ( strcmp(m->m_cgi,"addColl") == 0 ||
		     // lowercase support. camelcase is obsolete.
		     strcmp(m->m_cgi,"addcoll") == 0 ||
		     strcmp(m->m_cgi,"addCrawl") == 0 ||
		     strcmp(m->m_cgi,"addBulk" ) == 0 ||
		     strcmp(m->m_cgi,"reset" ) == 0 ||
		     strcmp(m->m_cgi,"restart" ) == 0 ) {
			// if we wanted to we could make the data the
			// new parmCollnum since we already store the old
			// collnum in the parm rec key
			parmCollnum = g_collectiondb.reserveCollNum();
			//
			//
			// NOTE: the old collnum is in the "val" already
			// like "&reset=462" or "&addColl=test"
			//
			//
			// sanity. if all are full! we hit our limit of
			// 32k collections. should increase collnum_t from
			// int16_t to int32_t...
			if ( parmCollnum == -1 ) {
				g_errno = EBADENGINEER;
				return false;
			}
		}

		// . DIFFBOT HACK: so ppl can manually restart a spider round
		// . val can be 0 or 1 or anything. i.e. roundStart=0 works.
		// . map this parm to another parm with the round start
		//   time (current time) and the new round # as the args.
		// . this will call CommandForceNextSpiderRound() function
		//   on every shard with these args, "tmpVal".
		if ( cr && strcmp(m->m_cgi,"roundStart") == 0 ) {
			// use the current time so anything spidered before
			// this time (the round start time) will be respidered
			//sprintf(tmp,"%"UINT32"",getTimeGlobalNoCore());
			//val = tmp;
			char tmpVal[64];
			// use the same round start time for all shards
			sprintf(tmpVal,
				"%"UINT32",%"INT32""
				,(uint32_t)getTimeGlobalNoCore()
				,cr->m_spiderRoundNum+1
				);
			// . also add command to reset crawl/process counts
			//   so if you hit maxToProcess/maxToCrawl it will
			//   not stop the round from restarting
			// . CommandResetCrawlCounts()
			if ( ! addNewParmToList1 ( parmList ,
						   parmCollnum ,
						   tmpVal, // a string
						   0 , // occNum (for arrays)
						   "forceround" ) )
				return false;
			// don't bother going below
			continue;
		}

		// if a collection name was also provided, assume that is
		// the target of the reset/delete/restart. we still
		// need PageAddDelete.cpp to work...
		if ( cr &&
		     ( strcmp(m->m_cgi,"reset" ) == 0 ||
		       strcmp(m->m_cgi,"delete" ) == 0 ||
		       strcmp(m->m_cgi,"restart" ) == 0 ) ) 
			// the collnum to reset/restart/del
			// given as a string.
			val = oldCollName;

		//
		// CLOUD SEARCH ENGINE SUPPORT
		//

		//
		// if this is the "delcoll" parm then "c" may have been
		// excluded from http request, therefore isCollAdmin and
		// isMasterAdmin may be false, so see if they have permission
		// for the "val" collection for this one...
		bool hasPerm = false;
#ifndef PRIVACORE_SAFE_VERSION
		if ( m->m_page == PAGE_DELCOLL &&
		     strcmp(m->m_cgi,"delcoll") == 0 ) {
			// permission override for /admin/delcoll cmd & parm
			hasPerm = g_conf.isCollAdminForColl (sock,hr,val);
		}
#endif

#ifndef PRIVACORE_SAFE_VERSION
		// if this IP c-block as already added a collection then do not
		// allow it to add another.
		if ( m->m_page == PAGE_ADDCOLL &&
		     g_conf.m_allowCloudUsers &&
		     ! isMasterAdmin &&
		     strcmp(m->m_cgi,"addcoll")==0 ) {
			// see if user's c block has already added a collection
			int32_t numAdded = 0;
			if ( numAdded >= 1 ) {
				g_errno = ENOPERM;
				log("parms: already added a collection from "
				    "this cloud user's c-block.");
				return false;
			}
			hasPerm = true;
		}
#endif

		// master controls require root permission
		if ( m->m_obj == OBJ_CONF && ! isMasterAdmin ) {
			log("parms: could not run root parm \"%s\" no perm.",
			    m->m_title);
			continue;
		}

		// need to have permission for collection for collrec parms
		if ( m->m_obj == OBJ_COLL && ! isCollAdmin && ! hasPerm ) {
			log("parms: could not run coll parm \"%s\" no perm.",
			    m->m_title);
			continue;
		}

		// add the cmd parm
		if ( ! addNewParmToList2 ( parmList ,
					   // it might be a collection-less
					   // command like 'gb stop' which
					   // uses the "save=1" parm.
					   // this is the "new" collnum to
					   // create in the case of
					   // add/reset/restart, but in the
					   // case of delete it is -1 or old.
					   parmCollnum ,
					   // the argument to the function...
					   // in the case of delete, the 
					   // collnum to delete in ascii.
					   // in the case of add, the name
					   // of the new coll. in the case
					   // of reset/restart the OLD 
					   // collnum is ascii to delete.
					   val,
					   occNum ,
					   m ) )
			return false;
	}

	// if we are one page url filters, turn off all checkboxes!
	// html should really transmit them as =0 if they are unchecked!!
	// "fe" is a url filter expression for the first row.
	//if ( hr->hasField("fe") && page == PAGE_FILTERS && cr ) {
	//	for ( int32_t i = 0 ; i < cr->m_numRegExs ; i++ ) {
	//		//cr->m_harvestLinks  [i] = 0;
	//		//cr->m_spidersEnabled[i] = 0;
	//		if ( ! addNewParmToList2 ( parmList ,
	//					   cr->m_collnum,
	//					   "0",
	//					   i,
	//	}
	//}


	//
	// CLOUD SEARCH ENGINE SUPPORT
	//
	// provide userip so when adding a new collection we can
	// store it in the collection rec to ensure that the same
	// IP address cannot add more than one collection.
	//
#ifndef PRIVACORE_SAFE_VERSION
	if ( sock && page == PAGE_ADDCOLL ) {
		char *ipStr = iptoa(sock->m_ip);
		int32_t occNum;
		Parm *um = getParmFast1 ( "userip" , &occNum); // NULL = occNum
		if ( ! addNewParmToList2 ( parmList ,
					   // HACK! operate on the to-be-added
					   // collrec, if there was an addcoll
					   // reset or restart coll cmd...
					   parmCollnum ,
					   ipStr, // val ,
					   occNum ,
					   um ) )
			return false;
	}
#endif


	//
	// now add the parms that are NOT commands
	//
	
	// loop through cgi parms
	for ( int32_t i = 0 ; i < hr->getNumFields() ; i++ ) {
		// get cgi parm name
		char *field = hr->getField    ( i );
		// get value of the cgi field
		char *val  = hr->getValue   (i);

		// get the occurence # if its regex. this is the row #
		// in the url filters table, since those parms repeat names.
		// url filter expression.
		//if ( strcmp(field,"fe") == 0 ) occNum++;

		// convert field to parm
		int32_t occNum;
		Parm *m = getParmFast1 ( field , &occNum );

		//
		// map "pause" to spidering enabled
		//
		if ( strcmp(field,"pause"     ) == 0 ||
		     strcmp(field,"pauseCrawl") == 0 ) {
			m = getParmFast1 ( "cse",  &occNum);
			if      ( val && val[0] == '0' ) val = "1";
			else if ( val && val[0] == '1' ) val = "0";
			if ( ! m ) { char *xx=NULL;*xx=0; }
		}

		if ( ! m ) continue;

		// skip if IS a command parm, like "addcoll", we did that above
		if ( m->m_type == TYPE_CMD ) 
			continue;

		if ( m->m_obj != OBJ_CONF && m->m_obj != OBJ_COLL )
			continue;


		//
		// CLOUD SEARCH ENGINE SUPPORT
		//
		// master controls require root permission. otherwise, just
		// knowing the collection name is enough for a cloud user
		// to change settings.
		//
		bool hasPerm = false;

		// master controls require root permission
		if ( m->m_obj == OBJ_CONF && ! isMasterAdmin ) {
			log("parms: could not set root parm \"%s\" no perm.",
			    m->m_title);
			continue;
		}

		// need to have permission for collection for collrec parms
		if ( m->m_obj == OBJ_COLL && ! isCollAdmin && ! hasPerm ) {
			log("parms: could not set coll parm \"%s\" no perm.",
			    m->m_title);
			continue;
		}

		// convert spiderRoundStartTime=0 (roundStart=0 roundStart=1) 
		// to spiderRoundStartTime=<currenttime>+30secs
		// so that will force the next spider round to kick in
		/*
		bool restartRound = false;
		char tmp[24];
		if ( strcmp(field,"roundStart")==0 && 
		     val && (val[0]=='0'||val[0]=='1') && val[1]==0 ) 
			sprintf(tmp,"%"UINT32"",(int32_t)getTimeGlobalNoCore()+0);
			val = tmp;
		}
		*/

		// add it to a list now
		if ( ! addNewParmToList2 ( parmList ,
					   // HACK! operate on the to-be-added
					   // collrec, if there was an addcoll
					   // reset or restart coll cmd...
					   parmCollnum ,
					   val ,
					   occNum ,
					   m ) )
			return false;
	}


	return true;
}

Parm *Parms::getParmFast2 ( int32_t cgiHash32 ) {
	static HashTableX s_pht;
	static char s_phtBuf[26700];
	static bool s_init = false;

	if ( ! s_init ) {
		// init hashtable
		s_pht.set ( 4,sizeof(char *),2048,s_phtBuf,26700,
			    false,0,"phttab" );
		// reduce hash collisions:
		s_pht.m_useKeyMagic = true;
		// wtf?
		if ( m_numParms <= 0 ) init();
		if ( m_numParms <= 0 ) { char *xx=NULL;*xx=0; }
		// fill up hashtable
		for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
			// get it
			Parm *parm = &m_parms[i];
			// skip parms that are not for conf or coll lest
			// it bitch that "c" is duplicated...
			if ( parm->m_obj != OBJ_CONF &&
			     parm->m_obj != OBJ_COLL )
				continue;
			// skip comments
			if ( parm->m_type == TYPE_COMMENT ) continue;
			if ( parm->m_type == TYPE_FILEUPLOADBUTTON ) continue;
			// skip if no cgi
			if ( ! parm->m_cgi ) continue;
			// get its hash of its cgi
			int32_t ph32 = parm->m_cgiHash;
			// sanity!
			if ( s_pht.isInTable ( &ph32 ) ) {
				// get the dup guy
				Parm *dup = *(Parm **)s_pht.getValue(&ph32);
				// same underlying parm?
				// like for "all spiders on" vs.
				// "all spiders off"?
				if ( dup->m_off == parm->m_off )
					continue;
				// otherwise bitch about it and drop core
				log("parms: dup parm h32=%"INT32" "
				    "\"%s\" vs \"%s\"",
				    ph32, dup->m_title,parm->m_title);
				char *xx=NULL;*xx=0;
			}
			// add that to hash table
			s_pht.addKey ( &ph32 , &parm );
		}
		// do not do this again
		s_init = true;
	}

	Parm **pp = (Parm **)s_pht.getValue ( &cgiHash32 );
	if ( ! pp ) return NULL;
	return *pp;
}


Parm *Parms::getParmFast1 ( char *cgi , int32_t *occNum ) {
	// strip off the %"INT32" for things like 'fe3' for example
	// because that is the occurence # for parm arrays.
	int32_t clen = gbstrlen(cgi);

	char *d = NULL;

	if ( clen > 1 ) {
		d = cgi + clen - 1;
		while ( is_digit(*d) ) d--;
		d++;
	}

	int32_t h32;

	// assume not an array
	if ( occNum ) *occNum = -1;

	if ( d && *d ) {
		if ( occNum ) *occNum = atol(d);
		h32 = hash32 ( cgi , d - cgi );
	}
	else 
		h32 = hash32n ( cgi );

	Parm *m = getParmFast2 ( h32 );

	if ( ! m ) return NULL;

	// the first element does not have a number after it
	if ( m->isArray() && occNum && *occNum == -1 )
		*occNum = 0;

	return m;
}

////////////
//
// functions for distributing/syncing parms to/with all hosts
//
////////////

class ParmNode {
public:
	SafeBuf m_parmList;
	int32_t m_numRequests;
	int32_t m_numReplies;
	int32_t m_numGoodReplies;
	int32_t m_numHostsTotal;
	class ParmNode *m_prevNode;
	class ParmNode *m_nextNode;
	int64_t m_parmId;
	bool m_calledCallback;
	int32_t m_startTime;
	void *m_state;
	void (* m_callback)(void *state);
	bool m_sendToGrunts;
	bool m_sendToProxies;
	int32_t m_hostId; // -1 means send parm update to all hosts
	// . if not -1 then [m_hostId,m_hostId2] is a range
	// . used by main.cpp cmd line cmds like 'gb stop 3-5'
	int32_t m_hostId2; 
};

static ParmNode *s_headNode = NULL;
static ParmNode *s_tailNode = NULL;
static int64_t s_parmId = 0LL;

// . will send the parm update request to each host and retry forever, 
//   until dead hosts come back up
// . keeps parm update requests in order received
// . returns true and sets g_errno on error
// . returns false if blocked and will call your callback
bool Parms::broadcastParmList ( SafeBuf *parmList ,
				void    *state ,
				void   (* callback)(void *) ,
				bool sendToGrunts ,
				bool sendToProxies ,
				// this is -1 if sending to all hosts
				int32_t hostId ,
				// this is not -1 if its range [hostId,hostId2]
				int32_t hostId2 ) {

	// empty list?
	if ( parmList->getLength() <= 0 ) return true;

	// only us? no need for this then. we now do this...
	//if ( g_hostdb.m_numHosts <= 1 ) return true;

	// make a new parm transmit node
	ParmNode *pn = (ParmNode *)mmalloc ( sizeof(ParmNode) , "parmnode" );
	if ( ! pn ) return true;
	pn->m_parmList.constructor();

	// update the ticket #. we use this to keep things ordered too.
	// this should never be zero since it starts off at zero.
	s_parmId++;

	// set it
	pn->m_parmList.stealBuf ( parmList );
	pn->m_numRequests    = 0;
	pn->m_numReplies     = 0;
	pn->m_numGoodReplies = 0;
	pn->m_numHostsTotal  = 0;
	pn->m_prevNode       = NULL;
	pn->m_nextNode       = NULL;
	pn->m_parmId         = s_parmId; // take a ticket
	pn->m_calledCallback = false;
	pn->m_startTime      = getTimeLocal();
	pn->m_state          = state;
	pn->m_callback       = callback;
	pn->m_sendToGrunts   = sendToGrunts;
	pn->m_sendToProxies  = sendToProxies;
	pn->m_hostId         = hostId;
	pn->m_hostId2        = hostId2; // a range? then not -1 here.

	// store it ordered in our linked list of parm transmit nodes
	if ( ! s_tailNode ) {
		s_headNode = pn;
		s_tailNode = pn;
	}
	else {
		// link pn at end of tail
		s_tailNode->m_nextNode = pn;
		pn->m_prevNode = s_tailNode;
		// pn becomes the new tail
		s_tailNode = pn;
	}

	// just the regular proxies, not compression proxies
	if ( pn->m_sendToProxies ) 
		pn->m_numHostsTotal += g_hostdb.getNumProxies();

	if ( pn->m_sendToGrunts )
		pn->m_numHostsTotal += g_hostdb.getNumGrunts();

	if ( hostId >= 0 )
		pn->m_numHostsTotal = 1;

	// pump the parms out to other hosts in the network
	doParmSendingLoop ( );

	// . if waiting for more replies to come in that should be in soon
	// . doParmSendingLoop() is called when a reply comes in so that
	//   the next requests can be sent out
	//if ( waitingForLiveHostsToReply() ) return false;

	// all done. how did this happen?
	//return true;

	// wait for replies
	return false;
}

void tryToCallCallbacks ( ) {

	ParmNode *pn = s_headNode;
	int32_t now = getTimeLocal();

	for ( ; pn ; pn = pn->m_nextNode ) {
		// skip if already called callback
		if ( pn->m_calledCallback ) continue;
		// should we call the callback?
		bool callIt = false;
		// 8 seconds is enough to wait for all replies to come in
		if ( now - pn->m_startTime > 8 ) callIt = true;
		if ( pn->m_numReplies >= pn->m_numRequests ) callIt = true;
		if ( ! callIt ) continue;
		// callback is NULL for updating parms like spiderRoundNum
		// in Spider.cpp
		if ( pn->m_callback ) pn->m_callback ( pn->m_state );
		pn->m_calledCallback = true;
	}
}

void gotParmReplyWrapper ( void *state , UdpSlot *slot ) {

	// don't let upserver free the send buf! that's the ParmNode parmlist
	slot->m_sendBufAlloc = NULL;

	// in case host table is dynamically modified, go by #
	Host *h = g_hostdb.getHost((int32_t)(PTRTYPE)state);

	int32_t parmId = h->m_currentParmIdInProgress;

	ParmNode *pn = h->m_currentNodePtr;

	// inc this count
	pn->m_numReplies++;

	// nothing in progress now
	h->m_currentParmIdInProgress = 0;
	h->m_currentNodePtr = NULL;

	// this is usually timeout on a dead host i guess
	if ( g_errno ) {
		log("parms: got parm update reply from host #%"INT32": %s",
		    h->m_hostId,mstrerror(g_errno));
	}


	// . note it so we do not retry every 1ms!
	// . and only retry on time outs or no mem errors for now...
	// . it'll retry once every 10 seconds using the sleep
	//   wrapper below
	if ( g_errno != EUDPTIMEDOUT &&  g_errno != ENOMEM ) 
		g_errno = 0;

	if ( g_errno ) {
		// remember error info for retry
		h->m_lastTryError = g_errno;
		h->m_lastTryTime = getTimeLocal();
		// if a host timed out he could be dead, so try to call
		// the callback for this "pn" anyway. if the only hosts we
		// do not have replies for are dead, then we'll call the
		// callback, but still keep trying to send to them.
		tryToCallCallbacks ();
		// try to send more i guess? i think this is right otherwise
		// the callback might not ever get called
		g_parms.doParmSendingLoop();
		return;
	}

	// no error, otherwise
	h->m_lastTryError = 0;

	// successfully completed
	h->m_lastParmIdCompleted = parmId;

	// inc this count
	pn->m_numGoodReplies++;

	// . this will try to call any callback that can be called
	// . for instances, if the "pn" has recvd all the replies
	// . OR if the remaining hosts are "DEAD"
	// . the callback is in the "pn"
	tryToCallCallbacks ();

	// nuke it?
	if ( pn->m_numGoodReplies >= pn->m_numHostsTotal &&
	     pn->m_numReplies >= pn->m_numRequests ) {

		// . we must always be the head lest we send out of order.
		// . ParmNodes only destined to a specific hostid are ignored
		//   for this check, only look at those whose m_hostId is -1
		if(pn != s_headNode && pn->m_hostId==-1){
			log("parms: got parm request out of band. not head.");
		}

		// a new head
		if ( pn == s_headNode ) {
			// sanity
			if ( pn->m_prevNode ) { char *xx=NULL;*xx=0; }
			// the guy after us is the new head
			s_headNode = pn->m_nextNode;
		}

		// a new tail?
		if ( pn == s_tailNode ) {
			// sanity
			if ( pn->m_nextNode ) { char *xx=NULL;*xx=0; }
			// the guy before us is the new tail
			s_tailNode = pn->m_prevNode;
		}

		// empty?
		if ( ! s_headNode ) s_tailNode = NULL;

		// wtf?
		if ( ! pn->m_calledCallback ) { char *xx=NULL;*xx=0; }

		// do callback first before freeing pn
		//if ( pn->m_callback ) pn->m_callback ( pn->m_state );

		if ( pn->m_prevNode ) 
			pn->m_prevNode->m_nextNode = pn->m_nextNode;

		if ( pn->m_nextNode )
			pn->m_nextNode->m_prevNode = pn->m_prevNode;

		mfree ( pn , sizeof(ParmNode) , "pndfr");
	}

	// try to send more for him
	g_parms.doParmSendingLoop();
}

void parmLoop ( int fd , void *state ) {
	g_parms.doParmSendingLoop();
}

static bool s_registeredSleep = false;
static bool s_inLoop = false;

// . host #0 runs this to send out parms in the the parm queue (linked list)
//   to all other hosts.
// . he also sends to himself, if m_sendToGrunts is true
bool Parms::doParmSendingLoop ( ) {

	if ( ! s_headNode ) return true;

	if ( s_inLoop ) return true;

	s_inLoop = true;

	if ( ! s_registeredSleep &&
	     ! g_loop.registerSleepCallback(2000,NULL,parmLoop,0) )
		log("parms: failed to reg parm loop");

	// do not re-register
	s_registeredSleep = true;

	int32_t now = getTimeLocal();

	// try to send a parm update request to each host
	for ( int32_t i = 0 ; i < g_hostdb.m_numHosts ; i++ ) {
		// get it
		Host *h = g_hostdb.getHost(i);
		// skip ourselves, host #0. we now send to ourselves
		// so updateParm() will be called on us...
		//if ( h->m_hostId == g_hostdb.m_myHostId ) continue;
		// . if in progress, gotta wait for that to complete
		// . 0 is not a legit parmid, it starts at 1
		if ( h->m_currentParmIdInProgress ) continue;
		// if his last completed parmid is the current he is uptodate
		if ( h->m_lastParmIdCompleted == s_parmId ) continue;
		// if last try had an error, wait 10 secs i guess
		if ( h->m_lastTryError &&
		     h->m_lastTryError != EUDPTIMEDOUT &&
		     now - h->m_lastTryTime < 10 )
			continue;
		// otherwise get him the next to send
		ParmNode *pn = s_headNode;
		for ( ; pn ; pn = pn->m_nextNode ) {
			// stop when we got a parmnode we have not sent to
			// him yet, we'll send it now
			if ( pn->m_parmId > h->m_lastParmIdCompleted ) break;
		}
		// nothing? strange. something is not right.
		if ( ! pn ) { 
			log("parms: pn is null");
			break;
			char *xx=NULL; *xx=0; 
		}

		// give him a free pass? some parm updates are directed to 
		// a single host, we use this for syncing parms at startup.
		if ( pn->m_hostId >= 0 && 
		     pn->m_hostId2 == -1 && // not a range
		     h->m_hostId != pn->m_hostId ) {
			// assume we sent it to him
			h->m_lastParmIdCompleted = pn->m_parmId;
			h->m_currentNodePtr = NULL;
			continue;
		}

		// range? if not in range, give free pass
		if ( pn->m_hostId >= 0 && 
		     pn->m_hostId2 >= 0 &&
		     ( h->m_hostId < pn->m_hostId ||
		       h->m_hostId > pn->m_hostId2 ) ) {
			// assume we sent it to him
			h->m_lastParmIdCompleted = pn->m_parmId;
			h->m_currentNodePtr = NULL;
			continue;
		}

			
		// force completion if we should NOT send to him
		if ( (h->isProxy() && ! pn->m_sendToProxies) ||
		     (h->isGrunt() && ! pn->m_sendToGrunts ) ) {
			h->m_lastParmIdCompleted = pn->m_parmId;
			h->m_currentNodePtr = NULL;
			continue;
		}

		// debug log
		log(LOG_INFO,"parms: sending parm request "
		    "to hostid %"INT32"",h->m_hostId);

		// count it
		pn->m_numRequests++;
		// ok, he's available
		if ( ! g_udpServer.sendRequest ( pn->m_parmList.getBufStart(),
						 pn->m_parmList.length() ,
						 // a new msgtype
						 0x3f,
						 h->m_ip, // ip
						 h->m_port, // port
						 h->m_hostId ,
						 NULL, // retslot
						 (void *)(PTRTYPE)h->m_hostId , // state
						 gotParmReplyWrapper ,
						 30*1000 , // timeout msecs
						 -1 , // backoff
						 -1 , // maxwait
						 NULL , // replybuf
						 0 , // replybufmaxsize
						 0 ) ) { // niceness
			log("parms: faild to send: %s",mstrerror(g_errno));
			continue;
		}
		// flag this
		h->m_currentParmIdInProgress = pn->m_parmId;
		h->m_currentNodePtr = pn;
	}

	s_inLoop = false;

	return true;
}

void handleRequest3fLoop ( void *weArg ) ;

void handleRequest3fLoop2 ( void *state , UdpSlot *slot ) {
	handleRequest3fLoop(state);
}

// if a tree is saving while we are trying to delete a collnum (or reset)
// then the call to updateParm() below returns false and we must re-call
// in this sleep wrapper here
void handleRequest3fLoop3 ( int fd , void *state ) {
	g_loop.unregisterSleepCallback(state,handleRequest3fLoop3);
	handleRequest3fLoop(state);	
}

// . host #0 is requesting that we update some parms
void handleRequest3fLoop ( void *weArg ) {
	WaitEntry *we = (WaitEntry *)weArg;

	CollectionRec *cx = NULL;

	// process them
	char *p = we->m_parmPtr;
	for ( ; p < we->m_parmEnd ; ) {
		// shortcut
		char *rec = p;
		// get size
		int32_t dataSize = *(int32_t *)(rec+sizeof(key96_t));
		int32_t recSize = sizeof(key96_t) + 4 + dataSize;
		// skip it
		p += recSize;

		// get the actual parm
		Parm *parm = getParmFromParmRec ( rec );

		if ( ! parm ) {
			int32_t h32 = getHashFromParmRec(rec);
			log("parms: unknown parm sent to us hash=%"INT32"",h32);
			for ( int32_t i = 0 ; i < g_parms.m_numParms ; i++ ) {
				Parm *x = &g_parms.m_parms[i];
				if ( x->m_cgiHash != h32 ) continue;
				log("parms: unknown parm=%s",x->m_title);
				break;
			}
			continue;
		}

		// if was the cmd to save & exit then first send a reply back
		if ( ! we->m_sentReply &&
		     parm->m_cgi && 
		     parm->m_cgi[0] == 's' &&
		     parm->m_cgi[1] == 'a' &&
		     parm->m_cgi[2] == 'v' &&
		     parm->m_cgi[3] == 'e' &&
		     parm->m_cgi[4] == '\0' ) {
			// do not re-do this
			we->m_sentReply = 1;
			// note it
			log("parms: sending early parm update reply");
			// wait for reply to be sent and ack'd
			g_udpServer.sendReply_ass( NULL, 0, NULL, 0, we->m_slot, we, handleRequest3fLoop2 );
			return;
		}


		// . determine if it alters the url filters
		// . if those were changed we have to nuke doledb and
		//   waiting tree in Spider.cpp and rebuild them!
		if ( parm->m_flags & PF_REBUILDURLFILTERS )
			we->m_doRebuilds = true;

		if ( parm->m_flags & PF_REBUILDPROXYTABLE )
			we->m_doProxyRebuild = true;

		if ( parm->m_flags & PF_REBUILDACTIVELIST )
			we->m_rebuildActiveList = true;

		// get collnum i guess
		if ( parm->m_type != TYPE_CMD )
			we->m_collnum = getCollnumFromParmRec ( rec );

		// see if our spider round changes
		int32_t oldRound; 
		if ( we->m_collnum >= 0 && ! cx ) {
			cx = g_collectiondb.getRec ( we->m_collnum );
			// i guess coll might gotten deleted! so check cx
			if ( cx ) oldRound = cx->m_spiderRoundNum;
		}

		// . this returns false if blocked, returns true and sets
		//   g_errno on error
		// . it'll block if trying to delete a coll when the tree
		//   is saving or something (CommandDeleteColl())
		if ( ! g_parms.updateParm ( rec , we ) ) {
			////////////
			//
			// . it blocked! it will call we->m_callback when done
			// . we must re-call
			// . try again in 100ms
			//
			////////////
			if(!g_loop.registerSleepCallback(100, 
							 we ,
							 handleRequest3fLoop3,
							 0 ) ){// niceness
				log("parms: failed to reg sleeper");
				return;
			}
			log("parms: updateParm blocked. waiting.");
			return;
		}

		if ( cx && oldRound != cx->m_spiderRoundNum )
			we->m_updatedRound = true;

		// do the next parm
		we->m_parmPtr = p;

		// error?
		if ( ! g_errno ) continue;
		// this could mean failed to add coll b/c out of disk or
		// something else that is bad
		we->m_errno = g_errno;
	}

	// one last thing... kinda hacky. if we change certain spidering parms
	// we have to do a couple rebuilds.

	// reset page round counts
	if ( we->m_updatedRound && cx ) {
		// Spider.cpp will reset the *ThisRound page counts and
		// the sent notification flag
		spiderRoundIncremented ( cx );
	}

	// basically resetting the spider here...
	if ( we->m_doRebuilds && cx ) {
		// . this tells Spider.cpp to rebuild the spider queues
		// . this is NULL if spider stuff never initialized yet,
		//   like if you just added the collection
		if ( cx->m_spiderColl )
			cx->m_spiderColl->m_waitingTreeNeedsRebuild = true;
		// . assume we have urls ready to spider too
		// . no, because if they change the filters and there are
		//   still no urls to spider i don't want to get another
		//   email alert!!
		//cr->m_localCrawlInfo .m_hasUrlsReadyToSpider = true;
		//cr->m_globalCrawlInfo.m_hasUrlsReadyToSpider = true;
		// . reconstruct the url filters if we were a custom crawl
		// . this is used to abstract away the complexity of url
		//   filters in favor of simple regular expressions and
		//   substring matching for diffbot
		cx->rebuildUrlFilters();
	}

	if ( we->m_rebuildActiveList && cx ) 
		g_spiderLoop.m_activeListValid = false;

	// if user changed the list of proxy ips rebuild the binary
	// array representation of the proxy ips we have
	if ( we->m_doProxyRebuild )
		buildProxyTable();
		
	// note it
	if ( ! we->m_sentReply )
		log("parms: sending parm update reply");

	// send back reply now. empty reply for the most part
	if ( we->m_errno && !we->m_sentReply ) {
		g_udpServer.sendErrorReply( we->m_slot, we->m_errno );
	} else if ( !we->m_sentReply ) {
		g_udpServer.sendReply_ass( NULL, 0, NULL, 0, we->m_slot );
	}

	// all done
	mfree ( we , sizeof(WaitEntry) , "weparm" );
	return;
}

// . host #0 is requesting that we update some parms
// . the readbuf in the request is the list of the parms
void handleRequest3f ( UdpSlot *slot , int32_t niceness ) {

	// sending to host #0 is not right...
	//if ( g_hostdb.m_hostId == 0 ) { char *xx=NULL;*xx=0; }

	char *parmRecs = slot->m_readBuf;
	char *parmEnd  = parmRecs + slot->m_readBufSize;

	log("parms: got parm update request. size=%"INT32".",
	    (int32_t)(parmEnd-parmRecs));

	// make a new waiting entry
	WaitEntry *we ;
	we = (WaitEntry *) mmalloc ( sizeof(WaitEntry),"weparm");
	if ( !we ) {
		g_udpServer.sendErrorReply( slot, g_errno );
		return;
	}

	we->m_slot = slot;
	we->m_callback = handleRequest3fLoop;
	we->m_parmPtr = parmRecs;
	we->m_parmEnd = parmEnd;
	we->m_errno = 0;
	we->m_doRebuilds = false;
	we->m_rebuildActiveList = false;
	we->m_updatedRound = false;
	we->m_doProxyRebuild = false;
	we->m_collnum = -1;
	we->m_sentReply = 0;

	handleRequest3fLoop ( we );
}




////
//
// functions for syncing parms with host #0
//
////

// 1. we do not accept any recs into rdbs until in sync with host #0
// 2. at startup we send the hash of all parms for each collrec and
//    for g_conf (collnum -1) to host #0, then he will send us all the
//    parms for a collrec (or g_conf) if we are out of sync.
// 3. when host #0 changes a parm it lets everyone know via broadcastParmList()
// 4. only host #0 may initiate parm changes. so don't let that go down!
// 5. once in sync a host can drop recs for collnums that are invalid
// 6. until in parm sync with host #0 reject adds to collnums we don't 
//    have with ETRYAGAIN in Msg4.cpp


void tryToSyncWrapper ( int fd , void *state ) {
	g_parms.syncParmsWithHost0();
}

// host #0 just sends back an empty reply, but it will hit us with
// 0x3f parmlist requests. that way it uses the same mechanism and can
// guarantee ordering of the parm update requests
void gotReplyFromHost0Wrapper ( void *state , UdpSlot *slot ) {
	// ignore his reply unless error?
	if ( g_errno ) {
		log("parms: got error syncing with host 0: %s. Retrying.",
		    mstrerror(g_errno));
		// re-try it!
		g_parms.m_triedToSync = false;
	}
	else {
		log("parms: synced with host #0");
		// do not re-call
		g_loop.unregisterSleepCallback(NULL,tryToSyncWrapper);
	}

	g_errno = 0;
}

// returns false and sets g_errno on error, true otherwise
bool Parms::syncParmsWithHost0 ( ) {

	if ( m_triedToSync ) return true;

	m_triedToSync = true;

	m_inSyncWithHost0 = false;

	// dont sync with ourselves
	if ( g_hostdb.m_hostId == 0 ) {
		m_inSyncWithHost0 = true;
		return true;
	}

	// only grunts for now can sync, not proxies, so stop if we are proxy
	if ( g_hostdb.m_myHost->m_type != HT_GRUNT ) {
		m_inSyncWithHost0 = true;
		return true;
	}


	SafeBuf hashList;
	
	if ( ! makeSyncHashList ( &hashList ) ) return false;

	// copy for sending
	SafeBuf sendBuf;
	if ( ! sendBuf.safeMemcpy ( &hashList ) ) return false;
	if ( sendBuf.getCapacity() != hashList.length() ){char *xx=NULL;*xx=0;}
	if ( sendBuf.length() != hashList.length()  ){char *xx=NULL;*xx=0;}

	// allow udpserver to free it
	char *request = sendBuf.getBufStart();
	int32_t  requestLen = sendBuf.length();
	sendBuf.detachBuf();

	Host *h = g_hostdb.getHost(0);

	log("parms: trying to sync with host #0");

	// . send it off. use 3e i guess
	// . host #0 will reply using msg4 really
	// . msg4 guarantees ordering of requests
	// . there will be a record that is CMD_INSYNC so when we get
	//   that we set g_parms.m_inSyncWithHost0 to true
	if ( ! g_udpServer.sendRequest ( request ,//hashList.getBufStart() ,
					 requestLen, //hashList.length() ,
					 0x3e , // msgtype
					 h->m_ip, // ip
					 h->m_port, // port
					 h->m_hostId , // hostid , host #0!!!
					 NULL, // retslot
					 NULL , // state
					 gotReplyFromHost0Wrapper ,
					 udpserver_sendrequest_infinite_timeout ) ) { // timeout in msecs
		log("parms: error syncing with host 0: %s",mstrerror(g_errno));
		return false;
	}

	// wait now
	return true;
}

// . here host #0 is receiving a sync request from another host
// . host #0 scans this list of hashes to make sure the requesting host is
//   in sync
// . host #0 will broadcast parm updates by calling broadcastParmList() which
//   uses 0x3f, so this just returns and empty reply on success
// . sends CMD "addcoll" and "delcoll" cmd parms as well
// . include an "insync" command parm as last parm
void handleRequest3e ( UdpSlot *slot , int32_t niceness ) {
	// right now we must be host #0
	if ( g_hostdb.m_hostId != 0 ) {
		g_errno = EBADENGINEER;
	hadError:
		g_udpServer.sendErrorReply( slot, g_errno );
		return;
	}

	//
	// 0. scan our collections and clear a flag
	//
	for ( int32_t i = 0 ; i < g_collectiondb.m_numRecs ; i++ ) {
		// skip if empty
		CollectionRec *cr = g_collectiondb.m_recs[i];
		if ( ! cr ) continue;
		// clear flag
		cr->m_hackFlag = 0;
	}

	Host *host = slot->m_host;
	int32_t hostId = -1;
	if ( host ) hostId = host->m_hostId;

	SafeBuf replyBuf;

	//
	// 1. update parms on collections we both have
	// 2. tell him to delete collections we do not have but he does
	//
	SafeBuf tmp;
	char *p = slot->m_readBuf;
	char *pend = p + slot->m_readBufSize;
	for ( ; p < pend ; ) {
		// get collnum
		collnum_t c = *(collnum_t *)p;
		p += sizeof(collnum_t);
		// then coll NAME hash
		uint32_t collNameHash32 = *(int32_t *)p;
		p += 4;
		// sanity check. -1 means g_conf. i guess.
		if ( c < -1 ) { char *xx=NULL;*xx=0; }
		// and parm hash
		int64_t h64 = *(int64_t *)p;
		p += 8;
		// if we being host #0 do not have this collnum tell 
		// him to delete it!
		CollectionRec *cr = NULL;
		if ( c >= 0 ) cr = g_collectiondb.getRec ( c );

		// if collection names are different delete it
		if ( cr && collNameHash32 != hash32n ( cr->m_coll ) ) {
			log("sync: host had collnum %i but wrong name, "
			    "name not %s like it should be",(int)c,cr->m_coll);
			cr = NULL;
		}

		if ( c >= 0 && ! cr ) {
			// note in log
			logf(LOG_INFO,"sync: telling host #%"INT32" to delete "
			     "collnum %"INT32"", hostId,(int32_t)c);
			// add the parm rec as a parm cmd
			if (! g_parms.addNewParmToList1( &replyBuf,
							 c,
							 NULL,
							 -1,
							 "delete"))
				goto hadError;
			// ok, get next collection hash
			continue;
		}
		// set our hack flag so we know he has this collection
		if ( cr ) cr->m_hackFlag = 1;
		// get our parmlist for that collnum
		tmp.reset();
		// c is -1 for g_conf
		if ( ! g_parms.addAllParmsToList ( &tmp, c ) ) goto hadError;
		// get checksum of that
		int64_t m64 = hash64 ( tmp.getBufStart(),tmp.length() );
		// if match, keep chugging, that's in sync
		if ( h64 == m64 ) continue;
		// note in log
		logf(LOG_INFO,"sync: sending all parms for collnum %"INT32" "
		     "to host #%"INT32"", (int32_t)c, hostId);
		// otherwise, send him the list
		if ( ! replyBuf.safeMemcpy ( &tmp ) ) goto hadError;
	}

	//
	// 3. now if he's missing one of our collections tell him to add it
	//
	for ( int32_t i = 0 ; i < g_collectiondb.m_numRecs ; i++ ) {
		// skip if empty
		CollectionRec *cr = g_collectiondb.m_recs[i];
		if ( ! cr ) continue;
		// clear flag
		if ( cr->m_hackFlag ) continue;
		//char *cmdStr = "addColl";
		// now use lowercase, not camelcase
		char *cmdStr = "addcoll";
		if ( cr->m_isCustomCrawl == 1 ) cmdStr = "addCrawl";
		if ( cr->m_isCustomCrawl == 2 ) cmdStr = "addBulk";
		// note in log
		logf(LOG_INFO,"sync: telling host #%"INT32" to add "
		     "collnum %"INT32" coll=%s", hostId,(int32_t)cr->m_collnum,
		     cr->m_coll);
		// add the parm rec as a parm cmd
		if ( ! g_parms.addNewParmToList1 ( &replyBuf,
						   (collnum_t)i,
						   cr->m_coll, // parm val
						   -1,
						   cmdStr ) )
			goto hadError;
		// and the parmlist for it
		if (!g_parms.addAllParmsToList (&replyBuf, i ) ) goto hadError;
	}

	// . final parm is the in sync stamp of approval which will set
	//   g_parms.m_inSyncWithHost0 to true. CommandInSync()
	// .  use -1 for collnum for this cmd
	if ( ! g_parms.addNewParmToList1 ( &replyBuf,-1,NULL,-1,"insync"))
		goto hadError;

	// this should at least have the in sync command
	log("parms: sending %"INT32" bytes of parms to sync to host #%"INT32"",
	    replyBuf.length(),hostId);

	// . use the broadcast call here so things keep their order!
	// . we do not need a callback when they have been completely
	//   broadcasted to all hosts so use NULL for that
	// . crap, we only want to send this to host #x ...
	g_parms.broadcastParmList ( &replyBuf , NULL , NULL , 
				    true , // sendToGrunts?
				    false ,  // sendToProxies?
				    hostId );

	// but do send back an empty reply to this 0x3e request
	g_udpServer.sendReply_ass ( NULL,0,NULL,0,slot);
	
	// send that back now
	//g_udpServer.sendReply_ass ( replyBuf.getBufStart() ,
	//			    replyBuf.length() ,
	//			    replyBuf.getBufStart() ,
	//			    replyBuf.getCapacity() ,
	//			    slot );
	// udpserver will free it
	//replyBuf.detachBuf();
}


// get the hash of every collection's parmlist
bool Parms::makeSyncHashList ( SafeBuf *hashList ) {
	SafeBuf tmp;

	// first do g_conf, collnum -1!
	for ( int32_t i = -1 ; i < g_collectiondb.m_numRecs ; i++ ) {
		// shortcut
		CollectionRec *cr = NULL;
		if ( i >= 0 ) cr = g_collectiondb.m_recs[i];
		// skip if empty
		if ( i >=0 && ! cr ) continue;
		// clear since last time
		tmp.reset();
		// g_conf? if i is -1 do g_conf
		if ( ! addAllParmsToList ( &tmp , i ) )
			return false;
		// store collnum first as 4 bytes
		if ( ! hashList->safeMemcpy ( &i , sizeof(collnum_t) ) )
			return false;
		// then store the collection name hash, 32 bit hash
		uint32_t collNameHash32 = 0;
		if ( cr ) collNameHash32 = hash32n ( cr->m_coll );
		if ( ! hashList->safeMemcpy ( &collNameHash32, 4 ) )
			return false;
		// hash the parms
		int64_t h64 = hash64 ( tmp.getBufStart(),tmp.length() );
		// and store it
		if ( ! hashList->pushLongLong ( h64 ) )
			return false;
	}
	return true;
}

int32_t Parm::getNumInArray ( collnum_t collnum ) {
	char *obj = (char *)&g_conf;
	if ( m_obj == OBJ_COLL ) {
		CollectionRec *cr = g_collectiondb.getRec ( collnum );
		if ( ! cr ) return -1;
		obj = (char *)cr;
	}
	// # in array is before it
	return *(int32_t *)(obj+m_off-4);
}

// . we use this for syncing parms between hosts
// . called by convertAllCollRecsToParmList
// . returns false and sets g_errno on error
// . "rec" can be CollectionRec or g_conf ptr
bool Parms::addAllParmsToList ( SafeBuf *parmList, collnum_t collnum ) {

	// loop over parms
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {
		// get it
		Parm *parm = &m_parms[i];
		// skip comments
		if ( parm->m_type == TYPE_COMMENT ) continue;
		if ( parm->m_type == TYPE_FILEUPLOADBUTTON ) continue;
		// cmds
		if ( parm->m_type == TYPE_CMD ) continue;
		if ( parm->m_type == TYPE_BOOL2 ) continue;

		// daily merge last started. do not sync this...
		if ( parm->m_type == TYPE_LONG_CONST ) continue;

		if ( collnum == -1 && parm->m_obj != OBJ_CONF ) continue;
		if ( collnum >=  0 && parm->m_obj != OBJ_COLL ) continue;
		if ( collnum < -1 ) { char *xx=NULL;*xx=0; }

		// like 'statsdb max cache mem' etc.
		if ( parm->m_flags & PF_NOSYNC ) continue;

		// sanity, need cgi hash to look up the parm on the 
		// receiving end
		if ( parm->m_cgiHash == 0 ) { 
			log("parms: no cgi for parm %s",parm->m_title);
			char *xx=NULL; *xx=0;
		}

		int32_t occNum = -1;
		int32_t maxOccNum = 0;

		if ( parm->isArray() ) {
			maxOccNum = parm->getNumInArray(collnum) ;
			occNum = 0;
		}

		for ( ; occNum < maxOccNum ; occNum ++ ) {
			// add each occ # to list
			if ( ! addCurrentParmToList2 ( parmList ,
						       collnum ,
						       occNum ,
						       parm ) )
				return false;
			/*
			  //
			  // use this to debug parm list checksums being off
			  //
			int64_t h64 ;
			h64 = hash64 ( parmList->getBufStart(),
				       parmList->length() );
			// note it for debugging hash
			SafeBuf xb;
			parm->printVal ( &xb ,collnum,occNum);
			log("parms: adding (h=%"XINT64") parm %s = %s",
			    h64,parm->m_title,xb.getBufStart());
			*/
		}

	}
	return true;
}	

void resetImportLoopFlag () ;

// . this adds the key if not a cmd key to parmdb rdbtree
// . this executes cmds
// . this updates the CollectionRec which may disappear later and be fully
//   replaced by Parmdb, just an RdbTree really.
// . returns false if blocked
// . returns true and sets g_errno on error
bool Parms::updateParm ( char *rec , WaitEntry *we ) {

	collnum_t collnum = getCollnumFromParmRec ( rec );

	g_errno = 0;

	Parm *parm = getParmFromParmRec ( rec );

	if ( ! parm ) {
		log("parmdb: could not find parm for rec");
		g_errno = EBADENGINEER;
		return true;
	}

	// cmd to execute?
	if ( parm->m_type == TYPE_CMD ||
	     // sitelist is a safebuf but it requires special deduping
	     // logic to update it so it uses CommandUpdateSiteList() to
	     // do the updating
	     parm->m_func ) {
		// all parm rec data for TYPE_CMD should be ascii/utf8 chars
		// and should be \0 terminated
		char *data = getDataFromParmRec ( rec );
		int32_t dataSize = getDataSizeFromParmRec ( rec );
		if ( dataSize == 0 ) data = NULL;
		log("parmdb: running function for "
		    "parm \"%s\" (collnum=%"INT32") args=\"%s\""
		    , parm->m_title
		    , (int32_t)collnum
		    , data 
		    );

		// sets g_errno on error
		if ( parm->m_func ) {
			parm->m_func ( rec );
			return true;
		}

		// fix core from using "roundstart=1" on non-existent coll
		if ( ! parm->m_func2 ) {
			return true;
		}

		// . returns true and sets g_errno on error
		// . returns false if blocked
		// . this is for CommandDeleteColl() and CommandResetColl()
		if ( parm->m_func2 ( rec , we ) ) return true;

		// . it did not complete.
		// . we need to re-call it using sleep wrapper above
		return false;
	}

	// "cr" will remain null when updating g_conf and collnum -1
	CollectionRec *cr = NULL;
	if ( collnum >= 0 ) {
		cr = g_collectiondb.getRec ( collnum );
		if ( ! cr ) {
			char *ps = "unknown parm";
			if ( parm ) ps = parm->m_title;
			log("parmdb: invalid collnum %"INT32" for parm \"%s\"",
			    (int32_t)collnum,ps);
			g_errno = ENOCOLLREC;
			return true;
		}
	}

	// what are we updating?
	void *base = NULL;

	// we might have a collnum specified even if parm is global,
	// maybe there are some collection/local parms specified as well
	// that that collnum applies to
	if ( parm->m_obj == OBJ_COLL ) base = cr;
	else                           base = &g_conf;

	if ( ! base ) {
		log("parms: no collrec (%"INT32") to change parm",(int32_t)collnum);
		g_errno = ENOCOLLREC;
		return true;
	}
		
	int32_t occNum = getOccNumFromParmRec ( rec );

	// get data
	int32_t dataSize = *(int32_t *)(rec+sizeof(key96_t));
	char *data = rec+sizeof(key96_t)+4;

	// point to where to copy the data into collrect
	char *dst = (char *)base + parm->m_off;
	// point to count in case it is an array
	int32_t *countPtr = NULL;
	// array?
	if ( parm->isArray() ) {
		if ( occNum < 0 ) {
			log("parms: bad occnum for %s",parm->m_title);
			return false;
		}
		// point to count in case it is an array
		countPtr = (int32_t *)(dst - 4);
		// now point "dst" to the occNum-th element
		dst += parm->m_size * occNum;
	}

	//
	// compare parm to see if it changed value
	//
	SafeBuf val1;
	parm->printVal ( &val1 , collnum , occNum );

	// if parm is a safebuf...
	if ( parm->m_type == TYPE_SAFEBUF ) {
		// point to it
		SafeBuf *sb = (SafeBuf *)dst;
		// nuke it
		sb->purge();
		// require that the \0 be part of the update i guess
		//if ( ! data || dataSize <= 0 ) { char *xx=NULL;*xx=0; }
		// check for \0
		if ( data && dataSize > 0 ) {
			if ( data[dataSize-1] != '\0') { char *xx=NULL;*xx=0;}
			// this means that we can not use string POINTERS as 
			// parms!! don't include \0 as part of length
			sb->safeStrcpy ( data ); // , dataSize );
			// ensure null terminated
			sb->nullTerm();
			sb->setLabel("parm2");
		}
		//return true;
		// sanity
		// we no longer include the \0 in the dataSize...so a dataSize
		// of 0 means empty string...
		//if ( data[dataSize-1] != '\0' ) { char *xx=NULL;*xx=0; }
	}
	else {
		// and copy the data into collrec or g_conf
		gbmemcpy ( dst , data , dataSize );
	}

	SafeBuf val2;
	parm->printVal ( &val2 , collnum , occNum );

	// did this parm change value?
	bool changed = true;
	if ( strcmp ( val1.getBufStart() , val2.getBufStart() ) == 0 )
		changed = false;

	// . update array count if necessary
	// . parm might not have changed value based on what was in there
	//   by default, but for PAGE_FILTERS the default value in the row
	//   for this parm might have been zero! so we gotta update its
	//   "count" in that scenario even though the parm val was unchanged.
	if ( parm->isArray() ) {
		// the int32_t before the array is the # of elements
		int32_t currentCount = *countPtr;
		// update our # elements in our array if this is bigger
		int32_t newCount = occNum + 1;
		bool updateCount = false;
		if ( newCount > currentCount ) updateCount = true;
		// do not update counts if we are url filters
		// and we are currently >= the expression count. we have
		// to have a non-empty expression at the end in order to
		// add the expression. this prevents the empty line from
		// being added!
		if ( parm->m_page == PAGE_FILTERS &&
		     cr->m_regExs[occNum].getLength() == 0 )
			updateCount = false;
		// and for other pages, like master ips, skip if empty!
		// PAGE_PASSWORDS, PAGE_MASTERPASSWORDS, ...
		if ( parm->m_page != PAGE_FILTERS && ! changed )
			updateCount = false;

		// ok, increment the array count of items in the array
		if ( updateCount )
			*countPtr = newCount;
	}

	// all done if value was unchanged
	if ( ! changed )
		return true;

	// show it
	log("parms: updating parm \"%s\" "
	    "(%s[%"INT32"]) (collnum=%"INT32") from \"%s\" -> \"%s\"",
	    parm->m_title,
	    parm->m_cgi,
	    occNum,
	    (int32_t)collnum,
	    val1.getBufStart(),
	    val2.getBufStart());

	if ( cr ) cr->m_needsSave = true;

	// HACK #2
	if ( base == cr && dst == (char *)&cr->m_importEnabled )
		resetImportLoopFlag();

	//
	// HACK
	//
	// special hack. if spidering re-enabled then reset last spider
	// attempt time to 0 to avoid the "has no more urls to spider"
	// msg followed by the reviving url msg.
	if ( base == cr && dst == (char *)&cr->m_spideringEnabled )
		cr->m_localCrawlInfo.m_lastSpiderAttempt = 0;
	if ( base == &g_conf && dst == (char *)&g_conf.m_spideringEnabled ){
		for(int32_t i = 0;i<g_collectiondb.m_numRecs;i++){
			CollectionRec *cr = g_collectiondb.m_recs[i];
			if ( ! cr ) continue;
			cr->m_localCrawlInfo.m_lastSpiderAttempt = 0;
		}
	}

	//
	// if user changed the crawl/process max then reset here so
	// spiders will resume
	// 
	if ( base == cr && 
	     dst == (char *)&cr->m_maxToCrawl &&
	     cr->m_spiderStatus == SP_MAXTOCRAWL ) {
		// reset this for rebuilding of active spider collections
		// so this collection can be in the linked list again
		cr->m_spiderStatus = SP_INPROGRESS;
		// rebuild list of active spider collections then
		g_spiderLoop.m_activeListValid = false;
	}

	if ( base == cr && 
	     dst == (char *)&cr->m_maxToProcess &&
	     cr->m_spiderStatus == SP_MAXTOPROCESS ) {
		// reset this for rebuilding of active spider collections
		// so this collection can be in the linked list again
		cr->m_spiderStatus = SP_INPROGRESS;
		// rebuild list of active spider collections then
		g_spiderLoop.m_activeListValid = false;
	}

	if ( base == cr && 
	     dst == (char *)&cr->m_maxCrawlRounds &&
	     cr->m_spiderStatus == SP_MAXROUNDS ) {
		// reset this for rebuilding of active spider collections
		// so this collection can be in the linked list again
		cr->m_spiderStatus = SP_INPROGRESS;
		// rebuild list of active spider collections then
		g_spiderLoop.m_activeListValid = false;
	}

	//
	// END HACK
	//

	// all done
	return true;
}

bool Parm::printVal ( SafeBuf *sb , collnum_t collnum , int32_t occNum ) {

	CollectionRec *cr = NULL;
	if ( collnum >= 0 ) cr = g_collectiondb.getRec ( collnum );

	// no value if no storage record offset
	//if ( m_off < 0 ) return true;

	char *base;
	if ( m_obj == OBJ_COLL ) base = (char *)cr;
	else                     base = (char *)&g_conf;

	if ( ! base ) {
		log("parms: no collrec (%"INT32") to change parm",(int32_t)collnum);
		g_errno = ENOCOLLREC;
		return true;
	}
		
	// point to where to copy the data into collrect
	char *val = (char *)base + m_off;

	if ( isArray() && occNum < 0 ) {
		log("parms: bad occnum for %s",m_title);
		return false;
	}

	// add array index to ptr
	if ( isArray() ) val += m_size * occNum;


	if ( m_type == TYPE_SAFEBUF ) {
		// point to it
		SafeBuf *sb2 = (SafeBuf *)val;
		return sb->safePrintf("%s",sb2->getBufStart());
	}

	if ( m_type == TYPE_STRING || 
	     m_type == TYPE_STRINGBOX || 
	     m_type == TYPE_SAFEBUF ||
	     m_type == TYPE_STRINGNONEMPTY )
		return sb->safePrintf("%s",val);

	if ( m_type == TYPE_LONG || m_type == TYPE_LONG_CONST ) 
		return sb->safePrintf("%"INT32"",*(int32_t *)val);

	if ( m_type == TYPE_DATE ) 
		return sb->safePrintf("%"INT32"",*(int32_t *)val);

	if ( m_type == TYPE_DATE2 ) 
		return sb->safePrintf("%"INT32"",*(int32_t *)val);

	if ( m_type == TYPE_FLOAT ) 
		return sb->safePrintf("%f",*(float *)val);

	if ( m_type == TYPE_LONG_LONG ) 
		return sb->safePrintf("%"INT64"",*(int64_t *)val);

	if ( m_type == TYPE_CHARPTR ) {
		if ( val ) return sb->safePrintf("%s",val);
		return true;
	}

	if ( m_type == TYPE_BOOL ||
	     m_type == TYPE_BOOL2 ||
	     m_type == TYPE_CHECKBOX ||
	     m_type == TYPE_PRIORITY2 ||
	     m_type == TYPE_UFP ||
	     m_type == TYPE_CHAR )
		return sb->safePrintf("%hhx",*val);

	if ( m_type == TYPE_CMD ) 
		return sb->safePrintf("CMD");

	if ( m_type == TYPE_IP )
		// may print 0.0.0.0
		return sb->safePrintf("%s",iptoa(*(int32_t *)val) );

	log("parms: missing parm type!!");

	char *xx=NULL;*xx=0;
	return false;
}

bool printUrlExpressionExamples ( SafeBuf *sb ) {
		sb->safePrintf(
			       "<style>"
			       ".poo { background-color:#%s;}\n"
			       "</style>\n" ,
			       LIGHT_BLUE );

		sb->safePrintf (
			  "<table %s>"
			  "<tr><td colspan=2><center>"
			  "<b>"
			  "Supported Expressions</b>"
			  "</td></tr>"

			  "<tr class=poo><td>default</td>"
			  "<td>Matches every url."
			  "</td></tr>"

			  "<tr class=poo><td>^http://whatever</td>"
			  "<td>Matches if the url begins with "
			  "<i>http://whatever</i>"
			  "</td></tr>"

			  "<tr class=poo><td>$.css</td>"
			  "<td>Matches if the url ends with \".css\"."
			  "</td></tr>"

			  "<tr class=poo><td>foobar</td>"
			  "<td>Matches if the url CONTAINS <i>foobar</i>."
			  "</td></tr>"

			  "<tr class=poo><td>tld==uk,jp</td>"
			  "<td>Matches if url's TLD ends in \"uk\" or \"jp\"."
			  "</td></tr>"

			  /*
			  "<tr class=poo><td>doc:quality&lt;40</td>"
			  "<td>Matches if document quality is "
			  "less than 40. Can be used for assigning to spider "
			  "priority.</td></tr>"

			  "<tr class=poo><td>doc:quality&lt;40 && tag:ruleset==22</td>"
			  "<td>Matches if document quality less than 40 and "
			  "belongs to ruleset 22. Only for assinging to "
			  "spider priority.</td></tr>"

			  "<tr class=poo><td><nobr>"
			  "doc:quality&lt;40 && tag:manualban==1</nobr></td>"
			  "<td>Matches if document quality less than 40 and "
			  "is has a value of \"1\" for its \"manualban\" "
			  "tag.</td></tr>"

			  "<tr class=poo><td>tag:ruleset==33 && doc:quality&lt;40</td>"
			  "<td>Matches if document quality less than 40 and "
			  "belongs to ruleset 33. Only for assigning to "
			  "spider priority or a banned ruleset.</td></tr>"
			  */

			  "<tr class=poo><td><a name=hopcount></a>"
			  "hopcount<4 && iswww</td>"
			  "<td>Matches if document has a hop count of 4, and "
			  "is a \"www\" url (or domain-only url).</td></tr>"
			  
			  "<tr class=poo><td>hopcount</td>"
			  "<td>All root urls, those that have only a single "
			  "slash for their path, and no cgi parms, have a "
			  "hop count of 0. Also, all RSS urls, ping "
			  "server urls and site roots (as defined in the "
			  "site rules table) have a hop count of 0. Their "
			  "outlinks have a hop count of 1, and the outlinks "
			  "of those outlinks a hop count of 2, etc."
			  "</td></tr>"

			  "<tr class=poo><td>sitepages</td>"
			  "<td>The number of pages that are currently indexed "
			  "for the subdomain of the URL. "
			  "Used for doing quotas."
			  "</td></tr>"


			  // MDW: 7/11/2014 take this out until it works.
			  // problem is that the quota table m_localTable
			  // in Spider.cpp gets reset for each firstIp scan,
			  // and we have a.walmart.com and b.walmart.com
			  // with different first ips even though on same 
			  // domain. perhaps we should use the domain as the
			  // key to getting the firstip for and subdomain.
			  // but out whole selection algo in spider.cpp is
			  // firstIp based, so it scans all the spiderrequests
			  // from a single firstip to get the winner for that
			  // firstip. 
			  

			  // "<tr class=poo><td>domainpages</td>"
			  // "<td>The number of pages that are currently indexed "
			  // "for the domain of the URL. "
			  // "Used for doing quotas."
			  // "</td></tr>"

			  "<tr class=poo><td>siteadds</td>"
			  "<td>The number URLs manually added to the "
			  "subdomain of the URL. Used to guage a subdomain's "
			  "popularity."
			  "</td></tr>"

			  // taken out for the same reason as domainpages
			  // above was taken out. see expanation up there.
			  // "<tr class=poo><td>domainadds</td>"
			  // "<td>The number URLs manually added to the "
			  // "domain of the URL. Used to guage a domain's "
			  // "popularity."
			  // "</td></tr>"



			  "<tr class=poo><td>isrss | !isrss</td>"
			  "<td>Matches if document is an RSS feed. Will "
			  "only match this rule if the document has been "
			  "successfully spidered before, because it requires "
			  "downloading the document content to see if it "
			  "truly is an RSS feed.."
			  "</td></tr>"

			  "<tr class=poo><td>isrssext | !isrssext</td>"
			  "<td>Matches if url ends in .xml .rss or .atom. "
			  "TODO: Or if the link was in an "
			  "alternative link tag."
			  "</td></tr>"

			  //"<tr class=poo><td>!isrss</td>"
			  //"<td>Matches if document is NOT an rss feed."
			  //"</td></tr>"

			  "<tr class=poo><td>ispermalink | !ispermalink</td>"
			  "<td>Matches if document is a permalink. "
			  "When harvesting outlinks we <i>guess</i> if they "
			  "are a permalink by looking at the structure "
			  "of the url.</td></tr>"

			  //"<tr class=poo><td>!ispermalink</td>"
			  //"<td>Matches if document is NOT a permalink."
			  //"</td></tr>"

			  /*
			  "<tr class=poo><td>outlink | !outlink</td>"
			  "<td>"
			  "<b>This is true if url being added to spiderdb "
			  "is an outlink from the page being spidered. "
			  "Otherwise, the url being added to spiderdb "
			  "directly represents the page being spidered. It "
			  "is often VERY useful to partition the Spiderdb "
			  "records based on this criteria."
			  "</td></tr>"
			  */

			  "<tr class=poo><td><nobr>isnewoutlink | !isnewoutlink"
			  "</nobr></td>"
			  "<td>"
			  "This is true since the outlink was not there "
			  "the last time we spidered the page we harvested "
			  "it from."
			  "</td></tr>"

			  "<tr class=poo><td>hasreply | !hasreply</td>"
			  "<td>"
			  "This is true if we have tried to spider "
			  "this url, even if we got an error while trying."
			  "</td></tr>"

			  "<tr class=poo><td>isnew | !isnew</td>"
			  "<td>"
			  "This is the opposite of hasreply above. A url "
			  "is new if it has no spider reply, including "
			  "error replies. So once a url has been attempted to "
			  "be spidered then this will be false even if there "
			  "was any kind of error."
			  "</td></tr>"

			  "<tr class=poo><td>lastspidertime >= "
			  "<b>{roundstart}</b></td>"
			  "<td>"
			  "This is true if the url's last spidered time "
			  "indicates it was spidered already for this "
			  "current round of spidering. When no more urls "
			  "are available for spidering, then gigablast "
			  "automatically sets {roundstart} to the current "
			  "time so all the urls can be spidered again. This "
			  "is how you do round-based spidering. "
			  "You have to use the respider frequency as well "
			  "to adjust how often you want things respidered."
			  "</td></tr>"

			  "<tr class=poo><td>urlage</td>"
			  "<td>"
			  "This is the time, in seconds, since a url was first "
			  "added to spiderdb to be spidered. This is "
			  "its discovery date. "
			  "Can use <, >, <=, >=, ==, != comparison operators."
			  "</td></tr>"
			  

			  //"<tr class=poo><td>!newoutlink</td>"
			  //"<td>Matches if document is NOT a new outlink."
			  //"</td></tr>"

			  "<tr class=poo><td>age</td>"
			  "<td>"
			  "How old is the doucment <b>in seconds</b>. "
			  "The age is based on the publication date of "
			  "the document, which could also be the "
			  "time that the document was last significantly "
			  "modified. If this date is unknown then the age "
			  "will be -1 and only match the expression "
			  "<i>age==-1</i>. "
			  "When harvesting links, we guess the publication "
			  "date of the oulink by detecting dates contained "
			  "in the url itself, which is popular among some "
			  "forms of permalinks. This allows us to put "
			  "older permalinks into a slower spider queue."
			  "</td></tr>"

			  "<tr class=poo><td>spiderwaited &lt; 3600</td>"
			  "<td>"
			  "<i>spiderwaited</i> is how many seconds have elapsed "
			  "since the last time "
			  "we tried to spider/download the url. "
			  "The constaint containing <i>spiderwaited</i> will "
			  "fail to be matched if the url has never been "
			  "attempted to be spidered/downloaded before. Therefore, "
			  "it will only ever match urls that have a spider reply "
			  "of some sort, so there is no need to add an additional "
			  "<i>hasreply</i>-based constraint."
			  "</td></tr>"


			  "<tr class=poo><td>"
			  "<a name=insitelist>"
			  "insitelist | !insitelist"
			  "</a>"
			  "</td>"
			  "<td>"
			  "This is true if the url matches a pattern in "
			  "the list of sites on the <a href=/admin/sites>"
			  "site list</a> page. That site list is useful for "
			  "adding a large number of sites that can not be "
			  "accomodated by the url fitlers table. Plus "
			  "it is higher performance and easier to use, but "
			  "lacks the url filter table's "
			  "fine level of control."
			  "</td></tr>"

			  "<tr class=poo><td>"
			  "<a name=isaddurl>"
			  "isaddurl | !isaddurl"
			  "</a>"
			  "</td>"
			  "<td>"
			  "This is true if the url was added from the add "
			  "url interface or API."
			  //"This replaces the add url priority "
			  //"parm."
			  "</td></tr>"

			  "<tr class=poo><td>isinjected | !isinjected</td>"
			  "<td>"
			  "This is true if the url was directly "
			  "injected from the "
			  "<a href=/admin/inject>inject page</a> or API."
			  "</td></tr>"

			  "<tr class=poo><td>isreindex | !isreindex</td>"
			  "<td>"
			  "This is true if the url was added from the "
			  "<a href=/admin/reindex>query reindex</a> "
			  "interface. The request does not contain "
			  "a url, but only a docid, that way we can add "
			  "millions of search results very quickly without "
			  "having to lookup each of their urls. You should "
			  "definitely have this if you use the reindexing "
			  "feature. "
			  "You can set max spiders to 0 "
			  "for non "
			  "isreindex requests while you reindex or delete "
			  "the results of a query for extra speed."
			  "</td></tr>"

			  "<tr class=poo><td>ismanualadd | !ismanualadd</td>"
			  "<td>"
			  "This is true if the url was added manually. "
			  "Which means it matches isaddurl, isinjected, "
			  " or isreindex. as opposed to only "
			  "being discovered from the spider. "
			  "</td></tr>"

			  "<tr class=poo><td><nobr>inpingserver | !inpingserver"
			  "</nobr></td>"
			  "<td>"
			  "This is true if the url has an inlink from "
			  "a recognized ping server. Ping server urls are "
			  "hard-coded in Url.cpp. <b><font color=red> "
			  "pingserver urls are assigned a hop count of 0"
			  "</font></b>"
			  "</td></tr>"

			  "<tr class=poo><td>isparentrss | !isparentrss</td>"
			  "<td>"
			  "If a parent of the URL was an RSS page "
			  "then this will be matched."
			  "</td></tr>"

			  "<tr class=poo><td>isparentsitemap | "
			  "!isparentsitemap</td>"
			  "<td>"
			  "If a parent of the URL was a sitemap.xml page "
			  "then this will be matched."
			  "</td></tr>"

			  /*
			  "<tr class=poo><td>parentisnew | !parentisnew</td>"
			  "<td>"
			  "<b>Parent providing this outlink is not currently "
			  "in the index but is trying to be added right now. "
			  "</b>This is a special expression in that "
			  "it only applies to assigning spider priorities "
			  "to outlinks we are harvesting on a page.</b>" 
			  "</td></tr>"
			  */

			  "<tr class=poo><td>isindexed | !isindexed</td>"
			  "<td>"
			  "This url matches this if in the index already. "
			  "</td></tr>"

			  "<tr class=poo><td>errorcount==1</td>"
			  "<td>"
			  "The number of times the url has failed to "
			  "be indexed. 1 means just the last time, two means "
			  "the last two times. etc. Any kind of error parsing "
			  "the document (bad utf8, bad charset, etc.) "
			  "or any HTTP status error, like 404 or "
			  "505 is included in this count, in addition to "
			  "\"temporary\" errors like DNS timeouts."
			  "</td></tr>"

			  "<tr class=poo><td>errorcode==32880</td>"
			  "<td>"
			  "If the last time it was spidered it had this "
			  "numeric error code. See the error codes in "
			  "Errno.cpp. In this particular example 32880 is "
			  "for EBADURL."
			  "</td></tr>"

			  "<tr class=poo><td>hastmperror</td>"
			  "<td>"
			  "This is true if the last spider attempt resulted "
			  "in an error like EDNSTIMEDOUT or a similar error, "
			  "usually indicative of a temporary internet "
			  "failure, or local resource failure, like out of "
			  "memory, and should be retried soon. "
			  "Currently: "
			  "dns timed out, "
			  "tcp timed out, "
			  "dns dead, "
			  "network unreachable, "
			  "host unreachable, "
			  "diffbot internal error, "
			  "out of memory."
			  "</td></tr>"

			  "<tr class=poo><td>percentchangedperday&lt=5</td>"
			  "<td>"
			  "Looks at how much a url's page content has changed "
			  "between the last two times it was spidered, and "
			  "divides that percentage by the number of days. "
			  "So if a URL's last two downloads were 10 days "
			  "apart and its page content changed 30%% then "
			  "the <i>percentchangedperday</i> will be 3. "
			  "Can use <, >, <=, >=, ==, != comparison operators. "
			  "</td></tr>"

			  "<tr class=poo><td>sitenuminlinks&gt;20</td>"
			  "<td>"
			  "How many inlinks does the URL's site have? "
			  "We only count non-spammy inlinks, and at most only "
			  "one inlink per IP address C-Class is counted "
			  "so that a webmaster who owns an entire C-Class "
			  "of IP addresses will only have his inlinks counted "
			  "once."
			  "Can use <, >, <=, >=, ==, != comparison operators. "
			  "</td></tr>"


			  "<tr class=poo><td>numinlinks&gt;20</td>"
			  "<td>"
			  "How many inlinks does the URL itself have? "
			  "We only count one link per unique C-Class IP "
			  "address "
			  "so that a webmaster who owns an entire C-Class "
			  "of IP addresses will only have her inlinks counted "
			  "once."
			  "Can use <, >, <=, >=, ==, != comparison operators. "
			  "This is useful for spidering popular URLs quickly."
			  "</td></tr>"


			  "<tr class=poo><td>httpstatus==404</td>"
			  "<td>"
			  "For matching the URL based on the http status "
			  "of its last download. Does not apply to URLs "
			  "that have not yet been successfully downloaded."
			  "Can use <, >, <=, >=, ==, != comparison operators. "
			  "</td></tr>"

			  /*
			  "<tr class=poo><td>priority==30</td>"
			  "<td>"
			  "<b>If the current priority of the url is 30, then "
			  "it will match this expression. Does not apply "
			  "to outlinks, of course."
			  "</td></tr>"

			  "<tr class=poo><td>parentpriority==30</td>"
			  "<td>"
			  "<b>This is a special expression in that "
			  "it only applies to assigning spider priorities "
			  "to outlinks we are harvesting on a page.</b> "
			  "Matches if the url being added to spider queue "
			  "is from a parent url in priority queue 30. "
			  "The parent's priority queue is the one it got "
			  "moved into while being spidered. So if it was "
			  "in priority 20, but ended up in 25, then 25 will "
			  "be used when scanning the URL Filters table for "
			  "each of its outlinks. Only applies "
			  "to the FIRST time the url is added to spiderdb. "
			  "Use <i>parentpriority==-3</i> to indicate the "
			  "parent was FILTERED and <i>-2</i> to indicate "
			  "the parent was BANNED. A parentpriority of "
			  "<i>-1</i>"
			  " means that the urls is not a link being added to "
			  "spiderdb but rather a url being spidered."
			  "</td></tr>"

			  "<tr class=poo><td>inlink==...</td>"
			  "<td>"
			  "If the url has an inlinker which contains the "
			  "given substring, then this rule is matched. "
			  "We use this like <i>inlink=www.weblogs.com/"
			  "shortChanges.xml</i> to detect if a page is in "
			  "the ping server or not, and if it is, then we "
			  "assign it to a slower-spidering queue, because "
			  "we can reply on the ping server for updates. Saves "
			  "us from having to spider all the blogspot.com "
			  "subdomains a couple times a day each."
			  "</td></tr>"
			  */

			  //"NOTE: Until we get the link info to get the doc "
			  //"quality before calling msg8 in Msg16.cpp, we "
			  //"can not involve doc:quality for purposes of "
			  //"assigning a ruleset, unless banning it.</td>"

			  "<tr class=poo><td><nobr>tld!=com,org,edu"// && "
			  //"doc:quality&lt;70"
			  "</nobr></td>"
			  "<td>Matches if the "
			  "url's TLD does NOT end in \"com\", \"org\" or "
			  "\"edu\". "
			  "</td></tr>"

			  "<tr class=poo><td><nobr>lang==zh_cn,de"
			  "</nobr></td>"
			  "<td>Matches if "
			  "the url's content is in the language \"zh_cn\" or "
			  "\"de\". See table below for supported language "
			  "abbreviations. Used to only keep certain languages "
			  "in the index. This is hacky because the language "
			  "may not be known at spider time, so Gigablast "
			  "will check after downloading the document to "
			  "see if the language <i>spider priority</i> is "
			  "DELETE thereby discarding it.</td></tr>"
			  //"NOTE: Until we move the language "
			  //"detection up before any call to XmlDoc::set1() "
			  //"in Msg16.cpp, we can not use for purposes of "
			  //"assigning a ruleset, unless banning it.</td>"
			  //"</tr>"

			  "<tr class=poo><td><nobr>lang!=xx,en,de"
			  "</nobr></td>"
			  "<td>Matches if "
			  "the url's content is NOT in the language \"xx\" "
			  "(unknown), \"en\" or \"de\". "
			  "See table below for supported language "
			  "abbreviations.</td></tr>"

			  "<tr class=poo><td><nobr>parentlang==zh_cn,zh_tw,xx"
			  "</nobr></td>"
			  "<td>Matches if "
			  "the url's referring parent url is primarily in "
			  "this language. Useful for prioritizing spidering "
			  "pages of a certain language."
			  "See table below for supported language "
			  "abbreviations."
			  "</td></tr>"

			  /*
			  "<tr class=poo><td>link:gigablast</td>"
			  "<td>Matches if the document links to gigablast."
			  "</td></tr>"

			  "<tr class=poo><td>searchbox:gigablast</td>"
			  "<td>Matches if the document has a submit form "
			  "to gigablast."
			  "</td></tr>"

			  "<tr class=poo><td>site:dmoz</td>"
			  "<td>Matches if the document is directly or "
			  "indirectly in the DMOZ directory."
			  "</td></tr>"

			  "<tr class=poo><td>tag:spam>X</td>"
			  "<td>Matches if the document's tagdb record "
			  "has a score greater than X for the sitetype, "
			  "'spam' in this case. "
			  "Can use <, >, <=, >=, ==, != comparison operators. "
			  "Other sitetypes include: "
			  "..."
			  "</td></tr>"
			  */

			  "<tr class=poo><td>iswww | !iswww</td>"
			  "<td>Matches if the url's hostname is www or domain "
			  "only. For example: <i>www.xyz.com</i> would match, "
			  "and so would <i>abc.com</i>, but "
			  "<i>foo.somesite.com</i> would NOT match."
			  "</td></tr>"


			  "<tr class=poo><td>isroot | !isroot</td>"
			  "<td>Matches if the URL is a root URL. Like if "
			  "its path is just '/'. Example: http://www.abc.com "
			  "is a root ur but http://www.abc.com/foo is not. "
			  "</td></tr>"


			  "<tr class=poo><td>isonsamedomain | !isonsamedomain</td>"
			  "<td>"
			  "This is true if the url is from the same "
			  "DOMAIN as the page from which it was "
			  "harvested."
			  //"Only effective for links being added from a page "
			  //"being spidered, because this information is "
			  //"not preserved in the titleRec."
			  "</td></tr>"


			  "<tr class=poo><td><nobr>"
			  "isonsamesubdomain | !isonsamesubdomain"
			  "</nobr></td>"
			  "<td>"
			  "This is true if the url is from the same "
			  "SUBDOMAIN as the page from which it was "
			  "harvested."
			  //"Only effective for links being added from a page "
			  //"being spidered, because this information is "
			  //"not preserved in the titleRec."
			  "</td></tr>"

			  "<tr class=poo><td>ismedia | !ismedia</td>"
			  "<td>"
			  "Does the url have a media or css related "
			  "extension. Like gif, jpg, mpeg, css, etc.? "
			  "</td></tr>"


			  "<tr class=poo><td>tag:<i>tagname</i></td>"
			  "<td>"
			  "This is true if the url is tagged with this "
			  "<i>tagname</i> in the site list. Read about tags "
			  "on the <a href=/admin/settings>"//#examples>"
			  "site list</a> "
			  "page."
			  "</td></tr>"



			  "</td></tr></table><br><br>\n",
			  TABLE_STYLE );


		// show the languages you can use
		sb->safePrintf (
			  "<table %s>"
			  "<tr><td colspan=2><center>"
			  "<b>"
			  "Supported Language Abbreviations "
			  "for lang== Filter</b>"
			  "</td></tr>",
			  TABLE_STYLE );
		for ( int32_t i = 0 ; i < 256 ; i++ ) {
			char *lang1 = getLanguageAbbr   ( i );
			char *lang2 = getLanguageString ( i );
			if ( ! lang1 ) continue;
			sb->safePrintf("<tr class=poo>"
				       "<td>%s</td><td>%s</td></tr>\n",
				      lang1,lang2);
		}
		// wrap it up
		sb->safePrintf("</table><br><br>");
		return true;
}

// . copy/clone parms from one collrec to another
// . returns false and sets g_errno on error
// . if doing this after creating a new collection on host #0 we have to call
//   syncParmsWithHost0() to get all the shards in sync.
bool Parms::cloneCollRec ( char *dstCR , char *srcCR ) {

	// now set THIS based on the parameters in the xml file
	for ( int32_t i = 0 ; i < m_numParms ; i++ ) {

		// get it
		Parm *m = &m_parms[i];
		if ( m->m_obj != OBJ_COLL ) continue;

		//log(LOG_DEBUG, "Parms: %s: parm: %s", filename, m->m_xml);
		// . there are 2 object types, coll recs and g_conf, aka
		//   OBJ_COLL and OBJ_CONF.

		// skip comments and command
		if ( !(m->m_flags & PF_CLONE) ) continue;

		// get parm data ptr
		char *src = srcCR + m->m_off;
		char *dst = dstCR + m->m_off;

		// if not an array use this
		if ( ! m->isArray() ) {
			if ( m->m_type == TYPE_SAFEBUF ) {
				SafeBuf *a = (SafeBuf *)src;
				SafeBuf *b = (SafeBuf *)dst;
				b->reset();
				b->safeMemcpy ( a );
				b->nullTerm();
			}
			else {
				// this should work for most types
				gbmemcpy ( dst , src , m->m_size );
			}
			continue;
		}

		//
		// arrays only below here
		//

		// for arrays only
		int32_t *srcNum = (int32_t *)(src-4);
		int32_t *dstNum = (int32_t *)(dst-4);

		// array can have multiple values
		for ( int32_t j = 0 ; j < *srcNum ; j++ ) {

			if ( m->m_type == TYPE_SAFEBUF ) {
				SafeBuf *a = (SafeBuf *)src;
				SafeBuf *b = (SafeBuf *)dst;
				b->reset();
				b->safeMemcpy ( a );
				b->nullTerm();
			}
			else {
				// this should work for most types
				gbmemcpy ( dst , src , m->m_size );
			}

			src += m->m_size;
			dst += m->m_size;

		}

		// update # elements in array
		*dstNum = *srcNum;

	}
	return true;
}
