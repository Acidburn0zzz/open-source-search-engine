#include "PingServer.h"
#include "UdpServer.h"
#include "UdpSlot.h"
#include "Conf.h"
#include "HttpServer.h"
#include "Proxy.h"
#include "Repair.h" 
#include "Process.h"
#include "DailyMerge.h"
#include "SpiderLoop.h"
#include "Collectiondb.h"
#include "Rebalance.h"
#include "Stats.h"
#include "ip.h"
#include "Mem.h"
#include "Version.h"

#define PAGER_BUF_SIZE (10*1024)

// from main.cpp. when keepalive script restarts us this is true
bool g_recoveryMode = false;
int32_t g_recoveryLevel = 0;

// a global class extern'd in .h file
PingServer g_pingServer;

static void sleepWrapper ( int fd , void *state ) ;

static void updatePingTime ( Host *h , int32_t *pingPtr , int32_t tripTime ) ;


bool PingServer::registerHandler ( ) {
	// . we'll handle msgTypes of 0x11 for pings
	// . register ourselves with the udp server
	// . it calls our callback when it receives a msg of type 0x0A
	if ( ! g_udpServer.registerHandler ( msg_type_11, handleRequest11 )) {
		return false;
	}

	// save this
	m_pingSpacer = g_conf.m_pingSpacer;

	m_sleepCallbackRegistrationSequencer = 0;

	// . this was 500ms but now when a host shuts downs it sends all other
	//   hosts a msg saying so... PingServer::broadcastShutdownNotes() ...
	// . so i put it up to 2000ms to save bandwidth some
	// . but these packets are small and cheap so now let's send out 10
	//   every second to keep on top of things
	// . so in a network of 128 hosts we'll cycle through in 12.8 seconds
	//   and set a host to dead on avg will take about 13 seconds
	if ( ! g_loop.registerSleepCallback ( g_conf.m_pingSpacer , (void *)(PTRTYPE)m_sleepCallbackRegistrationSequencer, sleepWrapper , 0 ) ) {
		return false;
	}

	// we have disabled syncing for now
	return true;
}

static int32_t s_outstandingPings = 0;

// . gets filename that contains the hosts from the Conf file
// . return false on errro
// . g_errno may NOT be set
bool PingServer::init ( ) {
	m_i             =  0;
	m_numRequests2  =  0;
	m_numReplies2   =  0;
	m_minRepairMode             = -1;
	m_maxRepairMode             = -1;
	m_minRepairModeBesides0     = -1;
	m_minRepairModeHost         = NULL;
	m_maxRepairModeHost         = NULL;
	m_minRepairModeBesides0Host = NULL;

	m_hostsConfInDisagreement = false;
	m_hostsConfInAgreement = false;

	// we're done
	return true;
}

void PingServer::sendPingsToAll ( ) {
	// if we are the query/spider compr. proxy then do not send out pings
	if ( g_hostdb.m_myHost->m_type & HT_QCPROXY ) return;
	if ( g_hostdb.m_myHost->m_type & HT_SCPROXY ) return;

	if(g_listNumTotal<=0)
		return; //uhm, are we being called before hostdb has been initialized?
	
	for(int loop_iterations=0; loop_iterations<g_listNumTotal  && s_outstandingPings < g_conf.m_maxOutstandingPings; loop_iterations++) {
		// get the next host in line
		Host     *h    = g_listHosts [ m_i ];
		uint32_t  ip   = g_listIps   [ m_i ];
		uint16_t  port = g_listPorts [ m_i ];
		// point to next ip/port, and wrap if we should
		m_i = (m_i+1)%g_listNumTotal;
		
		// skip if no udp port. might be http, dns or https.
		if ( port != h->m_port ) continue;
		// if he is in progress, skip as well. check for shotgun ip.
		if ( ip == h->m_ip && h->m_inProgress1 ) continue;
		if ( ip != h->m_ip && h->m_inProgress2 ) continue;
		// do not ping query compression proxies or spider comp proxies
		if ( h->m_type & HT_QCPROXY ) continue;
		if ( h->m_type & HT_SCPROXY ) continue;
		// try to launch the request
		pingHost ( h , ip , port ) ;
	}

	
	// . check if pingSpacer was updated via master controls and fix our
	//   sleep wrapper callback interval if so
	// update pingSpacer callback tick if it changed since last time
	if ( m_pingSpacer == g_conf.m_pingSpacer ) return;
	// register new one
	if ( ! g_loop.registerSleepCallback ( m_pingSpacer , (void *)(PTRTYPE)(m_sleepCallbackRegistrationSequencer+1) , sleepWrapper , 0 ) ) {
		static char logged = 0;
		if ( ! logged )
			log("net: Could not update ping spacer.");
		logged = 1;
		return;
	}
	// now it is safe to unregister last callback then
	g_loop.unregisterSleepCallback((void *)(PTRTYPE)m_sleepCallbackRegistrationSequencer, sleepWrapper);

	m_sleepCallbackRegistrationSequencer++;
}


