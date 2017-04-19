/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2007-2009 Robert Daniel Brummayer.
 *  Copyright (C) 2007-2015 Armin Biere.
 *  Copyright (C) 2012-2017 Aina Niemetz.
 *  Copyright (C) 2012-2017 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorexp.h"

#include "btorabort.h"
#include "btoraig.h"
#include "btoraigvec.h"
#include "btorbeta.h"
#include "btorlog.h"
#include "btorrewrite.h"
#include "utils/btorexpiter.h"
#include "utils/btorhashint.h"
#include "utils/btorhashptr.h"
#include "utils/btormisc.h"
#include "utils/btorutil.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*------------------------------------------------------------------------*/

#define BTOR_UNIQUE_TABLE_LIMIT 30

#define BTOR_FULL_UNIQUE_TABLE(table)   \
  ((table).num_elements >= (table).size \
   && btor_log_2_util ((table).size) < BTOR_UNIQUE_TABLE_LIMIT)

/*------------------------------------------------------------------------*/

const char *const g_btor_op2str[BTOR_NUM_OPS_NODE] = {
    [BTOR_INVALID_NODE] = "invalid", [BTOR_BV_CONST_NODE] = "const",
    [BTOR_BV_VAR_NODE] = "var",      [BTOR_PARAM_NODE] = "param",
    [BTOR_SLICE_NODE] = "slice",     [BTOR_AND_NODE] = "and",
    [BTOR_BV_EQ_NODE] = "beq",       [BTOR_FUN_EQ_NODE] = "feq",
    [BTOR_ADD_NODE] = "add",         [BTOR_MUL_NODE] = "mul",
    [BTOR_ULT_NODE] = "ult",         [BTOR_SLL_NODE] = "sll",
    [BTOR_SRL_NODE] = "srl",         [BTOR_UDIV_NODE] = "udiv",
    [BTOR_UREM_NODE] = "urem",       [BTOR_CONCAT_NODE] = "concat",
    [BTOR_APPLY_NODE] = "apply",     [BTOR_LAMBDA_NODE] = "lambda",
    [BTOR_COND_NODE] = "cond",       [BTOR_ARGS_NODE] = "args",
    [BTOR_UF_NODE] = "uf",           [BTOR_UPDATE_NODE] = "update",
    [BTOR_PROXY_NODE] = "proxy",
};

/*------------------------------------------------------------------------*/

static unsigned hash_primes[] = {333444569u, 76891121u, 456790003u};

#define NPRIMES ((int) (sizeof hash_primes / sizeof *hash_primes))

/*------------------------------------------------------------------------*/

/* do not move these two functions to the header (circular dependency) */

bool
btor_is_bv_cond_node (const BtorNode *exp)
{
  return btor_is_cond_node (exp)
         && btor_is_bitvec_sort (BTOR_REAL_ADDR_NODE (exp)->btor,
                                 btor_exp_get_sort_id (exp));
}

bool
btor_is_fun_cond_node (const BtorNode *exp)
{
  return btor_is_cond_node (exp)
         && btor_is_fun_sort (BTOR_REAL_ADDR_NODE (exp)->btor,
                              btor_exp_get_sort_id (exp));
}

/*------------------------------------------------------------------------*/

static void
inc_exp_ref_counter (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  BtorNode *real_exp;

  (void) btor;
  real_exp = BTOR_REAL_ADDR_NODE (exp);
  BTOR_ABORT (real_exp->refs == INT_MAX, "Node reference counter overflow");
  real_exp->refs++;
}

void
btor_inc_exp_ext_ref_counter (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  BtorNode *real_exp = BTOR_REAL_ADDR_NODE (exp);
  BTOR_ABORT (real_exp->ext_refs == INT_MAX, "Node reference counter overflow");
  real_exp->ext_refs += 1;
  btor->external_refs += 1;
}

void
btor_dec_exp_ext_ref_counter (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  BTOR_REAL_ADDR_NODE (exp)->ext_refs -= 1;
  btor->external_refs -= 1;
}

BtorNode *
btor_copy_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  inc_exp_ref_counter (btor, exp);
  return exp;
}

/*------------------------------------------------------------------------*/

static inline unsigned int
hash_slice_exp (BtorNode *e, uint32_t upper, uint32_t lower)
{
  unsigned int hash;
  assert (upper >= lower);
  hash = hash_primes[0] * (unsigned int) BTOR_REAL_ADDR_NODE (e)->id;
  hash += hash_primes[1] * (unsigned int) upper;
  hash += hash_primes[2] * (unsigned int) lower;
  return hash;
}

static inline unsigned int
hash_bv_exp (Btor *btor, BtorNodeKind kind, int arity, BtorNode *e[])
{
  unsigned int hash = 0;
  int i;
#ifndef NDEBUG
  if (btor_get_opt (btor, BTOR_OPT_SORT_EXP) > 0
      && btor_is_binary_commutative_node_kind (kind))
    assert (arity == 2), assert (BTOR_REAL_ADDR_NODE (e[0])->id
                                 <= BTOR_REAL_ADDR_NODE (e[1])->id);
#else
  (void) btor;
  (void) kind;
#endif
  assert (arity <= NPRIMES);
  for (i = 0; i < arity; i++)
    hash += hash_primes[i] * (unsigned int) BTOR_REAL_ADDR_NODE (e[i])->id;
  return hash;
}

/* Computes hash value of expresssion by children ids */
static unsigned int
compute_hash_exp (Btor *btor, BtorNode *exp, int table_size)
{
  assert (exp);
  assert (table_size > 0);
  assert (btor_is_power_of_2_util (table_size));
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_is_bv_var_node (exp));
  assert (!btor_is_uf_node (exp));

  unsigned int hash = 0;

  if (btor_is_bv_const_node (exp))
    hash = btor_hash_bv (btor_const_get_bits (exp));
  /* hash for lambdas is computed once during creation. afterwards, we always
   * have to use the saved hash value since hashing of lambdas requires all
   * parameterized nodes and their inputs (cf. hash_lambda_exp), which may
   * change at some point. */
  else if (btor_is_lambda_node (exp))
    hash = btor_get_ptr_hash_table (exp->btor->lambdas, (BtorNode *) exp)
               ->data.as_int;
  else if (exp->kind == BTOR_SLICE_NODE)
    hash = hash_slice_exp (
        exp->e[0], btor_slice_get_upper (exp), btor_slice_get_lower (exp));
  else
    hash = hash_bv_exp (btor, exp->kind, exp->arity, exp->e);
  hash &= table_size - 1;
  return hash;
}

static void
remove_from_nodes_unique_table_exp (Btor *btor, BtorNode *exp)
{
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));

  unsigned int hash;
  BtorNode *cur, *prev;

  if (!exp->unique) return;

  assert (btor);
  assert (btor->nodes_unique_table.num_elements > 0);

  hash = compute_hash_exp (btor, exp, btor->nodes_unique_table.size);
  prev = 0;
  cur  = btor->nodes_unique_table.chains[hash];

  while (cur != exp)
  {
    assert (cur);
    assert (BTOR_IS_REGULAR_NODE (cur));
    prev = cur;
    cur  = cur->next;
  }
  assert (cur);
  if (!prev)
    btor->nodes_unique_table.chains[hash] = cur->next;
  else
    prev->next = cur->next;

  btor->nodes_unique_table.num_elements--;

  exp->unique = 0; /* NOTE: this is not debugging code ! */
  exp->next   = 0;
}

/* Delete local data of expression.
 *
 * Virtual reads and simplified expressions have to be handled by the
 * calling function, e.g. 'btor_release_exp', to avoid recursion.
 *
 * We use this function to update operator stats
 */
static void
erase_local_data_exp (Btor *btor, BtorNode *exp, int free_sort)
{
  assert (btor);
  assert (exp);

  assert (BTOR_IS_REGULAR_NODE (exp));

  assert (!exp->unique);
  assert (!exp->erased);
  assert (!exp->disconnected);
  assert (!btor_is_invalid_node (exp));

  BtorMemMgr *mm;
  BtorPtrHashTable *static_rho;
  BtorPtrHashTableIterator it;

  mm = btor->mm;
  //  BTORLOG ("%s: %s", __FUNCTION__, node2string (exp));

  switch (exp->kind)
  {
    case BTOR_BV_CONST_NODE:
      btor_free_bv (mm, btor_const_get_bits (exp));
      if (btor_const_get_invbits (exp))
        btor_free_bv (mm, btor_const_get_invbits (exp));
      btor_const_set_bits (exp, 0);
      btor_const_set_invbits (exp, 0);
      break;
    case BTOR_LAMBDA_NODE:
      static_rho = btor_lambda_get_static_rho (exp);
      if (static_rho)
      {
        btor_init_ptr_hash_table_iterator (&it, static_rho);
        while (btor_has_next_ptr_hash_table_iterator (&it))
        {
          btor_release_exp (btor, it.bucket->data.as_ptr);
          btor_release_exp (btor, btor_next_ptr_hash_table_iterator (&it));
        }
        btor_delete_ptr_hash_table (static_rho);
        ((BtorLambdaNode *) exp)->static_rho = 0;
      }
      /* fall through intended */
    case BTOR_UPDATE_NODE:
    case BTOR_UF_NODE:
      if (exp->rho)
      {
        btor_delete_ptr_hash_table (exp->rho);
        exp->rho = 0;
      }
      break;
    case BTOR_COND_NODE:
      if (btor_is_fun_cond_node (exp) && exp->rho)
      {
        btor_delete_ptr_hash_table (exp->rho);
        exp->rho = 0;
      }
      break;
    default: break;
  }

  if (free_sort)
  {
    assert (btor_exp_get_sort_id (exp));
    btor_release_sort (btor, btor_exp_get_sort_id (exp));
    btor_exp_set_sort_id (exp, 0);
  }

  if (exp->av)
  {
    btor_release_delete_aigvec (btor->avmgr, exp->av);
    exp->av = 0;
  }
  exp->erased = 1;
}

static void
remove_from_hash_tables (Btor *btor, BtorNode *exp, bool keep_symbol)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_is_invalid_node (exp));

  BtorHashTableData data;

  switch (exp->kind)
  {
    case BTOR_BV_VAR_NODE:
      btor_remove_ptr_hash_table (btor->bv_vars, exp, 0, 0);
      break;
    case BTOR_LAMBDA_NODE:
      btor_remove_ptr_hash_table (btor->lambdas, exp, 0, 0);
      break;
    case BTOR_UF_NODE: btor_remove_ptr_hash_table (btor->ufs, exp, 0, 0); break;
    case BTOR_FUN_EQ_NODE:
      btor_remove_ptr_hash_table (btor->feqs, exp, 0, 0);
      break;
    default: break;
  }

  if (!keep_symbol && btor_get_ptr_hash_table (btor->node2symbol, exp))
  {
    btor_remove_ptr_hash_table (btor->node2symbol, exp, 0, &data);
    if (data.as_str[0] != 0)
    {
      btor_remove_ptr_hash_table (btor->symbols, data.as_str, 0, 0);
      btor_freestr (btor->mm, data.as_str);
    }
  }

  if (btor_get_ptr_hash_table (btor->parameterized, exp))
  {
    btor_remove_ptr_hash_table (btor->parameterized, exp, 0, &data);
    assert (data.as_ptr);
    btor_delete_int_hash_table (data.as_ptr);
  }
}

/* Disconnects a child from its parent and updates its parent list */
static void
disconnect_child_exp (Btor *btor, BtorNode *parent, int pos)
{
  assert (btor);
  assert (parent);
  assert (BTOR_IS_REGULAR_NODE (parent));
  assert (btor == parent->btor);
  assert (!btor_is_bv_const_node (parent));
  assert (!btor_is_bv_var_node (parent));
  assert (!btor_is_uf_node (parent));
  assert (pos >= 0);
  assert (pos <= 2);

  (void) btor;
  BtorNode *first_parent, *last_parent;
  BtorNode *real_child, *tagged_parent;

  tagged_parent = BTOR_TAG_NODE (parent, pos);
  real_child    = BTOR_REAL_ADDR_NODE (parent->e[pos]);
  real_child->parents--;
  first_parent = real_child->first_parent;
  last_parent  = real_child->last_parent;
  assert (first_parent);
  assert (last_parent);

  /* if a parameter is disconnected from a lambda we have to reset
   * 'lambda_exp' of the parameter in order to keep a valid state */
  if (btor_is_lambda_node (parent)
      && pos == 0
      /* if parent gets rebuilt via substitute_and_rebuild, it might
       * result in a new lambda term, where the param is already reused.
       * if this is the case param is already bound by a different lambda
       * and we are not allowed to reset param->lambda_exp to 0. */
      && btor_param_get_binding_lambda (parent->e[0]) == parent)
    btor_param_set_binding_lambda (parent->e[0], 0);

  /* only one parent? */
  if (first_parent == tagged_parent && first_parent == last_parent)
  {
    assert (!parent->next_parent[pos]);
    assert (!parent->prev_parent[pos]);
    real_child->first_parent = 0;
    real_child->last_parent  = 0;
  }
  /* is parent first parent in the list? */
  else if (first_parent == tagged_parent)
  {
    assert (parent->next_parent[pos]);
    assert (!parent->prev_parent[pos]);
    real_child->first_parent                    = parent->next_parent[pos];
    BTOR_PREV_PARENT (real_child->first_parent) = 0;
  }
  /* is parent last parent in the list? */
  else if (last_parent == tagged_parent)
  {
    assert (!parent->next_parent[pos]);
    assert (parent->prev_parent[pos]);
    real_child->last_parent                    = parent->prev_parent[pos];
    BTOR_NEXT_PARENT (real_child->last_parent) = 0;
  }
  /* detach parent from list */
  else
  {
    assert (parent->next_parent[pos]);
    assert (parent->prev_parent[pos]);
    BTOR_PREV_PARENT (parent->next_parent[pos]) = parent->prev_parent[pos];
    BTOR_NEXT_PARENT (parent->prev_parent[pos]) = parent->next_parent[pos];
  }
  parent->next_parent[pos] = 0;
  parent->prev_parent[pos] = 0;
  parent->e[pos]           = 0;
}

/* Disconnect children of expression in parent list and if applicable from
 * unique table.  Do not touch local data, nor any reference counts.  The
 * kind of the expression becomes 'BTOR_DISCONNECTED_NODE' in debugging mode.
 *
 * Actually we have the sequence
 *
 *   UNIQUE -> !UNIQUE -> ERASED -> DISCONNECTED -> INVALID
 *
 * after a unique or non uninque expression is allocated until it is
 * deallocated.  We also have loop back from DISCONNECTED to !UNIQUE
 * if an expression is rewritten and reused as PROXY.
 */
static void
disconnect_children_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_is_invalid_node (exp));
  assert (!exp->unique);
  assert (exp->erased);
  assert (!exp->disconnected);

  int i;

  for (i = 0; i < exp->arity; i++) disconnect_child_exp (btor, exp, i);
  exp->disconnected = 1;
}

#ifndef NDEBUG
static bool
is_valid_kind (BtorNodeKind kind)
{
  return 0 <= (int) kind && kind < BTOR_NUM_OPS_NODE;
}
#endif

static void
set_kind (Btor *btor, BtorNode *exp, BtorNodeKind kind)
{
  assert (is_valid_kind (kind));
  assert (is_valid_kind (exp->kind));

  assert (!BTOR_INVALID_NODE);

  if (exp->kind)
  {
    assert (btor->ops[exp->kind].cur > 0);
    btor->ops[exp->kind].cur--;
  }

  if (kind)
  {
    btor->ops[kind].cur++;
    assert (btor->ops[kind].cur > 0);
    if (btor->ops[kind].cur > btor->ops[kind].max)
      btor->ops[kind].max = btor->ops[kind].cur;
  }

  exp->kind = kind;
}

