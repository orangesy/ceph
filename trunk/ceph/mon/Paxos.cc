// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "Paxos.h"
#include "Monitor.h"
#include "MonitorStore.h"

#include "messages/MMonPaxos.h"

#include "config.h"
#undef dout
#define  derr(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) cerr << g_clock.now() << " mon" << whoami << (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << ".paxos(" << machine_name << " " << get_statename(state) << " lc " << last_committed << ") "
#define  dout(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) cout << g_clock.now() << " mon" << whoami << (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << ".paxos(" << machine_name << " " << get_statename(state) << " lc " << last_committed << ") "


// ---------------------------------

// PHASE 1

// leader
void Paxos::collect(version_t oldpn)
{
  // we're recoverying, it seems!
  state = STATE_RECOVERING;
  assert(mon->is_leader());

  // reset the number of lasts received
  accepted_pn = get_new_proposal_number(MAX(accepted_pn, oldpn));
  accepted_pn_from = last_committed;
  num_last = 1;
  old_accepted_v = 0;
  old_accepted_pn = 0;
  old_accepted_value.clear();

  dout(10) << "collect with pn " << accepted_pn << endl;

  // send collect
  for (set<int>::const_iterator p = mon->get_quorum().begin();
       p != mon->get_quorum().end();
       ++p) {
    if (*p == whoami) continue;
    
    MMonPaxos *collect = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_COLLECT, machine_id);
    collect->last_committed = last_committed;
    collect->pn = accepted_pn;
    mon->messenger->send_message(collect, mon->monmap->get_inst(*p));
  }
}


// peon
void Paxos::handle_collect(MMonPaxos *collect)
{
  dout(10) << "handle_collect " << *collect << endl;

  assert(mon->is_peon()); // mon epoch filter should catch strays

  // we're recoverying, it seems!
  state = STATE_RECOVERING;

  // reply
  MMonPaxos *last = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_LAST, machine_id);
  last->last_committed = last_committed;
  
  // do we have an accepted but uncommitted value?
  //  (it'll be at last_committed+1)
  bufferlist bl;
  if (mon->store->exists_bl_sn(machine_name, last_committed+1)) {
    mon->store->get_bl_sn(bl, machine_name, last_committed+1);
    assert(bl.length() > 0);
    dout(10) << "sharing our accepted but uncommitted value for " << last_committed+1 << endl;
    last->values[last_committed+1] = bl;
    last->old_accepted_pn = accepted_pn;
  }

  // can we accept this pn?
  if (collect->pn > accepted_pn) {
    // ok, accept it
    accepted_pn = collect->pn;
    accepted_pn_from = collect->pn_from;
    dout(10) << "accepting pn " << accepted_pn << " from " << accepted_pn_from << endl;
  } else {
    // don't accept!
    dout(10) << "NOT accepting pn " << collect->pn << " from " << collect->pn_from 
	     << ", we already accepted " << accepted_pn << " from " << accepted_pn_from 
	     << endl;
  }
  last->pn = accepted_pn;
  last->pn_from = accepted_pn_from;

  // and share whatever data we have
  for (version_t v = collect->last_committed;
       v <= last_committed;
       v++) {
    if (mon->store->exists_bl_sn(machine_name, v)) {
      mon->store->get_bl_sn(last->values[v], machine_name, v);
      dout(10) << " sharing " << v << " " 
	       << last->values[v].length() << " bytes" << endl;
    }
  }

  // send reply
  mon->messenger->send_message(last, collect->get_source_inst());
  delete collect;
}


