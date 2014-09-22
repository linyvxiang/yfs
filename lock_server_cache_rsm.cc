// the caching lock server implementation

#include "lock_server_cache_rsm.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"
#include <algorithm>


static void *
revokethread(void *x)
{
  lock_server_cache_rsm *sc = (lock_server_cache_rsm *) x;
  sc->revoker();
  return 0;
}

static void *
retrythread(void *x)
{
  lock_server_cache_rsm *sc = (lock_server_cache_rsm *) x;
  sc->retryer();
  return 0;
}

lock_server_cache_rsm::lock_server_cache_rsm(class rsm *_rsm) 
  : rsm (_rsm)
{
  pthread_t th;
  int r = pthread_create(&th, NULL, &revokethread, (void *) this);
  VERIFY (r == 0);
  r = pthread_create(&th, NULL, &retrythread, (void *) this);
  VERIFY (r == 0);

  pthread_mutex_init(&lock_stat_map_lock, NULL);
  rsm->set_state_transfer(this);
}

void
lock_server_cache_rsm::revoker()
{

  // This method should be a continuous loop, that sends revoke
  // messages to lock holders whenever another client wants the
  // same lock
  while(1) {
      revoke_entry client_to_revoke;
      revoke_queue.deq(&client_to_revoke);
      rpcc *rpc_c = handle(client_to_revoke.client_id).safebind();
      if(rpc_c) {
          int r;
          rpc_c->call(rlock_protocol::revoke, client_to_revoke.lid,
              client_to_revoke.xid, r); 
      }
  }

}


void
lock_server_cache_rsm::retryer()
{

  // This method should be a continuous loop, waiting for locks
  // to be released and then sending retry messages to those who
  // are waiting for it.
    while(1) {
        retry_entry retry_client;
        retry_queue.deq(&retry_client);
        rpcc *rpc_c = handle(retry_client.client_id).safebind();
        if(rpc_c) {
            int r;
            rpc_c->call(rlock_protocol::retry, retry_client.lid,
                retry_client.xid, r);
        }
    }
}


int lock_server_cache_rsm::acquire(lock_protocol::lockid_t lid, std::string id, 
             lock_protocol::xid_t xid, int &)
{
  lock_protocol::status ret = lock_protocol::OK;
  std::map<lock_protocol::lockid_t, lock_stat>::iterator it;
  std::string client_to_revoke = "";

  pthread_mutex_lock(&lock_stat_map_lock);

  if(!rsm->amiprimary()) {
      ret = lock_protocol::RPCERR;
  } else {
      it = lock_stat_map.find(lid);
      if(it == lock_stat_map.end()) {
          it = lock_stat_map.insert(std::make_pair(lid, lock_stat())).first;
      }
      lock_stat &cur_lock = it->second;
      client_xid_map::iterator xid_it = cur_lock.highest_xid_from_client.find(id);

      if(xid_it == cur_lock.highest_xid_from_client.end() ||
          xid_it->second < xid) {
          cur_lock.highest_xid_release_reply.erase(id);
          xid_it->second = xid;
          if(it->second.holded) {
              ret = lock_protocol::RETRY;
              it->second.waiter.insert(id);

              if(!it->second.revoke) {
                  it->second.revoke = true;
                  revoke_queue.enq(revoke_entry(it->second.holder, lid,
                          cur_lock.highest_xid_from_client[it->second.holder]));
              }
          } else {
              ret = lock_protocol::OK;
              it->second.holded = true;
              it->second.holder = id;
              it->second.revoke = false;
              it->second.waiter.erase(id);        

              if(!it->second.waiter.empty()) {
                  it->second.revoke = true;
                  revoke_queue.enq(revoke_entry(id, lid, xid));
              }
          }
          cur_lock.highest_xid_acquire_reply[id] = ret;
      } else if(xid_it->second == xid) {
          ret = cur_lock.highest_xid_acquire_reply[id];
      } else {
          tprintf("ERROR acquire with wrong xid\n");
          ret = lock_protocol::RPCERR;
      }
  }

  pthread_mutex_unlock(&lock_stat_map_lock);
  return ret;
}

int 
lock_server_cache_rsm::release(lock_protocol::lockid_t lid, std::string id, 
         lock_protocol::xid_t xid, int &r)
{
  lock_protocol::status ret = lock_protocol::OK;
  std::map<lock_protocol::lockid_t, lock_stat>::iterator it;
  std::string client_to_wake_up;

  pthread_mutex_lock(&lock_stat_map_lock);

  if(!rsm->amiprimary()) {

      ret = lock_protocol::RPCERR;

  } else {
      it = lock_stat_map.find(lid);

      if(it == lock_stat_map.end()) {
          tprintf("ERROR try to release a lock that doesn't exist\n")
              ret = lock_protocol::NOENT;
      } else {
          lock_stat &cur_lock = it->second;
          client_xid_map::iterator xid_it = cur_lock.highest_xid_from_client.find(id);
          if(xid_it == cur_lock.highest_xid_from_client.end()) {
              tprintf("ERROR try to release a lock that haven't recorded\n"); 
              ret = lock_protocol::RPCERR;
          } else if(xid < xid_it->second) {
              tprintf("ERROR try to release a lock with a old xid\n");
              ret = lock_protocol::RPCERR;
          } else {
              client_reply_map::iterator reply_it = cur_lock.highest_xid_release_reply.find(id);
              if(reply_it == cur_lock.highest_xid_release_reply.end()) {

                  cur_lock.holded = false;
                  cur_lock.holder = "";
                  cur_lock.highest_xid_release_reply.insert(std::make_pair(id, xid));
                  if(!cur_lock.waiter.empty()) {
                      std::string retryer = *cur_lock.waiter.begin();
                      retry_queue.enq(retry_entry(retryer, lid,
                              cur_lock.highest_xid_from_client[retryer]));
                  }

              } else {
                  ret = reply_it->second;
              }

          }

      }
  }

  pthread_mutex_unlock(&lock_stat_map_lock);
  return ret;
}