static void
really_deallocate_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor == exp->btor);
  assert (!exp->unique);
  assert (exp->disconnected);
  assert (exp->erased);
  assert (exp->id);
  assert (BTOR_PEEK_STACK (btor->nodes_id_table, exp->id) == exp);
  BTOR_POKE_STACK (btor->nodes_id_table, exp->id, 0);

  BtorMemMgr *mm;

  mm = btor->mm;

  set_kind (btor, exp, BTOR_INVALID_NODE);

  if (btor_is_bv_const_node (exp))
  {
    btor_free_bv (btor->mm, btor_const_get_bits (exp));
    if (btor_const_get_invbits (exp))
      btor_free_bv (btor->mm, btor_const_get_invbits (exp));
  }
  btor_free (mm, exp, exp->bytes);
}

static void
recursively_release_exp (Btor *btor, BtorNode *root)
{
  assert (btor);
  assert (root);
  assert (BTOR_IS_REGULAR_NODE (root));
  assert (root->refs == 1);

  BtorNodePtrStack stack;
  BtorMemMgr *mm;
  BtorNode *cur;
  int i;

  mm = btor->mm;

  BTOR_INIT_STACK (mm, stack);
  cur = root;
  goto RECURSIVELY_RELEASE_NODE_ENTER_WITHOUT_POP;

  do
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (stack));

    if (cur->refs > 1)
      cur->refs--;
    else
    {
    RECURSIVELY_RELEASE_NODE_ENTER_WITHOUT_POP:
      assert (cur->refs == 1);
      assert (!cur->ext_refs || cur->ext_refs == 1);
      assert (cur->parents == 0);

      for (i = cur->arity - 1; i >= 0; i--) BTOR_PUSH_STACK (stack, cur->e[i]);

      if (cur->simplified)
      {
        BTOR_PUSH_STACK (stack, cur->simplified);
        cur->simplified = 0;
      }

      remove_from_nodes_unique_table_exp (btor, cur);
      erase_local_data_exp (btor, cur, 1);

      /* It is safe to access the children here, since they are pushed
       * on the stack and will be released later if necessary.
       */
      remove_from_hash_tables (btor, cur, 0);
      disconnect_children_exp (btor, cur);
      really_deallocate_exp (btor, cur);
    }
  } while (!BTOR_EMPTY_STACK (stack));
  BTOR_RELEASE_STACK (stack);
}

void
btor_release_exp (Btor *btor, BtorNode *root)
{
  assert (btor);
  assert (root);
  assert (btor == BTOR_REAL_ADDR_NODE (root)->btor);

  root = BTOR_REAL_ADDR_NODE (root);

  assert (root->refs > 0);

  if (root->refs > 1)
    root->refs--;
  else
    recursively_release_exp (btor, root);
}

/*------------------------------------------------------------------------*/

void
btor_set_to_proxy_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor == exp->btor);
  assert (exp->simplified);

  int i;
  BtorNode *e[3];

  remove_from_nodes_unique_table_exp (btor, exp);
  /* also updates op stats */
  erase_local_data_exp (btor, exp, 0);
  assert (exp->arity <= 3);
  BTOR_CLR (e);
  for (i = 0; i < exp->arity; i++) e[i] = exp->e[i];
  remove_from_hash_tables (btor, exp, 1);
  disconnect_children_exp (btor, exp);

  for (i = 0; i < exp->arity; i++) btor_release_exp (btor, e[i]);

  set_kind (btor, exp, BTOR_PROXY_NODE);

  exp->disconnected  = 0;
  exp->erased        = 0;
  exp->arity         = 0;
  exp->parameterized = 0;
}

/*------------------------------------------------------------------------*/

void
btor_exp_set_btor_id (Btor *btor, BtorNode *exp, int32_t id)
{
  assert (btor);
  assert (exp);
  assert (id);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  assert (btor_is_bv_var_node (exp) || btor_is_uf_array_node (exp));

  (void) btor;
  BtorNode *real_exp;
  BtorPtrHashBucket *b;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  b        = btor_get_ptr_hash_table (btor->inputs, real_exp);
  assert (b);
  b->data.as_int = id;
}

int32_t
btor_exp_get_btor_id (BtorNode *exp)
{
  assert (exp);

  int32_t id = 0;
  Btor *btor;
  BtorNode *real_exp;
  BtorPtrHashBucket *b;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  btor     = real_exp->btor;

  if ((b = btor_get_ptr_hash_table (btor->inputs, real_exp)))
    id = b->data.as_int;
  if (BTOR_IS_INVERTED_NODE (exp)) return -id;
  return id;
}

BtorNode *
btor_match_node_by_id (Btor *btor, int id)
{
  assert (btor);
  assert (id > 0);
  if (id >= BTOR_COUNT_STACK (btor->nodes_id_table)) return 0;
  return btor_copy_exp (btor, BTOR_PEEK_STACK (btor->nodes_id_table, id));
}

BtorNode *
btor_get_node_by_id (Btor *btor, int32_t id)
{
  assert (btor);
  bool is_inverted = id < 0;
  id               = abs (id);
  if (id >= BTOR_COUNT_STACK (btor->nodes_id_table)) return 0;
  return BTOR_COND_INVERT_NODE (is_inverted,
                                BTOR_PEEK_STACK (btor->nodes_id_table, id));
}

/*------------------------------------------------------------------------*/

char *
btor_get_symbol_exp (Btor *btor, const BtorNode *exp)
{
  /* do not pointer-chase! */
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  BtorPtrHashBucket *b;

  b = btor_get_ptr_hash_table (btor->node2symbol, BTOR_REAL_ADDR_NODE (exp));
  if (b) return b->data.as_str;
  return 0;
}

void
btor_set_symbol_exp (Btor *btor, BtorNode *exp, const char *symbol)
{
  /* do not pointer-chase! */
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  assert (symbol);
  assert (!btor_get_ptr_hash_table (btor->symbols, (char *) symbol));

  BtorPtrHashBucket *b;
  char *sym;

  exp = BTOR_REAL_ADDR_NODE (exp);
  sym = btor_strdup (btor->mm, symbol);
  btor_add_ptr_hash_table (btor->symbols, sym)->data.as_ptr = exp;
  b = btor_get_ptr_hash_table (btor->node2symbol, exp);

  if (b)
  {
    btor_remove_ptr_hash_table (btor->symbols, b->data.as_str, 0, 0);
    btor_freestr (btor->mm, b->data.as_str);
  }
  else
    b = btor_add_ptr_hash_table (btor->node2symbol, exp);

  b->data.as_str = sym;
}

BtorNode *
btor_get_node_by_symbol (Btor *btor, const char *sym)
{
  assert (btor);
  assert (sym);
  BtorPtrHashBucket *b;
  b = btor_get_ptr_hash_table (btor->symbols, (char *) sym);
  if (!b) return 0;
  return b->data.as_ptr;
}

BtorNode *
btor_match_node_by_symbol (Btor *btor, const char *sym)
{
  assert (btor);
  assert (sym);
  return btor_copy_exp (btor, btor_get_node_by_symbol (btor, sym));
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_match_node (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  int id;
  BtorNode *res;

  id = BTOR_REAL_ADDR_NODE (exp)->id;
  assert (id > 0);
  if (id >= BTOR_COUNT_STACK (btor->nodes_id_table)) return 0;
  res = btor_copy_exp (btor, BTOR_PEEK_STACK (btor->nodes_id_table, id));
  return BTOR_IS_INVERTED_NODE (exp) ? BTOR_INVERT_NODE (res) : res;
}

/*------------------------------------------------------------------------*/

/* Compares expressions by id */
int
btor_compare_exp_by_id (const BtorNode *exp0, const BtorNode *exp1)
{
  assert (exp0);
  assert (exp1);

  int id0, id1;

  id0 = btor_exp_get_id (exp0);
  id1 = btor_exp_get_id (exp1);
  if (id0 < id1) return -1;
  if (id0 > id1) return 1;
  return 0;
}

int
btor_compare_exp_by_id_qsort_desc (const void *p, const void *q)
{
  BtorNode *a = BTOR_REAL_ADDR_NODE (*(BtorNode **) p);
  BtorNode *b = BTOR_REAL_ADDR_NODE (*(BtorNode **) q);
  return b->id - a->id;
}

int
btor_compare_exp_by_id_qsort_asc (const void *p, const void *q)
{
  BtorNode *a = BTOR_REAL_ADDR_NODE (*(BtorNode **) p);
  BtorNode *b = BTOR_REAL_ADDR_NODE (*(BtorNode **) q);
  return a->id - b->id;
}

/* Computes hash value of expression by id */
unsigned int
btor_hash_exp_by_id (const BtorNode *exp)
{
  assert (exp);
  return (unsigned int) btor_exp_get_id (exp) * 7334147u;
}

/*------------------------------------------------------------------------*/

uint32_t
btor_get_exp_width (Btor *btor, const BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (!btor_is_fun_node (exp));
  assert (!btor_is_args_node (exp));
  return btor_get_width_bitvec_sort (btor, btor_exp_get_sort_id (exp));
}

uint32_t
btor_get_fun_exp_width (Btor *btor, const BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));

  assert (btor_is_fun_sort (btor, btor_exp_get_sort_id (exp)));
  return btor_get_width_bitvec_sort (
      btor, btor_get_codomain_fun_sort (btor, btor_exp_get_sort_id (exp)));
}

uint32_t
btor_get_index_exp_width (Btor *btor, const BtorNode *e_array)
{
  assert (btor);
  assert (e_array);
  assert (btor == BTOR_REAL_ADDR_NODE (e_array)->btor);

  assert (btor_is_array_sort (btor, btor_exp_get_sort_id (e_array))
          || btor_is_fun_sort (btor, btor_exp_get_sort_id (e_array)));
  return btor_get_width_bitvec_sort (
      btor, btor_get_index_array_sort (btor, btor_exp_get_sort_id (e_array)));
}

/*------------------------------------------------------------------------*/

BtorBitVector *
btor_const_get_bits (BtorNode *exp)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  return ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->bits;
}

BtorBitVector *
btor_const_get_invbits (BtorNode *exp)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  return ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->invbits;
}

void
btor_const_set_bits (BtorNode *exp, BtorBitVector *bits)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->bits = bits;
}

void
btor_const_set_invbits (BtorNode *exp, BtorBitVector *bits)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->invbits = bits;
}

/*------------------------------------------------------------------------*/

uint32_t
btor_get_fun_arity (Btor *btor, BtorNode *exp)
{
  (void) btor;
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  exp = btor_simplify_exp (btor, exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor_is_fun_sort (btor, btor_exp_get_sort_id (exp)));
  return btor_get_arity_fun_sort (btor, btor_exp_get_sort_id (exp));
}

int
btor_get_args_arity (Btor *btor, BtorNode *exp)
{
  (void) btor;
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  exp = btor_simplify_exp (btor, exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor_is_args_node (exp));
  return btor_get_arity_tuple_sort (btor, btor_exp_get_sort_id (exp));
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_lambda_get_body (BtorNode *lambda)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  return ((BtorLambdaNode *) lambda)->body;
}

void
btor_lambda_set_body (BtorNode *lambda, BtorNode *body)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  ((BtorLambdaNode *) lambda)->body = body;
}

BtorPtrHashTable *
btor_lambda_get_static_rho (BtorNode *lambda)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  return ((BtorLambdaNode *) lambda)->static_rho;
}

void
btor_lambda_set_static_rho (BtorNode *lambda, BtorPtrHashTable *static_rho)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  ((BtorLambdaNode *) lambda)->static_rho = static_rho;
}

BtorPtrHashTable *
btor_lambda_copy_static_rho (Btor *btor, BtorNode *lambda)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  assert (btor_lambda_get_static_rho (lambda));

  BtorNode *data, *key;
  BtorPtrHashTableIterator it;
  BtorPtrHashTable *static_rho;

  btor_init_ptr_hash_table_iterator (&it, btor_lambda_get_static_rho (lambda));
  static_rho = btor_new_ptr_hash_table (btor->mm,
                                        (BtorHashPtr) btor_hash_exp_by_id,
                                        (BtorCmpPtr) btor_compare_exp_by_id);
  while (btor_has_next_ptr_hash_table_iterator (&it))
  {
    data = btor_copy_exp (btor, it.bucket->data.as_ptr);
    key  = btor_copy_exp (btor, btor_next_ptr_hash_table_iterator (&it));
    btor_add_ptr_hash_table (static_rho, key)->data.as_ptr = data;
  }
  return static_rho;
}

void
btor_lambda_delete_static_rho (Btor *btor, BtorNode *lambda)
{
  BtorPtrHashTable *static_rho;
  BtorPtrHashTableIterator it;

  static_rho = btor_lambda_get_static_rho (lambda);
  if (!static_rho) return;

  btor_init_ptr_hash_table_iterator (&it, static_rho);
  while (btor_has_next_ptr_hash_table_iterator (&it))
  {
    btor_release_exp (btor, it.bucket->data.as_ptr);
    btor_release_exp (btor, btor_next_ptr_hash_table_iterator (&it));
  }
  btor_delete_ptr_hash_table (static_rho);
  btor_lambda_set_static_rho (lambda, 0);
}

/*------------------------------------------------------------------------*/

uint32_t
btor_slice_get_upper (BtorNode *slice)
{
  assert (btor_is_slice_node (slice));
  return ((BtorSliceNode *) BTOR_REAL_ADDR_NODE (slice))->upper;
}

uint32_t
btor_slice_get_lower (BtorNode *slice)
{
  assert (btor_is_slice_node (slice));
  return ((BtorSliceNode *) BTOR_REAL_ADDR_NODE (slice))->lower;
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_param_get_binding_lambda (BtorNode *param)
{
  assert (btor_is_param_node (param));
  return ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->lambda_exp;
}

void
btor_param_set_binding_lambda (BtorNode *param, BtorNode *lambda)
{
  assert (btor_is_param_node (param));
  assert (!lambda || btor_is_lambda_node (lambda));
  ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->lambda_exp = lambda;
}

bool
btor_param_is_bound (BtorNode *param)
{
  assert (btor_is_param_node (param));
  return btor_param_get_binding_lambda (param) != 0;
}

BtorNode *
btor_param_get_assigned_exp (BtorNode *param)
{
  assert (btor_is_param_node (param));
  return ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->assigned_exp;
}

BtorNode *
btor_param_set_assigned_exp (BtorNode *param, BtorNode *exp)
{
  assert (btor_is_param_node (param));
  assert (!exp || btor_exp_get_sort_id (param) == btor_exp_get_sort_id (exp));
  return ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->assigned_exp = exp;
}

/*------------------------------------------------------------------------*/

BtorNodePair *
btor_new_exp_pair (Btor *btor, BtorNode *exp1, BtorNode *exp2)
{
  assert (btor);
  assert (exp1);
  assert (exp2);
  assert (btor == BTOR_REAL_ADDR_NODE (exp1)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (exp2)->btor);

  int id1, id2;
  BtorNodePair *result;

  BTOR_NEW (btor->mm, result);
  id1 = btor_exp_get_id (exp1);
  id2 = btor_exp_get_id (exp2);
  if (id2 < id1)
  {
    result->exp1 = btor_copy_exp (btor, exp2);
    result->exp2 = btor_copy_exp (btor, exp1);
  }
  else
  {
    result->exp1 = btor_copy_exp (btor, exp1);
    result->exp2 = btor_copy_exp (btor, exp2);
  }
  return result;
}

void
btor_delete_exp_pair (Btor *btor, BtorNodePair *pair)
{
  assert (btor);
  assert (pair);
  btor_release_exp (btor, pair->exp1);
  btor_release_exp (btor, pair->exp2);
  BTOR_DELETE (btor->mm, pair);
}

unsigned int
btor_hash_exp_pair (const BtorNodePair *pair)
{
  unsigned int result;
  assert (pair);
  result = (unsigned int) BTOR_REAL_ADDR_NODE (pair->exp1)->id;
  result += (unsigned int) BTOR_REAL_ADDR_NODE (pair->exp2)->id;
  result *= 7334147u;
  return result;
}

int
btor_compare_exp_pair (const BtorNodePair *pair1, const BtorNodePair *pair2)
{
  assert (pair1);
  assert (pair2);

  int result;

  result = btor_exp_get_id (pair1->exp1);
  result -= btor_exp_get_id (pair2->exp1);
  if (result != 0) return result;
  result = btor_exp_get_id (pair1->exp2);
  result -= btor_exp_get_id (pair2->exp2);
  return result;
}

/*------------------------------------------------------------------------*/
#ifndef NDEBUG
/*------------------------------------------------------------------------*/

bool
btor_precond_slice_exp_dbg (Btor *btor,
                            const BtorNode *exp,
                            uint32_t upper,
                            uint32_t lower)
{
  assert (btor);
  assert (exp);
  assert (!BTOR_REAL_ADDR_NODE (exp)->simplified);
  assert (!btor_is_fun_node (exp));
  assert (upper >= lower);
  assert (upper < btor_get_exp_width (btor, exp));
  assert (BTOR_REAL_ADDR_NODE (exp)->btor == btor);
  return true;
}

static bool
precond_ext_exp_dbg (Btor *btor, const BtorNode *exp)
{
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));
  return true;
}

