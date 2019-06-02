/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  2016-2019, VU University Amsterdam
			      CWI, Amsterdam
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "pl-incl.h"
#include "pl-trie.h"
#include "pl-indirect.h"
#define AC_TERM_WALK_POP 1
#include "pl-termwalk.c"

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This file implements tries of  terms.  The   trie  itself  lives  in the
program space and is represented by a (symbol) handle. This implies that
tries are subject to garbage collection.

A path through a trie represents a  sequence of tokens. For representing
terms, these tokens are functor symbols,   variables  and atomic values.
The _value_ associated with a  term  always   appears  in  a _leaf_ node
because a sequence that represents a term   is  _never_ the prefix of of
the sequence of another term.

TODO
  - Limit size of the tries
  - Avoid using a hash-table for small number of branches
  - Thread safe reclaiming
    - Reclaim single-child node after moving to a hash
    - Make pruning the trie thread-safe
  - Provide deletion from a trie
  - Make trie_gen/3 take the known prefix into account
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define RESERVED_TRIE_VAL(n) (((word)(-(intptr_t)n)<<LMASK_BITS) | TAG_VAR)
#define TRIE_ERROR_VAL       RESERVED_TRIE_VAL(1)
#define TRIE_KEY_POP         RESERVED_TRIE_VAL(2)

#define NVARS_FAST 100

/* Will eventually be shared in pl-wam.c */
typedef enum
{ uread = 0,				/* Unification in read-mode */
  uwrite				/* Unification in write mode */
} unify_mode;

typedef struct ukey_state
{ trie	       *trie;			/* Trie for indirects */
  Word		ptr;			/* current location */
  unify_mode	umode;			/* unification mode */
  size_t	max_var_seen;
  size_t	vars_allocated;		/* # variables allocated */
  Word*		vars;
  Word		var_buf[NVARS_FAST];	/* quick var buffer */
} ukey_state;

static void	trie_destroy(trie *trie);
static int	unify_key(ukey_state *state, word key ARG_LD);
static void	init_ukey_state(ukey_state *state, trie *trie, Word p);
static void	destroy_ukey_state(ukey_state *state);


		 /*******************************
		 *	       SYMBOL		*
		 *******************************/

typedef struct tref
{ trie *trie;				/* represented trie */
} tref;

