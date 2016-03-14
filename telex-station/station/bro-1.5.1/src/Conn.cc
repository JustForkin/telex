// $Id: Conn.cc 6219 2008-10-01 05:39:07Z vern $
//
// See the file "COPYING" in the main distribution directory for copyright.

#include "config.h"

#include <ctype.h>

#include "Net.h"
#include "NetVar.h"
#include "Conn.h"
#include "Event.h"
#include "Sessions.h"
#include "Logger.h"
#include "Timer.h"
#include "PIA.h"
#include "binpac.h"

HashKey* ConnID::BuildConnKey() const
	{
	Key key;

	// Lookup up connection based on canonical ordering, which is
	// the smaller of <src addr, src port> and <dst addr, dst port>
	// followed by the other.
	if ( is_one_way ||
	     addr_port_canon_lt(src_addr, src_port, dst_addr, dst_port) )
		{
		copy_addr(src_addr, key.ip1);
		copy_addr(dst_addr, key.ip2);
		key.port1 = src_port;
		key.port2 = dst_port;
		}
	else
		{
		copy_addr(dst_addr, key.ip1);
		copy_addr(src_addr, key.ip2);
		key.port1 = dst_port;
		key.port2 = src_port;
		}

	return new HashKey(&key, sizeof(key));
	}

void ConnectionTimer::Init(Connection* arg_conn, timer_func arg_timer,
				int arg_do_expire)
	{
	conn = arg_conn;
	timer = arg_timer;
	do_expire = arg_do_expire;
	Ref(conn);
	}

ConnectionTimer::~ConnectionTimer()
	{
	if ( conn->RefCnt() < 1 )
		internal_error("reference count inconsistency in ~ConnectionTimer");

	conn->RemoveTimer(this);
	Unref(conn);
	}

void ConnectionTimer::Dispatch(double t, int is_expire)
	{
	if ( is_expire && ! do_expire )
		return;

	// Remove ourselves from the connection's set of timers so
	// it doesn't try to cancel us.
	conn->RemoveTimer(this);

	(conn->*timer)(t);

	if ( conn->RefCnt() < 1 )
		internal_error("reference count inconsistency in ConnectionTimer::Dispatch");
	}

IMPLEMENT_SERIAL(ConnectionTimer, SER_CONNECTION_TIMER);

bool ConnectionTimer::DoSerialize(SerialInfo* info) const
	{
	DO_SERIALIZE(SER_CONNECTION_TIMER, Timer);

	// We enumerate all the possible timer functions here ... This
	// has to match the list is DoUnserialize()!
	char type = 0;

	if ( timer == timer_func(&Connection::DeleteTimer) )
		type = 1;
	else if ( timer == timer_func(&Connection::InactivityTimer) )
		type = 2;
	else if ( timer == timer_func(&Connection::StatusUpdateTimer) )
		type = 3;
	else if ( timer == timer_func(&Connection::RemoveConnectionTimer) )
		type = 4;
	else
		internal_error("unknown function in ConnectionTimer::DoSerialize()");

	return conn->Serialize(info) && SERIALIZE(type) && SERIALIZE(do_expire);
	}

bool ConnectionTimer::DoUnserialize(UnserialInfo* info)
	{
	DO_UNSERIALIZE(Timer);

	conn = Connection::Unserialize(info);
	if ( ! conn )
		return false;

	char type;

	if ( ! UNSERIALIZE(&type) || ! UNSERIALIZE(&do_expire) )
		return false;

	switch ( type ) {
	case 1:
		timer = timer_func(&Connection::DeleteTimer);
		break;
	case 2:
		timer = timer_func(&Connection::InactivityTimer);
		break;
	case 3:
		timer = timer_func(&Connection::StatusUpdateTimer);
		break;
	case 4:
		timer = timer_func(&Connection::RemoveConnectionTimer);
		break;
	default:
		info->s->Error("unknown connection timer function");
		return false;
	}

	return true;
	}

unsigned int Connection::total_connections = 0;
unsigned int Connection::current_connections = 0;
unsigned int Connection::external_connections = 0;

IMPLEMENT_SERIAL(Connection, SER_CONNECTION);

