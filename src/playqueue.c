/*
 *  Playqueue
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

#include "showtime.h"
#include "navigator.h"
#include "backend/backend.h"
#include "playqueue.h"
#include "media.h"
#include "notifications.h"

static int shuffle_lfg;

static int playqueue_shuffle_mode = 0;

static int playqueue_repeat_mode = 0;

#define PLAYQUEUE_URL "playqueue:"

static prop_t *playqueue_root;
static prop_t *playqueue_nodes;

static void *player_thread(void *aux);

static media_pipe_t *playqueue_mp;


/**
 *
 */
static hts_mutex_t playqueue_mutex;


TAILQ_HEAD(playqueue_entry_queue, playqueue_entry);

static struct playqueue_entry_queue playqueue_entries;
static struct playqueue_entry_queue playqueue_shuffled_entries;
static int playqueue_length;

playqueue_entry_t *pqe_current;


/**
 *
 */
static prop_t *playqueue_source; 
static prop_sub_t *playqueue_source_sub;
static struct playqueue_entry_queue playqueue_source_entries;
static prop_t *playqueue_startme;

static void update_pq_meta(void);

/**
 *
 */
static void
pqe_unref(playqueue_entry_t *pqe)
{
  if(atomic_add(&pqe->pqe_refcount, -1) != 1)
    return;

  assert(pqe->pqe_linked == 0);
  assert(pqe->pqe_originator == NULL);
  assert(pqe->pqe_urlsub == NULL);
  assert(pqe->pqe_typesub == NULL);

  free(pqe->pqe_url);
  if(pqe->pqe_originator != NULL)
    prop_ref_dec(pqe->pqe_originator);

  prop_destroy(pqe->pqe_node);

  free(pqe);
}

/**
 *
 */
static void
pqe_ref(playqueue_entry_t *pqe)
{
  atomic_add(&pqe->pqe_refcount, 1);
}

/**
 *
 */
static void
pqe_event_dtor(event_t *e)
{
  playqueue_event_t *pe = (playqueue_event_t *)e;
  if(pe->pe_pqe != NULL)
    pqe_unref(pe->pe_pqe);
  free(e);
}

/**
 *
 */
static event_t *
pqe_event_create(playqueue_entry_t *pqe, int jump)
{
   playqueue_event_t *e;

   e = event_create(jump ? EVENT_PLAYQUEUE_JUMP : EVENT_PLAYQUEUE_ENQ,
		    sizeof(playqueue_event_t));
   e->h.e_dtor = pqe_event_dtor;

   e->pe_pqe = pqe;
   pqe_ref(pqe);

   return &e->h;
}


/**
 *
 */
static void
pqe_play(playqueue_entry_t *pqe, int jump)
{
  event_t *e = pqe_event_create(pqe, jump);
  mp_enqueue_event(playqueue_mp, e);
  event_unref(e);
}


/**
 *
 */
static void
pqe_unsubscribe(playqueue_entry_t *pqe)
{
  if(pqe->pqe_urlsub != NULL) {
    prop_unsubscribe(pqe->pqe_urlsub);
    pqe->pqe_urlsub = NULL;
  }

  if(pqe->pqe_typesub != NULL) {
    prop_unsubscribe(pqe->pqe_typesub);
    pqe->pqe_typesub = NULL;
  }
}


/**
 *
 */
static void
pqe_remove_from_sourcequeue(playqueue_entry_t *pqe)
{
  TAILQ_REMOVE(&playqueue_source_entries, pqe, pqe_source_link);

  pqe_unsubscribe(pqe);

  prop_ref_dec(pqe->pqe_originator);
  pqe->pqe_originator = NULL;

  pqe_unref(pqe);
}


/**
 *
 */
static void
pq_renumber(playqueue_entry_t *pqe)
{
  int num;

  pqe = pqe ? TAILQ_PREV(pqe, playqueue_entry_queue, pqe_linear_link) : NULL;

  if(pqe == NULL) {
    pqe = TAILQ_FIRST(&playqueue_entries);
    num = 1;
  } else {
    num = pqe->pqe_index + 1;
    pqe = TAILQ_NEXT(pqe, pqe_linear_link);
  }

  for(; pqe != NULL; pqe = TAILQ_NEXT(pqe, pqe_linear_link)) {
    pqe->pqe_index = num++;
  }
}