bool
btor_precond_regular_unary_bv_exp_dbg (Btor *btor, const BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (!BTOR_REAL_ADDR_NODE (exp)->simplified);
  assert (!btor_is_fun_node (exp));
  assert (BTOR_REAL_ADDR_NODE (exp)->btor == btor);
  return true;
}

bool
btor_precond_eq_exp_dbg (Btor *btor, const BtorNode *e0, const BtorNode *e1)
{
  BtorNode *real_e0, *real_e1;

  assert (btor);
  assert (e0);
  assert (e1);

  real_e0 = BTOR_REAL_ADDR_NODE (e0);
  real_e1 = BTOR_REAL_ADDR_NODE (e1);

  assert (real_e0);
  assert (real_e1);
  assert (real_e0->btor == btor);
  assert (real_e1->btor == btor);
  assert (!real_e0->simplified);
  assert (!real_e1->simplified);
  assert (btor_exp_get_sort_id (real_e0) == btor_exp_get_sort_id (real_e1));
  assert (real_e0->is_array == real_e1->is_array);
  assert (!btor_is_fun_node (real_e0)
          || (BTOR_IS_REGULAR_NODE (e0) && BTOR_IS_REGULAR_NODE (e1)));
  return true;
}

bool
btor_precond_concat_exp_dbg (Btor *btor, const BtorNode *e0, const BtorNode *e1)
{
  assert (btor);
  assert (e0);
  assert (e1);
  assert (!BTOR_REAL_ADDR_NODE (e0)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e1)->simplified);
  assert (!btor_is_fun_node (e0));
  assert (!btor_is_fun_node (e1));
  assert (btor_get_exp_width (btor, e0)
          <= INT_MAX - btor_get_exp_width (btor, e1));
  assert (BTOR_REAL_ADDR_NODE (e0)->btor == btor);
  assert (BTOR_REAL_ADDR_NODE (e1)->btor == btor);
  return true;
}

bool
btor_precond_shift_exp_dbg (Btor *btor, const BtorNode *e0, const BtorNode *e1)
{
  assert (btor);
  assert (e0);
  assert (e1);
  assert (!BTOR_REAL_ADDR_NODE (e0)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e1)->simplified);
  assert (!btor_is_fun_node (e0));
  assert (!btor_is_fun_node (e1));
  assert (btor_get_exp_width (btor, e0) > 1);
  assert (btor_is_power_of_2_util (btor_get_exp_width (btor, e0)));
  assert (btor_log_2_util (btor_get_exp_width (btor, e0))
          == btor_get_exp_width (btor, e1));
  assert (BTOR_REAL_ADDR_NODE (e0)->btor == btor);
  assert (BTOR_REAL_ADDR_NODE (e1)->btor == btor);
  return true;
}

bool
btor_precond_regular_binary_bv_exp_dbg (Btor *btor,
                                        const BtorNode *e0,
                                        const BtorNode *e1)
{
  assert (btor);
  assert (e0);
  assert (e1);
  assert (!BTOR_REAL_ADDR_NODE (e0)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e1)->simplified);
  assert (!btor_is_fun_node (e0));
  assert (!btor_is_fun_node (e1));
  assert (btor_exp_get_sort_id (e0) == btor_exp_get_sort_id (e1));
  assert (BTOR_REAL_ADDR_NODE (e0)->btor == btor);
  assert (BTOR_REAL_ADDR_NODE (e1)->btor == btor);
  return true;
}

bool
btor_precond_read_exp_dbg (Btor *btor,
                           const BtorNode *e_array,
                           const BtorNode *e_index)
{
  assert (btor);
  assert (e_array);
  assert (e_index);
  assert (BTOR_IS_REGULAR_NODE (e_array));
  assert (btor_is_fun_node (e_array));
  assert (!e_array->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e_index)->simplified);
  assert (!btor_is_fun_node (e_index));
  assert (btor_get_index_array_sort (btor, btor_exp_get_sort_id (e_array))
          == btor_exp_get_sort_id (e_index));
  assert (BTOR_REAL_ADDR_NODE (e_array)->btor == btor);
  assert (BTOR_REAL_ADDR_NODE (e_index)->btor == btor);
  assert (e_array->is_array);
  return true;
}

bool
btor_precond_write_exp_dbg (Btor *btor,
                            const BtorNode *e_array,
                            const BtorNode *e_index,
                            const BtorNode *e_value)
{
  assert (btor);
  assert (e_array);
  assert (e_index);
  assert (e_value);
  assert (BTOR_IS_REGULAR_NODE (e_array));
  assert (btor_is_fun_node (e_array));
  assert (!e_array->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e_index)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e_value)->simplified);
  assert (!btor_is_fun_node (e_index));
  assert (!btor_is_fun_node (e_value));
  assert (btor_get_index_array_sort (btor, btor_exp_get_sort_id (e_array))
          == btor_exp_get_sort_id (e_index));
  assert (btor_get_element_array_sort (btor, btor_exp_get_sort_id (e_array))
          == btor_exp_get_sort_id (e_value));
  assert (BTOR_REAL_ADDR_NODE (e_array)->btor == btor);
  assert (BTOR_REAL_ADDR_NODE (e_index)->btor == btor);
  assert (BTOR_REAL_ADDR_NODE (e_value)->btor == btor);
  assert (e_array->is_array);
  return true;
}

bool
btor_precond_cond_exp_dbg (Btor *btor,
                           const BtorNode *e_cond,
                           const BtorNode *e_if,
                           const BtorNode *e_else)
{
  assert (btor);
  assert (e_cond);
  assert (e_if);
  assert (e_else);
  assert (!BTOR_REAL_ADDR_NODE (e_cond)->simplified);
  assert (btor_get_exp_width (btor, e_cond) == 1);

  BtorNode *real_e_if, *real_e_else;

  real_e_if   = BTOR_REAL_ADDR_NODE (e_if);
  real_e_else = BTOR_REAL_ADDR_NODE (e_else);

  assert (!real_e_if->simplified);
  assert (!real_e_else->simplified);
  assert (btor_exp_get_sort_id (real_e_if)
          == btor_exp_get_sort_id (real_e_else));
  assert (BTOR_REAL_ADDR_NODE (e_cond)->btor == btor);
  assert (real_e_if->btor == btor);
  assert (real_e_else->btor == btor);
  assert (real_e_if->is_array == real_e_else->is_array);
  return true;
}

bool
btor_precond_apply_exp_dbg (Btor *btor,
                            const BtorNode *fun,
                            const BtorNode *args)
{
  assert (btor);
  assert (fun);
  assert (args);
  assert (BTOR_IS_REGULAR_NODE (fun));
  assert (BTOR_IS_REGULAR_NODE (args));
  assert (btor_is_fun_node (fun));
  assert (btor_is_args_node (args));
  assert (btor_get_domain_fun_sort (btor, btor_exp_get_sort_id (fun))
          == btor_exp_get_sort_id (args));
  return true;
}

/*------------------------------------------------------------------------*/
#endif
/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/

static unsigned int
hash_lambda_exp (Btor *btor,
                 BtorNode *param,
                 BtorNode *body,
                 BtorIntHashTable *params)
{
  assert (btor);
  assert (param);
  assert (body);
  assert (BTOR_IS_REGULAR_NODE (param));
  assert (btor_is_param_node (param));

  int i;
  unsigned int hash = 0;
  BtorNode *cur, *real_cur;
  BtorNodePtrStack visit;
  BtorIntHashTable *marked;

  marked = btor_new_int_hash_table (btor->mm);
  BTOR_INIT_STACK (btor->mm, visit);
  BTOR_PUSH_STACK (visit, (BtorNode *) body);

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    if (btor_contains_int_hash_table (marked, real_cur->id)) continue;

    if (!real_cur->parameterized)
    {
      hash += btor_exp_get_id (cur);
      continue;
    }

    /* paramterized lambda already hashed, we can use already computed hash
     * value instead of recomputing it */
    if (btor_is_lambda_node (real_cur))
    {
      hash += btor_get_ptr_hash_table (btor->lambdas, real_cur)->data.as_int;
      hash += real_cur->kind;
      hash += real_cur->e[0]->kind;
      continue;
    }
    else if (btor_is_param_node (real_cur) && real_cur != param && params)
      btor_add_int_hash_table (params, real_cur->id);

    btor_add_int_hash_table (marked, real_cur->id);
    hash += BTOR_IS_INVERTED_NODE (cur) ? -real_cur->kind : real_cur->kind;
    for (i = 0; i < real_cur->arity; i++)
      BTOR_PUSH_STACK (visit, real_cur->e[i]);
  }
  BTOR_RELEASE_STACK (visit);
  btor_delete_int_hash_table (marked);
  return hash;
}

static bool
is_sorted_bv_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[])
{
  if (!btor_get_opt (btor, BTOR_OPT_SORT_EXP)) return 1;
  if (!btor_is_binary_commutative_node_kind (kind)) return 1;
  if (e[0] == e[1]) return 1;
  if (BTOR_INVERT_NODE (e[0]) == e[1] && BTOR_IS_INVERTED_NODE (e[1])) return 1;
  return BTOR_REAL_ADDR_NODE (e[0])->id <= BTOR_REAL_ADDR_NODE (e[1])->id;
}

static void
sort_bv_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[])
{
  if (!is_sorted_bv_exp (btor, kind, e)) BTOR_SWAP (BtorNode *, e[0], e[1]);
}

/* Connects child to its parent and updates list of parent pointers.
 * Expressions are inserted at the beginning of the regular parent list
 */
static void
connect_child_exp (Btor *btor, BtorNode *parent, BtorNode *child, int pos)
{
  assert (btor);
  assert (parent);
  assert (BTOR_IS_REGULAR_NODE (parent));
  assert (btor == parent->btor);
  assert (child);
  assert (btor == BTOR_REAL_ADDR_NODE (child)->btor);
  assert (pos >= 0);
  assert (pos <= 2);
  assert (btor_simplify_exp (btor, child) == child);
  assert (!btor_is_args_node (child) || btor_is_args_node (parent)
          || btor_is_apply_node (parent) || btor_is_update_node (parent));

  (void) btor;
  int tag, insert_beginning = 1;
  BtorNode *real_child, *first_parent, *last_parent, *tagged_parent;

  /* set specific flags */

  /* set parent parameterized if child is parameterized */
  if (!btor_is_lambda_node (parent)
      && BTOR_REAL_ADDR_NODE (child)->parameterized)
    parent->parameterized = 1;

  // TODO (ma): why don't we bind params here?

  if (btor_is_fun_cond_node (parent) && BTOR_REAL_ADDR_NODE (child)->is_array)
    parent->is_array = 1;

  if (BTOR_REAL_ADDR_NODE (child)->lambda_below) parent->lambda_below = 1;

  if (BTOR_REAL_ADDR_NODE (child)->apply_below) parent->apply_below = 1;

  BTOR_REAL_ADDR_NODE (child)->parents++;
  inc_exp_ref_counter (btor, child);

  /* update parent lists */

  if (btor_is_apply_node (parent)) insert_beginning = 0;

  real_child     = BTOR_REAL_ADDR_NODE (child);
  parent->e[pos] = child;
  tagged_parent  = BTOR_TAG_NODE (parent, pos);

  assert (!parent->prev_parent[pos]);
  assert (!parent->next_parent[pos]);

  /* no parent so far? */
  if (!real_child->first_parent)
  {
    assert (!real_child->last_parent);
    real_child->first_parent = tagged_parent;
    real_child->last_parent  = tagged_parent;
  }
  /* add parent at the beginning of the list */
  else if (insert_beginning)
  {
    first_parent = real_child->first_parent;
    assert (first_parent);
    parent->next_parent[pos] = first_parent;
    tag                      = btor_exp_get_tag (first_parent);
    BTOR_REAL_ADDR_NODE (first_parent)->prev_parent[tag] = tagged_parent;
    real_child->first_parent                             = tagged_parent;
  }
  /* add parent at the end of the list */
  else
  {
    last_parent = real_child->last_parent;
    assert (last_parent);
    parent->prev_parent[pos] = last_parent;
    tag                      = btor_exp_get_tag (last_parent);
    BTOR_REAL_ADDR_NODE (last_parent)->next_parent[tag] = tagged_parent;
    real_child->last_parent                             = tagged_parent;
  }
}

static void
setup_node_and_add_to_id_table (Btor *btor, void *ptr)
{
  assert (btor);
  assert (ptr);

  BtorNode *exp;
  int id;

  exp = (BtorNode *) ptr;
  assert (!BTOR_IS_INVERTED_NODE (exp));
  assert (!exp->id);

  exp->refs = 1;
  exp->btor = btor;
  btor->stats.expressions++;
  id = BTOR_COUNT_STACK (btor->nodes_id_table);
  BTOR_ABORT (id == INT_MAX, "expression id overflow");
  exp->id = id;
  BTOR_PUSH_STACK (btor->nodes_id_table, exp);
  assert (BTOR_COUNT_STACK (btor->nodes_id_table) == exp->id + 1);
  assert (BTOR_PEEK_STACK (btor->nodes_id_table, exp->id) == exp);
  btor->stats.node_bytes_alloc += exp->bytes;

  if (btor_is_apply_node (exp)) exp->apply_below = 1;
}

static BtorNode *
new_const_exp_node (Btor *btor, BtorBitVector *bits)
{
  assert (btor);
  assert (bits);

  BtorBVConstNode *exp;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_BV_CONST_NODE);
  exp->bytes = sizeof *exp;
  btor_exp_set_sort_id ((BtorNode *) exp, btor_bitvec_sort (btor, bits->width));
  setup_node_and_add_to_id_table (btor, exp);
  btor_const_set_bits ((BtorNode *) exp, btor_copy_bv (btor->mm, bits));
  btor_const_set_invbits ((BtorNode *) exp, btor_not_bv (btor->mm, bits));
  return (BtorNode *) exp;
}