Connection::Connection(NetSessions* s, HashKey* k, double t, const ConnID* id)
	{
	sessions = s;
	key = k;
	start_time = last_time = t;

	copy_addr(id->src_addr, orig_addr);
	copy_addr(id->dst_addr, resp_addr);
	orig_port = id->src_port;
	resp_port = id->dst_port;
	proto = TRANSPORT_UNKNOWN;

	conn_val = 0;
	orig_endp = resp_endp = 0;
	login_conn = 0;

	is_active = 1;
	skip = 0;
	weird = 0;
	persistent = 0;

	suppress_event = 0;

	record_contents = record_packets = 1;

	timers_canceled = 0;
	inactivity_timeout = 0;
	installed_status_timer = 0;

	finished = 0;

	hist_seen = 0;
	history = "";

	root_analyzer = 0;
	primary_PIA = 0;

	++current_connections;
	++total_connections;

	TimerMgr::Tag* tag = current_iosrc->GetCurrentTag();
	conn_timer_mgr = tag ? new TimerMgr::Tag(*tag) : 0;

	if ( conn_timer_mgr )
		{
		++external_connections;
		// We schedule a timer which removes this connection from memory
		// indefinitively into the future. Ii will expire when the timer
		// mgr is drained but not before.
		ADD_TIMER(&Connection::RemoveConnectionTimer, 1e20, 1,
				TIMER_REMOVE_CONNECTION);
		}
	}

Connection::~Connection()
	{
	if ( ! finished )
		internal_error("Done() not called before destruction of Connection");

	CancelTimers();

	if ( conn_val )
		{
		conn_val->SetOrigin(0);
		Unref(conn_val);
		}

	delete key;
	delete root_analyzer;
	delete conn_timer_mgr;

	--current_connections;
	if ( conn_timer_mgr )
		--external_connections;
	}

void Connection::Done()
	{
	finished = 1;

	if ( root_analyzer && ! root_analyzer->IsFinished() )
		root_analyzer->Done();
	}

void Connection::NextPacket(double t, int is_orig,
			const IP_Hdr* ip, int len, int caplen,
			const u_char*& data,
			int& record_packet, int& record_content,
			// arguments for reproducing packets
			const struct pcap_pkthdr* hdr,
			const u_char* const pkt,
			int hdr_size)
	{
	current_hdr = hdr;
	current_hdr_size = hdr_size;
	current_timestamp = t;
	current_pkt = pkt;

	if ( Skipping() )
		return;

	if ( root_analyzer )
		{
		record_current_packet = record_packet;
		record_current_content = record_content;
		root_analyzer->NextPacket(len, data, is_orig, -1, ip, caplen);
		record_packet = record_current_packet;
		record_content = record_current_content;
		}
	else
		last_time = t;

	current_hdr = 0;
	current_hdr_size = 0;
	current_timestamp = 0;
	current_pkt = 0;
	}

void Connection::SetLifetime(double lifetime)
	{
	ADD_TIMER(&Connection::DeleteTimer, network_time + lifetime, 0,
			TIMER_CONN_DELETE);
	}

bool Connection::IsReuse(double t, const u_char* pkt)
	{
	return root_analyzer && root_analyzer->IsReuse(t, pkt);
	}

void Connection::DeleteTimer(double /* t */)
	{
	if ( is_active )
		Event(connection_timeout, 0);

	sessions->Remove(this);
	}

void Connection::InactivityTimer(double t)
	{
	// If the inactivity_timeout is zero, there has been an active
	// timeout once, but it's disabled now. We do nothing then.
	if ( inactivity_timeout )
		{
		if ( last_time + inactivity_timeout <= t )
			{
			Event(connection_timeout, 0);
			sessions->Remove(this);
			++killed_by_inactivity;
			}
		else
			ADD_TIMER(&Connection::InactivityTimer,
					last_time + inactivity_timeout, 0,
					TIMER_CONN_INACTIVITY);
		}
	}

void Connection::RemoveConnectionTimer(double t)
	{
	Event(connection_state_remove, 0);
	sessions->Remove(this);
	}