void PingServer::pingHost ( Host *h , uint32_t ip , uint16_t port ) {

	int32_t hostId = h->m_hostId;

	// don't ping again if already in progress
	if ( ip == h->m_ip && h->m_inProgress1 ) return;
	if ( ip != h->m_ip && h->m_inProgress2 ) return;
	// time now
	int64_t nowmsLocal = gettimeofdayInMilliseconds();
	// stamp it
	h->m_lastPing = nowmsLocal;
	// count it
	s_outstandingPings++;
	// consider it in progress
	if ( ip == h->m_ip ) h->m_inProgress1 = true;
	else                 h->m_inProgress2 = true;
	// . if he is not dead, set this first send time
	// . this can be the first time we sent an unanswered ping
	// . this is only used to determine when to send emails since we
	//   can now send the email a configurable amount of seconds after the
	//   host as already been registered as dead. i hope this will fix the
	//   problem of the hitachis causing bus resets all the time. because
	//   we make the dead threshold only a few seconds to protect 
	//   performance, but make the email time much higher so i can sleep
	//   at night.
	// . in the beginning all hosts are considered dead, but the email code
	//   should not be 0, it should be -2
	// . BOTH eth ports must be dead for host to be dead
	bool isAlive = false;
	if ( h->m_ping        < g_conf.m_deadHostTimeout ) isAlive = true;
	if ( g_conf.m_useShotgun && 
	     h->m_pingShotgun < g_conf.m_deadHostTimeout ) isAlive = true;
	if ( isAlive && h->m_emailCode == 0 ) h->m_startTime =nowmsLocal;//now2
	Host *me = g_hostdb.m_myHost;
	// we send our stats to host "h"
	
	//first we update our pinginfo
	PingInfo newPingInfo;

	newPingInfo.m_unused9 = 0;
	newPingInfo.m_unused3 = 0;
	newPingInfo.m_unused11 = 0;
	newPingInfo.m_unused14 = 0;

	// let the receiver know our repair mode
	newPingInfo.m_repairMode = g_repairMode;

	newPingInfo.m_unused2 = 0;

	newPingInfo.m_unused3 = 0;

	// our num recs, docsIndexed
	newPingInfo.m_totalDocsIndexed = (int32_t)g_process.getTotalDocsIndexed();

	// and hosts.conf crc
	newPingInfo.m_hostsConfCRC = g_hostdb.getCRC();

	// ensure crc is legit
	if ( g_hostdb.getCRC() == 0 ) { g_process.shutdownAbort(true); }

	// disk usage (df -ka)
	newPingInfo.m_unused7 = 0.0;

	newPingInfo.m_unused18 = 0;

	newPingInfo.m_unused8 = 0;

	newPingInfo.m_unused15 = 0;

	newPingInfo.m_hostId = me->m_hostId;

	newPingInfo.m_unused0 = 0;

	newPingInfo.m_unused12 = 0;

	newPingInfo.m_unused13 = 0;

	newPingInfo.m_unused4 = 0.0;

	memset(newPingInfo.m_unused16, 0, sizeof(newPingInfo.m_unused16));

	g_hostdb.updatePingInfo(g_hostdb.m_myHost,newPingInfo);
	
	//our pinginfo has been updated. Now send to the target host

	// the proxy may be interfacing with the temporary cluster while
	// we update the main cluster...
	//int32_t port = h->m_port;
	if ( g_proxy.isProxyRunning() && g_conf.m_useTmpCluster ) {
		port++;
	}

    if ( h->m_isProxy ) {
	    hostId = -1;
    }

	if (g_udpServer.sendRequest((char *)&g_hostdb.m_myHost->m_pingInfo, sizeof(PingInfo), msg_type_11, ip, port, hostId, NULL, (void *)h, gotReplyWrapperP, g_conf.m_deadHostTimeout, 0, NULL, 1000, 2000)) {
		return;
	}
	// it had an error, so dec the count
	s_outstandingPings--;
	// consider it out of progress
	if ( ip == h->m_ip ) h->m_inProgress1 = false;
	else                 h->m_inProgress2 = false;
	// had an error
	log("net: Pinging host #%" PRId32" had error: %s.",
	    h->m_hostId,mstrerror(g_errno) );
	// reset it cuz it's not a showstopper
	g_errno = 0;
}

// . this is the last hostId we sent an alert about
// . it is -1 if he has come back up since
// . it is -1 if we've sent no alerts
// . it is so we only send out one email even though a bunch of consecutive
//   hosts right before us (in hostid ranking) may have gone down. The admin
//   only needs one notification.
static int32_t s_lastSentHostId = -1;