static BtorNode *
new_slice_exp_node (Btor *btor, BtorNode *e0, uint32_t upper, uint32_t lower)
{
  assert (btor);
  assert (e0);
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (upper < btor_get_exp_width (btor, e0));
  assert (upper >= lower);

  BtorSliceNode *exp = 0;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_SLICE_NODE);
  exp->bytes = sizeof *exp;
  exp->arity = 1;
  exp->upper = upper;
  exp->lower = lower;
  btor_exp_set_sort_id ((BtorNode *) exp,
                        btor_bitvec_sort (btor, upper - lower + 1));
  setup_node_and_add_to_id_table (btor, exp);
  connect_child_exp (btor, (BtorNode *) exp, e0, 0);
  return (BtorNode *) exp;
}

static BtorNode *
new_lambda_exp_node (Btor *btor, BtorNode *e_param, BtorNode *e_exp)
{
  assert (btor);
  assert (e_param);
  assert (BTOR_IS_REGULAR_NODE (e_param));
  assert (btor_is_param_node (e_param));
  assert (!btor_param_is_bound (e_param));
  assert (e_exp);
  assert (btor == e_param->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_exp)->btor);

  BtorSortId s, domain, codomain;
  BtorSortIdStack param_sorts;
  BtorLambdaNode *lambda_exp;
  BtorTupleSortIterator it;
  BtorPtrHashBucket *b;
  BtorIntHashTable *params;

  BTOR_INIT_STACK (btor->mm, param_sorts);

  BTOR_CNEW (btor->mm, lambda_exp);
  set_kind (btor, (BtorNode *) lambda_exp, BTOR_LAMBDA_NODE);
  lambda_exp->bytes        = sizeof *lambda_exp;
  lambda_exp->arity        = 2;
  lambda_exp->lambda_below = 1;
  setup_node_and_add_to_id_table (btor, (BtorNode *) lambda_exp);
  connect_child_exp (btor, (BtorNode *) lambda_exp, e_param, 0);
  connect_child_exp (btor, (BtorNode *) lambda_exp, e_exp, 1);

  BTOR_PUSH_STACK (param_sorts, btor_exp_get_sort_id (e_param));
  /* curried lambdas (functions) */
  if (btor_is_lambda_node (e_exp))
  {
    btor_lambda_set_body (
        (BtorNode *) lambda_exp,
        btor_simplify_exp (btor, btor_lambda_get_body (e_exp)));
    btor_init_tuple_sort_iterator (
        &it,
        btor,
        btor_get_domain_fun_sort (btor, btor_exp_get_sort_id (e_exp)));
    while (btor_has_next_tuple_sort_iterator (&it))
    {
      s = btor_next_tuple_sort_iterator (&it);
      BTOR_PUSH_STACK (param_sorts, s);
    }

    if ((b = btor_get_ptr_hash_table (btor->parameterized, e_exp)))
    {
      params = b->data.as_ptr;
      btor_remove_int_hash_table (params, e_param->id);
      btor_remove_ptr_hash_table (btor->parameterized, e_exp, 0, 0);
      if (params->count > 0)
      {
        btor_add_ptr_hash_table (btor->parameterized, lambda_exp)->data.as_ptr =
            params;
        lambda_exp->parameterized = 1;
      }
      else
        btor_delete_int_hash_table (params);
    }
  }
  else
    btor_lambda_set_body ((BtorNode *) lambda_exp, e_exp);

  domain =
      btor_tuple_sort (btor, param_sorts.start, BTOR_COUNT_STACK (param_sorts));
  codomain = btor_exp_get_sort_id (lambda_exp->body);
  btor_exp_set_sort_id ((BtorNode *) lambda_exp,
                        btor_fun_sort (btor, domain, codomain));

  btor_release_sort (btor, domain);
  BTOR_RELEASE_STACK (param_sorts);

  assert (!BTOR_REAL_ADDR_NODE (lambda_exp->body)->simplified);
  assert (!btor_is_lambda_node (lambda_exp->body));
  assert (!btor_get_ptr_hash_table (btor->lambdas, lambda_exp));
  (void) btor_add_ptr_hash_table (btor->lambdas, lambda_exp);
  /* set lambda expression of parameter */
  btor_param_set_binding_lambda (e_param, (BtorNode *) lambda_exp);
  return (BtorNode *) lambda_exp;
}

static BtorNode *
new_args_exp_node (Btor *btor, int arity, BtorNode *e[])
{
  assert (btor);
  assert (arity > 0);
  assert (arity <= 3);
  assert (e);

  int i;
  BtorArgsNode *exp;
  BtorSortIdStack sorts;
  BtorTupleSortIterator it;
#ifndef NDEBUG
  for (i = 0; i < arity; i++) assert (e[i]);
#endif

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_ARGS_NODE);
  exp->bytes = sizeof (*exp);
  exp->arity = arity;
  setup_node_and_add_to_id_table (btor, exp);

  for (i = 0; i < arity; i++)
    connect_child_exp (btor, (BtorNode *) exp, e[i], i);

  /* create tuple sort for argument node */
  BTOR_INIT_STACK (btor->mm, sorts);
  for (i = 0; i < arity; i++)
  {
    if (btor_is_args_node (e[i]))
    {
      assert (i == 2);
      assert (BTOR_IS_REGULAR_NODE (e[i]));
      btor_init_tuple_sort_iterator (&it, btor, btor_exp_get_sort_id (e[i]));
      while (btor_has_next_tuple_sort_iterator (&it))
        BTOR_PUSH_STACK (sorts, btor_next_tuple_sort_iterator (&it));
    }
    else
      BTOR_PUSH_STACK (sorts, btor_exp_get_sort_id (e[i]));
  }
  btor_exp_set_sort_id (
      (BtorNode *) exp,
      btor_tuple_sort (btor, sorts.start, BTOR_COUNT_STACK (sorts)));
  BTOR_RELEASE_STACK (sorts);
  return (BtorNode *) exp;
}

static BtorNode *
new_node (Btor *btor, BtorNodeKind kind, int arity, BtorNode *e[])
{
  assert (btor);
  assert (arity > 0);
  assert (arity <= 3);
  assert (btor_is_binary_node_kind (kind) || btor_is_ternary_node_kind (kind));
  assert (e);

#ifndef NDEBUG
  if (btor_get_opt (btor, BTOR_OPT_SORT_EXP) > 0
      && btor_is_binary_commutative_node_kind (kind))
    assert (arity == 2), assert (BTOR_REAL_ADDR_NODE (e[0])->id
                                 <= BTOR_REAL_ADDR_NODE (e[1])->id);
#endif

  int i;
  BtorBVNode *exp;
  BtorSortId sort;

#ifdef NDEBUG
  for (i = 0; i < arity; i++)
  {
    assert (e[i]);
    assert (btor == BTOR_REAL_ADDR_NODE (e[i])->btor);
  }
#endif

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, kind);
  exp->bytes = sizeof (*exp);
  exp->arity = arity;
  setup_node_and_add_to_id_table (btor, exp);

  switch (kind)
  {
    case BTOR_COND_NODE:
      sort = btor_copy_sort (btor, btor_exp_get_sort_id (e[1]));
      break;

    case BTOR_UPDATE_NODE:
      sort = btor_copy_sort (btor, btor_exp_get_sort_id (e[0]));
      break;

    case BTOR_CONCAT_NODE:
      sort = btor_bitvec_sort (
          btor,
          btor_get_exp_width (btor, e[0]) + btor_get_exp_width (btor, e[1]));
      break;

    case BTOR_FUN_EQ_NODE:
    case BTOR_BV_EQ_NODE:
    case BTOR_ULT_NODE: sort = btor_bool_sort (btor); break;

    case BTOR_APPLY_NODE:
      sort = btor_copy_sort (
          btor, btor_get_codomain_fun_sort (btor, btor_exp_get_sort_id (e[0])));
      break;

    default:
      assert (kind == BTOR_AND_NODE || kind == BTOR_ADD_NODE
              || kind == BTOR_MUL_NODE || kind == BTOR_SLL_NODE
              || kind == BTOR_SRL_NODE || kind == BTOR_UDIV_NODE
              || kind == BTOR_UREM_NODE);
      sort = btor_copy_sort (btor, btor_exp_get_sort_id (e[0]));
  }

  btor_exp_set_sort_id ((BtorNode *) exp, sort);

  for (i = 0; i < arity; i++)
    connect_child_exp (btor, (BtorNode *) exp, e[i], i);

  if (kind == BTOR_FUN_EQ_NODE)
  {
    assert (!btor_get_ptr_hash_table (btor->feqs, exp));
    btor_add_ptr_hash_table (btor->feqs, exp)->data.as_int = 0;
  }

  return (BtorNode *) exp;
}

/* Search for constant expression in hash table. Returns 0 if not found. */
static BtorNode **
find_const_exp (Btor *btor, BtorBitVector *bits)
{
  assert (btor);
  assert (bits);

  BtorNode *cur, **result;
  unsigned int hash;

  hash = btor_hash_bv (bits);
  hash &= btor->nodes_unique_table.size - 1;
  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (btor_is_bv_const_node (cur)
        && btor_get_exp_width (btor, cur) == bits->width
        && !btor_compare_bv (btor_const_get_bits (cur), bits))
      break;
    else
    {
      result = &cur->next;
      cur    = *result;
    }
  }
  return result;
}

/* Search for slice expression in hash table. Returns 0 if not found. */
static BtorNode **
find_slice_exp (Btor *btor, BtorNode *e0, uint32_t upper, uint32_t lower)
{
  assert (btor);
  assert (e0);
  assert (upper >= lower);

  BtorNode *cur, **result;
  unsigned int hash;

  hash = hash_slice_exp (e0, upper, lower);
  hash &= btor->nodes_unique_table.size - 1;
  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->kind == BTOR_SLICE_NODE && cur->e[0] == e0
        && btor_slice_get_upper (cur) == upper
        && btor_slice_get_lower (cur) == lower)
      break;
    else
    {
      result = &cur->next;
      cur    = *result;
    }
  }
  return result;
}

static BtorNode **
find_bv_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[], uint32_t arity)
{
  bool equal;
  uint32_t i;
  unsigned int hash;
  BtorNode *cur, **result;

  assert (kind != BTOR_SLICE_NODE);
  assert (kind != BTOR_BV_CONST_NODE);

  sort_bv_exp (btor, kind, e);
  hash = hash_bv_exp (btor, kind, arity, e);
  hash &= btor->nodes_unique_table.size - 1;

  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->kind == kind && cur->arity == arity)
    {
      equal = true;
      /* special case for bv eq; (= (bvnot a) b) == (= a (bvnot b)) */
      if (kind == BTOR_BV_EQ_NODE && cur->e[0] == BTOR_INVERT_NODE (e[0])
          && cur->e[1] == BTOR_INVERT_NODE (e[1]))
        break;
      for (i = 0; i < arity && equal; i++)
        if (cur->e[i] != e[i]) equal = false;
      if (equal) break;
#ifndef NDEBUG
      if (btor_get_opt (btor, BTOR_OPT_SORT_EXP) > 0
          && btor_is_binary_commutative_node_kind (kind))
        assert (arity == 2),
            assert (e[0] == e[1] || BTOR_INVERT_NODE (e[0]) == e[1]
                    || !(cur->e[0] == e[1] && cur->e[1] == e[0]));
#endif
    }
    result = &(cur->next);
    cur    = *result;
  }
  return result;
}

static int compare_lambda_exp (Btor *, BtorNode *, BtorNode *, BtorNode *);

static BtorNode **
find_lambda_exp (Btor *btor,
                 BtorNode *param,
                 BtorNode *body,
                 unsigned int *lambda_hash,
                 BtorIntHashTable *params,
                 int compare_lambdas)
{
  assert (btor);
  assert (param);
  assert (body);
  assert (BTOR_IS_REGULAR_NODE (param));
  assert (btor_is_param_node (param));

  BtorNode *cur, **result;
  unsigned int hash;

  hash = hash_lambda_exp (btor, param, body, params);
  if (lambda_hash) *lambda_hash = hash;
  hash &= btor->nodes_unique_table.size - 1;
  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->kind == BTOR_LAMBDA_NODE
        && ((param == cur->e[0] && body == cur->e[1])
            || ((!cur->parameterized && compare_lambdas
                 && compare_lambda_exp (btor, param, body, cur)))))
      break;
    else
    {
      result = &cur->next;
      cur    = *result;
    }
  }
  assert (!*result || btor_is_lambda_node (*result));
  return result;
}

static int
compare_lambda_exp (Btor *btor,
                    BtorNode *param,
                    BtorNode *body,
                    BtorNode *lambda)
{
  assert (btor);
  assert (param);
  assert (body);
  assert (BTOR_IS_REGULAR_NODE (param));
  assert (btor_is_param_node (param));
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  assert (!lambda->parameterized);

  int i, equal = 0;
  BtorMemMgr *mm;
  BtorNode *cur, *real_cur, **result, *subst_param, **e, *l0, *l1;
  BtorPtrHashTable *cache, *param_map;
  BtorPtrHashBucket *b, *bb;
  BtorNodePtrStack stack, args;
  BtorNodeIterator it, iit;

  mm          = btor->mm;
  subst_param = lambda->e[0];

  if (btor_exp_get_sort_id (subst_param) != btor_exp_get_sort_id (param)
      || btor_exp_get_sort_id (body) != btor_exp_get_sort_id (lambda->e[1]))
    return 0;

  cache = btor_new_ptr_hash_table (mm, 0, 0);

  /* create param map */
  param_map = btor_new_ptr_hash_table (mm, 0, 0);
  btor_add_ptr_hash_table (param_map, param)->data.as_ptr = subst_param;

  if (btor_is_lambda_node (body) && btor_is_lambda_node (lambda->e[1]))
  {
    btor_init_lambda_iterator (&it, body);
    btor_init_lambda_iterator (&iit, lambda->e[1]);
    while (btor_has_next_lambda_iterator (&it))
    {
      if (!btor_has_next_lambda_iterator (&iit)) goto NOT_EQUAL;

      l0 = btor_next_lambda_iterator (&it);
      l1 = btor_next_lambda_iterator (&iit);

      if (btor_exp_get_sort_id (l0) != btor_exp_get_sort_id (l1))
        goto NOT_EQUAL;

      param       = l0->e[0];
      subst_param = l1->e[0];
      assert (BTOR_IS_REGULAR_NODE (param));
      assert (BTOR_IS_REGULAR_NODE (subst_param));
      assert (btor_is_param_node (param));
      assert (btor_is_param_node (subst_param));

      if (btor_exp_get_sort_id (param) != btor_exp_get_sort_id (subst_param))
        goto NOT_EQUAL;

      btor_add_ptr_hash_table (param_map, param)->data.as_ptr = subst_param;
    }
  }
  else if (btor_is_lambda_node (body) || btor_is_lambda_node (lambda->e[1]))
    goto NOT_EQUAL;

  BTOR_INIT_STACK (mm, args);
  BTOR_INIT_STACK (mm, stack);
  BTOR_PUSH_STACK (stack, body);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    if (!real_cur->parameterized)
    {
      BTOR_PUSH_STACK (args, cur);
      continue;
    }

    b = btor_get_ptr_hash_table (cache, real_cur);

    if (!b)
    {
      b = btor_add_ptr_hash_table (cache, real_cur);
      BTOR_PUSH_STACK (stack, cur);
      for (i = real_cur->arity - 1; i >= 0; i--)
        BTOR_PUSH_STACK (stack, real_cur->e[i]);
    }
    else if (!b->data.as_ptr)
    {
      assert (BTOR_COUNT_STACK (args) >= real_cur->arity);
      args.top -= real_cur->arity;
      e = args.top;

      if (btor_is_slice_node (real_cur))
      {
        result = find_slice_exp (btor,
                                 e[0],
                                 btor_slice_get_upper (real_cur),
                                 btor_slice_get_lower (real_cur));
      }
      else if (btor_is_lambda_node (real_cur))
      {
        result = find_lambda_exp (btor, e[0], e[1], 0, 0, 0);
      }
      else if (btor_is_param_node (real_cur))
      {
        if ((bb = btor_get_ptr_hash_table (param_map, real_cur)))
          result = (BtorNode **) &bb->data.as_ptr;
        else
          result = &real_cur;
      }
      else
      {
        assert (!btor_is_lambda_node (real_cur));
        result = find_bv_exp (btor, real_cur->kind, e, real_cur->arity);
      }

      if (!*result)
      {
        BTOR_RESET_STACK (args);
        break;
      }

      BTOR_PUSH_STACK (args, BTOR_COND_INVERT_NODE (cur, *result));
      b->data.as_ptr = *result;
    }
    else
    {
      assert (b->data.as_ptr);
      BTOR_PUSH_STACK (args, BTOR_COND_INVERT_NODE (cur, b->data.as_ptr));
    }
  }
  assert (BTOR_COUNT_STACK (args) <= 1);

  if (!BTOR_EMPTY_STACK (args)) equal = BTOR_TOP_STACK (args) == lambda->e[1];

  BTOR_RELEASE_STACK (stack);
  BTOR_RELEASE_STACK (args);