static int
write_trie_ref(IOSTREAM *s, atom_t aref, int flags)
{ tref *ref = PL_blob_data(aref, NULL, NULL);
  (void)flags;

  Sfprintf(s, "<trie>(%p)", ref->trie);
  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
GC a trie. Note that the  Prolog predicate trie_destroy/1 merely empties
the trie, leaving its destruction to the atom garbage collector.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
release_trie_ref(atom_t aref)
{ tref *ref = PL_blob_data(aref, NULL, NULL);
  trie *t;

  if ( (t=ref->trie) )
    trie_destroy(t);			/* can be called twice */

  return TRUE;
}


static int
save_trie(atom_t aref, IOSTREAM *fd)
{ tref *ref = PL_blob_data(aref, NULL, NULL);
  (void)fd;

  return PL_warning("Cannot save reference to <trie>(%p)", ref->trie);
}


static atom_t
load_trie(IOSTREAM *fd)
{ (void)fd;

  return PL_new_atom("<saved-trie-ref>");
}


static PL_blob_t trie_blob =
{ PL_BLOB_MAGIC,
  PL_BLOB_UNIQUE,
  "trie",
  release_trie_ref,
  NULL,
  write_trie_ref,
  NULL,
  save_trie,
  load_trie
};

		 /*******************************
		 *	     THE TRIE		*
		 *******************************/

static trie_node       *new_trie_node(trie *trie, word key);
static void		clear_vars(Word k, size_t var_number ARG_LD);
static void		destroy_node(trie *trie, trie_node *n);
static void		clear_node(trie *trie, trie_node *n, int dealloc);
static inline void	release_value(word value);


static inline void
acquire_key(word key)
{ if ( isAtom(key) )
    PL_register_atom(key);
}

static inline void
release_key(word key)
{ if ( isAtom(key) )
    PL_unregister_atom(key);
}


trie *
trie_create(void)
{ trie *trie;

  if ( (trie = PL_malloc(sizeof(*trie))) )
  { memset(trie, 0, sizeof(*trie));
    trie->magic = TRIE_MAGIC;

    return trie;
  } else
  { PL_resource_error("memory");
    return NULL;
  }
}


static void
trie_destroy(trie *trie)
{ DEBUG(MSG_TRIE_GC, Sdprintf("Destroying trie %p\n", trie));
  trie_empty(trie);
  PL_free(trie);
}


void
trie_empty(trie *trie)
{ trie->magic = TRIE_CMAGIC;

  if ( !trie->references )
  { indirect_table *it = trie->indirects;

    clear_node(trie, &trie->root, FALSE);	/* TBD: verify not accessed */
    if ( it && COMPARE_AND_SWAP(&trie->indirects, it, NULL) )
      destroy_indirect_table(it);
  }
}


void
trie_clean(trie *trie)
{ if ( trie->magic == TRIE_CMAGIC )
    trie_empty(trie);
}


static trie_node *
get_child(trie_node *n, word key ARG_LD)
{ trie_children children = n->children;

  if ( children.any )
  { switch( children.any->type )
    { case TN_KEY:
	if ( children.key->key == key )
	  return children.key->child;
        return NULL;
      case TN_HASHED:
	return lookupHTable(children.hash->table, (void*)key);
      default:
	assert(0);
    }
  }

  return NULL;
}


static trie_node *
new_trie_node(trie *trie, word key)
{ trie_node *n;

  if ( trie->alloc_pool )
  { if ( trie->alloc_pool->size+sizeof(trie_node) <= trie->alloc_pool->limit )
    { ATOMIC_ADD(&trie->alloc_pool->size, sizeof(trie_node));
    } else
    { PL_resource_error("table_space");
      return NULL;
    }
  }

  if ( (n = PL_malloc(sizeof(*n))) )
  { ATOMIC_INC(&trie->node_count);
    memset(n, 0, sizeof(*n));
    acquire_key(key);
    n->key = key;
  } else
  { PL_resource_error("memory");
  }

  return n;
}


static void
clear_node(trie *trie, trie_node *n, int dealloc)
{ trie_children children;

next:
  children = n->children;

  if ( trie->release_node )
    (*trie->release_node)(trie, n);

  release_key(n->key);
  if ( n->value )
    release_value(n->value);

  if ( children.any &&
       COMPARE_AND_SWAP(&n->children.any, children.any, NULL) )
  { if ( dealloc )
    { ATOMIC_DEC(&trie->node_count);
      if ( trie->alloc_pool )
	ATOMIC_SUB(&trie->alloc_pool->size, sizeof(trie_node));
      PL_free(n);
    }

    switch( children.any->type )
    { case TN_KEY:
      { n = children.key->child;
        PL_free(children.key);
	dealloc = TRUE;
	goto next;
      }
      case TN_HASHED:
      { Table table = children.hash->table;
	TableEnum e = newTableEnum(table);
	void *k, *v;

	PL_free(children.hash);

	while(advanceTableEnum(e, &k, &v))
	{ clear_node(trie, v, TRUE);
	}

	freeTableEnum(e);
	destroyHTable(table);
	break;
      }
    }
  }
}

static void
destroy_node(trie *trie, trie_node *n)
{ clear_node(trie, n, TRUE);
}


/*
 * Prune a branch of the trie that does not end in a node.  This should
 * be used after deletion or unsuccessful insertion, e.g., by trying to
 * insert a cyclic term
 *
 * TBD: Need to think about concurrency here.
 */

void
prune_node(trie *trie, trie_node *n)
{ trie_node *p;
  int empty = TRUE;

  for(; empty && n->parent; n = p)
  { trie_children children;

    p = n->parent;
    children = p->children;

    if ( children.any )
    { switch( children.any->type )
      { case TN_KEY:
	  if ( COMPARE_AND_SWAP(&p->children.any, children.any, NULL) )
	    PL_free(children.any);
	  break;
	case TN_HASHED:
	  deleteHTable(children.hash->table, (void*)n->key);
	  empty = children.hash->table->size == 0;
	  break;
      }
    }

    destroy_node(trie, n);
  }
}


static trie_node *
insert_child(trie *trie, trie_node *n, word key ARG_LD)
{ for(;;)
  { trie_children children = n->children;
    trie_node *new = new_trie_node(trie, key);

    if ( !new )
      return NULL;			/* resource error */

    if ( children.any )
    { switch( children.any->type )
      { case TN_KEY:
	{ if ( children.key->key == key )
	  { return children.key->child;
	  } else
	  { trie_children_hashed *hnode = PL_malloc(sizeof(*hnode));

	    hnode->type  = TN_HASHED;
	    hnode->table = newHTable(4);
	    addHTable(hnode->table, (void*)children.key->key,
				    children.key->child);
	    addHTable(hnode->table, (void*)key, (void*)new);

	    if ( COMPARE_AND_SWAP(&n->children.hash, children.hash, hnode) )
	    { PL_free(children.any);		/* TBD: Safely free */
	      new->parent = n;
	      return new;
	    }
	    destroy_node(trie, new);
	    destroyHTable(hnode->table);
	    PL_free(hnode);
	    continue;
	  }
	}
	case TN_HASHED:
	{ trie_node *old = addHTable(children.hash->table,
				     (void*)key, (void*)new);

	  if ( new == old )
	  { new->parent = n;
	  } else
	  { destroy_node(trie, new);
	  }
	  return old;
	}
	default:
	  assert(0);
      }
    } else
    { trie_children_key *child = PL_malloc(sizeof(*child));

      child->type  = TN_KEY;
      child->key   = key;
      child->child = new;

      if ( COMPARE_AND_SWAP(&n->children.key, NULL, child) )
      { child->child->parent = n;
	return child->child;
      }
      destroy_node(trie, new);
      PL_free(child);
    }
  }
}


static trie_node *
follow_node(trie *trie, trie_node *n, word value, int add ARG_LD)
{ trie_node *child;

  if ( (child=get_child(n, value PASS_LD)) )
    return child;

  if ( add )
    return insert_child(trie, n, value PASS_LD);
  else
    return NULL;
}


static word
trie_intern_indirect(trie *trie, word w, int add ARG_LD)
{ for(;;)
  { if ( trie->indirects )
    { return intern_indirect(trie->indirects, w, add PASS_LD);
    } else
    { indirect_table *newtab = new_indirect_table();

      if ( !COMPARE_AND_SWAP(&trie->indirects, NULL, newtab) )
	destroy_indirect_table(newtab);
    }
  }
}


#define TRIE_LOOKUP_CONTAINS_ATTVAR	-10
#define TRIE_LOOKUP_CYCLIC		-11

/* If there is an error, we prune the part that we have created.
 * We should only start the prune from a new node though.  To be sure
 * we do so we first add a new node.  As this is for exception handling
 * only, the performance loss is not vital.
 */

static void
prune_error(trie *trie, trie_node *node ARG_LD)
{ prune_node(trie, follow_node(trie, node, TRIE_ERROR_VAL, TRUE PASS_LD));
}


int
trie_lookup(trie *trie, trie_node **nodep, Word k, int add ARG_LD)
{ term_agenda_P agenda;
  trie_node *node = &trie->root;
  size_t var_number = 0;
  int rc = TRUE;
  int compounds = 0;

  initTermAgenda_P(&agenda, 1, k);
  while( node )
  { Word p;
    word w;

    if ( !(p=nextTermAgenda_P(&agenda)) )
      break;
    if ( p == AC_TERM_POP )
    { if ( !(node = follow_node(trie, node, TRIE_KEY_POP, add PASS_LD)) )
	break;
      continue;
    }

    w = *p;
    switch( tag(w) )
    { case TAG_VAR:
	if ( isVar(w) )
	  *p = w = ((((word)++var_number))<<LMASK_BITS)|TAG_VAR;
        node = follow_node(trie, node, w, add PASS_LD);
	break;
      case TAG_ATTVAR:
	rc = TRIE_LOOKUP_CONTAINS_ATTVAR;

        prune_error(trie, node PASS_LD);
        node = NULL;
        break;
      case TAG_COMPOUND:
      { Functor f = valueTerm(w);
        size_t arity = arityFunctor(f->definition);

	if ( add && ++compounds == 1000 && !is_acyclic(p PASS_LD) )
	{ rc = TRIE_LOOKUP_CYCLIC;
	  prune_error(trie, node PASS_LD);
	  node = NULL;
	} else
	{ node = follow_node(trie, node, f->definition, add PASS_LD);
	  pushWorkAgenda_P(&agenda, arity, f->arguments);
	}
	break;
      }
      default:
      { if ( !isIndirect(w) )
	{ node = follow_node(trie, node, w, add PASS_LD);
	} else
	{ word i = trie_intern_indirect(trie, w, add PASS_LD);

	  if ( i )
	    node = follow_node(trie, node, i, add PASS_LD);
	  else
	    node = NULL;
	}
      }
    }
  }
  clearTermAgenda_P(&agenda);
  clear_vars(k, var_number PASS_LD);

  if ( rc == TRUE )
  { if ( node )
      *nodep = node;
    else
      rc = FALSE;
  }

  return rc;
}


static void
clear_vars(Word k, size_t var_number ARG_LD)
{ if ( var_number > 0 )
  { term_agenda agenda;
    Word p;

    initTermAgenda(&agenda, 1, k);
    while( var_number > 0 && (p=nextTermAgenda(&agenda)) )
    { word w = *p;

      switch( tag(w) )
      { case TAG_VAR:
	{ if ( !isVar(*p) )
	  { setVar(*p);
	    --var_number;
	  }
	  break;
	}
        case TAG_COMPOUND:
	{ Functor f = valueTerm(w);
	  int arity = arityFunctor(f->definition);

	  pushWorkAgenda(&agenda, arity, f->arguments);
	  break;
	}
      }
    }
    clearTermAgenda(&agenda);

    assert(var_number == 0);
  }
}


trie *
get_trie_form_node(trie_node *node)
{ trie *trie_ptr;

  for( ; node->parent; node = node->parent )
    ;
  trie_ptr = (trie *)((char*)node - offsetof(trie, root));
  assert(trie_ptr->magic == TRIE_MAGIC);

  return trie_ptr;
}


int
is_ground_trie_node(trie_node *node)
{ for( ; node->parent; node = node->parent )
  { if ( tag(node->key) == TAG_VAR && node->key != TRIE_KEY_POP )
      return FALSE;
  }

  return TRUE;
}


		 /*******************************
		 *    BUILD TERM FROM PATH	*
		 *******************************/

#define MAX_FAST 256

int
unify_trie_term(trie_node *node, term_t term ARG_LD)
{ word fast[MAX_FAST];
  Word keys = fast;
  size_t keys_allocated = MAX_FAST;
  size_t kc = 0;
  int rc = TRUE;
  trie *trie_ptr;
  fid_t fid;
						/* get the keys */
  for( ; node->parent; node = node->parent )
  { if ( kc == keys_allocated )
    { keys_allocated *= 2;
      if ( keys == fast )
      { if ( (keys = malloc(sizeof(*keys)*keys_allocated)) )
	  memcpy(keys, fast, sizeof(fast));
	else
	  return PL_resource_error("memory");
      } else
      { Word newkeys;
	if ( !(newkeys=realloc(keys, sizeof(*keys)*keys_allocated)) )
	{ free(keys);
	  return PL_resource_error("memory");
	}
	keys = newkeys;
      }
    }

    keys[kc++] = node->key;
  }
  trie_ptr = (trie *)((char*)node - offsetof(trie, root));
  assert(trie_ptr->magic == TRIE_MAGIC);

  fid = PL_open_foreign_frame();
  for(;;)
  { ukey_state ustate;
    size_t i;

  retry:
    init_ukey_state(&ustate, trie_ptr, valTermRef(term));
    for(i=kc; i-- > 0; )
    { if ( (rc=unify_key(&ustate, keys[i] PASS_LD)) != TRUE )
      { destroy_ukey_state(&ustate);
	if ( rc == FALSE )
	  goto out;
	PL_rewind_foreign_frame(fid);
	if ( makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT) )
	  goto retry;
      }
    }
    destroy_ukey_state(&ustate);
    break;
  }

out:
  PL_close_foreign_frame(fid);
  if ( keys != fast )
    free(keys);

  return rc;
}