// leader
void Paxos::handle_last(MMonPaxos *last)
{
  dout(10) << "handle_last " << *last << endl;

  if (!mon->is_leader()) {
    dout(10) << "not leader, dropping" << endl;
    delete last;
    return;
  }

  // share committed values?
  if (last->last_committed < last_committed) {
    // share committed values
    dout(10) << "sending commit to " << last->get_source() << endl;
    MMonPaxos *commit = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_COMMIT, machine_id);
    for (version_t v = last->last_committed;
	 v <= last_committed;
	 v++) {
      mon->store->get_bl_sn(commit->values[v], machine_name, v);
      dout(10) << "sharing " << v << " " 
	       << commit->values[v].length() << " bytes" << endl;
    }
    mon->messenger->send_message(commit, last->get_source_inst());
  }

  // did we receive a committed value?
  if (last->last_committed > last_committed) {
    for (version_t v = last_committed;
	 v <= last->last_committed;
	 v++) {
      mon->store->put_bl_sn(last->values[v], machine_name, v);
      dout(10) << "committing " << v << " " 
	       << last->values[v].length() << " bytes" << endl;
    }
    last_committed = last->last_committed;
    mon->store->put_int(last_committed, machine_name, "last_commtted");
    dout(10) << "last_committed now " << last_committed << endl;
  }
      
  // do they accept your pn?
  if (last->old_accepted_pn > accepted_pn) {
    // no, try again.
    dout(10) << " they had a higher pn than us, picking a new one." << endl;
    collect(last->old_accepted_pn);
  } else {
    // yes, they accepted our pn.  great.
    num_last++;
    dout(10) << " they accepted our pn, we now have " 
	     << num_last << " peons" << endl;

    // did this person send back an accepted but uncommitted value?
    if (last->old_accepted_pn &&
	last->old_accepted_pn > old_accepted_pn) {
      old_accepted_v = last->last_committed+1;
      old_accepted_pn = last->old_accepted_pn;
      old_accepted_value = last->values[old_accepted_v];
      dout(10) << "we learned an old (possible) value for " << old_accepted_v 
	       << " pn " << old_accepted_pn
	       << " " << old_accepted_value.length() << " bytes"
	       << endl;
    }
    
    // is that everyone?
    if (num_last == mon->get_quorum().size()) {
      // did we learn an old value?
      if (old_accepted_v == last_committed+1 &&
	  old_accepted_value.length()) {
	dout(10) << "that's everyone.  begin on old learned value" << endl;
	begin(old_accepted_value);
      } else {
	// active!
	dout(10) << "that's everyone.  active!" << endl;
	state = STATE_ACTIVE;
	finish_contexts(waiting_for_active);
	extend_lease();
      }
    }
  }

  delete last;
}


// leader
void Paxos::begin(bufferlist& v)
{
  dout(10) << "begin for " << last_committed+1 << " " 
	   << new_value.length() << " bytes"
	   << endl;

  assert(mon->is_leader());

  assert(is_active());
  state = STATE_UPDATING;

  // we must already have a majority for this to work.
  assert(mon->get_quorum().size() == 1 ||
	 num_last > (unsigned)mon->monmap->num_mon/2);
  
  // and no value, yet.
  assert(new_value.length() == 0);
  
  // accept it ourselves
  num_accepted = 1;
  new_value = v;
  mon->store->put_bl_sn(new_value, machine_name, last_committed+1);

  if (mon->get_quorum().size() == 1) {
    // we're alone, take it easy
    commit();
    state = STATE_ACTIVE;
    finish_contexts(waiting_for_commit);
    finish_contexts(waiting_for_active);
    return;
  }

  // ask others to accept it to!
  for (set<int>::const_iterator p = mon->get_quorum().begin();
       p != mon->get_quorum().end();
       ++p) {
    if (*p == whoami) continue;
    
    dout(10) << " sending begin to mon" << *p << endl;
    MMonPaxos *begin = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_BEGIN, machine_id);
    begin->values[last_committed+1] = new_value;
    begin->last_committed = last_committed;
    begin->pn = accepted_pn;
    
    mon->messenger->send_message(begin, mon->monmap->get_inst(*p));
  }

  // set timeout event
  accept_timeout_event = new C_AcceptTimeout(this);
  mon->timer.add_event_after(g_conf.mon_accept_timeout, accept_timeout_event);
}

