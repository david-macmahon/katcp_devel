#ifdef KATCP_EXPERIMENTAL

/* could use a two step endpoint multiplexer to achieve what a message does */ 

/* probably need a global endpoint run function */

/* may wish to have names for endpoints (ergh - avoidable as dangling pointers now less of an issue) */

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include <unistd.h>

#include <katcp.h>
#include <katpriv.h>
#include <katcl.h>

#define KATCP_MESSAGE_WACK    0x1 /* wants a reply, even if other side has gone away */

#define KATCP_EPFLAG_FREEABLE 0x1
#define KATCP_EPFLAG_OWNED    0x2
#define KATCP_EPFLAG_STALLED  0x4

#define KATCP_ENDPOINT_MAGIC 0x4c06dd5c

#ifdef KATCP_CONSISTENCY_CHECKS
void sane_endpoint_katcp(struct katcp_endpoint *ep)
{
  if(ep == NULL){
    fprintf(stderr, "received a null pointer, was expecting an endpoint\n");
    abort();
  }
  if(ep->e_magic != KATCP_ENDPOINT_MAGIC){
    fprintf(stderr, "bad magic field in endpoint 0x%x, was expecting 0x%x\n", ep->e_magic, KATCP_ENDPOINT_MAGIC);
    abort();
  }
}
#else
#define sane_endpoint_katcp(ep)
#endif


/* internals ************************************************************/

static struct katcp_message *create_message_katcp(struct katcp_dispatch *d, struct katcp_endpoint *from, struct katcp_endpoint *to, struct katcl_parse *px, int acknowledged);
static void destroy_message_katcp(struct katcp_dispatch *d, struct katcp_message *msg);
static int queue_message_katcp(struct katcp_dispatch *d, struct katcp_message *msg);

static void clear_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep);
static void free_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep);

void forget_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep);
void reference_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep);

/* setup/destroy routines for messages **********************************/

static struct katcp_message *create_message_katcp(struct katcp_dispatch *d, struct katcp_endpoint *from, struct katcp_endpoint *to, struct katcl_parse *px, int acknowledged)
{
  struct katcp_message *msg;

  msg = malloc(sizeof(struct katcp_message));
  if(msg == NULL){
    return NULL;
  }

  msg->m_flags = 0;
  if(acknowledged){
#ifdef KATCP_CONSISTENCY_CHECKS
    if(from == NULL){
      fprintf(stderr, "endpoint: acknowledged messages need a sender\n");
      abort();
    }
#endif
    msg->m_flags |= KATCP_MESSAGE_WACK;
  }

  msg->m_from  = from;
  reference_endpoint_katcp(d, from);

  msg->m_to    = to;
  reference_endpoint_katcp(d, to);

  msg->m_parse = copy_parse_katcl(px);
  if(msg->m_parse == NULL){
    destroy_message_katcp(d, msg);
    return NULL;
  }

  return msg;
}

static int queue_message_katcp(struct katcp_dispatch *d, struct katcp_message *msg)
{
  /* separation between create and queue functions to allow turning around for messages requiring a reply */

  struct katcp_endpoint *ep;

  ep = msg->m_to;
  if(ep == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "endpoint: no destination set\n");
    abort();
#endif
    return -1;
  }

  if(add_tail_gueue_katcl(ep->e_queue, msg)){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to queue message");
    return -1;
  }

  return 0;
}

static void destroy_message_katcp(struct katcp_dispatch *d, struct katcp_message *msg)
{
  if(msg == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "endpoint: attempting to destroy null message\n");
    abort();
#endif
    return;
  }

#ifdef KATCP_CONSISTENCY_CHECKS
  if(msg->m_flags & KATCP_MESSAGE_WACK){
    fprintf(stderr, "endpoint: expected an acknowledged request to be turned around, not destroyed\n");
    sleep(1);
  }
#endif

  if(msg->m_parse){
    destroy_parse_katcl(msg->m_parse);
    msg->m_parse = NULL;
  }

  if(msg->m_from){
    forget_endpoint_katcp(d, msg->m_from);
    msg->m_from = NULL;
  }

  if(msg->m_to){
    forget_endpoint_katcp(d, msg->m_to);
    msg->m_to = NULL;
  }

  free(msg);
}

/* message sending api function ***********************************************/

int send_message_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *from, struct katcp_endpoint *to, struct katcl_parse *px, int acknowledged)
{
  struct katcp_message *msg;

  msg = create_message_katcp(d, from, to, px, acknowledged);
  if(msg == NULL){
    return -1;
  }

  if(queue_message_katcp(d, msg) < 0){
    destroy_message_katcp(d, msg);
    return -1;
  }

  return 0;
}

/* endpoints ************************************************************/

/* setup/destroy routines for endpoints *********************************/

int init_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep, int (*wake)(struct katcp_dispatch *d, struct katcp_endpoint *ep, struct katcp_message *msg, void *data), void (*release)(struct katcp_dispatch *d, void *data), void *data);