/**
 *
 */
static void
pqe_remove_from_globalqueue(playqueue_entry_t *pqe)
{
  playqueue_entry_t *next = TAILQ_NEXT(pqe, pqe_linear_link);

  assert(pqe->pqe_linked == 1);
  prop_unparent(pqe->pqe_node);
  playqueue_length--;
  TAILQ_REMOVE(&playqueue_entries, pqe, pqe_linear_link);
  TAILQ_REMOVE(&playqueue_shuffled_entries, pqe, pqe_shuffled_link);
  pqe->pqe_linked = 0;
  pqe_unref(pqe);
  if(next != NULL)
    pq_renumber(next);
  update_pq_meta();
}



/**
 *
 */
static void
playqueue_clear(void)
{
  playqueue_entry_t *pqe;

  if(playqueue_source != NULL) {
    prop_destroy(playqueue_source);
    playqueue_source = NULL;
  }

  if(playqueue_source_sub != NULL) {
    prop_unsubscribe(playqueue_source_sub);
    playqueue_source_sub = NULL;
  }

  while((pqe = TAILQ_FIRST(&playqueue_source_entries)) != NULL)
    pqe_remove_from_sourcequeue(pqe);

  while((pqe = TAILQ_FIRST(&playqueue_entries)) != NULL)
    pqe_remove_from_globalqueue(pqe);

  if(playqueue_startme != NULL) {
    prop_ref_dec(playqueue_startme);
    playqueue_startme = NULL;
  }
}


/**
 *
 */
static void
pqe_insert_shuffled(playqueue_entry_t *pqe)
{
  int v;
  playqueue_entry_t *n;

  shuffle_lfg = shuffle_lfg * 1664525 + 1013904223;

  playqueue_length++;
  v = (unsigned int)shuffle_lfg % playqueue_length;

  n = TAILQ_FIRST(&playqueue_shuffled_entries);

  for(; n != NULL && v >= 0; v--) {
    n = TAILQ_NEXT(n, pqe_shuffled_link);
  }

  if(n != NULL) {
    TAILQ_INSERT_BEFORE(n, pqe, pqe_shuffled_link);
  } else {
    TAILQ_INSERT_TAIL(&playqueue_shuffled_entries, pqe, pqe_shuffled_link);
  }
}


/**
 *
 */
static void
source_set_url(void *opaque, const char *str)
{
  playqueue_entry_t *pqe = opaque;

  if(str == NULL)
    return;

  free(pqe->pqe_url);
  pqe->pqe_url = strdup(str);

  if(pqe->pqe_startme) {
    pqe_play(pqe, 1);
    pqe->pqe_startme = 0;
  }
}


/**
 *
 */
static void
source_set_type(void *opaque, const char *str)
{
  playqueue_entry_t *pqe = opaque;

  if(str != NULL)
    pqe->pqe_playable = !strcmp(str, "audio") || !strcmp(str, "track");
}


/**
 *
 */
static playqueue_entry_t *
find_source_entry_by_prop(prop_t *p)
{
  playqueue_entry_t *pqe;

  TAILQ_FOREACH(pqe, &playqueue_source_entries, pqe_source_link)
    if(pqe->pqe_originator == p)
      return pqe;
  return NULL;
}


/**
 *
 */