void PingServer::gotReplyWrapperP(void *state, UdpSlot *slot) {
	// state is the host
	Host *h = (Host *)state;
	if( !h ) {
		log(LOG_LOGIC,"net: pingserver: gotReplyWrapperP: state is NULL (no host)!");
		return;
	}
	
	// un-count it
	s_outstandingPings--;
	// don't let udp server free our send buf, we own it
	slot->m_sendBufAlloc = NULL;
	// update ping time
	int64_t nowms    = gettimeofdayInMilliseconds();
	int64_t tripTime = nowms - slot->getFirstSendTime() ;
	// ensure not negative, clock might have been adjusted!
	if ( tripTime < 0 ) tripTime = 0;
	// point to the right ping time, for the original port or for the 
	// shotgun port
	int32_t *pingPtr = NULL;
	if ( slot->getIp() == h->m_ipShotgun ) pingPtr = &h->m_pingShotgun;
	// original overrides shotgun, in case ips match
	if ( slot->getIp() == h->m_ip        ) pingPtr = &h->m_ping;
	// otherwise... wierd!!
	if ( ! pingPtr ) pingPtr = &h->m_ping;
	if ( g_errno == EUDPTIMEDOUT ) tripTime = g_conf.m_deadHostTimeout;
	updatePingTime ( h , pingPtr , tripTime );
	// sanity checks
	if ( slot->getIp()==h->m_ip && !h->m_inProgress1) {g_process.shutdownAbort(true);}
	if ( slot->getIp()!=h->m_ip && !h->m_inProgress2) {g_process.shutdownAbort(true);}
	// consider it out of progress
	if ( slot->getIp() == h->m_ip ) h->m_inProgress1 = false;
	else                         h->m_inProgress2 = false;
	// count all replies
	if ( ! g_errno ) h->m_numPingReplies++;

	// clear g_errno so we don't think any functions below set it
	g_errno = 0;

	// is the whole host alive? if either port is up it is still alive
	bool isAlive = false;
	if ( g_conf.m_useShotgun && 
	     h->m_pingShotgun < g_conf.m_deadHostTimeout ) isAlive = true;
	if ( h->m_ping < g_conf.m_deadHostTimeout        ) isAlive = true;
		
	if ( isAlive ) {
		// if host is not dead clear his sentEmail bit so if he goes
		// down again we may be obliged to send another email, provided
		// we are the hostid right after him
		// allow him to receive emails again
		h->m_emailCode = 0;
		// if he was the subject of the last alert we emailed and now
		// he is back up then we are free to send another alert about
		// any other host that goes down
		if ( h->m_hostId == s_lastSentHostId ) s_lastSentHostId = -1;
	} else {
		// . if his ping was dead, try to send an email alert to the admin
		// . returns false if blocked, true otherwise
		// . sets g_errno on erro
		// . send it iff both ports (if using shotgun) are dead
		if ( nowms - h->m_startTime >= g_conf.m_sendEmailTimeout)
			g_pingServer.sendEmail ( h ) ;
	}

	// reset in case sendEmail() set it
	g_errno = 0;
}


