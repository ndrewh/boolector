#include "boolector.h"
#include "btoraig.h"
#include "btoraigvec.h"
#include "btorbtor.h"
#include "stdio.h"

#include <stdio.h>
#include <string.h>

#define BTOR_HAVE_ISATTY

#ifdef BTOR_HAVE_ISATTY
#include <unistd.h>
#endif

static void
die (int prefix, const char* fmt, ...)
{
  va_list ap;
  if (prefix) fputs ("*** synthebtor: ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  exit (1);
}

int
main (int argc, char** argv)
{
  int i, j, verbosity = 0, close_input = 0, close_output = 0, binary;
  const char *input_name = "<stdin>", *output_name = "<stdout>";
  FILE *input_file = stdin, *output_file = stdout, *file;
  const char* parse_error;
  BtorParseResult model;
  BtorAIGVecMgr* avmgr;
  BtorAIGPtrStack stack;
  BtorAIG *aig, **p;
  BtorParser* parser;
  BtorAIGMgr* amgr;
  BtorExpMgr* emgr;
  BtorMemMgr* mem;
  BtorAIGVec* av;

  for (i = 1; i < argc; i++)
  {
    if (!strcmp (argv[i], "-h"))
    {
      printf ("usage: synthebor [-h][-v][<input>[<output>]]\n");
      exit (0);
    }
    else if (!strcmp (argv[i], "-v"))
      verbosity++;
    else if (argv[i][0] == '-')
      die (1, "invalid command line option '%s'", argv[i]);
    else if (close_output)
      die (1, "too many files");
    else if (close_input)
    {
      if (!strcmp (argv[i], input_name))
        die (1, "input and output are the same");

      if (!(file = fopen (argv[i], "w")))
        die (1, "can not write '%s'", argv[i]);

      output_file  = file;
      output_name  = argv[i];
      close_output = 1;
    }
    else if (!(file = fopen (argv[i], "r")))
      die (1, "can not read '%s'", argv[i]);
    else
    {
      input_file  = file;
      input_name  = argv[i];
      close_input = 1;
    }
  }

  emgr   = btor_new_exp_mgr (2, 0, verbosity, 0);
  parser = btor_btor_parser_api->init (emgr, verbosity);

  parse_error =
      btor_btor_parser_api->parse (parser, input_file, input_name, &model);
  if (parse_error) die (0, parse_error);

  if (!model.nroots) die (1, "no roots in '%s'", input_name);

  mem   = btor_get_mem_mgr_exp_mgr (emgr);
  avmgr = btor_get_aigvec_mgr_exp_mgr (emgr);
  amgr  = btor_get_aig_mgr_aigvec_mgr (avmgr);

  BTOR_INIT_STACK (stack);
  for (i = 0; i < model.nroots; i++)
  {
    av = btor_exp_to_aigvec (emgr, model.roots[i]);
    for (j = 0; j < av->len; j++)
    {
      aig = btor_copy_aig (amgr, av->aigs[j]);
      BTOR_PUSH_STACK (mem, stack, aig);
    }
    btor_release_delete_aigvec (avmgr, av);
  }

  binary = 0;
#ifdef BTOR_HAVE_ISATTY
  if (close_output || !isatty (1)) binary = 1;
#endif
  btor_dump_aigs (
      amgr, binary, output_file, stack.start, BTOR_COUNT_STACK (stack));

  for (p = stack.start; p < stack.top; p++) btor_release_aig (amgr, *p);
  BTOR_RELEASE_STACK (mem, stack);

  btor_btor_parser_api->reset (parser);
  btor_delete_exp_mgr (emgr);

  if (close_input) fclose (input_file);

  if (close_output) fclose (output_file);

  return 0;
}