static void
add_from_source(prop_t *p, playqueue_entry_t *before)
{
  playqueue_entry_t *pqe;

  pqe = calloc(1, sizeof(playqueue_entry_t));
  prop_ref_inc(p);
  pqe->pqe_refcount = 1;
  pqe->pqe_originator = p;

  if(p == playqueue_startme) {
    pqe->pqe_startme = 1;
    prop_ref_dec(playqueue_startme);
    playqueue_startme = NULL;
  }

  /**
   * We assume it's playable until we know better (see source_set_type)
   */
  pqe->pqe_playable = 1;

  if(before != NULL) {
    TAILQ_INSERT_BEFORE(before, pqe, pqe_source_link);
  } else {
    TAILQ_INSERT_TAIL(&playqueue_source_entries, pqe, pqe_source_link);
  }

  prop_ref_inc(p);

  pqe->pqe_node = prop_create(NULL, NULL);
  prop_link(p, pqe->pqe_node);

  pqe->pqe_urlsub = 
    prop_subscribe(0,
		   PROP_TAG_NAME("self", "url"),
		   PROP_TAG_CALLBACK_STRING, source_set_url, pqe,
		   PROP_TAG_MUTEX, &playqueue_mutex,
		   PROP_TAG_NAMED_ROOT, p, "self",
		   NULL);

  pqe->pqe_typesub = 
    prop_subscribe(0,
		   PROP_TAG_NAME("self", "type"),
		   PROP_TAG_CALLBACK_STRING, source_set_type, pqe,
		   PROP_TAG_MUTEX, &playqueue_mutex,
		   PROP_TAG_NAMED_ROOT, p, "self",
		   NULL);


  pqe_ref(pqe); // Ref for global queue

  pqe->pqe_linked = 1;
  if(before != NULL) {
    assert(before->pqe_linked == 1);
    TAILQ_INSERT_BEFORE(before, pqe, pqe_linear_link);
  } else {
    TAILQ_INSERT_TAIL(&playqueue_entries, pqe, pqe_linear_link);
  }
  pq_renumber(pqe);

  pqe_insert_shuffled(pqe);
  update_pq_meta();

  if(prop_set_parent_ex(pqe->pqe_node, playqueue_nodes, 
			before ? before->pqe_node : NULL, NULL))
    abort();
}


/**
 *
 */
static void
del_from_source(playqueue_entry_t *pqe)
{
  pqe_remove_from_sourcequeue(pqe);
  if(pqe->pqe_linked)
    pqe_remove_from_globalqueue(pqe);
}


/**
 *
 */
static void
move_track(playqueue_entry_t *pqe, playqueue_entry_t *before)
{
  playqueue_length--; // pqe_insert_shuffled() will increase it

  TAILQ_REMOVE(&playqueue_source_entries, pqe, pqe_source_link);
  TAILQ_REMOVE(&playqueue_entries, pqe, pqe_linear_link);
  TAILQ_REMOVE(&playqueue_shuffled_entries, pqe, pqe_shuffled_link);
  
  if(before != NULL) {
    TAILQ_INSERT_BEFORE(before, pqe, pqe_source_link);
  } else {
    TAILQ_INSERT_TAIL(&playqueue_source_entries, pqe, pqe_source_link);
  }

  if(before != NULL) {
    TAILQ_INSERT_BEFORE(before, pqe, pqe_linear_link);
  } else {
    TAILQ_INSERT_TAIL(&playqueue_entries, pqe, pqe_linear_link);
  }

  pq_renumber(NULL);

  pqe_insert_shuffled(pqe);

  prop_move(pqe->pqe_node, before ? before->pqe_node : NULL);

  update_pq_meta();
}


/**
 *
 */
static void
siblings_populate(void *opaque, prop_event_t event, ...)
{
  prop_t *p;
  playqueue_entry_t *pqe;

  va_list ap;
  va_start(ap, event);

  switch(event) {
  case PROP_ADD_CHILD:
    p = va_arg(ap, prop_t *);
    add_from_source(p, NULL);
    break;

 case PROP_ADD_CHILD_BEFORE:
    p = va_arg(ap, prop_t *);
    pqe = find_source_entry_by_prop(va_arg(ap, prop_t *));
    assert(pqe != NULL);
    add_from_source(p, pqe);
    break;
    
  case PROP_SET_DIR:
  case PROP_SET_VOID:
    break;

  case PROP_DEL_CHILD:
    pqe = find_source_entry_by_prop(va_arg(ap, prop_t *));
    assert(pqe != NULL);
    del_from_source(pqe);
    break;

  case PROP_MOVE_CHILD:
    pqe  = find_source_entry_by_prop(va_arg(ap, prop_t *));
    assert(pqe  != NULL);
    move_track(pqe, find_source_entry_by_prop(va_arg(ap, prop_t *)));
    break;

  case PROP_REQ_DELETE_MULTI:
    break;

  default:
    fprintf(stderr, "siblings_populate(): Can't handle event %d, aborting\n",
	    event);
    abort();
  }
}