// this may be called from a signal handler now...
void PingServer::handleRequest11(UdpSlot *slot , int32_t /*niceness*/) {
	// get request 
	int32_t  requestSize = slot->m_readBufSize;
	char *request     = slot->m_readBuf;
	// get the ip/port of requester
	uint32_t ip    = slot->getIp();
	uint16_t port = slot->getPort();
	// get the host entry
	Host *h = g_hostdb.getUdpHost ( ip , port );
	// we may be the temporary cluster (grep for useTmpCluster) and
	// the proxy is sending pings from its original port plus 1
	if ( ! h ) {
		h = g_hostdb.getUdpHost ( ip , port + 1 );
	}
	if ( ! h ) {
		// size of 3 means it is a debug ping from
		// proxy server is a connectIp, so don't display the message
		// ./gb ./hosts.conf <hid> --udp
		if ( requestSize != 3 && ! g_conf.isConnectIp(ip) )
			log(LOG_LOGIC,"net: pingserver: No host for "
			    "dstip=%s port=%hu tid=%" PRId32" fromhostid=%" PRId32,
			    iptoa(ip),port,slot->getTransId(),slot->getHostId());
		// set "useSameSwitch" to true so even if shotgunning is on
		// the udp server will send the reply back to the same ip/port
		// from which we got the request
		g_udpServer.sendReply(NULL, 0, NULL, 0, slot, NULL, NULL, 500, 1000, true);
		return;
	}


	// point to the correct ping time. this request may have come from
	// the shotgun network, or the primary network.
	int32_t *pingPtr = NULL;
	if ( slot->getIp() == h->m_ipShotgun ) {
		pingPtr = &h->m_pingShotgun;
	}

	// original overrides shotgun, in case ips match
	if ( slot->getIp() == h->m_ip) {
		pingPtr = &h->m_ping;
	}

	// otherwise... wierd!!
	if ( ! pingPtr ) {
		pingPtr = &h->m_ping;
	}

	// reply msg
	char *reply     = NULL;
	int32_t  replySize = 0;

	if(requestSize == sizeof(PingInfo)) {
		// sanity
		const PingInfo *pi2 = (PingInfo *)request;
		if ( pi2->m_hostId != h->m_hostId ) { 
			g_process.shutdownAbort(true); 
		}

		if( h != g_hostdb.getMyHost() ) {
			// only copy statistics if it is not from ourselves
			h->m_pingInfo = *pi2;
		}

		// we finally got a ping reply from him
		h->m_gotPingReply = true;

		// . what repair mode is the requester in?
		// . 0 means not in repair mode
		// . 1 means in repair mode waiting for a local spider or 
		//   titledb merge to stop
		// . 2 means in repair mode and local spiders and local titledb
		//   merging have stopped and are will not start up again
		// . 3 means all hosts have entered mode 2 so we can start
		//   repairing
		// . 4 means done repairing and ready to exit repair mode,
		//   but waiting for other hosts to be mode 4 or 0 before
		//   it can exit, too, and go back to mode 0
		char mode = h->m_pingInfo.m_repairMode;

		// if his mode is 2 he is ready to start repairing
		// because he has stopped all spiders and titledb
		// merges from happening. if he just entered mode
		// 2 now, check to see if all hosts are in mode 2 now.
		char oldMode = h->m_repairMode;
		// update his repair mode
		h->m_repairMode = mode;
		// . get the MIN repair mode of all hosts
		// . expensive, so only do if a host changes modes
		if ( oldMode != mode || g_pingServer.m_minRepairMode == -1 ) {
			g_pingServer.setMinRepairMode ( h );
		}
		// make it a normal ping now
		requestSize = 8;
	}

	PingServer *ps = &g_pingServer;
	ps->m_hostsConfInDisagreement = false;
	ps->m_hostsConfInAgreement = false;


	//
	// do all hosts have the same hosts.conf????
	//
	// if some hosts are dead then we will not set either flag.
	//
	// scan all grunts for agreement. do this line once per sec?
	//
	int32_t agree = 0;
	int32_t i; 
	for ( i = 0 ; i < g_hostdb.getNumGrunts() ; i++ ) {
		Host *h2 = &g_hostdb.m_hosts[i];			

		// skip if not received yet
		if ( ! h2->isHostsConfCRCKnown() ) {
			continue;
		}

		// badness?
		if ( !h2->hasSameHostsConfCRC() ) {
			ps->m_hostsConfInDisagreement = true;
			break;
		}
		// count towards agreement
		agree++;
	}

	// if all in agreement, set this flag
	if ( agree == g_hostdb.getNumGrunts() ) {
		ps->m_hostsConfInAgreement = true;
	}

	// if request is 5 bytes, must be a host telling us he's shutting down
	if ( requestSize == 5 ) {
		// his byte #4 is sendEmailAlert
		bool sendEmailAlert = request[4];
		// and make him dead
		int32_t deadTime = g_conf.m_deadHostTimeout + 1;
		updatePingTime ( h , &h->m_ping        , deadTime );
		updatePingTime ( h , &h->m_pingShotgun , deadTime );

		// don't email admin if sendEmailAlert is false
		if ( ! sendEmailAlert ) {
			h->m_emailCode = -2;
		} else {
			// otherwise, launch him one now
			// . this returns false if blocked, true otherw
			// . sets g_errno on erro
			g_pingServer.sendEmail ( h );
			// reset in case sendEmail() set it
			g_errno = 0;
		}
		// if we are a proxy and he had an outstanding 0xfd msg
		// then we need to re-route that! likewise any msg
		// that has not got a full reply needs to be timedout
		// right now so we can re-route it...
		g_udpServer.timeoutDeadHosts ( h );

	}
	// all pings now deliver a timestamp of the sending host
	else 
	if ( requestSize == 8 ) {
		reply     = NULL;
		replySize = 0;
	}
	// otherwise, unknown request size
	else {
		log(LOG_LOGIC,"net: pingserver: Unknown request size of "
		    "%" PRId32" bytes. You are probably running a different gb "
		    "version on this host. check the hosts table for "
		    "version info.", requestSize);
	}
	// always send back an empty reply
	g_udpServer.sendReply(reply, replySize, NULL, 0, slot, NULL, NULL, 500, 1000, true);
}