struct katcp_endpoint *create_endpoint_katcp(struct katcp_dispatch *d, int (*wake)(struct katcp_dispatch *d, struct katcp_endpoint *ep, struct katcp_message *msg, void *data), void (*release)(struct katcp_dispatch *d, void *data), void *data)
{
  struct katcp_endpoint *ep;

  ep = malloc(sizeof(struct katcp_endpoint));
  if(ep == NULL){
    return NULL;
  }

  ep->e_flags = 0;
  
  ep->e_flags |= KATCP_EPFLAG_FREEABLE;

  /* TODO */

  if(init_endpoint_katcp(d, ep, wake, release, data) < 0){
    free_endpoint_katcp(d, ep);
    return NULL;
  }

  return ep;
}

int init_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep, int (*wake)(struct katcp_dispatch *d, struct katcp_endpoint *ep, struct katcp_message *msg, void *data), void (*release)(struct katcp_dispatch *d, void *data), void *data)
{
  struct katcp_shared *s;

  s = d->d_shared;

  ep->e_magic = KATCP_ENDPOINT_MAGIC;
  ep->e_refcount = 0; 

  ep->e_queue = create_gueue_katcl(NULL); /* TODO: may need a release function */

  if(ep->e_queue == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to initialise endpoint");
    return -1;
  }

  ep->e_wake    = wake;
  ep->e_release = release;
  ep->e_data    = data;

  if(wake != NULL){
    ep->e_flags |= KATCP_EPFLAG_OWNED;
  }

  ep->e_next = s->s_endpoints;
  s->s_endpoints = ep;

  return 0;
}

/* internal endpoint destruction ***************************************************/

static void clear_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep)
{
  sane_endpoint_katcp(ep);

#ifdef KATCP_CONSISTENCY_CHECKS
  if(ep->e_refcount > 0){
    fprintf(stderr, "endpoint: logic failure: attempting to clear endpoint which is still referenced\n");
    abort();
  }
#endif
  
  if(ep->e_queue){
#ifdef KATCP_CONSISTENCY_CHECKS
    if(size_gueue_katcl(ep->e_queue) > 0){
      fprintf(stderr, "endpoint: logic failure: attempting to clear endpoint which still has items in queue\n");
      abort();
    }
#endif
    destroy_gueue_katcl(ep->e_queue);
    ep->e_queue = NULL;
  }

  ep->e_wake = NULL;
  if(ep->e_release){
    (*(ep->e_release))(d, ep->e_data);
    ep->e_release = NULL;
  }
  ep->e_data = NULL;

  ep->e_next = NULL;

  return;
}

static void free_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep)
{
  clear_endpoint_katcp(d, ep);

  if(ep->e_flags & KATCP_EPFLAG_FREEABLE){
    ep->e_flags &= ~KATCP_EPFLAG_FREEABLE;
    free(ep);
#ifdef KATCP_CONSISTENCY_CHECKS
  } else {
    fprintf(stderr, "endpoint: logic failure: attempting to free an endpoint which is not freestanding\n");
    abort();
#endif
  }
}

/* logic for clients of endpoints ******************************************/

void reference_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep)
{
  sane_endpoint_katcp(ep);

  ep->e_refcount++;
}

void forget_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep)
{
  sane_endpoint_katcp(ep);

  if(ep->e_refcount > 0){
    ep->e_refcount--;
#ifdef KATCP_CONSISTENCY_CHECKS
  } else {
    fprintf(stderr, "endpoint: logic failure: releasing an endpoint which should be gone\n");
    abort();
#endif
  }

  /* do deallocation in global run_endpoints_katcp */
}

/* logic for owners of endpoints *******************************************/

int vturnaround_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep, struct katcp_message *msg, int code, char *fmt, va_list args)
{
  /* WARNING: msg should be removed from previous queues beforehand */
  int result;

  result = 0;

#ifdef KATCP_CONSISTENCY_CHECKS
  if(ep != msg->m_to){
    fprintf(stderr, "endpoint: problem: message in incorrect queue (%p != %p)\n", ep, msg->m_to);
    abort();
  }
#endif
  if(msg->m_flags & KATCP_MESSAGE_WACK){
#ifdef KATCP_CONSISTENCY_CHECKS
    if(msg->m_parse == NULL){
      fprintf(stderr, "endpoint: logic failure: message set to require acknowledgment but doesn't have a message\n");
      abort();
    }

    if(msg->m_from == NULL){
      fprintf(stderr, "endpoint: logic failure: no sender given for acknowledgement\n");
      abort();
    }
#endif

    /* return to sender */

    msg->m_parse = turnaround_extra_parse_katcl(msg->m_parse, code, fmt, args);
    if(msg->m_parse == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
      fprintf(stderr, "endpoint: unable to turnaround parse message\n");
      sleep(1);
#endif
      result = (-1);
    }

    forget_endpoint_katcp(d, msg->m_to);
    msg->m_to = msg->m_from;
    msg->m_from = NULL;

    msg->m_flags &= ~KATCP_MESSAGE_WACK;

    if(queue_message_katcp(d, msg) < 0){
      destroy_message_katcp(d, msg);
      result = (-1);
    }
  } else {
    destroy_message_katcp(d, msg);
  }

  return -1;
}