/**
 *
 */
void
playqueue_load_with_source(prop_t *track, prop_t *source)
{
  playqueue_entry_t *pqe;

  hts_mutex_lock(&playqueue_mutex);

  TAILQ_FOREACH(pqe, &playqueue_entries, pqe_linear_link) {
    if(pqe->pqe_originator == track) {
      pqe_play(pqe, 1);
      hts_mutex_unlock(&playqueue_mutex);
      return;
    }
  }

  playqueue_clear();

  playqueue_startme = track;
  prop_ref_inc(playqueue_startme);

  playqueue_source_sub = 
    prop_subscribe(0,
		   PROP_TAG_NAME("self", "nodes"),
		   PROP_TAG_CALLBACK, siblings_populate, NULL,
		   PROP_TAG_MUTEX, &playqueue_mutex,
		   PROP_TAG_NAMED_ROOT, source, "self", 
		   NULL);

  playqueue_source = prop_xref_addref(source);

  hts_mutex_unlock(&playqueue_mutex);
}


/**
 *
 */
static void
playqueue_enqueue(prop_t *track)
{
  playqueue_entry_t *pqe, *before;
  prop_t *p;
  char url[URL_MAX];

  p = prop_get_by_name(PNVEC("self", "url"), 1,
		       PROP_TAG_NAMED_ROOT, track, "self",
		       NULL);
  
  if(prop_get_string(p, url, sizeof(url))) {
    prop_ref_dec(p);
    return;
  }
  prop_ref_dec(p);

  hts_mutex_lock(&playqueue_mutex);

  pqe = calloc(1, sizeof(playqueue_entry_t));
  pqe->pqe_url = strdup(url);

  pqe->pqe_node = prop_create(NULL, NULL);
  pqe->pqe_enq = 1;
  pqe->pqe_refcount = 1;
  pqe->pqe_linked = 1;
  pqe->pqe_playable = 1;

  prop_link_ex(prop_create(track, "metadata"),
	       prop_create(pqe->pqe_node, "metadata"),
	       NULL, 1);

  prop_set_string(prop_create(pqe->pqe_node, "url"), url);
  prop_set_string(prop_create(pqe->pqe_node, "type"), "audio");

  before = TAILQ_NEXT(pqe_current, pqe_linear_link);

  /* Skip past any previously enqueued entries */
  while(before != NULL && before->pqe_enq)
    before = TAILQ_NEXT(before, pqe_linear_link);

  if(before == NULL) {
    TAILQ_INSERT_TAIL(&playqueue_entries, pqe, pqe_linear_link);

    if(prop_set_parent(pqe->pqe_node, playqueue_nodes))
      abort();

  } else {
    TAILQ_INSERT_BEFORE(before, pqe, pqe_linear_link);

    if(prop_set_parent_ex(pqe->pqe_node, playqueue_nodes,
			  before->pqe_node, NULL))
      abort();
  }
  pq_renumber(pqe);
  pqe_insert_shuffled(pqe);

  update_pq_meta();

  hts_mutex_unlock(&playqueue_mutex);
}


/**
 * Load playqueue based on the given url.
 *
 * This function is responsible for freeing (or using) the
 * supplied meta prop tree.
 *
 * If enq is set we don't clear the playqueue, instead we insert the
 * entry after the current track (or after the last enqueued track)
 *
 * That way users may 'stick in' track in the current playqueue
 */