// . sets m_minRepairMode
// . only call this when host "h" changes repair mode
void PingServer::setMinRepairMode(const Host *hhh) {
	// . this host is the holder of the min now, return if not a match
	// . do not return if m_minRepairMode has not been set at all though
	bool returnNow = true;
	if ( m_minRepairModeHost         == hhh ) returnNow = false;
	if ( m_maxRepairModeHost         == hhh ) returnNow = false;
	if ( m_minRepairModeBesides0Host == hhh ) returnNow = false;
	if ( m_minRepairMode             ==  -1 ) returnNow = false;
	if ( m_maxRepairMode             ==  -1 ) returnNow = false;
	if ( m_minRepairModeBesides0     ==  -1 ) returnNow = false;
	if ( returnNow ) return;
	// count the mins
	int32_t  min   = -1;
	int32_t  max   = -1;
	int32_t  min0  = -1;
	const Host *minh  = NULL;
	const Host *maxh  = NULL;
	const Host *minh0 = NULL;
	// scan to find new min
	for ( int32_t i = 0 ; i < g_hostdb.getNumHosts() ; i++ ) {
		// count if not dead
		Host *h = &g_hostdb.m_hosts[i];
		// . if it is us, the repairMode is not updated because we
		//   do not ping ourselves.
		// . we check ourselves in the the getMinRepairMode() and
		//   getNumHostsInRepairMode7() functions defined in 
		//   PingServer.h
		if ( h == g_hostdb.m_myHost ) continue;
		// get repair mode
		int32_t repairMode = h->m_repairMode;
		// is it a min?
		if ( repairMode < min || min == -1 ) {
			// we got a new minner
			min  = repairMode;
			minh = h;
		}
		// is it a max?
		if ( repairMode > max || max == -1 ) {
			// we got a new minner
			max  = repairMode;
			maxh = h;
		}
		// . min0 is the lowest repair mode that is not 0
		// . if they are all 0, then it will be 0
		if ( repairMode == 0 ) continue;
		if ( repairMode < min0 || min0 == -1 ) {
			min0  = repairMode;
			minh0 = h;
		}
	}
	// set these guys to the min
	m_minRepairMode         = min;
	m_minRepairModeHost     = minh;
	// and these to max
	m_maxRepairMode         = max;
	m_maxRepairModeHost     = maxh;
	// if they are all 0 then this will be 0
	m_minRepairModeBesides0     = min0;
	m_minRepairModeBesides0Host = minh0;
}

//
// This code is used to cyclically ping all hosts in the network
//

// this wrapper is called once 100ms, or so...
static void sleepWrapper ( int fd , void *state ) {
	// . in fact, launch as many pings as we can right now
	g_pingServer.sendPingsToAll();//pingNextHost ( );
}

//////////////////////////////////////////////////////
// email sending code
//////////////////////////////////////////////////////

// we can send emails when a host is detected as dead