void *
map_trie_node(trie_node *n,
	      void* (*map)(trie_node *n, void *ctx), void *ctx)
{ trie_children children;
  void *rc;

next:
  children = n->children;

  if ( (rc=(*map)(n, ctx)) != NULL )
    return rc;

  if ( children.any  )
  { switch( children.any->type )
    { case TN_KEY:
      { n = children.key->child;
	goto next;
      }
      case TN_HASHED:
      { Table table = children.hash->table;
	TableEnum e = newTableEnum(table);
	void *k, *v;

	while(advanceTableEnum(e, &k, &v))
	{ if ( (rc=map_trie_node(v, map, ctx)) != NULL )
	  { freeTableEnum(e);
	    return rc;
	  }
	}

	freeTableEnum(e);
	break;
      }
    }
  }

  return NULL;
}


typedef struct trie_stats
{ size_t bytes;
  size_t nodes;
  size_t hashes;
  size_t values;
} trie_stats;


static void *
stat_node(trie_node *n, void *ctx)
{ trie_stats *stats = ctx;
  trie_children children = n->children;

  stats->nodes++;
  stats->bytes += sizeof(*n);
  if ( n->value )
    stats->values++;

  if ( children.any )
  { switch( children.any->type )
    { case TN_KEY:
	stats->bytes += sizeof(*children.key);
        break;
      case TN_HASHED:
	stats->bytes += sizeofTable(children.hash->table);
	stats->hashes++;
	break;
      default:
	assert(0);
    }
  }

  return NULL;
}


