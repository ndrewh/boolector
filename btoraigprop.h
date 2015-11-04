/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#ifndef BTORAIGPROP_H_INCLUDED
#define BTORAIGPROP_H_INCLUDED

#include "btorslv.h"

#define BTOR_AIGPROP_SOLVER(btor) ((BtorAIGPropSolver *) (btor)->slv)

struct BtorAIGPropSolver
{
  BTOR_SOLVER_STRUCT;

  BtorPtrHashTable *roots;

  /* statistics */
  struct
  {
    int moves;
    int restarts;
  } stats;
};

typedef struct BtorAIGPropSolver BtorAIGPropSolver;

BtorSolver *btor_new_aigprop_solver (Btor *btor);

#endif