int turnaround_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep, struct katcp_message *msg, int code, char *fmt, ...)
{
  va_list args;
  int result;

  va_start(args, fmt);
  result = vturnaround_endpoint_katcp(d, ep, msg, code, fmt, args);
  va_end(args);

  return result;
}


void release_endpoint_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep)
{
  struct katcp_message *msg;

  sane_endpoint_katcp(ep);

  if(ep->e_flags & KATCP_EPFLAG_OWNED){
    ep->e_flags &= ~KATCP_EPFLAG_OWNED;
  } else {
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "endpoint: logic failure: attempting to release abandoned endpoint\n");
    abort();
#endif
    return;
  }

  ep->e_wake = NULL;
  ep->e_data = NULL;

  while((msg = remove_head_gueue_katcl(ep->e_queue)) != NULL){
    turnaround_endpoint_katcp(d, ep, msg, KATCP_RESULT_FAIL, "handler detached");
  }

  /* do actual cleanup in global run_endpoints */

}

void release_endpoints_katcp(struct katcp_dispatch *d)
{
  struct katcp_endpoint *ep;
  struct katcp_shared *s;

  s = d->d_shared;

  ep = s->s_endpoints;
  while(ep){
    if(ep->e_flags & KATCP_EPFLAG_OWNED){
      release_endpoint_katcp(d, ep);
    }
    ep = ep->e_next;
  }

  /* WARNING: may not be sufficient to run once ? */
  run_endpoints_katcp(d);

#ifdef KATCP_CONSISTENCY_CHECKS
  if(s->s_endpoints){
    fprintf(stderr, "endpoint: endpoints not collected at shutdown\n");
  }
#endif
}

void run_endpoints_katcp(struct katcp_dispatch *d)
{
  struct katcp_endpoint *ep, *ex;
  struct katcp_shared *s;
  struct katcp_message *msg, *msx;
  int result;

  s = d->d_shared;

  ex = NULL;
  ep = s->s_endpoints;
  while(ep){
    if((ep->e_flags & (KATCP_EPFLAG_OWNED | KATCP_EPFLAG_STALLED)) == KATCP_EPFLAG_OWNED){
      msg = get_head_gueue_katcl(ep->e_queue);
      if(msg != NULL){
        result = (*(ep->e_wake))(d, ep, msg, ep->e_data);
        switch(result & KATCP_MASK_ENDPOINT){
          case KATCP_ENDPOINT_OWN :
            /* all done internal to wake callback */
            break;
          case KATCP_ENDPOINT_FAIL :
          case KATCP_ENDPOINT_OK :
            msx = remove_head_gueue_katcl(ep->e_queue);
#ifdef KATCP_CONSISTENCY_CHECKS
            if(msx != msg){
              fprintf(stderr, "endpoint: major corruption in queue: get head returns %p, remove head returns %p\n", msg, msx);
              abort();
            }
#endif
            turnaround_endpoint_katcp(d, ep, msx, ((result & KATCP_MASK_ENDPOINT) == KATCP_ENDPOINT_OK) ? KATCP_RESULT_OK : KATCP_RESULT_FAIL, NULL);

            break;
          case KATCP_ENDPOINT_QUICK :
            /* unchanged, run again immediately */
            break;
          case KATCP_ENDPOINT_STALL :
            ep->e_flags |= KATCP_EPFLAG_STALLED;
            break;
          case KATCP_ENDPOINT_DEAD :
            ep->e_flags &= ~KATCP_EPFLAG_OWNED;
            break;
          default :
#ifdef KATCP_CONSISTENCY_CHECKS
            fprintf(stderr, "endpoint: bad return code %d from wake callback\n", result);
            abort();
#endif
            break;
        }
        if((result & KATCP_ENDPOINT_DEAD) == KATCP_ENDPOINT_DEAD){
          /* owner has gone away */
          release_endpoint_katcp(d, ep);
        }
      }
    }

    if((ep->e_flags & KATCP_EPFLAG_OWNED) || (ep->e_refcount > 0)){
      /* still something to do */
      ex = ep;
      ep = ep->e_next;
    } else {
      /* gone */
      ex->e_next = ep->e_next;

      if(ep->e_flags & KATCP_EPFLAG_FREEABLE){
        free_endpoint_katcp(d, ep);
      } else {
        clear_endpoint_katcp(d, ep);
      }

      ep = ex->e_next;
    }

  }

}

#endif