void Connection::SetInactivityTimeout(double timeout)
	{
	// We add a new inactivity timer even if there already is one.  When
	// it fires, we always use the current value to check for inactivity.
	if ( timeout )
		ADD_TIMER(&Connection::InactivityTimer,
				last_time + timeout, 0, TIMER_CONN_INACTIVITY);

	inactivity_timeout = timeout;
	}

void Connection::EnableStatusUpdateTimer()
	{
	if ( connection_status_update && connection_status_update_interval )
		{
		ADD_TIMER(&Connection::StatusUpdateTimer,
			network_time + connection_status_update_interval, 0,
			TIMER_CONN_STATUS_UPDATE);
		installed_status_timer = 1;
		}
	}

void Connection::StatusUpdateTimer(double t)
	{
	val_list* vl = new val_list(1);
	vl->append(BuildConnVal());
	ConnectionEvent(connection_status_update, 0, vl);
	ADD_TIMER(&Connection::StatusUpdateTimer,
			network_time + connection_status_update_interval, 0,
			TIMER_CONN_STATUS_UPDATE);
	}

RecordVal* Connection::BuildConnVal()
	{
	if ( ! conn_val )
		{
		conn_val = new RecordVal(connection_type);

		TransportProto prot_type = ConnTransport();

		RecordVal* id_val = new RecordVal(conn_id);
		id_val->Assign(0, new AddrVal(orig_addr));
		id_val->Assign(1, new PortVal(ntohs(orig_port), prot_type));
		id_val->Assign(2, new AddrVal(resp_addr));
		id_val->Assign(3, new PortVal(ntohs(resp_port), prot_type));
		conn_val->Assign(0, id_val);

		orig_endp = new RecordVal(endpoint);
		orig_endp->Assign(0, new Val(0, TYPE_COUNT));
		orig_endp->Assign(1, new Val(0, TYPE_COUNT));
		orig_endp->Assign(2, new Val(0, TYPE_COUNT));
		conn_val->Assign(1, orig_endp);

		resp_endp = new RecordVal(endpoint);
		resp_endp->Assign(0, new Val(0, TYPE_COUNT));
		resp_endp->Assign(1, new Val(0, TYPE_COUNT));
		resp_endp->Assign(2, new Val(0, TYPE_COUNT));
		conn_val->Assign(2, resp_endp);

		// conn_val->Assign(3, new Val(start_time, TYPE_TIME));	// ###
		conn_val->Assign(5, new TableVal(string_set));	// service
		conn_val->Assign(6, new StringVal(""));	// addl
		conn_val->Assign(7, new Val(0, TYPE_COUNT));	// hot
		conn_val->Assign(8, new StringVal(""));	// history
		}

	if ( root_analyzer )
		{
		root_analyzer->UpdateEndpointVal(orig_endp, 1);
		root_analyzer->UpdateEndpointVal(resp_endp, 0);
		}

	conn_val->Assign(3, new Val(start_time, TYPE_TIME));	// ###
	conn_val->Assign(4, new Val(last_time - start_time, TYPE_INTERVAL));
	conn_val->Assign(8, new StringVal(history.c_str()));

	conn_val->SetOrigin(this);

	Ref(conn_val);

	return conn_val;
	}

Analyzer* Connection::FindAnalyzer(AnalyzerID id)
	{
	return root_analyzer ? root_analyzer->FindChild(id) : 0;
	}

Analyzer* Connection::FindAnalyzer(AnalyzerTag::Tag tag)
	{
	return root_analyzer ? root_analyzer->FindChild(tag) : 0;
	}

void Connection::AppendAddl(const char* str)
	{
	Unref(BuildConnVal());

	const char* old = conn_val->Lookup(6)->AsString()->CheckString();
	const char* format = *old ? "%s %s" : "%s%s";

	conn_val->Assign(6, new StringVal(fmt(format, old, str)));
	}

// Returns true if the character at s separates a version number.
static inline bool is_version_sep(const char* s, const char* end)
	{
	return
		// foo-1.2.3
			(s < end - 1 && ispunct(s[0]) && isdigit(s[1])) ||
		// foo-v1.2.3
			(s < end - 2 && ispunct(s[0]) &&
			 tolower(s[1]) == 'v' && isdigit(s[2])) ||
		// foo 1.2.3
			isspace(s[0]);
	}