NOT_EQUAL:
  btor_delete_ptr_hash_table (cache);
  btor_delete_ptr_hash_table (param_map);
  return equal;
}

static BtorNode **
find_exp (Btor *btor,
          BtorNodeKind kind,
          BtorNode *e[],
          uint32_t arity,
          unsigned int *lambda_hash,
          BtorIntHashTable *params)
{
  assert (btor);
  assert (arity > 0);
  assert (e);

#ifndef NDEBUG
  uint32_t i;
  for (i = 0; i < arity; i++) assert (e[i]);
#endif

  if (kind == BTOR_LAMBDA_NODE)
    return find_lambda_exp (btor, e[0], e[1], lambda_hash, params, 1);
  else if (lambda_hash)
    *lambda_hash = 0;

  return find_bv_exp (btor, kind, e, arity);
}

/* Enlarges unique table and rehashes expressions. */
static void
enlarge_nodes_unique_table (Btor *btor)
{
  assert (btor);

  BtorMemMgr *mm;
  int size, new_size, i;
  unsigned int hash;
  BtorNode *cur, *temp, **new_chains;

  mm       = btor->mm;
  size     = btor->nodes_unique_table.size;
  new_size = size ? 2 * size : 1;
  BTOR_CNEWN (mm, new_chains, new_size);
  for (i = 0; i < size; i++)
  {
    cur = btor->nodes_unique_table.chains[i];
    while (cur)
    {
      assert (BTOR_IS_REGULAR_NODE (cur));
      assert (!btor_is_bv_var_node (cur));
      assert (!btor_is_uf_node (cur));
      temp             = cur->next;
      hash             = compute_hash_exp (btor, cur, new_size);
      cur->next        = new_chains[hash];
      new_chains[hash] = cur;
      cur              = temp;
    }
  }
  BTOR_DELETEN (mm, btor->nodes_unique_table.chains, size);
  btor->nodes_unique_table.size   = new_size;
  btor->nodes_unique_table.chains = new_chains;
}

BtorNode *
btor_const_exp (Btor *btor, const BtorBitVector *bits)
{
  assert (btor);
  assert (bits);

  bool inv;
  BtorBitVector *lookupbits;
  BtorNode **lookup;

  /* normalize constants, constants are always even */
  if (btor_get_bit_bv (bits, 0))
  {
    lookupbits = btor_not_bv (btor->mm, bits);
    inv        = true;
  }
  else
  {
    lookupbits = btor_copy_bv (btor->mm, bits);
    inv        = false;
  }

  lookup = find_const_exp (btor, lookupbits);
  if (!*lookup)
  {
    if (BTOR_FULL_UNIQUE_TABLE (btor->nodes_unique_table))
    {
      enlarge_nodes_unique_table (btor);
      lookup = find_const_exp (btor, lookupbits);
    }
    *lookup = new_const_exp_node (btor, lookupbits);
    assert (btor->nodes_unique_table.num_elements < INT_MAX);
    btor->nodes_unique_table.num_elements += 1;
    (*lookup)->unique = 1;
  }
  else
    inc_exp_ref_counter (btor, *lookup);

  assert (BTOR_IS_REGULAR_NODE (*lookup));

  btor_free_bv (btor->mm, lookupbits);

  if (inv) return BTOR_INVERT_NODE (*lookup);
  return *lookup;
}

static BtorNode *
int_min_exp (Btor *btor, uint32_t width)
{
  assert (btor);
  assert (width > 0);

  BtorBitVector *bv;
  BtorNode *result;

  bv = btor_new_bv (btor->mm, width);
  btor_set_bit_bv (bv, bv->width - 1, 1);
  result = btor_const_exp (btor, bv);
  btor_free_bv (btor->mm, bv);
  return result;
}

BtorNode *
btor_zero_exp (Btor *btor, BtorSortId sort)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));

  uint32_t width;
  BtorNode *result;
  BtorBitVector *bv;

  width  = btor_get_width_bitvec_sort (btor, sort);
  bv     = btor_new_bv (btor->mm, width);
  result = btor_const_exp (btor, bv);
  btor_free_bv (btor->mm, bv);
  return result;
}

BtorNode *
btor_ones_exp (Btor *btor, BtorSortId sort)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));

  uint32_t width;
  BtorNode *result;
  BtorBitVector *bv;

  width  = btor_get_width_bitvec_sort (btor, sort);
  bv     = btor_ones_bv (btor->mm, width);
  result = btor_const_exp (btor, bv);
  btor_free_bv (btor->mm, bv);
  return result;
}

BtorNode *
btor_one_exp (Btor *btor, BtorSortId sort)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));

  uint32_t width;
  BtorNode *result;
  BtorBitVector *bv;

  width  = btor_get_width_bitvec_sort (btor, sort);
  bv     = btor_one_bv (btor->mm, width);
  result = btor_const_exp (btor, bv);
  btor_free_bv (btor->mm, bv);
  return result;
}

BtorNode *
btor_int_exp (Btor *btor, int32_t i, BtorSortId sort)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));

  uint32_t width;
  BtorNode *result;
  BtorBitVector *bv;

  width  = btor_get_width_bitvec_sort (btor, sort);
  bv     = btor_int64_to_bv (btor->mm, i, width);
  result = btor_const_exp (btor, bv);
  btor_free_bv (btor->mm, bv);
  return result;
}

BtorNode *
btor_unsigned_exp (Btor *btor, uint32_t u, BtorSortId sort)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));

  uint32_t width;
  BtorNode *result;
  BtorBitVector *bv;

  width  = btor_get_width_bitvec_sort (btor, sort);
  bv     = btor_uint64_to_bv (btor->mm, u, width);
  result = btor_const_exp (btor, bv);
  btor_free_bv (btor->mm, bv);
  return result;
}

BtorNode *
btor_true_exp (Btor *btor)
{
  assert (btor);

  BtorSortId sort;
  BtorNode *result;

  sort   = btor_bitvec_sort (btor, 1);
  result = btor_one_exp (btor, sort);
  btor_release_sort (btor, sort);
  return result;
}

BtorNode *
btor_false_exp (Btor *btor)
{
  assert (btor);

  BtorSortId sort;
  BtorNode *result;

  sort   = btor_bitvec_sort (btor, 1);
  result = btor_zero_exp (btor, sort);
  btor_release_sort (btor, sort);
  return result;
}

BtorNode *
btor_var_exp (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));
  assert (!symbol || !btor_get_ptr_hash_table (btor->symbols, (char *) symbol));

  BtorBVVarNode *exp;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_BV_VAR_NODE);
  exp->bytes = sizeof *exp;
  setup_node_and_add_to_id_table (btor, exp);
  btor_exp_set_sort_id ((BtorNode *) exp, btor_copy_sort (btor, sort));
  (void) btor_add_ptr_hash_table (btor->bv_vars, exp);
  if (symbol) btor_set_symbol_exp (btor, (BtorNode *) exp, symbol);
  return (BtorNode *) exp;
}

BtorNode *
btor_param_exp (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (btor_is_bitvec_sort (btor, sort));
  assert (!symbol || !btor_get_ptr_hash_table (btor->symbols, (char *) symbol));

  BtorParamNode *exp;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_PARAM_NODE);
  exp->bytes         = sizeof *exp;
  exp->parameterized = 1;
  btor_exp_set_sort_id ((BtorNode *) exp, btor_copy_sort (btor, sort));
  setup_node_and_add_to_id_table (btor, exp);
  if (symbol) btor_set_symbol_exp (btor, (BtorNode *) exp, symbol);
  return (BtorNode *) exp;
}

BtorNode *
btor_array_exp (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (btor_is_fun_sort (btor, sort));
  assert (
      btor_get_arity_tuple_sort (btor, btor_get_domain_fun_sort (btor, sort))
      == 1);

  BtorNode *exp;

  exp           = btor_uf_exp (btor, sort, symbol);
  exp->is_array = 1;

  return exp;
}

BtorNode *
btor_uf_exp (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (!symbol || !btor_get_ptr_hash_table (btor->symbols, (char *) symbol));

  BtorUFNode *exp;

  assert (btor_is_fun_sort (btor, sort));
  assert (btor_is_bitvec_sort (btor, btor_get_codomain_fun_sort (btor, sort))
          || btor_is_bool_sort (btor, btor_get_codomain_fun_sort (btor, sort)));

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_UF_NODE);
  exp->bytes = sizeof (*exp);
  btor_exp_set_sort_id ((BtorNode *) exp, btor_copy_sort (btor, sort));
  setup_node_and_add_to_id_table (btor, exp);
  (void) btor_add_ptr_hash_table (btor->ufs, exp);
  if (symbol) btor_set_symbol_exp (btor, (BtorNode *) exp, symbol);
  return (BtorNode *) exp;
}

static BtorNode *
unary_exp_slice_exp (Btor *btor, BtorNode *exp, uint32_t upper, uint32_t lower)
{
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  int inv;
  BtorNode **lookup;

  exp = btor_simplify_exp (btor, exp);

  assert (!btor_is_fun_node (exp));
  assert (upper >= lower);
  assert (upper < btor_get_exp_width (btor, exp));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0
      && BTOR_IS_INVERTED_NODE (exp))
  {
    inv = 1;
    exp = BTOR_INVERT_NODE (exp);
  }
  else
    inv = 0;

  lookup = find_slice_exp (btor, exp, upper, lower);
  if (!*lookup)
  {
    if (BTOR_FULL_UNIQUE_TABLE (btor->nodes_unique_table))
    {
      enlarge_nodes_unique_table (btor);
      lookup = find_slice_exp (btor, exp, upper, lower);
    }
    *lookup = new_slice_exp_node (btor, exp, upper, lower);
    assert (btor->nodes_unique_table.num_elements < INT_MAX);
    btor->nodes_unique_table.num_elements++;
    (*lookup)->unique = 1;
  }
  else
    inc_exp_ref_counter (btor, *lookup);
  assert (BTOR_IS_REGULAR_NODE (*lookup));
  if (inv) return BTOR_INVERT_NODE (*lookup);
  return *lookup;
}

BtorNode *
btor_slice_exp_node (Btor *btor, BtorNode *exp, uint32_t upper, uint32_t lower)
{
  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_slice_exp_dbg (btor, exp, upper, lower));
  return unary_exp_slice_exp (btor, exp, upper, lower);
}

static BtorNode *
create_exp (Btor *btor, BtorNodeKind kind, uint32_t arity, BtorNode *e[])
{
  assert (btor);
  assert (kind);
  assert (arity > 0);
  assert (arity <= 3);
  assert (e);

  uint32_t i;
  unsigned int lambda_hash;
  BtorNode **lookup, *simp_e[3];
  BtorIntHashTable *params = 0;

  for (i = 0; i < arity; i++)
  {
    assert (BTOR_REAL_ADDR_NODE (e[i])->btor == btor);
    simp_e[i] = btor_simplify_exp (btor, e[i]);
  }

  /* collect params only for function bodies */
  if (kind == BTOR_LAMBDA_NODE && !btor_is_lambda_node (e[1]))
    params = btor_new_int_hash_table (btor->mm);

  lookup = find_exp (btor, kind, simp_e, arity, &lambda_hash, params);
  if (!*lookup)
  {
    if (BTOR_FULL_UNIQUE_TABLE (btor->nodes_unique_table))
    {
      enlarge_nodes_unique_table (btor);
      lookup = find_exp (btor, kind, simp_e, arity, &lambda_hash, 0);
    }

    switch (kind)
    {
      case BTOR_LAMBDA_NODE:
        assert (arity == 2);
        *lookup = new_lambda_exp_node (btor, simp_e[0], simp_e[1]);
        btor_get_ptr_hash_table (btor->lambdas, *lookup)->data.as_int =
            lambda_hash;
        if (params)
        {
          if (params->count > 0)
          {
            btor_add_ptr_hash_table (btor->parameterized, *lookup)
                ->data.as_ptr        = params;
            (*lookup)->parameterized = 1;
          }
          else
            btor_delete_int_hash_table (params);
        }
        break;
      case BTOR_ARGS_NODE:
        *lookup = new_args_exp_node (btor, arity, simp_e);
        break;
      default: *lookup = new_node (btor, kind, arity, simp_e);
    }
    assert (btor->nodes_unique_table.num_elements < INT_MAX);
    btor->nodes_unique_table.num_elements++;
    (*lookup)->unique = 1;
  }
  else
  {
    inc_exp_ref_counter (btor, *lookup);
    if (params) btor_delete_int_hash_table (params);
  }
  assert (BTOR_IS_REGULAR_NODE (*lookup));
  return *lookup;
}

BtorNode *
btor_and_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_AND_NODE, 2, e);
}

BtorNode *
btor_eq_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  BtorNodeKind kind;

  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_eq_exp_dbg (btor, e[0], e[1]));
  if (btor_is_fun_node (e[0]))
    kind = BTOR_FUN_EQ_NODE;
  else
    kind = BTOR_BV_EQ_NODE;
  return create_exp (btor, kind, 2, e);
}

BtorNode *
btor_add_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_ADD_NODE, 2, e);
}

BtorNode *
btor_mul_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_MUL_NODE, 2, e);
}

BtorNode *
btor_ult_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_ULT_NODE, 2, e);
}

BtorNode *
btor_sll_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_SLL_NODE, 2, e);
}

BtorNode *
btor_srl_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_SRL_NODE, 2, e);
}

BtorNode *
btor_udiv_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_UDIV_NODE, 2, e);
}

BtorNode *
btor_urem_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_UREM_NODE, 2, e);
}

BtorNode *
btor_concat_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_concat_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_CONCAT_NODE, 2, e);
}

BtorNode *
btor_lambda_exp_node (Btor *btor, BtorNode *e_param, BtorNode *e_exp)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e_param);
  e[1] = btor_simplify_exp (btor, e_exp);
  return create_exp (btor, BTOR_LAMBDA_NODE, 2, e);
}