// peon
void Paxos::handle_begin(MMonPaxos *begin)
{
  dout(10) << "handle_begin " << *begin << endl;

  // can we accept this?
  if (begin->pn < accepted_pn) {
    dout(10) << " we accepted a higher pn " << accepted_pn << ", ignoring" << endl;
    delete begin;
    return;
  }
  assert(begin->pn == accepted_pn);
  assert(begin->last_committed == last_committed);
  
  // set state.
  state = STATE_UPDATING;
  lease_expire = utime_t();  // cancel lease

  // yes.
  version_t v = last_committed+1;
  dout(10) << "accepting value for " << v << " pn " << accepted_pn << endl;
  mon->store->put_bl_sn(begin->values[v], machine_name, v);
  
  // reply
  MMonPaxos *accept = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_ACCEPT, machine_id);
  accept->pn = accepted_pn;
  accept->last_committed = last_committed;
  mon->messenger->send_message(accept, begin->get_source_inst());
  
  delete begin;
}

// leader
void Paxos::handle_accept(MMonPaxos *accept)
{
  dout(10) << "handle_accept " << *accept << endl;
  
  if (accept->pn != accepted_pn) {
    // we accepted a higher pn, from some other leader
    dout(10) << " we accepted a higher pn " << accepted_pn << ", ignoring" << endl;
    delete accept;
    return;
  }
  if (last_committed > 0 &&
      accept->last_committed < last_committed-1) {
    dout(10) << " this is from an old round, ignoring" << endl;
    delete accept;
    return;
  }
  assert(accept->last_committed == last_committed ||   // not committed
	 accept->last_committed == last_committed-1);  // committed

  assert(state == STATE_UPDATING);
  num_accepted++;
  dout(10) << "now " << num_accepted << " have accepted" << endl;

  // new majority?
  if (num_accepted == (unsigned)mon->monmap->num_mon/2+1) {
    // yay, commit!
    // note: this may happen before the lease is reextended (below)
    dout(10) << "we got a majority, committing too" << endl;
    commit();
  }

  // done?
  if (num_accepted == mon->get_quorum().size()) {
    state = STATE_ACTIVE;
    finish_contexts(waiting_for_commit);
    finish_contexts(waiting_for_active);
    extend_lease();

    // cancel timeout event
    mon->timer.cancel_event(accept_timeout_event);
    accept_timeout_event = 0;
  }
}

void Paxos::accept_timeout()
{
  dout(5) << "accept timeout, calling fresh election" << endl;
  assert(mon->is_leader());
  assert(is_updating());
  mon->call_election();
}

void Paxos::commit()
{
  dout(10) << "commit " << last_committed+1 << endl;

  // commit locally
  last_committed++;
  mon->store->put_int(last_committed, machine_name, "last_committed");

  // tell everyone
  for (set<int>::const_iterator p = mon->get_quorum().begin();
       p != mon->get_quorum().end();
       ++p) {
    if (*p == whoami) continue;

    dout(10) << " sending commit to mon" << *p << endl;
    MMonPaxos *commit = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_COMMIT, machine_id);
    commit->values[last_committed] = new_value;
    commit->pn = accepted_pn;
    
    mon->messenger->send_message(commit, mon->monmap->get_inst(*p));
  }

  // get ready for a new round.
  new_value.clear();
}


void Paxos::handle_commit(MMonPaxos *commit)
{
  dout(10) << "handle_commit on " << commit->last_committed << endl;

  if (!mon->is_peon()) {
    dout(10) << "not a peon, dropping" << endl;
    assert(0);
    delete commit;
    return;
  }

  // commit locally.
  for (map<version_t,bufferlist>::iterator p = commit->values.begin();
       p != commit->values.end();
       ++p) {
    assert(p->first == last_committed+1);
    last_committed = p->first;
    mon->store->put_bl_sn(p->second, machine_name, last_committed);
  }
  mon->store->put_int(last_committed, machine_name, "last_committed");
  
  delete commit;
}  