void Connection::Match(Rule::PatternType type, const u_char* data, int len, bool is_orig, bool bol, bool eol, bool clear_state)
	{
	if ( primary_PIA )
		primary_PIA->Match(type, data, len, is_orig, bol, eol, clear_state);
	}

Val* Connection::BuildVersionVal(const char* s, int len)
	{
	Val* name = 0;
	Val* major = 0;
	Val* minor = 0;
	Val* minor2 = 0;
	Val* addl = 0;

	const char* last = s + len;
	const char* e = s;

	// This is all just a guess...

	// Eat non-alpha-numerical chars.
	for ( ; s < last && ! isalnum(*s); ++s )
		;

	// Leading characters are the program name.
	// (first character must not be a digit)
	if ( isalpha(*s) )
		{
		for ( e = s; e < last && ! is_version_sep(e, last); ++e )
			;

		if ( s != e )
			name = new StringVal(e - s, s);
		}

	// Find first number - that's the major version.
	for ( s = e; s < last && ! isdigit(*s); ++s )
		;
	for ( e = s; e < last && isdigit(*e); ++e )
		;

	if ( s != e )
		major = new Val(atoi(s), TYPE_INT);

	// Find second number seperated only by punctuation chars -
	// that's the minor version.
	for ( s = e; s < last && ispunct(*s); ++s )
		;
	for ( e = s; e < last && isdigit(*e); ++e )
		;

	if ( s != e )
		minor = new Val(atoi(s), TYPE_INT);

	// Find second number seperated only by punctuation chars; -
	// that's the minor version.
	for ( s = e; s < last && ispunct(*s); ++s )
		;
	for ( e = s; e < last && isdigit(*e); ++e )
		;

	if ( s != e )
		minor2 = new Val(atoi(s), TYPE_INT);

	// Anything after following punctuation and until next white space is
	// an additional version string.
	for ( s = e; s < last && ispunct(*s); ++s )
		;
	for ( e = s; e < last && ! isspace(*e); ++e )
		;

	if ( s != e )
		addl = new StringVal(e - s, s);

	// If we do not have a name yet, the next alphanumerical string is it.
	if ( ! name )
		{ // eat non-alpha-numerical characters
		for ( s = e; s < last && ! isalpha(*s); ++s )
			;

		// Get name.
		for ( e = s; e < last && (isalnum(*e) || *e == '_'); ++e )
			;

		if ( s != e )
			name = new StringVal(e - s, s);
		}

	// We need at least a name.
	if ( ! name )
		return 0;

	RecordVal* version = new RecordVal(software_version);
	version->Assign(0, major ? major : new Val(-1, TYPE_INT));
	version->Assign(1, minor ? minor : new Val(-1, TYPE_INT));
	version->Assign(2, minor2 ? minor2 : new Val(-1, TYPE_INT));
	version->Assign(3, addl ? addl : new StringVal(""));

	RecordVal* sw = new RecordVal(software);
	sw->Assign(0, name);
	sw->Assign(1, version);

	return sw;
	}

int Connection::VersionFoundEvent(const uint32* addr, const char* s, int len,
					Analyzer* analyzer)
	{
	if ( ! software_version_found && ! software_parse_error )
		return 1;

	if ( ! is_printable(s, len) )
		return 0;

	Val* val = BuildVersionVal(s, len);
	if ( ! val )
		{
		if ( software_parse_error )
			{
			val_list* vl = new val_list;
			vl->append(BuildConnVal());
			vl->append(new AddrVal(addr));
			vl->append(new StringVal(len, s));
			ConnectionEvent(software_parse_error, analyzer, vl);
			}
		return 0;
		}

	if ( software_version_found )
		{
		val_list* vl = new val_list;
		vl->append(BuildConnVal());
		vl->append(new AddrVal(addr));
		vl->append(val);
		vl->append(new StringVal(len, s));
		ConnectionEvent(software_version_found, 0, vl);
		}
	else
		Unref(val);

	return 1;
	}