BtorNode *
btor_lambda_exp (Btor *btor, BtorNode *e_param, BtorNode *e_exp)
{
  assert (btor);
  assert (BTOR_IS_REGULAR_NODE (e_param));
  assert (btor == e_param->btor);
  assert (btor_is_param_node (e_param));
  assert (!BTOR_REAL_ADDR_NODE (e_param)->simplified);
  assert (e_exp);
  assert (btor == BTOR_REAL_ADDR_NODE (e_exp)->btor);

  BtorNode *result;
  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_LAMBDA_NODE, e_param, e_exp);
  else
    result = btor_lambda_exp_node (btor, e_param, e_exp);
  assert (btor_is_fun_node (result));
  return result;
}

BtorNode *
btor_fun_exp (Btor *btor, BtorNode *params[], uint32_t paramc, BtorNode *exp)
{
  assert (btor);
  assert (paramc > 0);
  assert (params);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  assert (!btor_is_uf_node (exp));

  int i;
  BtorNode *fun      = btor_simplify_exp (btor, exp);
  BtorNode *prev_fun = 0;

  for (i = paramc - 1; i >= 0; i--)
  {
    assert (params[i]);
    assert (btor == BTOR_REAL_ADDR_NODE (params[i])->btor);
    assert (btor_is_param_node (params[i]));
    fun = btor_lambda_exp (btor, params[i], fun);
    if (prev_fun) btor_release_exp (btor, prev_fun);
    prev_fun = fun;
  }

  return fun;
}

/* more than 4 children are not possible as we only have 2 bit for storing
 * the position in the parent pointers */
#define ARGS_MAX_NUM_CHILDREN 3

BtorNode *
btor_args_exp (Btor *btor, BtorNode *args[], uint32_t argc)
{
  assert (btor);
  assert (argc > 0);
  assert (args);

  int i, cur_argc, cnt_args, rem_free, num_args;
  BtorNode *e[ARGS_MAX_NUM_CHILDREN];
  BtorNode *result = 0, *last = 0;

  /* arguments fit in one args node */
  if (argc <= ARGS_MAX_NUM_CHILDREN)
  {
    num_args = 1;
    rem_free = ARGS_MAX_NUM_CHILDREN - argc;
    cur_argc = argc;
  }
  /* arguments have to be split into several args nodes.
   * compute number of required args nodes */
  else
  {
    rem_free = argc % (ARGS_MAX_NUM_CHILDREN - 1);
    num_args = argc / (ARGS_MAX_NUM_CHILDREN - 1);
    /* we can store at most 1 more element into 'num_args' nodes
     * without needing an additional args node */
    if (rem_free > 1) num_args += 1;

    assert (num_args > 1);
    /* compute number of arguments in last args node */
    cur_argc = argc - (num_args - 1) * (ARGS_MAX_NUM_CHILDREN - 1);
  }
  cnt_args = cur_argc - 1;

  /* split up args in 'num_args' of args nodes */
  for (i = argc - 1; i >= 0; i--)
  {
    assert (cnt_args >= 0);
    assert (cnt_args <= ARGS_MAX_NUM_CHILDREN);
    assert (!btor_is_fun_node (args[i]));
    assert (btor == BTOR_REAL_ADDR_NODE (args[i])->btor);
    e[cnt_args] = btor_simplify_exp (btor, args[i]);
    cnt_args -= 1;

    assert (i > 0 || cnt_args < 0);
    if (cnt_args < 0)
    {
      result = create_exp (btor, BTOR_ARGS_NODE, cur_argc, e);

      /* init for next iteration */
      cur_argc    = ARGS_MAX_NUM_CHILDREN;
      cnt_args    = cur_argc - 1;
      e[cnt_args] = result;
      cnt_args -= 1;

      if (last) btor_release_exp (btor, last);

      last = result;
    }
  }

  assert (result);
  return result;
}

BtorNode *
btor_apply_exp_node (Btor *btor, BtorNode *fun, BtorNode *args)
{
  assert (btor);
  assert (fun);
  assert (args);
  assert (btor == BTOR_REAL_ADDR_NODE (fun)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (args)->btor);
  assert (btor_precond_apply_exp_dbg (btor, fun, args));

  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, fun);
  e[1] = btor_simplify_exp (btor, args);

  assert (BTOR_IS_REGULAR_NODE (e[0]));
  assert (BTOR_IS_REGULAR_NODE (e[1]));
  assert (btor_is_fun_node (e[0]));
  assert (btor_is_args_node (e[1]));

  /* eliminate nested functions */
  if (btor_is_lambda_node (e[0]) && e[0]->parameterized)
  {
    btor_assign_args (btor, e[0], args);
    BtorNode *result = btor_beta_reduce_bounded (btor, e[0], 1);
    btor_unassign_params (btor, e[0]);
    return result;
  }
  assert (!btor_is_fun_cond_node (e[0])
          || (!e[0]->e[1]->parameterized && !e[0]->e[2]->parameterized));
  return create_exp (btor, BTOR_APPLY_NODE, 2, e);
}

BtorNode *
btor_apply_exp (Btor *btor, BtorNode *fun, BtorNode *args)
{
  assert (btor);
  assert (fun);
  assert (args);
  assert (btor == BTOR_REAL_ADDR_NODE (fun)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (args)->btor);

  fun  = btor_simplify_exp (btor, fun);
  args = btor_simplify_exp (btor, args);
  assert (btor_is_fun_node (fun));
  assert (btor_is_args_node (args));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    return btor_rewrite_binary_exp (btor, BTOR_APPLY_NODE, fun, args);

  return btor_apply_exp_node (btor, fun, args);
}

BtorNode *
btor_apply_exps (Btor *btor, BtorNode *args[], uint32_t argc, BtorNode *fun)
{
  assert (btor);
  assert (argc > 0);
  assert (args);
  assert (fun);

  BtorNode *exp, *_args;

  _args = btor_args_exp (btor, args, argc);
  fun   = btor_simplify_exp (btor, fun);
  _args = btor_simplify_exp (btor, _args);

  exp = btor_apply_exp (btor, fun, _args);
  btor_release_exp (btor, _args);

  return exp;
}

BtorNode *
btor_cond_exp_node (Btor *btor,
                    BtorNode *e_cond,
                    BtorNode *e_if,
                    BtorNode *e_else)
{
  uint32_t i, arity;
  BtorNode *e[3], *cond, *lambda;
  BtorNodePtrStack params;
  BtorSort *sort;
  e[0] = btor_simplify_exp (btor, e_cond);
  e[1] = btor_simplify_exp (btor, e_if);
  e[2] = btor_simplify_exp (btor, e_else);
  assert (btor_precond_cond_exp_dbg (btor, e[0], e[1], e[2]));

  /* represent parameterized function conditionals (with parameterized
   * functions) as parameterized function
   * -> gets beta reduced in btor_apply_exp_node */
  if (btor_is_fun_node (e[1]) && (e[1]->parameterized || e[2]->parameterized))
  {
    BTOR_INIT_STACK (btor->mm, params);
    assert (btor_is_fun_sort (btor, btor_exp_get_sort_id (e[1])));
    arity = btor_get_fun_arity (btor, e[1]);
    sort  = btor_get_sort_by_id (btor, btor_exp_get_sort_id (e[1]));
    assert (sort->fun.domain->kind == BTOR_TUPLE_SORT);
    assert (sort->fun.domain->tuple.num_elements == arity);
    for (i = 0; i < arity; i++)
      BTOR_PUSH_STACK (
          params,
          btor_param_exp (btor, sort->fun.domain->tuple.elements[i]->id, 0));
    e[1]   = btor_apply_exps (btor, params.start, arity, e[1]);
    e[2]   = btor_apply_exps (btor, params.start, arity, e[2]);
    cond   = create_exp (btor, BTOR_COND_NODE, 3, e);
    lambda = btor_fun_exp (btor, params.start, arity, cond);
    while (!BTOR_EMPTY_STACK (params))
      btor_release_exp (btor, BTOR_POP_STACK (params));
    btor_release_exp (btor, e[1]);
    btor_release_exp (btor, e[2]);
    btor_release_exp (btor, cond);
    BTOR_RELEASE_STACK (params);
    return lambda;
  }
  return create_exp (btor, BTOR_COND_NODE, 3, e);
}

#if 0
BtorNode *
btor_bv_cond_exp_node (Btor * btor, BtorNode * e_cond, BtorNode * e_if,
		       BtorNode * e_else)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e_cond)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_if)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_else)->btor);

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    return btor_rewrite_ternary_exp (btor, BTOR_BCOND_NODE, e_cond, e_if, e_else);

  return btor_cond_exp_node (btor, e_cond, e_if, e_else);
}

// TODO: arbitrary conditionals on functions
BtorNode *
btor_array_cond_exp_node (Btor * btor, BtorNode * e_cond, BtorNode * e_if,
			  BtorNode * e_else)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e_cond)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_if)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_else)->btor);

  BtorNode *cond, *param, *lambda, *app_if, *app_else;

  e_cond = btor_simplify_exp (btor, e_cond);
  e_if = btor_simplify_exp (btor, e_if);
  e_else = btor_simplify_exp (btor, e_else);

  assert (BTOR_IS_REGULAR_NODE (e_if));
  assert (btor_is_fun_node (e_if));
  assert (BTOR_IS_REGULAR_NODE (e_else));
  assert (btor_is_fun_node (e_else));

  param = btor_param_exp (btor, btor_exp_get_sort_id (e_if), 0);
  app_if = btor_apply_exps (btor, &param, 1, e_if); 
  app_else = btor_apply_exps (btor, &param, 1, e_else);
  cond = btor_bv_cond_exp_node (btor, e_cond, app_if, app_else); 
  lambda = btor_lambda_exp (btor, param, cond); 
  lambda->is_array = 1;

  btor_release_exp (btor, param);
  btor_release_exp (btor, app_if);
  btor_release_exp (btor, app_else);
  btor_release_exp (btor, cond);
  
  return lambda;
}
#endif

BtorNode *
btor_not_exp (Btor *btor, BtorNode *exp)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));

  (void) btor;
  inc_exp_ref_counter (btor, exp);
  return BTOR_INVERT_NODE (exp);
}

BtorNode *
btor_add_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_ADD_NODE, e0, e1);
  else
    result = btor_add_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_neg_exp (Btor *btor, BtorNode *exp)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result, *one;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));
  one    = btor_one_exp (btor, btor_exp_get_sort_id (exp));
  result = btor_add_exp (btor, BTOR_INVERT_NODE (exp), one);
  btor_release_exp (btor, one);
  return result;
}

BtorNode *
btor_slice_exp (Btor *btor, BtorNode *exp, uint32_t upper, uint32_t lower)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_slice_exp_dbg (btor, exp, upper, lower));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_slice_exp (btor, exp, upper, lower);
  else
    result = btor_slice_exp_node (btor, exp, upper, lower);

  assert (result);
  return result;
}

BtorNode *
btor_or_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  return BTOR_INVERT_NODE (
      btor_and_exp (btor, BTOR_INVERT_NODE (e0), BTOR_INVERT_NODE (e1)));
}

BtorNode *
btor_eq_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_eq_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
  {
    if (btor_is_fun_node (e0))
      result = btor_rewrite_binary_exp (btor, BTOR_FUN_EQ_NODE, e0, e1);
    else
      result = btor_rewrite_binary_exp (btor, BTOR_BV_EQ_NODE, e0, e1);
  }
  else
    result = btor_eq_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_and_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_AND_NODE, e0, e1);
  else
    result = btor_and_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

static BtorNode *
create_bin_n_exp (Btor *btor,
                  BtorNode *(*func) (Btor *, BtorNode *, BtorNode *),
                  BtorNode *args[],
                  uint32_t argc)
{
  assert (argc > 0);

  uint32_t i;
  BtorNode *result, *tmp, *arg;

  result = 0;
  for (i = 0; i < argc; i++)
  {
    arg = args[i];
    if (result)
    {
      tmp = func (btor, arg, result);
      btor_release_exp (btor, result);
      result = tmp;
    }
    else
      result = btor_copy_exp (btor, arg);
  }
  assert (result);
  return result;
}

BtorNode *
btor_and_n_exp (Btor *btor, BtorNode *args[], uint32_t argc)
{
  return create_bin_n_exp (btor, btor_and_exp, args, argc);
}

BtorNode *
btor_xor_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, * or, *and;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  or     = btor_or_exp (btor, e0, e1);
  and    = btor_and_exp (btor, e0, e1);
  result = btor_and_exp (btor, or, BTOR_INVERT_NODE (and));
  btor_release_exp (btor, or);
  btor_release_exp (btor, and);
  return result;
}

BtorNode *
btor_xnor_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  return BTOR_INVERT_NODE (btor_xor_exp (btor, e0, e1));
}

BtorNode *
btor_concat_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_concat_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_CONCAT_NODE, e0, e1);
  else
    result = btor_concat_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_cond_exp (Btor *btor, BtorNode *e_cond, BtorNode *e_if, BtorNode *e_else)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e_cond)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_if)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_else)->btor);

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    return btor_rewrite_ternary_exp (
        btor, BTOR_COND_NODE, e_cond, e_if, e_else);

  return btor_cond_exp_node (btor, e_cond, e_if, e_else);
}

BtorNode *
btor_redor_exp (Btor *btor, BtorNode *exp)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result, *zero;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));

  zero   = btor_zero_exp (btor, btor_exp_get_sort_id (exp));
  result = BTOR_INVERT_NODE (btor_eq_exp (btor, exp, zero));
  btor_release_exp (btor, zero);
  return result;
}

BtorNode *
btor_redxor_exp (Btor *btor, BtorNode *exp)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result, *slice, *xor;
  int i, width;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));

  width = btor_get_exp_width (btor, exp);

  result = btor_slice_exp (btor, exp, 0, 0);
  for (i = 1; i < width; i++)
  {
    slice = btor_slice_exp (btor, exp, i, i);
    xor   = btor_xor_exp (btor, result, slice);
    btor_release_exp (btor, slice);
    btor_release_exp (btor, result);
    result = xor;
  }
  return result;
}

BtorNode *
btor_redand_exp (Btor *btor, BtorNode *exp)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result, *ones;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));

  ones   = btor_ones_exp (btor, btor_exp_get_sort_id (exp));
  result = btor_eq_exp (btor, exp, ones);
  btor_release_exp (btor, ones);
  return result;
}

BtorNode *
btor_uext_exp (Btor *btor, BtorNode *exp, uint32_t width)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result, *zero;
  BtorSortId sort;

  exp = btor_simplify_exp (btor, exp);
  assert (precond_ext_exp_dbg (btor, exp));

  if (width == 0)
    result = btor_copy_exp (btor, exp);
  else
  {
    assert (width > 0);
    sort = btor_bitvec_sort (btor, width);
    zero = btor_zero_exp (btor, sort);
    btor_release_sort (btor, sort);
    result = btor_concat_exp (btor, zero, exp);
    btor_release_exp (btor, zero);
  }
  return result;
}

BtorNode *
btor_sext_exp (Btor *btor, BtorNode *exp, uint32_t width)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *result, *zero, *ones, *neg, *cond;
  int exp_width;
  BtorSortId sort;

  exp = btor_simplify_exp (btor, exp);
  assert (precond_ext_exp_dbg (btor, exp));

  if (width == 0)
    result = btor_copy_exp (btor, exp);
  else
  {
    assert (width > 0);
    sort = btor_bitvec_sort (btor, width);
    zero = btor_zero_exp (btor, sort);
    ones = btor_ones_exp (btor, sort);
    btor_release_sort (btor, sort);
    exp_width = btor_get_exp_width (btor, exp);
    neg       = btor_slice_exp (btor, exp, exp_width - 1, exp_width - 1);
    cond      = btor_cond_exp (btor, neg, ones, zero);
    result    = btor_concat_exp (btor, cond, exp);
    btor_release_exp (btor, zero);
    btor_release_exp (btor, ones);
    btor_release_exp (btor, neg);
    btor_release_exp (btor, cond);
  }
  return result;
}