static void
stat_trie(trie *t, trie_stats *stats)
{ stats->bytes  = sizeof(*t) - sizeof(t->root);
  stats->nodes  = 0;
  stats->hashes = 0;
  stats->values = 0;

  acquire_trie(t);
  map_trie_node(&t->root, stat_node, stats);
  release_trie(t);
}




		 /*******************************
		 *	  PROLOG BINDING	*
		 *******************************/

atom_t
trie_symbol(trie *trie)
{ if ( !trie->symbol )
  { tref ref;
    int new;

    ref.trie = trie;
    trie->symbol = lookupBlob((void*)&ref, sizeof(ref),
			      &trie_blob, &new);
  }

  return trie->symbol;
}


trie *
symbol_trie(atom_t symbol)
{ void *data;
  size_t len;
  PL_blob_t *type;

  if ( (data = PL_blob_data(symbol, &len, &type)) && type == &trie_blob )
  { tref *ref = data;

    if ( ref->trie->magic == TRIE_MAGIC )
      return ref->trie;
  }

  assert(0);
  return NULL;
}


#define unify_trie(t, trie) unify_trie__LD(t, trie PASS_LD)

static int
unify_trie__LD(term_t t, trie *trie ARG_LD)
{ return PL_unify_atom(t, trie->symbol);
}

int
get_trie(term_t t, trie **tp)
{ void *data;
  PL_blob_t *type;

  if ( PL_get_blob(t, &data, NULL, &type) && type == &trie_blob )
  { tref *ref = data;

    if ( ref->trie->magic == TRIE_MAGIC )
    { *tp = ref->trie;
      return TRUE;
    }

    PL_existence_error("trie", t);
  } else
    PL_type_error("trie", t);

  return FALSE;
}


int
trie_error(int rc, term_t culprit)
{ switch(rc)
  { case TRIE_LOOKUP_CONTAINS_ATTVAR:
      return PL_type_error("free_of_attvar", culprit);
    case TRIE_LOOKUP_CYCLIC:
      return PL_type_error("acyclic_term", culprit);
    default:
      return FALSE;
  }
}

static
PRED_IMPL("trie_new", 1, trie_new, 0)
{ PRED_LD
  trie *trie;

  if ( (trie = trie_create()) )
  { atom_t symbol = trie_symbol(trie);
    int rc;

    rc = unify_trie(A1, trie);
    PL_unregister_atom(symbol);

    return rc;
  }

  return FALSE;
}


static
PRED_IMPL("is_trie", 1, is_trie, 0)
{ void *data;
  PL_blob_t *type;

  if ( PL_get_blob(A1, &data, NULL, &type) && type == &trie_blob )
  { tref *ref = data;

    if ( ref->trie->magic == TRIE_MAGIC )
      return TRUE;
  }

  return FALSE;
}


static
PRED_IMPL("trie_destroy", 1, trie_destroy, 0)
{ trie *trie;

  if ( get_trie(A1, &trie) )
  { trie_empty(trie);

    return TRUE;
  }

  return FALSE;
}


#define isRecord(w) (((w)&0x3) == 0)

static word
intern_value(term_t value ARG_LD)
{ Word vp = valTermRef(value);

  assert((TAG_INTEGER&0x3) && (TAG_ATOM&0x3));

  deRef(vp);
  if ( isAtom(*vp) || isTaggedInt(*vp) )
    return *vp;

  return (word)PL_record(value);
}


static inline void
release_value(word value)
{ if ( isAtom(value) )
    PL_unregister_atom(value);
  else if ( isRecord(value) )
    PL_erase((record_t)value);
}