std::string
lock_server_cache_rsm::marshal_state()
{
  std::ostringstream ost;
  std::string r;

  pthread_mutex_lock(&lock_stat_map_lock);

  marshall rep;
  unsigned int lock_stat_map_size = lock_stat_map.size();
  rep << lock_stat_map_size;
  std::map<lock_protocol::lockid_t, lock_stat>::iterator it;

  for(it = lock_stat_map.begin(); it != lock_stat_map.end(); it++) {

      lock_protocol::lockid_t lid = it->first;
      rep << lid;

      lock_stat &cur_lock = it->second;
      rep << cur_lock.holded;
      rep << cur_lock.revoke;
      rep << cur_lock.holder;
  //    rep << cur_lock.waiter;
      unsigned int waiter_size = cur_lock.waiter.size();
      rep << waiter_size;
      std::set<std::string>::iterator waiter_it;

      for(waiter_it = cur_lock.waiter.begin();
                    waiter_it != cur_lock.waiter.end();
                            waiter_it++)
          rep << *waiter_it;

      unsigned int highest_xid_from_client_size = 
                cur_lock.highest_xid_from_client.size();
      rep << highest_xid_from_client_size;
      client_xid_map::iterator highest_xid_it;

      for(highest_xid_it = cur_lock.highest_xid_from_client.begin();
            highest_xid_it != cur_lock.highest_xid_from_client.end();
                                                highest_xid_it++) {
          rep << highest_xid_it->first;
          rep << highest_xid_it->second;
      }

      unsigned int highest_xid_acquire_reply_size = 
                    cur_lock.highest_xid_acquire_reply.size();
      rep << highest_xid_acquire_reply_size;
      client_reply_map::iterator highest_acquire_it;

      for(highest_acquire_it = cur_lock.highest_xid_acquire_reply.begin();
            highest_acquire_it != cur_lock.highest_xid_acquire_reply.end();
                    highest_acquire_it++) {
          rep << highest_acquire_it->first;
          rep << highest_acquire_it->second;
      }

      unsigned int highest_xid_release_reply_size =
                    cur_lock.highest_xid_release_reply.size();
      rep << highest_xid_release_reply_size;
      client_reply_map::iterator highest_release_it;

      for(highest_release_it = cur_lock.highest_xid_release_reply.begin();
            highest_release_it != cur_lock.highest_xid_release_reply.end();
                    highest_release_it++) {
          rep << highest_release_it->first;
          rep << highest_release_it->second;
      }

  }
  r = rep.str();
  pthread_mutex_unlock(&lock_stat_map_lock);

  return r;
}

void
lock_server_cache_rsm::unmarshal_state(std::string state)
{
    unmarshall rep(state);
    unsigned int lock_stat_map_size, cur_lock_index;

    pthread_mutex_lock(&lock_stat_map_lock);

    tprintf("y:about to unmarshal_state\n");

    rep >> lock_stat_map_size;
    for(cur_lock_index = 0; cur_lock_index < lock_stat_map_size;
                                            cur_lock_index++) {
        lock_protocol::lockid_t lid;
        rep >> lid;
        lock_stat cur_lock;
        rep >> cur_lock.holded;
        rep >> cur_lock.revoke;
        rep >> cur_lock.holder;
        tprintf("y:unmarshal here OK\n");

        unsigned int waiter_size;
        std::string tmp_waiter;
        rep >> waiter_size;
        while(waiter_size--) {
            rep >> tmp_waiter;
            cur_lock.waiter.insert(tmp_waiter);
        }

        unsigned int highest_xid_from_client_size;
        rep >> highest_xid_from_client_size;
        lock_protocol::xid_t tmp_xid;
        std::string tmp_id;
        while(highest_xid_from_client_size--) {
            rep >> tmp_id;
            rep >> tmp_xid;
            cur_lock.highest_xid_from_client.insert
                                (std::make_pair(tmp_id, tmp_xid));
        }

        unsigned int highest_xid_acquire_reply_size;
        rep >> highest_xid_acquire_reply_size;
        int tmp_acquire_xid;
        while(highest_xid_acquire_reply_size--) {
            rep >> tmp_id;
            rep >> tmp_acquire_xid;
            cur_lock.highest_xid_acquire_reply.insert
                                (std::make_pair(tmp_id, tmp_acquire_xid));
        }

        unsigned int highest_xid_release_reply_size;
        rep >> highest_xid_release_reply_size;
        int tmp_release_xid;
        while(highest_xid_release_reply_size--) {
            rep >> tmp_id;
            rep >> tmp_release_xid;
            cur_lock.highest_xid_release_reply.insert
                                (std::make_pair(tmp_id, tmp_release_xid));
        }

        lock_stat_map.insert(std::make_pair(lid, cur_lock));
    }

    pthread_mutex_unlock(&lock_stat_map_lock);
}

lock_protocol::status
lock_server_cache_rsm::stat(lock_protocol::lockid_t lid, int &r)
{
  printf("stat request\n");
  r = nacquire;
  return lock_protocol::OK;
}