BtorNode *
btor_nand_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  return BTOR_INVERT_NODE (btor_and_exp (btor, e0, e1));
}

BtorNode *
btor_nor_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  return BTOR_INVERT_NODE (btor_or_exp (btor, e0, e1));
}

BtorNode *
btor_implies_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor_get_exp_width (btor, e0) == 1);
  return BTOR_INVERT_NODE (btor_and_exp (btor, e0, BTOR_INVERT_NODE (e1)));
}

BtorNode *
btor_iff_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor_get_exp_width (btor, e0) == 1);
  return btor_eq_exp (btor, e0, e1);
}

BtorNode *
btor_ne_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_eq_exp_dbg (btor, e0, e1));
  return BTOR_INVERT_NODE (btor_eq_exp (btor, e0, e1));
}

BtorNode *
btor_uaddo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *uext_e1, *uext_e2, *add;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width   = btor_get_exp_width (btor, e0);
  uext_e1 = btor_uext_exp (btor, e0, 1);
  uext_e2 = btor_uext_exp (btor, e1, 1);
  add     = btor_add_exp (btor, uext_e1, uext_e2);
  result  = btor_slice_exp (btor, add, width, width);
  btor_release_exp (btor, uext_e1);
  btor_release_exp (btor, uext_e2);
  btor_release_exp (btor, add);
  return result;
}

BtorNode *
btor_saddo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sign_e1, *sign_e2, *sign_result;
  BtorNode *add, *and1, *and2, *or1, *or2;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width       = btor_get_exp_width (btor, e0);
  sign_e1     = btor_slice_exp (btor, e0, width - 1, width - 1);
  sign_e2     = btor_slice_exp (btor, e1, width - 1, width - 1);
  add         = btor_add_exp (btor, e0, e1);
  sign_result = btor_slice_exp (btor, add, width - 1, width - 1);
  and1        = btor_and_exp (btor, sign_e1, sign_e2);
  or1         = btor_and_exp (btor, and1, BTOR_INVERT_NODE (sign_result));
  and2        = btor_and_exp (
      btor, BTOR_INVERT_NODE (sign_e1), BTOR_INVERT_NODE (sign_e2));
  or2    = btor_and_exp (btor, and2, sign_result);
  result = btor_or_exp (btor, or1, or2);
  btor_release_exp (btor, and1);
  btor_release_exp (btor, and2);
  btor_release_exp (btor, or1);
  btor_release_exp (btor, or2);
  btor_release_exp (btor, add);
  btor_release_exp (btor, sign_e1);
  btor_release_exp (btor, sign_e2);
  btor_release_exp (btor, sign_result);
  return result;
}

BtorNode *
btor_mul_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_MUL_NODE, e0, e1);
  else
    result = btor_mul_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_umulo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *uext_e1, *uext_e2, *mul, *slice, *and, * or, **temps_e2;
  BtorSortId sort;
  int i, width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width = btor_get_exp_width (btor, e0);
  if (width == 1)
  {
    sort   = btor_bitvec_sort (btor, 1);
    result = btor_zero_exp (btor, sort);
    btor_release_sort (btor, sort);
    return result;
  }
  BTOR_NEWN (btor->mm, temps_e2, width - 1);
  temps_e2[0] = btor_slice_exp (btor, e1, width - 1, width - 1);
  for (i = 1; i < width - 1; i++)
  {
    slice       = btor_slice_exp (btor, e1, width - 1 - i, width - 1 - i);
    temps_e2[i] = btor_or_exp (btor, temps_e2[i - 1], slice);
    btor_release_exp (btor, slice);
  }
  slice  = btor_slice_exp (btor, e0, 1, 1);
  result = btor_and_exp (btor, slice, temps_e2[0]);
  btor_release_exp (btor, slice);
  for (i = 1; i < width - 1; i++)
  {
    slice = btor_slice_exp (btor, e0, i + 1, i + 1);
    and   = btor_and_exp (btor, slice, temps_e2[i]);
    or    = btor_or_exp (btor, result, and);
    btor_release_exp (btor, slice);
    btor_release_exp (btor, and);
    btor_release_exp (btor, result);
    result = or ;
  }
  uext_e1 = btor_uext_exp (btor, e0, 1);
  uext_e2 = btor_uext_exp (btor, e1, 1);
  mul     = btor_mul_exp (btor, uext_e1, uext_e2);
  slice   = btor_slice_exp (btor, mul, width, width);
  or      = btor_or_exp (btor, result, slice);
  btor_release_exp (btor, uext_e1);
  btor_release_exp (btor, uext_e2);
  btor_release_exp (btor, mul);
  btor_release_exp (btor, slice);
  btor_release_exp (btor, result);
  result = or ;
  for (i = 0; i < width - 1; i++) btor_release_exp (btor, temps_e2[i]);
  BTOR_DELETEN (btor->mm, temps_e2, width - 1);
  return result;
}

BtorNode *
btor_smulo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sext_e1, *sext_e2, *sign_e1, *sign_e2, *sext_sign_e1;
  BtorNode *sext_sign_e2, *xor_sign_e1, *xor_sign_e2, *mul, *slice, *slice_n;
  BtorNode *slice_n_minus_1, *xor, *and, * or, **temps_e2;
  int i, width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width = btor_get_exp_width (btor, e0);
  if (width == 1) return btor_and_exp (btor, e0, e1);
  if (width == 2)
  {
    sext_e1         = btor_sext_exp (btor, e0, 1);
    sext_e2         = btor_sext_exp (btor, e1, 1);
    mul             = btor_mul_exp (btor, sext_e1, sext_e2);
    slice_n         = btor_slice_exp (btor, mul, width, width);
    slice_n_minus_1 = btor_slice_exp (btor, mul, width - 1, width - 1);
    result          = btor_xor_exp (btor, slice_n, slice_n_minus_1);
    btor_release_exp (btor, sext_e1);
    btor_release_exp (btor, sext_e2);
    btor_release_exp (btor, mul);
    btor_release_exp (btor, slice_n);
    btor_release_exp (btor, slice_n_minus_1);
  }
  else
  {
    sign_e1      = btor_slice_exp (btor, e0, width - 1, width - 1);
    sign_e2      = btor_slice_exp (btor, e1, width - 1, width - 1);
    sext_sign_e1 = btor_sext_exp (btor, sign_e1, width - 1);
    sext_sign_e2 = btor_sext_exp (btor, sign_e2, width - 1);
    xor_sign_e1  = btor_xor_exp (btor, e0, sext_sign_e1);
    xor_sign_e2  = btor_xor_exp (btor, e1, sext_sign_e2);
    BTOR_NEWN (btor->mm, temps_e2, width - 2);
    temps_e2[0] = btor_slice_exp (btor, xor_sign_e2, width - 2, width - 2);
    for (i = 1; i < width - 2; i++)
    {
      slice = btor_slice_exp (btor, xor_sign_e2, width - 2 - i, width - 2 - i);
      temps_e2[i] = btor_or_exp (btor, temps_e2[i - 1], slice);
      btor_release_exp (btor, slice);
    }
    slice  = btor_slice_exp (btor, xor_sign_e1, 1, 1);
    result = btor_and_exp (btor, slice, temps_e2[0]);
    btor_release_exp (btor, slice);
    for (i = 1; i < width - 2; i++)
    {
      slice = btor_slice_exp (btor, xor_sign_e1, i + 1, i + 1);
      and   = btor_and_exp (btor, slice, temps_e2[i]);
      or    = btor_or_exp (btor, result, and);
      btor_release_exp (btor, slice);
      btor_release_exp (btor, and);
      btor_release_exp (btor, result);
      result = or ;
    }
    sext_e1         = btor_sext_exp (btor, e0, 1);
    sext_e2         = btor_sext_exp (btor, e1, 1);
    mul             = btor_mul_exp (btor, sext_e1, sext_e2);
    slice_n         = btor_slice_exp (btor, mul, width, width);
    slice_n_minus_1 = btor_slice_exp (btor, mul, width - 1, width - 1);
    xor             = btor_xor_exp (btor, slice_n, slice_n_minus_1);
    or              = btor_or_exp (btor, result, xor);
    btor_release_exp (btor, sext_e1);
    btor_release_exp (btor, sext_e2);
    btor_release_exp (btor, sign_e1);
    btor_release_exp (btor, sign_e2);
    btor_release_exp (btor, sext_sign_e1);
    btor_release_exp (btor, sext_sign_e2);
    btor_release_exp (btor, xor_sign_e1);
    btor_release_exp (btor, xor_sign_e2);
    btor_release_exp (btor, mul);
    btor_release_exp (btor, slice_n);
    btor_release_exp (btor, slice_n_minus_1);
    btor_release_exp (btor, xor);
    btor_release_exp (btor, result);
    result = or ;
    for (i = 0; i < width - 2; i++) btor_release_exp (btor, temps_e2[i]);
    BTOR_DELETEN (btor->mm, temps_e2, width - 2);
  }
  return result;
}

BtorNode *
btor_ult_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_ULT_NODE, e0, e1);
  else
    result = btor_ult_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_slt_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *determined_by_sign, *eq_sign, *ult, *eq_sign_and_ult;
  BtorNode *res, *s0, *s1, *r0, *r1, *l, *r;

  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width = btor_get_exp_width (btor, e0);
  if (width == 1) return btor_and_exp (btor, e0, BTOR_INVERT_NODE (e1));
  s0                 = btor_slice_exp (btor, e0, width - 1, width - 1);
  s1                 = btor_slice_exp (btor, e1, width - 1, width - 1);
  r0                 = btor_slice_exp (btor, e0, width - 2, 0);
  r1                 = btor_slice_exp (btor, e1, width - 2, 0);
  ult                = btor_ult_exp (btor, r0, r1);
  determined_by_sign = btor_and_exp (btor, s0, BTOR_INVERT_NODE (s1));
  l                  = btor_copy_exp (btor, determined_by_sign);
  r                  = btor_and_exp (btor, BTOR_INVERT_NODE (s0), s1);
  eq_sign = btor_and_exp (btor, BTOR_INVERT_NODE (l), BTOR_INVERT_NODE (r));
  eq_sign_and_ult = btor_and_exp (btor, eq_sign, ult);
  res             = btor_or_exp (btor, determined_by_sign, eq_sign_and_ult);
  btor_release_exp (btor, s0);
  btor_release_exp (btor, s1);
  btor_release_exp (btor, r0);
  btor_release_exp (btor, r1);
  btor_release_exp (btor, ult);
  btor_release_exp (btor, determined_by_sign);
  btor_release_exp (btor, l);
  btor_release_exp (btor, r);
  btor_release_exp (btor, eq_sign);
  btor_release_exp (btor, eq_sign_and_ult);
  return res;
}

BtorNode *
btor_ulte_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *ult;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  ult    = btor_ult_exp (btor, e1, e0);
  result = btor_not_exp (btor, ult);
  btor_release_exp (btor, ult);
  return result;
}

BtorNode *
btor_slte_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *slt;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  slt    = btor_slt_exp (btor, e1, e0);
  result = btor_not_exp (btor, slt);
  btor_release_exp (btor, slt);
  return result;
}

BtorNode *
btor_ugt_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  return btor_ult_exp (btor, e1, e0);
}

BtorNode *
btor_sgt_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  return btor_slt_exp (btor, e1, e0);
}

BtorNode *
btor_ugte_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *ult;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  ult    = btor_ult_exp (btor, e0, e1);
  result = btor_not_exp (btor, ult);
  btor_release_exp (btor, ult);
  return result;
}

BtorNode *
btor_sgte_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *slt;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  slt    = btor_slt_exp (btor, e0, e1);
  result = btor_not_exp (btor, slt);
  btor_release_exp (btor, slt);
  return result;
}

BtorNode *
btor_sll_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_SLL_NODE, e0, e1);
  else
    result = btor_sll_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_srl_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_SRL_NODE, e0, e1);
  else
    result = btor_srl_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_sra_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sign_e1, *srl1, *srl2;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));

  width   = btor_get_exp_width (btor, e0);
  sign_e1 = btor_slice_exp (btor, e0, width - 1, width - 1);
  srl1    = btor_srl_exp (btor, e0, e1);
  srl2    = btor_srl_exp (btor, BTOR_INVERT_NODE (e0), e1);
  result  = btor_cond_exp (btor, sign_e1, BTOR_INVERT_NODE (srl2), srl1);
  btor_release_exp (btor, sign_e1);
  btor_release_exp (btor, srl1);
  btor_release_exp (btor, srl2);
  return result;
}

BtorNode *
btor_rol_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sll, *neg_e2, *srl;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));

  sll    = btor_sll_exp (btor, e0, e1);
  neg_e2 = btor_neg_exp (btor, e1);
  srl    = btor_srl_exp (btor, e0, neg_e2);
  result = btor_or_exp (btor, sll, srl);
  btor_release_exp (btor, sll);
  btor_release_exp (btor, neg_e2);
  btor_release_exp (btor, srl);
  return result;
}

BtorNode *
btor_ror_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *srl, *neg_e2, *sll;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));

  srl    = btor_srl_exp (btor, e0, e1);
  neg_e2 = btor_neg_exp (btor, e1);
  sll    = btor_sll_exp (btor, e0, neg_e2);
  result = btor_or_exp (btor, srl, sll);
  btor_release_exp (btor, srl);
  btor_release_exp (btor, neg_e2);
  btor_release_exp (btor, sll);
  return result;
}

BtorNode *
btor_sub_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *neg_e2;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  neg_e2 = btor_neg_exp (btor, e1);
  result = btor_add_exp (btor, e0, neg_e2);
  btor_release_exp (btor, neg_e2);
  return result;
}

BtorNode *
btor_usubo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *uext_e1, *uext_e2, *add1, *add2, *one;
  BtorSortId sort;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width   = btor_get_exp_width (btor, e0);
  uext_e1 = btor_uext_exp (btor, e0, 1);
  uext_e2 = btor_uext_exp (btor, BTOR_INVERT_NODE (e1), 1);
  assert (width < INT_MAX);
  sort = btor_bitvec_sort (btor, width + 1);
  one  = btor_one_exp (btor, sort);
  btor_release_sort (btor, sort);
  add1   = btor_add_exp (btor, uext_e2, one);
  add2   = btor_add_exp (btor, uext_e1, add1);
  result = BTOR_INVERT_NODE (btor_slice_exp (btor, add2, width, width));
  btor_release_exp (btor, uext_e1);
  btor_release_exp (btor, uext_e2);
  btor_release_exp (btor, add1);
  btor_release_exp (btor, add2);
  btor_release_exp (btor, one);
  return result;
}

BtorNode *
btor_ssubo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sign_e1, *sign_e2, *sign_result;
  BtorNode *sub, *and1, *and2, *or1, *or2;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width       = btor_get_exp_width (btor, e0);
  sign_e1     = btor_slice_exp (btor, e0, width - 1, width - 1);
  sign_e2     = btor_slice_exp (btor, e1, width - 1, width - 1);
  sub         = btor_sub_exp (btor, e0, e1);
  sign_result = btor_slice_exp (btor, sub, width - 1, width - 1);
  and1        = btor_and_exp (btor, BTOR_INVERT_NODE (sign_e1), sign_e2);
  or1         = btor_and_exp (btor, and1, sign_result);
  and2        = btor_and_exp (btor, sign_e1, BTOR_INVERT_NODE (sign_e2));
  or2         = btor_and_exp (btor, and2, BTOR_INVERT_NODE (sign_result));
  result      = btor_or_exp (btor, or1, or2);
  btor_release_exp (btor, and1);
  btor_release_exp (btor, and2);
  btor_release_exp (btor, or1);
  btor_release_exp (btor, or2);
  btor_release_exp (btor, sub);
  btor_release_exp (btor, sign_e1);
  btor_release_exp (btor, sign_e2);
  btor_release_exp (btor, sign_result);
  return result;
}