int Connection::UnparsedVersionFoundEvent(const uint32* addr,
					const char* full, int len, Analyzer* analyzer)
	{
	// Skip leading white space.
	while ( len && isspace(*full) )
		{
		--len;
		++full;
		}

	if ( ! is_printable(full, len) )
		return 0;

	if ( software_unparsed_version_found )
		{
		val_list* vl = new val_list;
		vl->append(BuildConnVal());
		vl->append(new AddrVal(addr));
		vl->append(new StringVal(len, full));
		ConnectionEvent(software_unparsed_version_found, analyzer, vl);
		}

	return 1;
	}

void Connection::Event(EventHandlerPtr f, Analyzer* analyzer, const char* name)
	{
	if ( ! f )
		return;

	val_list* vl = new val_list(2);
	if ( name )
		vl->append(new StringVal(name));
	vl->append(BuildConnVal());

	ConnectionEvent(f, analyzer, vl);
	}

void Connection::Event(EventHandlerPtr f, Analyzer* analyzer, Val* v1, Val* v2)
	{
	if ( ! f )
		{
		Unref(v1);
		Unref(v2);
		return;
		}

	val_list* vl = new val_list(3);
	vl->append(BuildConnVal());
	vl->append(v1);

	if ( v2 )
		vl->append(v2);

	ConnectionEvent(f, analyzer, vl);
	}

void Connection::ConnectionEvent(EventHandlerPtr f, Analyzer* a, val_list* vl)
	{
	if ( ! f )
		{
		// This may actually happen if there is no local handler
		// and a previously existing remote handler went away.
		loop_over_list(*vl, i)
			Unref((*vl)[i]);
		delete vl;
		return;
		}

	// "this" is passed as a cookie for the event
	mgr.QueueEvent(f, vl, SOURCE_LOCAL,
			a ? a->GetID() : 0, GetTimerMgr(), this);
	}

void Connection::Weird(const char* name)
	{
	weird = 1;
	if ( conn_weird )
		Event(conn_weird, 0, name);
	else
		fprintf(stderr, "%.06f weird: %s\n", network_time, name);
	}

void Connection::Weird(const char* name, const char* addl)
	{
	weird = 1;
	if ( conn_weird_addl )
		{
		val_list* vl = new val_list(3);

		vl->append(new StringVal(name));
		vl->append(BuildConnVal());
		vl->append(new StringVal(addl));

		ConnectionEvent(conn_weird_addl, 0, vl);
		}

	else
		fprintf(stderr, "%.06f weird: %s (%s)\n", network_time, name, addl);
	}

void Connection::Weird(const char* name, int addl_len, const char* addl)
	{
	weird = 1;
	if ( conn_weird_addl )
		{
		val_list* vl = new val_list(3);

		vl->append(new StringVal(name));
		vl->append(BuildConnVal());
		vl->append(new StringVal(addl_len, addl));

		ConnectionEvent(conn_weird_addl, 0, vl);
		}

	else
		{
		fprintf(stderr, "%.06f weird: %s (", network_time, name);
		for ( int i = 0; i < addl_len; ++i )
			fputc(addl[i], stderr);
		fprintf(stderr, ")\n");
		}
	}

void Connection::AddTimer(timer_func timer, double t, int do_expire,
		TimerType type)
	{
	if ( timers_canceled )
		return;

	// If the key is cleared, the connection isn't stored in the connection
	// table anymore and will soon be deleted. We're not installing new
	// timers anymore then.
	if ( ! key )
		return;

	Timer* conn_timer = new ConnectionTimer(this, timer, t, do_expire, type);
	GetTimerMgr()->Add(conn_timer);
	timers.append(conn_timer);
	}

void Connection::RemoveTimer(Timer* t)
	{
	timers.remove(t);
	}

void Connection::CancelTimers()
	{
	// We are going to cancel our timers which, in turn, may cause them to
	// call RemoveTimer(), which would then modify the list we're just
	// traversing. Thus, we first make a copy of the list which we then
	// iterate through.
	timer_list tmp(timers.length());
	loop_over_list(timers, j)
		tmp.append(timers[j]);

	loop_over_list(tmp, i)
		GetTimerMgr()->Cancel(tmp[i]);

	timers_canceled = 1;
	timers.clear();
	}