// . returns false if blocked, true otherwise
// . sets g_errno on error
bool PingServer::sendEmail ( Host *h            , 
			     const char *errmsg,
			     bool  oom          ,
			     bool  forceIt) {
	// clear this
	g_errno = 0;
	// not if we have outstanding requests
	if ( m_numReplies2 < m_numRequests2 ) {
		log("net: Email not sent since there are %" PRId32" outstanding "
		    "replies.",m_numReplies2 - m_numRequests2);
		return true;
	}
	// throttle the oom sends
	if ( oom ) {
		static int32_t s_lastOOMTime = 0;
		int32_t now = getTimeLocal();
		// space 15 minutes apart
		if ( now - s_lastOOMTime < 15*60 ) return true;
		// set time
		s_lastOOMTime = now;

		// always force these now because they are messing up our latency graph
		forceIt = true;
	}
	// . even if we don't send an email, log it
	// . return if alerts disabled
	if ( ! g_conf.m_sendEmailAlerts && ! forceIt ) {
		// . only log if this is his first time as being detected dead
		// . this is useful cuz it might hint at a down link
		if ( h != NULL && h->m_emailCode == 0 ) {
			h->m_emailCode = 1;
			//log("net: Host #%" PRId32" is dead. Has not responded to "
			//    "ping in %" PRId32" ms.",h->m_hostId,
			//    (int32_t)g_conf.m_deadHostTimeout);
		}
		return true;
	}
	// bitch and bail if h is NULL and this is a dead host msg
	if ( ! h && errmsg == NULL) {
		g_errno = EBADENGINEER;
		log(LOG_LOGIC,"net: pingserver: Host ptr is NULL 2.");
		return true;
	}
	// if he's not open for alerts,return (see PingServer.h) for alert defn
	if (  h && h->m_emailCode != 0 ) return true;

	// don't send another email until the last us we alerted revives
	if ( h && s_lastSentHostId >= 0 ) {
		// in fact, even after the lastSentHostId host comes back
		// up, don't send to these guys who were dead before it came
		// back up
		h->m_emailCode = -5;
		return true;
	}

	char msgbuf[2048];
	if( h ) { //as a special case construct the error message here if 
		//  we have a host.
		// . are we the designated host to send the email alert?
		// . our hostid must be the next alive hostId
		int32_t dhid = h->m_hostId;
		Host *dh = g_hostdb.getHost ( dhid );
		Host *origdh = dh;
		//while ( dh && dh->m_ping >= g_conf.m_deadHostTimeout ) {
		int32_t totalCount = 0;
		while ( dh && ( g_hostdb.isDead ( dh ) || dh == origdh ) ) {
			if ( ++dhid >= g_hostdb.getNumHosts() ) dhid = 0;
			dh = g_hostdb.getHost ( dhid );
			if ( totalCount++ >= g_hostdb.getNumHosts() ) break;
		}
		// . if we're not the next alive host in line to send, bail
		// . if next-in-line crashes before he sends then there will be
		//   a cascade affect and we could end up sending a BUNCH of 
		//   emails so prevent that now by setting his m_emailCode
		if ( dhid != g_hostdb.m_hostId ) { 
			h->m_emailCode = -3; 
			return true; 
		}

		// mark him as in progress
		h->m_emailCode = -4;
		// a host or proxy?
		const char *nm = "Host";
		if ( h->m_isProxy ) nm = "Proxy";
		// note it in the log
		if ( oom ) 
			log("net: %s %s #%" PRId32" is out of mem for %" PRId32" ms. "
			    "Sending email alert.",h->m_hostname,nm,
			    h->m_hostId,
			    (int32_t)g_conf.m_sendEmailTimeout);
		else
			log("net: %s %s #%" PRId32" is dead. Has not responded to "
			    "ping in %" PRId32" ms. Sending email alert.",
			    h->m_hostname,nm,h->m_hostId,
			    (int32_t)g_conf.m_sendEmailTimeout);
		// . make the msg
		// . put host0 ip in ()'s so we know what network it was
		Host *h0 = g_hostdb.getHost ( 0 );
		int32_t ip0 = 0;
		if ( h0 ) ip0 = h0->m_ip;
		const char *desc = "dead";
		if ( oom ) desc = "out of memory";
		sprintf ( msgbuf , "%s %s %" PRId32" is %s. cluster=%s (%s)",
			  h->m_hostname,nm,
			  h->m_hostId, desc, g_conf.m_clusterName,iptoa(ip0));
		errmsg = msgbuf;
	} 

	// . returns false if blocked, true otherwise
	// . sets g_errno on error
	bool status = true;
	m_numRequests2 = 0;
	m_numReplies2  = 0;

	// set the max for sanity checking in gotdoc
	m_maxRequests2 = m_numRequests2;

	bool delay = g_conf.m_delayNonCriticalEmailAlerts;
	// oom is always critical
	if ( oom ) delay = false;

	// if delay non critical email alerts is true do not send email 
	// alerts about dead hosts to anyone except sysadmin@example.com
	// between 10:00pm and 9:30am unless all the other twins of the 
	// dead host are also dead. Instead, wait till after 9:30 am if 
	// the host is still dead.
	if ( delay && h ) {

		//check if the hosts twins are dead too
		int32_t numTwins = 0;
		Host *hosts = g_hostdb.getShard( h->m_shardNum,
						 &numTwins );
		int32_t i = 0;
		while ( i < numTwins ){
			if ( !g_hostdb.isDead ( hosts[i].m_hostId ) )
				break;
			i++;
		}

		//if no twin is alive, emergency ! send email !
		//if even one twin is alive, don't send now
		if ( i == numTwins ) goto skipSleep;

		return true;
	}

 skipSleep:

	bool e1 = g_conf.m_sendEmailAlertsToEmail1;
	bool e2 = g_conf.m_sendEmailAlertsToEmail2;
	bool e3 = g_conf.m_sendEmailAlertsToEmail3;
	bool e4 = g_conf.m_sendEmailAlertsToEmail4;

	if ( e1 ) {
		m_numRequests2++;
		m_maxRequests2++;
		char *mxHost = g_conf.m_email1MX;
		if ( ! sendAdminEmail ( h,
					g_conf.m_email1From,
					g_conf.m_email1Addr,
					errmsg,
					mxHost ) ) // g_conf.m_email1MX) )
			status = false;
	}
	if ( e2 ) {
		m_numRequests2++;
		m_maxRequests2++;
		char *mxHost = g_conf.m_email2MX;
		if ( ! sendAdminEmail ( h,
					g_conf.m_email2From,
					g_conf.m_email2Addr,
					errmsg,
					mxHost ) ) // g_conf.m_email2MX) )
			status = false;
	}
	if ( e3 ) {
		m_numRequests2++;
		m_maxRequests2++;
		char *mxHost = g_conf.m_email3MX;
		if ( ! sendAdminEmail ( h,
					g_conf.m_email3From,
					g_conf.m_email3Addr,
					errmsg,
					mxHost ) ) // g_conf.m_email3MX) )
			status = false;
	}
	if ( e4 ) {
		m_numRequests2++;
		m_maxRequests2++;
		char *mxHost = g_conf.m_email4MX;
		if ( ! sendAdminEmail ( h,
					g_conf.m_email4From,
					g_conf.m_email4Addr,
					errmsg,
					mxHost ) ) // g_conf.m_email4MX) )
			status = false;
	}

	// set the max for sanity checking below
	//m_maxRequests2 = m_numRequests2;
	// did we block or not? return true if nobody blocked
	return status;
}