static int
equal_value(word v1, word v2)
{ if ( v1 == v2 )
    return TRUE;

  if ( isRecord(v1) && isRecord(v2) )
    return variantRecords((record_t)v1, (record_t)v2);

  return FALSE;
}


static int
unify_value(term_t t, word value ARG_LD)
{ if ( !isRecord(value) )
  { return _PL_unify_atomic(t, value);
  } else
  { term_t t2;

    return ( (t2=PL_new_term_ref()) &&
	     PL_recorded((record_t)value, t2) &&
	     PL_unify(t, t2)
	   );
  }
}


int
put_trie_value(term_t t, trie_node *node ARG_LD)
{ if ( !isRecord(node->value) )
  { *valTermRef(t) = node->value;
    return TRUE;
  } else
  { return PL_recorded((record_t)node->value, t);
  }
}


int
set_trie_value_word(trie *trie, trie_node *node, word val)
{ if ( node->value )
  { if ( !equal_value(node->value, val) )
    { word old = node->value;

      acquire_key(val);
      node->value = val;
      release_value(old);

      return TRUE;
    } else
    { return FALSE;
    }
  } else
  { acquire_key(val);
    node->value = val;
    ATOMIC_INC(&trie->value_count);

    return TRUE;
  }
}

int
set_trie_value(trie *trie, trie_node *node, term_t value ARG_LD)
{ word val = intern_value(value PASS_LD);

  if ( !set_trie_value_word(trie, node, val) &&
       isRecord(val) )
    PL_erase((record_t)val);

  return TRUE;
}

void
trie_delete(trie *trie, trie_node *node, int prune)
{ if ( node->value )
  { if ( prune )
    { prune_node(trie, node);
    } else
    { word v = node->value;
      node->value = 0;
      release_value(v);
    }
    ATOMIC_DEC(&trie->value_count);
  }
}


/**
 * trie_insert(+Trie, +Key, +Value) is semidet.
 *
 * True if Key was added as a new   key  to the trie and associated with
 * Value. False if Key was already in the trie with Value
 *
 * @error permission_error if Key was associated with a different value
 */

static int
trie_insert(term_t Trie, term_t Key, term_t Value, trie_node **nodep,
	    int update ARG_LD)
{ trie *trie;

  if ( get_trie(Trie, &trie) )
  { Word kp;
    trie_node *node;
    int rc;

    kp	= valTermRef(Key);

    if ( (rc=trie_lookup(trie, &node, kp, TRUE PASS_LD)) == TRUE )
    { word val = intern_value(Value PASS_LD);

      if ( nodep )
	*nodep = node;

      if ( node->value )
      { if ( update )
	{ if ( !equal_value(node->value, val) )
	  { word old = node->value;

	    acquire_key(val);
	    node->value = val;
	    release_value(old);
	  } else if ( isRecord(val) )
	  { PL_erase((record_t)val);
	  }

	  return TRUE;
	} else
	{ if ( !equal_value(node->value, val) )
	    PL_permission_error("modify", "trie_key", Key);
	  if ( isRecord(val) )
	    PL_erase((record_t)val);

	  return FALSE;
	}
      }
      acquire_key(val);
      node->value = val;
      ATOMIC_INC(&trie->value_count);

      return TRUE;
    }

    return trie_error(rc, Key);
  }

  return FALSE;
}


/**
 * trie_insert(+Trie, +Key, +Value) is semidet.
 *
 * True if Key was added as a new   key  to the trie and associated with
 * Value. False if Key was already in the trie with Value
 *
 * @error permission_error if Key was associated with a different value
 */

static
PRED_IMPL("trie_insert", 3, trie_insert, 0)
{ PRED_LD

  return trie_insert(A1, A2, A3, NULL, FALSE PASS_LD);
}

/**
 * trie_update(+Trie, +Key, +Value) is semidet.
 *
 * Similar to trie_insert/3, but updates the associated value rather
 * then failing or raising an error.
 *
 * @error permission_error if Key was associated with a different value
 */

static
PRED_IMPL("trie_update", 3, trie_update, 0)
{ PRED_LD

  return trie_insert(A1, A2, A3, NULL, TRUE PASS_LD);
}


/**
 * trie_insert(+Trie, +Term, +Value, -Handle) is semidet.
 *
 * Add Term to Trie and unify Handle with a handle to the term.
 * Fails if Term is already in Trie.
 *
 * @bug Handle is currently a pointer.  In future versions we will
 * use a dynamic array for the trie nodes and return an integer to
 * guarantee safe lookup.
 */

static
PRED_IMPL("trie_insert", 4, trie_insert, 0)
{ PRED_LD
  trie_node *node;

  return ( trie_insert(A1, A2, A3, &node, FALSE PASS_LD) &&
	   PL_unify_pointer(A4, node) );
}


static
PRED_IMPL("trie_delete", 3, trie_delete, 0)
{ PRED_LD
  trie *trie;

  if ( get_trie(A1, &trie) )
  { Word kp;
    trie_node *node;
    int rc;

    kp = valTermRef(A2);

    if ( (rc=trie_lookup(trie, &node, kp, FALSE PASS_LD)) == TRUE )
    { if ( node->value )
      { if ( unify_value(A3, node->value PASS_LD) )
	{ prune_node(trie, node);
	  ATOMIC_DEC(&trie->value_count);
	  return TRUE;
	}
      }
      return FALSE;
    }

    return trie_error(rc, A2);
  }

  return FALSE;
}