TimerMgr* Connection::GetTimerMgr() const
	{
	if ( ! conn_timer_mgr )
		// Global manager.
		return timer_mgr;

	// We need to check whether the local timer manager still exists;
	// it may have already been timed out, in which case we fall back
	// to the global manager (though this should be rare).
	TimerMgr* local_mgr = sessions->LookupTimerMgr(conn_timer_mgr, false);
	return local_mgr ? local_mgr : timer_mgr;
	}

void Connection::FlipRoles()
	{
	uint32 tmp_addr[NUM_ADDR_WORDS];
	copy_addr(resp_addr, tmp_addr);
	copy_addr(orig_addr, resp_addr);
	copy_addr(tmp_addr, orig_addr);

	uint32 tmp_port = resp_port;
	resp_port = orig_port;
	orig_port = tmp_port;

	RecordVal* tmp_rc = resp_endp;
	resp_endp = orig_endp;
	orig_endp = tmp_rc;

	Unref(conn_val);
	conn_val = 0;

	if ( root_analyzer )
		root_analyzer->FlipRoles();
	}

int Connection::RewritingTrace()
	{
	return root_analyzer ? root_analyzer->RewritingTrace() : 0;
	}

Rewriter* Connection::TraceRewriter() const
	{
	return root_analyzer ? root_analyzer->TraceRewriter() : 0;
	}

unsigned int Connection::MemoryAllocation() const
	{
	return padded_sizeof(*this)
		+ (key ? key->MemoryAllocation() : 0)
		+ (timers.MemoryAllocation() - padded_sizeof(timers))
		+ (conn_val ? conn_val->MemoryAllocation() : 0)
		+ (root_analyzer ? root_analyzer->MemoryAllocation(): 0)
		// login_conn is just a casted 'this'.
		// primary_PIA is already contained in the analyzer tree.
		;
	}

unsigned int Connection::MemoryAllocationConnVal() const
	{
	return conn_val ? conn_val->MemoryAllocation() : 0;
	}

void Connection::Describe(ODesc* d) const
	{
	d->Add(start_time);
	d->Add("(");
	d->Add(last_time);
	d->AddSP(")");

	switch ( proto ) {
		case TRANSPORT_TCP:
			d->Add("TCP");
			break;

		case TRANSPORT_UDP:
			d->Add("UDP");
			break;

		case TRANSPORT_ICMP:
			d->Add("ICMP");
			break;

		case TRANSPORT_UNKNOWN:
			internal_error("unknown transport in Connction::Describe()");
			break;
		}

	d->SP();
	d->Add(dotted_addr(orig_addr));
	d->Add(":");
	d->Add(ntohs(orig_port));

	d->SP();
	d->AddSP("->");

	d->Add(dotted_addr(resp_addr));
	d->Add(":");
	d->Add(ntohs(resp_port));

	d->NL();
	}

bool Connection::Serialize(SerialInfo* info) const
	{
	return SerialObj::Serialize(info);
	}

Connection* Connection::Unserialize(UnserialInfo* info)
	{
	return (Connection*) SerialObj::Unserialize(info, SER_CONNECTION);
	}