bool PingServer::sendAdminEmail(Host  *h,
				const char  *fromAddress,
				const char  *toAddress,
				const char  *body,
				const char  *emailServIp) {
	char hostname[ 256];
	gethostname(hostname,sizeof(hostname));
	// create a new buffer
	char *buf = (char *) mmalloc ( PAGER_BUF_SIZE , "PingServer" );
	// fill the buffer
	char *p = buf;
	// helo line
	p += sprintf(p, "HELO %s\r\n",hostname);
	// mail line
	p += sprintf(p, "MAIL from:<%s>\r\n", fromAddress);
	// to line
	p += sprintf(p, "RCPT to:<%s>\r\n", toAddress);
	// data
	p += sprintf(p, "DATA\r\n");
	// body
	p += sprintf(p, "To: %s\r\n", toAddress);
	p += sprintf(p, "Subject: Sysadmin Event Message\r\n");
	// mime header must be separated from body by an extra \r\n
	p += sprintf(p, "\r\n");
	p += sprintf(p, "%s", body);
	// quit
	p += sprintf(p, "\r\n.\r\nQUIT\r\n");
	// get the length
	int32_t buffLen = (p - buf);
	// send the message
	TcpServer *ts = g_httpServer.getTcp();
	log ( LOG_WARN, "PingServer: Sending email to sysadmin:\n %s", buf );
	const char *ip = emailServIp;
	if ( !ts->sendMsg( ip, strlen( ip ), 25, buf, PAGER_BUF_SIZE, buffLen, buffLen, h, sentEmailWrapper,
	                   60 * 1000, 100 * 1024, 100 * 1024 ) ) {
		return false;
	}

	// we did not block, so update h->m_emailCode
	sentEmailWrapper ( h , NULL );

	// we did not block
	return true;
}


void PingServer::sentEmailWrapper( void *state, TcpSocket *s) {
	// keep track of how many we got
	g_pingServer.m_numReplies2++;
	if ( g_pingServer.m_numReplies2 > g_pingServer.m_maxRequests2 ) {
		log(LOG_LOGIC,"net: too many replies received. "
		    "requests:%" PRId32" replies:%" PRId32" maxrequests:%" PRId32,
		    g_pingServer.m_numRequests2, 
		    g_pingServer.m_numReplies2, 
		    g_pingServer.m_maxRequests2);
		//g_process.shutdownAbort(true);
	}
	Host *h = (Host *)state;

	if ( g_errno ) { 
		if(h) {
			log("net: Had error sending email to mobile for dead "
			    "hostId "
			    "#%" PRId32": %s.", h->m_hostId,mstrerror(g_errno));
		} else {
			log("net: Had error sending email to mobile for "
			    "int32_t latency: %s.", mstrerror(g_errno));
		}
		// mark as 0 on error to try sending again later
		//h->m_emailCode = 0; 
		// reset these errors just in case
		g_errno = 0;
		return;
	}
	// log it
	if(h)
		log("net: Email sent successfully for dead host #%" PRId32".",
		    h->m_hostId);
	else 
		log("net: Email sent successfully for int32_t latency.");
	// . show the reply
	// . seems to crash if we log the read buffer... no \0?
	if ( s && s->m_readBuf )
		log("net: Got messaging server reply #%" PRId32".\n%s",
		    g_pingServer.m_numReplies2,s->m_readBuf );
	// otherwise, success
	if(h) {
		h->m_emailCode = 1;
		// . mark him as the one email we sent
		// . don't send another email until he comes back up!
		// . it's just a waste
		s_lastSentHostId = h->m_hostId;
	}
}

//////////////////////////////////////////////////
// the shutdown broadcast
//////////////////////////////////////////////////

// when a host goes down gracefully it lets all its peers know so they
// do not send requests to it.