static
PRED_IMPL("trie_lookup", 3, trie_lookup, 0)
{ PRED_LD
  trie *trie;

  if ( get_trie(A1, &trie) )
  { Word kp;
    trie_node *node;
    int rc;

    kp = valTermRef(A2);

    if ( (rc=trie_lookup(trie, &node, kp, FALSE PASS_LD)) == TRUE )
    { if ( node->value )
	return unify_value(A3, node->value PASS_LD);
      return FALSE;
    }

    return trie_error(rc, A2);
  }

  return FALSE;
}


/**
 * trie_term(+Handle, -Term) is det.
 *
 * Retrieve a term for a handle returned by trie_insert/4.
 */

static
PRED_IMPL("trie_term", 2, trie_term, 0)
{ PRED_LD
  void *ptr;

  return ( PL_get_pointer_ex(A1, &ptr) &&
	   unify_trie_term(ptr, A2 PASS_LD)
	 );
}


/**
 * trie_gen(+Trie, ?Key, -Value) is nondet.
 *
 * True when Key-Value appears in Trie.
 *
 * This needs to keep  a  list  of   choice  points  for  each node with
 * multiple children. Eventually, this is probably going to be a virtual
 * machine extension, using real choice points.
 */

static void
init_ukey_state(ukey_state *state, trie *trie, Word p)
{ state->trie = trie;
  state->ptr  = p;
  state->umode = uread;
  state->max_var_seen = 0;
}

static void
destroy_ukey_state(ukey_state *state)
{ if ( state->max_var_seen && state->vars != state->var_buf )
    PL_free(state->vars);
}

static Word*
find_var(ukey_state *state, size_t index)
{ if ( index > state->max_var_seen )
  { assert(index == state->max_var_seen+1);

    if ( !state->max_var_seen )
    { state->vars_allocated = NVARS_FAST;
      state->vars = state->var_buf;
    } else if ( index >= state->vars_allocated )
    { if ( state->vars == state->var_buf )
      { state->vars = PL_malloc(sizeof(*state->vars)*NVARS_FAST*2);
	memcpy(state->vars, state->var_buf, sizeof(*state->vars)*NVARS_FAST);
      } else
      { state->vars = PL_realloc(state->vars,
				 sizeof(*state->vars)*state->vars_allocated*2);
      }
      state->vars_allocated *= 2;
    }
    state->vars[index] = NULL;
    state->max_var_seen = index;
  }

  return &state->vars[index];
}


static int
unify_key(ukey_state *state, word key ARG_LD)
{ Word p;

  if ( key == TRIE_KEY_POP )
  { Word wp = *--aTop;
    state->umode = ((int)(uintptr_t)wp & uwrite);
    state->ptr   = (Word)((intptr_t)wp&~uwrite);

    DEBUG(MSG_TRIE_PUT_TERM,
	  Sdprintf("U Popped %zd, mode=%d\n", state->ptr-gBase, state->umode));
    return TRUE;
  }

  p = state->ptr;
  if ( state->umode == uread )
    deRef(p);

  if ( tagex(key) == (TAG_ATOM|STG_GLOBAL) )
  { size_t arity = arityFunctor(key);

    DEBUG(MSG_TRIE_PUT_TERM,
	  Sdprintf("U Pushed %s %zd, mode=%d\n",
		   functorName(key), state->ptr+1-gBase, state->umode));
    pushArgumentStack((Word)((intptr_t)(state->ptr + 1)|state->umode));

    if ( state->umode == uwrite )
    { Word t;

      if ( (t=allocGlobalNoShift(arity+1)) )
      { t[0] = key;
	*p = consPtr(t, TAG_COMPOUND|STG_GLOBAL);
	state->ptr = &t[1];
	return TRUE;
      } else
	return GLOBAL_OVERFLOW;
    } else
    { if ( canBind(*p) )
      { state->umode = uwrite;

	if ( isAttVar(*p) )
	{ Word t;
	  word w;
	  size_t i;

	  if ( (t=allocGlobalNoShift(arity+1)) )
	  { if ( !hasGlobalSpace(0) )
	      return overflowCode(0);
	    w = consPtr(&t[0], TAG_COMPOUND|STG_GLOBAL);
	    t[0] = key;
	    for(i=0; i<arity; i++)
	      setVar(t[i+1]);
	    assignAttVar(p, &w PASS_LD);
	    state->ptr = &t[1];
	    return TRUE;
	  } else
	    return GLOBAL_OVERFLOW;
	} else
	{ Word t;

	  if ( (t=allocGlobalNoShift(arity+1)) )
	  { if ( unlikely(tTop+1 >= tMax) )
	      return  TRAIL_OVERFLOW;
	    t[0] = key;
	    Trail(p, consPtr(t, TAG_COMPOUND|STG_GLOBAL));
	    state->ptr = &t[1];
	    return TRUE;
	  } else
	    return GLOBAL_OVERFLOW;
	}
      } else if ( isTerm(*p) )
      { Functor f = valueTerm(*p);

	if ( f->definition == key )
	{ state->ptr = &f->arguments[0];
	  return TRUE;
	} else
	  return FALSE;
      } else
      { return FALSE;
      }
    }
  } else if ( tag(key) == TAG_VAR )
  { size_t index = (size_t)(key>>LMASK_BITS);
    Word *v = find_var(state, index);

    DEBUG(MSG_TRIE_PUT_TERM,
	  { char b1[64]; char b2[64];
	    Sdprintf("var %zd at %s (v=%p, *v=%s)\n",
		     index,
		     print_addr(state->ptr,b1),
		     v, print_addr(*v,b2));
	  });

    if ( state->umode == uwrite )
    { if ( !*v )
      { setVar(*state->ptr);
	*v = state->ptr;
      } else
      { *state->ptr = makeRefG(*v);
      }
    } else
    { if ( !*v )
      { *v = state->ptr;
      } else
      { int rc;

	if ( (rc=unify_ptrs(state->ptr, *v, ALLOW_RETCODE PASS_LD)) != TRUE )
	  return rc;
      }
    }

    state->ptr++;
    return TRUE;
  } else
  { word w;

    DEBUG(MSG_TRIE_PUT_TERM,
	  Sdprintf("%s at %s\n",
		   print_val(key, NULL), print_addr(state->ptr,NULL)));

    if ( isIndirect(key) )
    { w = extern_indirect_no_shift(state->trie->indirects, key PASS_LD);
      if ( !w )
	return GLOBAL_OVERFLOW;
    } else
      w = key;

    if ( state->umode == uwrite )
    { if ( isAtom(w) )
	pushVolatileAtom(w);
      *p = w;
    } else if ( canBind(*p) )
    { if ( isAtom(w) )
	pushVolatileAtom(w);

      if ( hasGlobalSpace(0) )
	bindConst(p, w);
      else
	return overflowCode(0);
    } else if ( *p != w )
      return FALSE;

    state->ptr++;
    return TRUE;
  }
}