void
playqueue_play(const char *url, prop_t *metadata)
{
  playqueue_entry_t *pqe;

  hts_mutex_lock(&playqueue_mutex);

  pqe = calloc(1, sizeof(playqueue_entry_t));
  pqe->pqe_url = strdup(url);

  pqe->pqe_node = prop_create(NULL, NULL);
  pqe->pqe_refcount = 1;
  pqe->pqe_linked = 1;
  pqe->pqe_playable = 1;
  if(prop_set_parent(metadata, pqe->pqe_node))
    abort();

  prop_set_string(prop_create(pqe->pqe_node, "url"), url);
  prop_set_string(prop_create(pqe->pqe_node, "type"), "audio");

  /* Clear out the current playqueue */
  playqueue_clear();

  /* Enqueue our new entry */
  TAILQ_INSERT_TAIL(&playqueue_entries, pqe, pqe_linear_link);
  pq_renumber(pqe);
  pqe_insert_shuffled(pqe);
  update_pq_meta();
  if(prop_set_parent(pqe->pqe_node, playqueue_nodes))
    abort();

  /* Tick player to play it */
  pqe_play(pqe, 1);
  hts_mutex_unlock(&playqueue_mutex);
}


/**
 *
 */
static void
playqueue_set_shuffle(void *opaque, int v)
{
  playqueue_shuffle_mode = v;
  TRACE(TRACE_DEBUG, "playqueue", "Shuffle set to %s", v ? "on" : "off");
  update_pq_meta();
}


/**
 *
 */
static void
playqueue_set_repeat(void *opaque, int v)
{
  playqueue_repeat_mode = v;
  TRACE(TRACE_DEBUG, "playqueue", "Repeat set to %s", v ? "on" : "off");
  update_pq_meta();
}


/**
 *
 */
static void
pq_eventsink(void *opaque, prop_event_t event, ...)
{
  event_t *e;
  event_playtrack_t *ep;

  va_list ap;
  va_start(ap, event);

  if(event != PROP_EXT_EVENT)
    return;

  e = va_arg(ap, event_t *);
  if(!event_is_type(e, EVENT_PLAYTRACK))
    return;

  ep = (event_playtrack_t *)e;
  if(ep->source == NULL)
    playqueue_enqueue(ep->track);
  else
    playqueue_load_with_source(ep->track, ep->source);
}


/**
 *
 */
static int
playqueue_init(void)
{
  shuffle_lfg = time(NULL);

  hts_mutex_init(&playqueue_mutex);

  playqueue_mp = mp_create("playqueue", "tracks", 0);

  TAILQ_INIT(&playqueue_entries);
  TAILQ_INIT(&playqueue_source_entries);
  TAILQ_INIT(&playqueue_shuffled_entries);

  prop_set_int(playqueue_mp->mp_prop_canShuffle, 1);
  prop_set_int(playqueue_mp->mp_prop_canRepeat, 1);

  prop_subscribe(0,
		 PROP_TAG_NAME("self", "shuffle"),
		 PROP_TAG_CALLBACK_INT, playqueue_set_shuffle, NULL,
		 PROP_TAG_NAMED_ROOT, playqueue_mp->mp_prop_root, "self",
		 NULL);

  prop_subscribe(0,
		 PROP_TAG_NAME("self", "repeat"),
		 PROP_TAG_CALLBACK_INT, playqueue_set_repeat, NULL,
		 PROP_TAG_NAMED_ROOT, playqueue_mp->mp_prop_root, "self",
		 NULL);

  playqueue_root = prop_create(prop_get_global(), "playqueue");
  playqueue_nodes = prop_create(playqueue_root, "nodes");

  hts_thread_create_detached("audioplayer", player_thread, NULL);

  prop_subscribe(0,
		 PROP_TAG_NAME("playqueue", "eventsink"),
		 PROP_TAG_CALLBACK, pq_eventsink, NULL,
		 PROP_TAG_ROOT, playqueue_root,
		 NULL);
  return 0;
}



nav_page_t *
playqueue_open(struct navigator *nav, const char *view)
{
  nav_page_t *n;
  prop_t *src, *metadata;

  n = nav_page_create(nav, "playqueue:", view, sizeof(nav_page_t),
		      NAV_PAGE_DONT_CLOSE_ON_BACK);

  prop_set_string(prop_create(n->np_prop_root, "view"), "list");

  src = prop_create(n->np_prop_root, "source");
  prop_set_string(prop_create(src, "type"), "playqueue");

  metadata = prop_create(src, "metadata");
  prop_set_string(prop_create(metadata, "title"), "Playqueue");

  prop_link(playqueue_nodes, prop_create(src, "nodes"));
  return n;
}