bool Connection::DoSerialize(SerialInfo* info) const
	{
	DO_SERIALIZE(SER_CONNECTION, BroObj);

	// First we write the members which are needed to
	// create the HashKey.
	for ( int j = 0; j < NUM_ADDR_WORDS; ++j )
		if ( ! SERIALIZE(orig_addr[j]) || ! SERIALIZE(resp_addr[j]) )
			return false;

	if ( ! SERIALIZE(orig_port) || ! SERIALIZE(resp_port) )
		return false;

	if ( ! SERIALIZE(timers.length()) )
		return false;

	loop_over_list(timers, i)
		if ( ! timers[i]->Serialize(info) )
			return false;

	SERIALIZE_OPTIONAL(conn_val);
	SERIALIZE_OPTIONAL(orig_endp);
	SERIALIZE_OPTIONAL(resp_endp);

	// FIXME: RuleEndpointState not yet serializable.
	// FIXME: Analyzers not yet serializable.

	return
		SERIALIZE(int(proto)) &&
		SERIALIZE(history) &&
		SERIALIZE(hist_seen) &&
		SERIALIZE(start_time) &&
		SERIALIZE(last_time) &&
		SERIALIZE(inactivity_timeout) &&
		SERIALIZE(suppress_event) &&
		SERIALIZE(login_conn != 0) &&
		SERIALIZE_BIT(installed_status_timer) &&
		SERIALIZE_BIT(timers_canceled) &&
		SERIALIZE_BIT(is_active) &&
		SERIALIZE_BIT(skip) &&
		SERIALIZE_BIT(weird) &&
		SERIALIZE_BIT(finished) &&
		SERIALIZE_BIT(record_packets) &&
		SERIALIZE_BIT(record_contents) &&
		SERIALIZE_BIT(persistent);
	}

bool Connection::DoUnserialize(UnserialInfo* info)
	{
	// Make sure this is initialized for the condition in Unserialize().
	persistent = 0;

	DO_UNSERIALIZE(BroObj);

	// Build the hash key first. Some of the recursive *::Unserialize()
	// functions may need it.
	for ( int i = 0; i < NUM_ADDR_WORDS; ++i )
		if ( ! UNSERIALIZE(&orig_addr[i]) || ! UNSERIALIZE(&resp_addr[i]) )
			goto error;

	if ( ! UNSERIALIZE(&orig_port) || ! UNSERIALIZE(&resp_port) )
		goto error;

	ConnID id;
	id.src_addr = orig_addr;
	id.dst_addr = resp_addr;
	// This doesn't work for ICMP. But I guess this is not really important.
	id.src_port = orig_port;
	id.dst_port = resp_port;
	id.is_one_way = 0;	// ### incorrect for ICMP
	key = id.BuildConnKey();

	int len;
	if ( ! UNSERIALIZE(&len) )
		goto error;

	while ( len-- )
		{
		Timer* t = Timer::Unserialize(info);
		if ( ! t )
			goto error;
		timers.append(t);
		}

	UNSERIALIZE_OPTIONAL(conn_val,
			(RecordVal*) Val::Unserialize(info, connection_type));
	UNSERIALIZE_OPTIONAL(orig_endp,
			(RecordVal*) Val::Unserialize(info, endpoint));
	UNSERIALIZE_OPTIONAL(resp_endp,
			(RecordVal*) Val::Unserialize(info, endpoint));

	int iproto;

	if ( ! (UNSERIALIZE(&iproto) &&
		UNSERIALIZE(&history) &&
		UNSERIALIZE(&hist_seen) &&
		UNSERIALIZE(&start_time) &&
		UNSERIALIZE(&last_time) &&
		UNSERIALIZE(&inactivity_timeout) &&
		UNSERIALIZE(&suppress_event)) )
		goto error;

	proto = static_cast<TransportProto>(iproto);

	bool has_login_conn;
	if ( ! UNSERIALIZE(&has_login_conn) )
		goto error;

	login_conn = has_login_conn ? (LoginConn*) this : 0;

	UNSERIALIZE_BIT(installed_status_timer);
	UNSERIALIZE_BIT(timers_canceled);
	UNSERIALIZE_BIT(is_active);
	UNSERIALIZE_BIT(skip);
	UNSERIALIZE_BIT(weird);
	UNSERIALIZE_BIT(finished);
	UNSERIALIZE_BIT(record_packets);
	UNSERIALIZE_BIT(record_contents);
	UNSERIALIZE_BIT(persistent);

	// Hmm... Why does each connection store a sessions ptr?
	sessions = ::sessions;

	root_analyzer = 0;
	primary_PIA = 0;
	conn_timer_mgr = 0;

	return true;

error:
	abort();
	CancelTimers();
	return false;
	}

void Connection::SetRootAnalyzer(TransportLayerAnalyzer* analyzer, PIA* pia)
	{
	root_analyzer = analyzer;
	primary_PIA = pia;
	}