void Paxos::extend_lease()
{
  assert(mon->is_leader());
  assert(is_active());

  lease_expire = g_clock.now();
  lease_expire += g_conf.mon_lease;
  acked_lease.clear();
  acked_lease.insert(whoami);

  dout(7) << "extend_lease now+" << g_conf.mon_lease << " (" << lease_expire << ")" << endl;

  // bcast
  for (set<int>::const_iterator p = mon->get_quorum().begin();
       p != mon->get_quorum().end();
       ++p) {
    if (*p == whoami) continue;
    MMonPaxos *lease = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_LEASE, machine_id);
    lease->last_committed = last_committed;
    lease->lease_expire = lease_expire;
    mon->messenger->send_message(lease, mon->monmap->get_inst(*p));
  }

  // wake people up
  finish_contexts(waiting_for_readable);
  finish_contexts(waiting_for_writeable);

  // set renew event
  lease_renew_event = new C_LeaseRenew(this);
  utime_t at = lease_expire;
  at -= g_conf.mon_lease;
  at += g_conf.mon_lease_renew_interval;
  mon->timer.add_event_at(at, lease_renew_event);	
  
  // set timeout event.
  //  if old timeout is still in place, leave it.
  if (!lease_ack_timeout_event) {
    lease_ack_timeout_event = new C_LeaseAckTimeout(this);
    mon->timer.add_event_after(g_conf.mon_lease_ack_timeout, lease_ack_timeout_event);
  }
}


// peon
void Paxos::handle_lease(MMonPaxos *lease)
{
  // sanity
  if (!mon->is_peon() ||
      last_committed != lease->last_committed) {
    dout(10) << "handle_lease i'm not a peon, or they're not the leader, or the last_committed doesn't match, dropping" << endl;
    delete lease;
    return;
  }
  
  // extend lease
  if (lease_expire < lease->lease_expire) 
    lease_expire = lease->lease_expire;

  state = STATE_ACTIVE;
  finish_contexts(waiting_for_active);
  
  dout(10) << "handle_lease on " << lease->last_committed
	   << " now " << lease_expire << endl;

  // ack
  MMonPaxos *ack = new MMonPaxos(mon->get_epoch(), MMonPaxos::OP_LEASE_ACK, machine_id);
  ack->last_committed = last_committed;
  ack->lease_expire = lease_expire;
  mon->messenger->send_message(ack, lease->get_source_inst());
  
  // kick waiters
  if (is_readable())
    finish_contexts(waiting_for_readable);

  delete lease;
}

void Paxos::handle_lease_ack(MMonPaxos *ack)
{
  int from = ack->get_source().num();

  if (acked_lease.count(from) == 0) {
    acked_lease.insert(from);
    
    if (acked_lease == mon->get_quorum()) {
      // yay!
      dout(10) << "handle_lease_ack from " << ack->get_source() 
	       << " -- got everyone" << endl;
      mon->timer.cancel_event(lease_ack_timeout_event);
      lease_ack_timeout_event = 0;
    } else {
      dout(10) << "handle_lease_ack from " << ack->get_source() 
	       << " -- still need "
	       << mon->get_quorum().size() - acked_lease.size()
	       << " more" << endl;
    }
  } else {
    dout(10) << "handle_lease_ack from " << ack->get_source() 
	     << " dup (lagging!), ignoring" << endl;
  }
  
  delete ack;
}

void Paxos::lease_ack_timeout()
{
  dout(5) << "lease_ack_timeout -- calling new election" << endl;
  assert(mon->is_leader());
  assert(is_active());
  mon->call_election();
}


/*
 * return a globally unique, monotonically increasing proposal number
 */
version_t Paxos::get_new_proposal_number(version_t gt)
{
  // read last
  version_t last = mon->store->get_int("last_paxos_proposal");
  if (last < gt) 
    last = gt;
  
  // update
  last /= 100;
  last++;

  // make it unique among all monitors.
  version_t pn = last*100 + (version_t)whoami;
  
  // write
  mon->store->put_int(pn, "last_paxos_proposal");

  dout(10) << "get_new_proposal_number = " << pn << endl;
  return pn;
}