/**
 *
 */
static nav_page_t *
be_playqueue_open(struct navigator *nav, const char *url0, const char *view,
		  char *errbuf, size_t errlen)
{
  return playqueue_open(nav, view);
}


/**
 *
 */
static int
be_playqueue_canhandle(const char *url)
{
  return !strncmp(url, PLAYQUEUE_URL, strlen(PLAYQUEUE_URL));
}



/**
 *
 */
static backend_t be_playqueue = {
  .be_init = playqueue_init,
  .be_canhandle = be_playqueue_canhandle,
  .be_open = be_playqueue_open,
};

BE_REGISTER(playqueue);

/**
 *
 */
static playqueue_entry_t *
playqueue_advance0(playqueue_entry_t *pqe, int reverse)
{
  playqueue_entry_t *cur = pqe;

  do {

    if(pqe->pqe_linked) {

      if(playqueue_shuffle_mode) {
	if(reverse) {
	  pqe = TAILQ_PREV(pqe, playqueue_entry_queue, pqe_shuffled_link);

	  if(playqueue_repeat_mode && pqe == NULL)
	    pqe = TAILQ_LAST(&playqueue_shuffled_entries,
			     playqueue_entry_queue);

	} else {
	  pqe = TAILQ_NEXT(pqe, pqe_shuffled_link);

	  if(playqueue_repeat_mode && pqe == NULL)
	    pqe = TAILQ_FIRST(&playqueue_shuffled_entries);
	}

      } else {

	if(reverse) {
	  pqe = TAILQ_PREV(pqe, playqueue_entry_queue, pqe_linear_link);

	  if(playqueue_repeat_mode && pqe == NULL)
	    pqe = TAILQ_LAST(&playqueue_entries, playqueue_entry_queue);

	} else {
	  pqe = TAILQ_NEXT(pqe, pqe_linear_link);

	  if(playqueue_repeat_mode && pqe == NULL)
	    pqe = TAILQ_FIRST(&playqueue_entries);
	}
      }

    } else {
      pqe = NULL;
    }
  } while(pqe != NULL && pqe != cur && pqe->pqe_playable == 0);
  return pqe;
}

/**
 *
 */
static void
update_pq_meta(void)
{
  media_pipe_t *mp = playqueue_mp;
  playqueue_entry_t *pqe = pqe_current;

  int can_skip_next = pqe && playqueue_advance0(pqe, 0);
  int can_skip_prev = pqe && playqueue_advance0(pqe, 1);

  prop_set_int(mp->mp_prop_canSkipForward,  can_skip_next);
  prop_set_int(mp->mp_prop_canSkipBackward, can_skip_prev);

  prop_set_int(prop_create(mp->mp_prop_root, "totalTracks"), playqueue_length);
  prop_t *p = prop_create(mp->mp_prop_root, "currentTrack");
  if(pqe != NULL)
    prop_set_int(p, pqe->pqe_index);
  else
    prop_set_void(p);
}


/**
 *
 */
static playqueue_entry_t *
playqueue_advance(playqueue_entry_t *pqe, int reverse)
{
  playqueue_entry_t *nxt;

  hts_mutex_lock(&playqueue_mutex);

  nxt = playqueue_advance0(pqe, reverse);

  if(nxt != NULL)
    pqe_ref(nxt);

  update_pq_meta();

  pqe_unref(pqe);

  hts_mutex_unlock(&playqueue_mutex);
  return nxt;
}


/**
 * Thread for actual playback
 */