typedef struct trie_choice
{ union
  { void *any;
    TableEnum table;
  } choice;
  word key;
  trie_node *child;
} trie_choice;

typedef struct
{ trie        *trie;		/* trie we operate on */
  int	       allocated;
  tmp_buffer   choicepoints;	/* Stack of trie state choicepoints */
} trie_gen_state;


static void
init_trie_state(trie_gen_state *state, trie *trie)
{ state->trie = trie;
  state->allocated = FALSE;
  initBuffer(&state->choicepoints);
}


#define base_choice(state) baseBuffer(&state->choicepoints, trie_choice)
#define top_choice(state) topBuffer(&state->choicepoints, trie_choice)


static void
clear_trie_state(trie_gen_state *state)
{ trie_choice *chp = base_choice(state);
  trie_choice *top = top_choice(state);

  for(; chp < top; chp++)
  { if ( chp->choice.table )
      freeTableEnum(chp->choice.table);
  }

  discardBuffer(&state->choicepoints);

  release_trie(state->trie);

  if ( state->allocated )
    freeForeignState(state, sizeof(*state));
}


trie_choice *
add_choice(trie_gen_state *state, trie_node *node)
{ trie_children children = node->children;
  trie_choice *ch = allocFromBuffer(&state->choicepoints, sizeof(*ch));

  if ( children.any )
  { switch( children.any->type )
    { case TN_KEY:
      {	word key   = children.key->key;

	ch->key    = key;
	ch->child  = children.key->child;
        ch->choice.any = NULL;
	break;
      }
      case TN_HASHED:
      { void *k, *v;

	ch->choice.table = newTableEnum(children.hash->table);
        advanceTableEnum(ch->choice.table, &k, &v);
	ch->key   = (word)k;
	ch->child = (trie_node*)v;
	break;
      }
      default:
	assert(0);
    }
  } else
  { memset(ch, 0, sizeof(*ch));
    ch->child = node;
  }

  return ch;
}


static int
descent_node(trie_gen_state *state, trie_choice *ch)
{ while( ch->child->children.any )
  { ch = add_choice(state, ch->child);
  }

  return ch->child->value != 0;
}


static int
advance_node(trie_choice *ch)
{ if ( ch->choice.table )
  { void *k, *v;

    if ( advanceTableEnum(ch->choice.table, &k, &v) )
    { ch->key   = (word)k;
      ch->child = (trie_node*)v;

      return TRUE;
    }
  }

  return FALSE;
}