void Paxos::cancel_events()
{
  if (accept_timeout_event) {
    mon->timer.cancel_event(accept_timeout_event);
    accept_timeout_event = 0;
  }
  if (lease_renew_event) {
    mon->timer.cancel_event(lease_renew_event);
    lease_renew_event = 0;
  }
  if (lease_ack_timeout_event) {
    mon->timer.cancel_event(lease_ack_timeout_event);
    lease_ack_timeout_event = 0;
  }  
}

void Paxos::leader_init()
{
  if (mon->get_quorum().size() == 1) {
    state = STATE_ACTIVE;			    
    return;
  } 
  cancel_events();
  state = STATE_RECOVERING;
  lease_expire = utime_t();
  dout(10) << "leader_init -- starting paxos recovery" << endl;
  collect(0);
}

void Paxos::peon_init()
{
  cancel_events();
  state = STATE_RECOVERING;
  lease_expire = utime_t();
  dout(10) << "peon_init -- i am a peon" << endl;

  // no chance to write now!
  finish_contexts(waiting_for_writeable, -1);
  finish_contexts(waiting_for_commit, -1);
}


void Paxos::dispatch(Message *m)
{
  // election in progress?
  if (mon->is_starting()) {
    dout(5) << "election in progress, dropping " << *m << endl;
    delete m;
    return;    
  }

  // check sanity
  assert(mon->is_leader() || 
	 (mon->is_peon() && m->get_source().num() == mon->get_leader()));
  
  switch (m->get_type()) {
    
  case MSG_MON_PAXOS:
    {
      MMonPaxos *pm = (MMonPaxos*)m;
      
      // NOTE: these ops are defined in messages/MMonPaxos.h
      switch (pm->op) {
	// learner
      case MMonPaxos::OP_COLLECT:
	handle_collect(pm);
	break;
      case MMonPaxos::OP_LAST:
	handle_last(pm);
	break;
      case MMonPaxos::OP_BEGIN:
	handle_begin(pm);
	break;
      case MMonPaxos::OP_ACCEPT:
	handle_accept(pm);
	break;		
      case MMonPaxos::OP_COMMIT:
	handle_commit(pm);
	break;
      case MMonPaxos::OP_LEASE:
	handle_lease(pm);
	break;
      case MMonPaxos::OP_LEASE_ACK:
	handle_lease_ack(pm);
	break;
      default:
	assert(0);
      }
    }
    break;
    
  default:
    assert(0);
  }
}




// -----------------
// service interface

// -- READ --

bool Paxos::is_readable()
{
  if (mon->get_quorum().size() == 1) return true;
  return 
    (mon->is_peon() || mon->is_leader()) &&
    is_active() &&
    g_clock.now() < lease_expire;
}

bool Paxos::read(version_t v, bufferlist &bl)
{
  if (!is_readable()) 
    return false;

  if (!mon->store->get_bl_sn(bl, machine_name, v))
    return false;
  return true;
}

version_t Paxos::read_current(bufferlist &bl)
{
  if (!is_readable()) 
    return 0;
  if (read(last_committed, bl))
    return last_committed;
  return 0;
}




// -- WRITE --

bool Paxos::is_writeable()
{
  if (mon->get_quorum().size() == 1) return true;
  return
    mon->is_leader() &&
    is_active() &&
    g_clock.now() < lease_expire;
}

bool Paxos::propose_new_value(bufferlist& bl, Context *oncommit)
{
  /*
  // writeable?
  if (!is_writeable()) {
    dout(5) << "propose_new_value " << last_committed+1 << " " << bl.length() << " bytes"
	    << " -- not writeable" << endl;
    if (oncommit) {
      oncommit->finish(-1);
      delete oncommit;
    }
    return false;
  }
  */
  
  assert(mon->is_leader() && is_active());

  // cancel lease renewal and timeout events.
  cancel_events();

  // ok!
  dout(5) << "propose_new_value " << last_committed+1 << " " << bl.length() << " bytes" << endl;
  if (oncommit)
    waiting_for_commit.push_back(oncommit);
  begin(bl);
  
  return true;
}