static void *
player_thread(void *aux)
{
  media_pipe_t *mp = playqueue_mp;
  playqueue_entry_t *pqe = NULL;
  playqueue_event_t *pe;
  event_t *e;
  prop_t *p, *m;
  char errbuf[100];

  while(1) {
    
    while(pqe == NULL) {
      /* Got nothing to play, enter STOP mode */

      hts_mutex_lock(&playqueue_mutex);
      pqe_current = NULL;
      update_pq_meta();
      hts_mutex_unlock(&playqueue_mutex);

      prop_unlink(mp->mp_prop_metadata);

      /* Drain queues */
      e = mp_wait_for_empty_queues(mp, 0);
      if(e != NULL) {
	/* Got event while waiting for drain */
	mp_flush(mp);
      } else {
	/* Nothing and media queues empty. */

	TRACE(TRACE_DEBUG, "playqueue", "Nothing on queue, waiting");
	/* Make sure we no longer claim current playback focus */
	mp_set_url(mp, NULL);
	mp_shutdown(playqueue_mp);
    
	/* ... and wait for an event */
	e = mp_dequeue_event(playqueue_mp);
      }


      if(event_is_type(e, EVENT_PLAYQUEUE_JUMP) ||
	 event_is_type(e, EVENT_PLAYQUEUE_ENQ)) {
	pe = (playqueue_event_t *)e;
	pqe = pe->pe_pqe;
	pe->pe_pqe = NULL;

      } else if(event_is_action(e, ACTION_PLAY) ||
		event_is_action(e, ACTION_PLAYPAUSE)) {
	hts_mutex_lock(&playqueue_mutex);

	pqe = TAILQ_FIRST(&playqueue_entries);
	if(pqe != NULL)
	  pqe_ref(pqe);

	hts_mutex_unlock(&playqueue_mutex);
      }

      event_unref(e);
    }

    if(pqe->pqe_url == NULL) {
      notify_add(NOTIFY_ERROR, NULL, 5, "Playqueue error: An entry lacks URL");
      pqe = playqueue_advance(pqe, 0);
      continue;
    }

    p = prop_get_by_name(PNVEC("self", "metadata"), 1,
			 PROP_TAG_NAMED_ROOT, pqe->pqe_node, "self",
			 NULL);
    prop_link_ex(p, mp->mp_prop_metadata, NULL, 1);
    prop_ref_dec(p);


    m = prop_get_by_name(PNVEC("self", "media"), 1,
			 PROP_TAG_NAMED_ROOT, pqe->pqe_node, "self",
			 NULL);
    prop_link(mp->mp_prop_root, m);

    hts_mutex_lock(&playqueue_mutex);

    mp_set_url(mp, pqe->pqe_url);
    pqe_current = pqe;
    update_pq_meta();
    hts_mutex_unlock(&playqueue_mutex);

    p = prop_get_by_name(PNVEC("self", "playing"), 1,
			 PROP_TAG_NAMED_ROOT, pqe->pqe_node, "self",
			 NULL);

    prop_set_int(p, 1);

    e = backend_play_audio(pqe->pqe_url, mp, errbuf, sizeof(errbuf));

    prop_set_int(p, 0);
    prop_ref_dec(p);

    // Unlink $self.media
    prop_unlink(m);
    prop_ref_dec(m);

    if(e == NULL) {
      notify_add(NOTIFY_ERROR, NULL, 5, "URL: %s\nPlayqueue error: %s",
		 pqe->pqe_url, errbuf);
      pqe = playqueue_advance(pqe, 0);
      continue;
    }

    if(event_is_action(e, ACTION_PREV_TRACK)) {
      pqe = playqueue_advance(pqe, 1);

    } else if(event_is_action(e, ACTION_NEXT_TRACK) ||
	      event_is_type  (e, EVENT_EOF)) {
      mp_end(mp);

      pqe = playqueue_advance(pqe, 0);

    } else if(event_is_action(e, ACTION_STOP) ||
	      event_is_action(e, ACTION_EJECT)) {
      pqe_unref(pqe);
      pqe = NULL;

    } else if(event_is_type(e, EVENT_PLAYQUEUE_JUMP)) {
      pqe_unref(pqe);

      pe = (playqueue_event_t *)e;
      pqe = pe->pe_pqe;
      pe->pe_pqe = NULL; // Avoid deref upon event unref

    } else {
      abort();
    }
    event_unref(e);
  }
}


/**
 *
 */
void
playqueue_event_handler(event_t *e)
{
  if(event_is_action(e, ACTION_PLAY) ||
     event_is_action(e, ACTION_PLAYPAUSE))
    mp_enqueue_event(playqueue_mp, e);
}