// . broadcast shutdown notes
// . returns false if blocked, true otherwise
// . does not set g_errno
bool PingServer::broadcastShutdownNotes ( bool    sendEmailAlert          ,
					  void   *state                   ,
					  void  (* callback)(void *state) ) {
	// only call once
	if ( m_numRequests != m_numReplies ) return true;
	// keep track
	m_numRequests = 0;
	m_numReplies  = 0;
	// save callback info
	m_broadcastState    = state;
	m_broadcastCallback = callback;
	// use this buffer
	static char s_buf [5];
	*(int32_t *)s_buf = g_hostdb.m_hostId;
	// followed by sendEmailAlert
	s_buf[4] = (char)sendEmailAlert;

	if(!g_hostdb.m_myHost->m_isProxy) {
		// send to proxy hosts too so they don't send to us
		for ( int32_t i = 0 ; i < g_hostdb.getNumProxy() ; i++ ) {
			// get host
			Host *h = g_hostdb.getProxy(i);
			// count as sent
			m_numRequests++;
			// send it right now
			// we are sending to a proxy!
			if (g_udpServer.sendRequest(s_buf, 5, msg_type_11, h->m_ip, h->m_port, -1, NULL, NULL, gotReplyWrapperP2, 3000, 0)) {
				continue;
			}
			// otherwise, had an error
			m_numReplies++;
			// reset g_errno
			g_errno = 0;
		}
	}


	// send a high priority msg to each host in network, except us
	for ( int32_t i = 0 ; i < g_hostdb.getNumHosts() ; i++ ) {
		// get host
		Host *h = &g_hostdb.m_hosts[i];
		// skip ourselves
		if ( h->m_hostId == g_hostdb.m_hostId ) continue;
		// count as sent
		m_numRequests++;
		// send it right now
		if (g_udpServer.sendRequest(s_buf, 5, msg_type_11, h->m_ip, h->m_port, h->m_hostId, NULL, NULL, gotReplyWrapperP2, 3000, 0)) {
			continue;
		}
		// otherwise, had an error
		m_numReplies++;
		// reset g_errno
		g_errno = 0;
	}
	// if already done return true
	if ( m_numReplies >= m_numRequests ) return true;
	// otherwise we blocked
	return false;
}

void PingServer::gotReplyWrapperP2(void *state, UdpSlot *slot) {
	// count it
	g_pingServer.m_numReplies++;
	// don't let udp server free our send buf, we own it
	slot->m_sendBufAlloc = NULL;
	// discard errors
	g_errno = 0;
	// bail if not done
	if ( g_pingServer.m_numReplies < g_pingServer.m_numRequests ) return ;
	// call our wrapper
	if ( g_pingServer.m_broadcastCallback )
	       g_pingServer.m_broadcastCallback(g_pingServer.m_broadcastState);
}

//////////////////////////////////////////////////
// the sync point section
//////////////////////////////////////////////////

// every 10 minutes, host #0 tells all hosts to store a "sync point".
// this will force all hosts to dump a list of the names of all the files
// they have created since the last "sync point" was stored. in addition
// to dumping this list to disk, the "sync point" itself (a timestamp)
// will be appened to the list so we know at what approximate times all
// the files were created. this if for doing incremental synchronization.
// all "sync points" are from host #0's clock.

static void updatePingTime ( Host *h , int32_t *pingPtr , int32_t tripTime ) {

	// sanity check
	if ( pingPtr != &h->m_ping && pingPtr != &h->m_pingShotgun ) { 
		g_process.shutdownAbort(true); }

	// do the actual update
	*pingPtr = tripTime;
	// do not go negative on us
	if ( *pingPtr < 0 ) *pingPtr = 0;

	// . record max ping
	// . wait 60 seconds for everyone to come up if we just started up
	if ( tripTime > h->m_pingMax &&
	     // do not count shotgun ips!
	     pingPtr == &h->m_ping &&
	     gettimeofdayInMilliseconds()-g_process.m_processStartTime >= 60000 ) {
		h->m_pingMax = tripTime;
		const char *desc = "";
		if ( pingPtr == &h->m_pingShotgun ) desc = " (shotgun)";
		if ( tripTime > 50 )
			log("gb: got new max ping time of %" PRId32" for "
			    "host #%" PRId32"%s ",tripTime,h->m_hostId,desc);
	}
}

void PingServer::sendEmailMsg ( int32_t *lastTimeStamp , const char *msg ) {
	// leave if we already sent and alert within 5 mins
	int32_t now = getTimeGlobal();
	if ( now - *lastTimeStamp < 5*60 ) return;
	// prepare msg to send
	char msgbuf[1024];
	snprintf(msgbuf, 1024,
		 "cluster %s : proxy: %s",
		 g_conf.m_clusterName,
		 msg);
	// send it, force it, so even if email alerts off, it sends it
	g_pingServer.sendEmail ( NULL   , // Host *h
				 msgbuf , // char *errmsg = NULL , 
				 false  , // bool oom = false ,
				 true   );// bool forceIt      = false );
	*lastTimeStamp = now;
	return;
}