BtorNode *
btor_udiv_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_UDIV_NODE, e0, e1);
  else
    result = btor_udiv_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_sdiv_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sign_e1, *sign_e2, *xor, *neg_e1, *neg_e2;
  BtorNode *cond_e1, *cond_e2, *udiv, *neg_udiv;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width = btor_get_exp_width (btor, e0);

  if (width == 1)
    return BTOR_INVERT_NODE (btor_and_exp (btor, BTOR_INVERT_NODE (e0), e1));

  sign_e1 = btor_slice_exp (btor, e0, width - 1, width - 1);
  sign_e2 = btor_slice_exp (btor, e1, width - 1, width - 1);
  /* xor: must result be signed? */
  xor    = btor_xor_exp (btor, sign_e1, sign_e2);
  neg_e1 = btor_neg_exp (btor, e0);
  neg_e2 = btor_neg_exp (btor, e1);
  /* normalize e0 and e1 if necessary */
  cond_e1  = btor_cond_exp (btor, sign_e1, neg_e1, e0);
  cond_e2  = btor_cond_exp (btor, sign_e2, neg_e2, e1);
  udiv     = btor_udiv_exp (btor, cond_e1, cond_e2);
  neg_udiv = btor_neg_exp (btor, udiv);
  /* sign result if necessary */
  result = btor_cond_exp (btor, xor, neg_udiv, udiv);
  btor_release_exp (btor, sign_e1);
  btor_release_exp (btor, sign_e2);
  btor_release_exp (btor, xor);
  btor_release_exp (btor, neg_e1);
  btor_release_exp (btor, neg_e2);
  btor_release_exp (btor, cond_e1);
  btor_release_exp (btor, cond_e2);
  btor_release_exp (btor, udiv);
  btor_release_exp (btor, neg_udiv);
  return result;
}

BtorNode *
btor_sdivo_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *int_min, *ones, *eq1, *eq2;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  int_min = int_min_exp (btor, btor_get_exp_width (btor, e0));
  ones    = btor_ones_exp (btor, btor_exp_get_sort_id (e1));
  eq1     = btor_eq_exp (btor, e0, int_min);
  eq2     = btor_eq_exp (btor, e1, ones);
  result  = btor_and_exp (btor, eq1, eq2);
  btor_release_exp (btor, int_min);
  btor_release_exp (btor, ones);
  btor_release_exp (btor, eq1);
  btor_release_exp (btor, eq2);
  return result;
}

BtorNode *
btor_urem_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  if (btor_get_opt (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    result = btor_rewrite_binary_exp (btor, BTOR_UREM_NODE, e0, e1);
  else
    result = btor_urem_exp_node (btor, e0, e1);

  assert (result);
  return result;
}

BtorNode *
btor_srem_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sign_e0, *sign_e1, *neg_e0, *neg_e1;
  BtorNode *cond_e0, *cond_e1, *urem, *neg_urem;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width = btor_get_exp_width (btor, e0);

  if (width == 1) return btor_and_exp (btor, e0, BTOR_INVERT_NODE (e1));

  sign_e0 = btor_slice_exp (btor, e0, width - 1, width - 1);
  sign_e1 = btor_slice_exp (btor, e1, width - 1, width - 1);
  neg_e0  = btor_neg_exp (btor, e0);
  neg_e1  = btor_neg_exp (btor, e1);
  /* normalize e0 and e1 if necessary */
  cond_e0  = btor_cond_exp (btor, sign_e0, neg_e0, e0);
  cond_e1  = btor_cond_exp (btor, sign_e1, neg_e1, e1);
  urem     = btor_urem_exp (btor, cond_e0, cond_e1);
  neg_urem = btor_neg_exp (btor, urem);
  /* sign result if necessary */
  /* result is negative if e0 is negative */
  result = btor_cond_exp (btor, sign_e0, neg_urem, urem);
  btor_release_exp (btor, sign_e0);
  btor_release_exp (btor, sign_e1);
  btor_release_exp (btor, neg_e0);
  btor_release_exp (btor, neg_e1);
  btor_release_exp (btor, cond_e0);
  btor_release_exp (btor, cond_e1);
  btor_release_exp (btor, urem);
  btor_release_exp (btor, neg_urem);
  return result;
}

BtorNode *
btor_smod_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e1)->btor);

  BtorNode *result, *sign_e0, *sign_e1, *neg_e0, *neg_e1, *cond_e0, *cond_e1;
  BtorNode *neg_e0_and_e1, *neg_e0_and_neg_e1, *zero, *e0_zero;
  BtorNode *neg_urem, *add1, *add2, *or1, *or2, *e0_and_e1, *e0_and_neg_e1;
  BtorNode *cond_case1, *cond_case2, *cond_case3, *cond_case4, *urem;
  BtorNode *urem_zero, *gadd1, *gadd2;
  uint32_t width;

  e0 = btor_simplify_exp (btor, e0);
  e1 = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  width     = btor_get_exp_width (btor, e0);
  zero      = btor_zero_exp (btor, btor_exp_get_sort_id (e0));
  e0_zero   = btor_eq_exp (btor, zero, e0);
  sign_e0   = btor_slice_exp (btor, e0, width - 1, width - 1);
  sign_e1   = btor_slice_exp (btor, e1, width - 1, width - 1);
  neg_e0    = btor_neg_exp (btor, e0);
  neg_e1    = btor_neg_exp (btor, e1);
  e0_and_e1 = btor_and_exp (
      btor, BTOR_INVERT_NODE (sign_e0), BTOR_INVERT_NODE (sign_e1));
  e0_and_neg_e1     = btor_and_exp (btor, BTOR_INVERT_NODE (sign_e0), sign_e1);
  neg_e0_and_e1     = btor_and_exp (btor, sign_e0, BTOR_INVERT_NODE (sign_e1));
  neg_e0_and_neg_e1 = btor_and_exp (btor, sign_e0, sign_e1);
  /* normalize e0 and e1 if necessary */
  cond_e0    = btor_cond_exp (btor, sign_e0, neg_e0, e0);
  cond_e1    = btor_cond_exp (btor, sign_e1, neg_e1, e1);
  urem       = btor_urem_exp (btor, cond_e0, cond_e1);
  urem_zero  = btor_eq_exp (btor, urem, zero);
  neg_urem   = btor_neg_exp (btor, urem);
  add1       = btor_add_exp (btor, neg_urem, e1);
  add2       = btor_add_exp (btor, urem, e1);
  gadd1      = btor_cond_exp (btor, urem_zero, zero, add1);
  gadd2      = btor_cond_exp (btor, urem_zero, zero, add2);
  cond_case1 = btor_cond_exp (btor, e0_and_e1, urem, zero);
  cond_case2 = btor_cond_exp (btor, neg_e0_and_e1, gadd1, zero);
  cond_case3 = btor_cond_exp (btor, e0_and_neg_e1, gadd2, zero);
  cond_case4 = btor_cond_exp (btor, neg_e0_and_neg_e1, neg_urem, zero);
  or1        = btor_or_exp (btor, cond_case1, cond_case2);
  or2        = btor_or_exp (btor, cond_case3, cond_case4);
  result     = btor_or_exp (btor, or1, or2);
  btor_release_exp (btor, zero);
  btor_release_exp (btor, e0_zero);
  btor_release_exp (btor, sign_e0);
  btor_release_exp (btor, sign_e1);
  btor_release_exp (btor, neg_e0);
  btor_release_exp (btor, neg_e1);
  btor_release_exp (btor, cond_e0);
  btor_release_exp (btor, cond_e1);
  btor_release_exp (btor, urem_zero);
  btor_release_exp (btor, cond_case1);
  btor_release_exp (btor, cond_case2);
  btor_release_exp (btor, cond_case3);
  btor_release_exp (btor, cond_case4);
  btor_release_exp (btor, urem);
  btor_release_exp (btor, neg_urem);
  btor_release_exp (btor, add1);
  btor_release_exp (btor, add2);
  btor_release_exp (btor, gadd1);
  btor_release_exp (btor, gadd2);
  btor_release_exp (btor, or1);
  btor_release_exp (btor, or2);
  btor_release_exp (btor, e0_and_e1);
  btor_release_exp (btor, neg_e0_and_e1);
  btor_release_exp (btor, e0_and_neg_e1);
  btor_release_exp (btor, neg_e0_and_neg_e1);
  return result;
}

BtorNode *
btor_read_exp (Btor *btor, BtorNode *e_array, BtorNode *e_index)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e_array)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_index)->btor);

  e_array = btor_simplify_exp (btor, e_array);
  e_index = btor_simplify_exp (btor, e_index);
  assert (btor_precond_read_exp_dbg (btor, e_array, e_index));

  return btor_apply_exps (btor, &e_index, 1, e_array);
}

BtorNode *
btor_lambda_write_exp (Btor *btor,
                       BtorNode *e_array,
                       BtorNode *e_index,
                       BtorNode *e_value)
{
  BtorNode *param, *e_cond, *e_if, *e_else, *bvcond, *args;
  BtorLambdaNode *lambda;
  BtorPtrHashBucket *b;

  param  = btor_param_exp (btor, btor_exp_get_sort_id (e_index), 0);
  e_cond = btor_eq_exp (btor, param, e_index);
  e_if   = btor_copy_exp (btor, e_value);
  e_else = btor_read_exp (btor, e_array, param);
  bvcond = btor_cond_exp (btor, e_cond, e_if, e_else);
  lambda = (BtorLambdaNode *) btor_lambda_exp (btor, param, bvcond);
  if (!lambda->static_rho)
  {
    lambda->static_rho =
        btor_new_ptr_hash_table (btor->mm,
                                 (BtorHashPtr) btor_hash_exp_by_id,
                                 (BtorCmpPtr) btor_compare_exp_by_id);
    args           = btor_args_exp (btor, &e_index, 1);
    b              = btor_add_ptr_hash_table (lambda->static_rho, args);
    b->data.as_ptr = btor_copy_exp (btor, e_value);
  }
  btor_release_exp (btor, e_if);
  btor_release_exp (btor, e_else);
  btor_release_exp (btor, e_cond);
  btor_release_exp (btor, bvcond);
  btor_release_exp (btor, param);

  lambda->is_array = 1;
  return (BtorNode *) lambda;
}

BtorNode *
btor_update_exp (Btor *btor, BtorNode *fun, BtorNode *args, BtorNode *value)
{
  BtorNode *e[3], *res;
  e[0] = btor_simplify_exp (btor, fun);
  e[1] = btor_simplify_exp (btor, args);
  e[2] = btor_simplify_exp (btor, value);
  assert (btor_is_fun_node (e[0]));
  assert (btor_is_args_node (e[1]));
  assert (!btor_is_fun_node (e[2]));

  if (BTOR_REAL_ADDR_NODE (e[0])->parameterized
      || BTOR_REAL_ADDR_NODE (e[1])->parameterized
      || BTOR_REAL_ADDR_NODE (e[2])->parameterized)
  {
    assert (btor_get_args_arity (btor, args) == 1);
    return btor_lambda_write_exp (btor, fun, args->e[0], value);
  }

  res = create_exp (btor, BTOR_UPDATE_NODE, 3, e);
  if (fun->is_array) res->is_array = 1;
  return res;
}

BtorNode *
btor_write_exp (Btor *btor,
                BtorNode *e_array,
                BtorNode *e_index,
                BtorNode *e_value)
{
  assert (btor);
  assert (btor_is_array_node (btor_simplify_exp (btor, e_array)));
  assert (btor == BTOR_REAL_ADDR_NODE (e_array)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_index)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_value)->btor);

  e_array = btor_simplify_exp (btor, e_array);
  e_index = btor_simplify_exp (btor, e_index);
  e_value = btor_simplify_exp (btor, e_value);
  assert (btor_precond_write_exp_dbg (btor, e_array, e_index, e_value));

  if (btor_get_opt (btor, BTOR_OPT_FUN_STORE_LAMBDAS)
      || BTOR_REAL_ADDR_NODE (e_index)->parameterized
      || BTOR_REAL_ADDR_NODE (e_value)->parameterized)
  {
    return btor_lambda_write_exp (btor, e_array, e_index, e_value);
  }
  else
  {
    BtorNode *args = btor_args_exp (btor, &e_index, 1);
    BtorNode *res  = btor_update_exp (btor, e_array, args, e_value);
    btor_release_exp (btor, args);
    res->is_array = 1;
    return res;
  }
}

BtorNode *
btor_inc_exp (Btor *btor, BtorNode *exp)
{
  BtorNode *one, *result;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));

  one    = btor_one_exp (btor, btor_exp_get_sort_id (exp));
  result = btor_add_exp (btor, exp, one);
  btor_release_exp (btor, one);
  return result;
}

BtorNode *
btor_dec_exp (Btor *btor, BtorNode *exp)
{
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  BtorNode *one, *result;

  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_regular_unary_bv_exp_dbg (btor, exp));

  one    = btor_one_exp (btor, btor_exp_get_sort_id (exp));
  result = btor_sub_exp (btor, exp, one);
  btor_release_exp (btor, one);
  return result;
}

BtorNode *
btor_create_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[], uint32_t arity)
{
  assert (arity > 0);
  assert (arity <= 3);

  switch (kind)
  {
    case BTOR_AND_NODE:
      assert (arity == 2);
      return btor_and_exp (btor, e[0], e[1]);
    case BTOR_BV_EQ_NODE:
    case BTOR_FUN_EQ_NODE:
      assert (arity == 2);
      return btor_eq_exp (btor, e[0], e[1]);
    case BTOR_ADD_NODE:
      assert (arity == 2);
      return btor_add_exp (btor, e[0], e[1]);
    case BTOR_MUL_NODE:
      assert (arity == 2);
      return btor_mul_exp (btor, e[0], e[1]);
    case BTOR_ULT_NODE:
      assert (arity == 2);
      return btor_ult_exp (btor, e[0], e[1]);
    case BTOR_SLL_NODE:
      assert (arity == 2);
      return btor_sll_exp (btor, e[0], e[1]);
    case BTOR_SRL_NODE:
      assert (arity == 2);
      return btor_srl_exp (btor, e[0], e[1]);
    case BTOR_UDIV_NODE:
      assert (arity == 2);
      return btor_udiv_exp (btor, e[0], e[1]);
    case BTOR_UREM_NODE:
      assert (arity == 2);
      return btor_urem_exp (btor, e[0], e[1]);
    case BTOR_CONCAT_NODE:
      assert (arity == 2);
      return btor_concat_exp (btor, e[0], e[1]);
    case BTOR_APPLY_NODE:
      assert (arity == 2);
      return btor_apply_exp (btor, e[0], e[1]);
    case BTOR_LAMBDA_NODE:
      assert (arity == 2);
      return btor_lambda_exp (btor, e[0], e[1]);
    case BTOR_COND_NODE:
      assert (arity == 3);
      return btor_cond_exp (btor, e[0], e[1], e[2]);
    case BTOR_UPDATE_NODE:
      assert (arity == 3);
      return btor_update_exp (btor, e[0], e[1], e[2]);
    default:
      assert (kind == BTOR_ARGS_NODE);
      return btor_args_exp (btor, e, arity);
  }
  return 0;
}