static int
next_choice(trie_gen_state *state)
{ trie_choice *btm = base_choice(state);
  trie_choice  *ch = top_choice(state)-1;

  while(ch >= btm)
  { if ( advance_node(ch) &&
	 descent_node(state, ch) )
      return TRUE;

    if ( ch->choice.table )
      freeTableEnum(ch->choice.table);

    state->choicepoints.top = (char*)ch;
    ch--;
  }

  return FALSE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Unify term with the term represented a trie path (list of trie_choice).
Returns one of TRUE, FALSE or *_OVERFLOW.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
unify_trie_path(term_t term, trie_node **tn, trie_gen_state *gstate ARG_LD)
{ ukey_state ustate;
  trie_choice *ch = base_choice(gstate);
  trie_choice *top = top_choice(gstate);

  init_ukey_state(&ustate, gstate->trie, valTermRef(term));
  for( ; ch < top; ch++ )
  { int rc;

    if ( (rc=unify_key(&ustate, ch->key PASS_LD)) != TRUE )
    { destroy_ukey_state(&ustate);
      return rc;
    }
  }

  destroy_ukey_state(&ustate);
  *tn = ch[-1].child;

  return TRUE;
}


foreign_t
trie_gen(term_t Trie, term_t Key, term_t Value,
	 term_t Data, int (*unify_data)(term_t, trie_node*, void *ctx ARG_LD),
	 void *ctx, control_t PL__ctx)
{ PRED_LD
  trie_gen_state state_buf;
  trie_gen_state *state;
  trie_node *n;
  fid_t fid;

  switch( CTX_CNTRL )
  { case FRG_FIRST_CALL:
    { trie *trie;

      if ( get_trie(Trie, &trie) )
      { if ( trie->root.children.any )
	{ acquire_trie(trie);
	  state = &state_buf;
	  init_trie_state(state, trie);
	  if ( !descent_node(state, add_choice(state, &trie->root)) &&
	       !next_choice(state) )
	  { clear_trie_state(state);
	    return FALSE;
	  }
	  break;
	}
      }
      return FALSE;
    }
    case FRG_REDO:
      state = CTX_PTR;
      break;
    case FRG_CUTTED:
      state = CTX_PTR;
      clear_trie_state(state);
      return TRUE;
    default:
      assert(0);
      return FALSE;
  }

  fid = PL_open_foreign_frame();
  for( ; !isEmptyBuffer(&state->choicepoints); next_choice(state) )
  { int rc;

    for(;;)
    { if ( (rc=unify_trie_path(Key, &n, state PASS_LD)) == TRUE )
	break;

      PL_rewind_foreign_frame(fid);
      if ( rc == FALSE )
	goto next;

      if ( makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT) )
	continue;

      clear_trie_state(state);
      return FALSE;				/* resource error */
    }

    DEBUG(CHK_SECURE, PL_check_data(Key));

    if ( (!Value || unify_value(Value, n->value PASS_LD)) &&
	 (!Data  || unify_data(Data, n, ctx PASS_LD)) )
    { if ( next_choice(state) )
      { if ( !state->allocated )
	{ trie_gen_state *nstate = allocForeignState(sizeof(*state));
	  TmpBuffer nchp = &nstate->choicepoints;
	  TmpBuffer ochp =  &state->choicepoints;

	  nstate->trie = state->trie;
	  nstate->allocated = TRUE;
	  if ( ochp->base == ochp->static_buffer )
	  { size_t bytes = ochp->top - ochp->base;
	    initBuffer(nchp);
	    nchp->top  = nchp->base + bytes;
	    memcpy(nchp->base, ochp->base, bytes);
	  } else
	  { nchp->base = ochp->base;
	    nchp->top  = ochp->top;
	    nchp->max  = ochp->max;
	  }

	  state = nstate;
	}
	PL_close_foreign_frame(fid);
	ForeignRedoPtr(state);
      } else
      { clear_trie_state(state);
	PL_close_foreign_frame(fid);
	return TRUE;
      }
    } else
      PL_rewind_foreign_frame(fid);

next:;
  }

  clear_trie_state(state);
  PL_close_foreign_frame(fid);
  return FALSE;
}


static
PRED_IMPL("trie_gen", 3, trie_gen, PL_FA_NONDETERMINISTIC)
{ return trie_gen(A1, A2, A3, 0, NULL, NULL, PL__ctx);
}

static
PRED_IMPL("trie_gen", 2, trie_gen, PL_FA_NONDETERMINISTIC)
{ return trie_gen(A1, A2, 0, 0, NULL, NULL, PL__ctx);
}

static int
unify_node_id(term_t t, trie_node *answer, void *ctx ARG_LD)
{ (void) ctx;

  return PL_unify_pointer(t, answer);
}

static
PRED_IMPL("$trie_gen_node", 3, trie_gen_node, PL_FA_NONDETERMINISTIC)
{ return trie_gen(A1, A2, 0, A3, unify_node_id, NULL, PL__ctx);
}



static
PRED_IMPL("$trie_property", 2, trie_property, 0)
{ PRED_LD
  trie *trie;

  if ( get_trie(A1, &trie) )
  { atom_t name; size_t arity;

    if ( PL_get_name_arity(A2, &name, &arity) && arity == 1 )
    { term_t arg = PL_new_term_ref();

      _PL_get_arg(1, A2, arg);

      if ( name == ATOM_node_count )
      { return PL_unify_integer(arg, trie->node_count);
      } else if ( name == ATOM_value_count )
      { return PL_unify_integer(arg, trie->value_count);
      } else if ( name == ATOM_size )
      { trie_stats stats;
	stat_trie(trie, &stats);
	return PL_unify_int64(arg, stats.bytes);
      } else if ( name == ATOM_hashed )
      { trie_stats stats;
	stat_trie(trie, &stats);
	return PL_unify_int64(arg, stats.hashes);
      }
    }
  }

  return FALSE;
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(trie)
  PRED_DEF("is_trie",             1, is_trie,            0)
  PRED_DEF("trie_new",            1, trie_new,           0)
  PRED_DEF("trie_destroy",        1, trie_destroy,       0)
  PRED_DEF("trie_insert",         3, trie_insert,        0)
  PRED_DEF("trie_insert",         4, trie_insert,        0)
  PRED_DEF("trie_update",         3, trie_update,        0)
  PRED_DEF("trie_lookup",         3, trie_lookup,        0)
  PRED_DEF("trie_delete",         3, trie_delete,        0)
  PRED_DEF("trie_term",		  2, trie_term,		 0)
  PRED_DEF("trie_gen",            3, trie_gen,	    PL_FA_NONDETERMINISTIC)
  PRED_DEF("trie_gen",            2, trie_gen,      PL_FA_NONDETERMINISTIC)
  PRED_DEF("$trie_gen_node",      3, trie_gen_node, PL_FA_NONDETERMINISTIC)
  PRED_DEF("$trie_property",      2, trie_property,      0)
EndPredDefs

void
initTries(void)
{ PL_register_blob_type(&trie_blob);
}
