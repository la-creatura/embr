#ifndef EMBR_CPP_INCLUDED
#define EMBR_CPP_INCLUDED

// this is the amalgamation entry point not to be #include'd
// exists so the modular sources under core/ and backends/ have one place that declares the intended include order
// using the generated embr.h is preferable

//   EMBR_PLUGIN { interp->bind("myfn", ...); }
#define EMBR_PLUGIN \
    extern "C" void embr_register(embr::Interpreter *interp)

#include "platform.h"
#include "diagnostics.h"
#include "types.h"
#include "fwd.h"
#include "token.h"
#include "value.h"
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "registry.h"
#include "../backends/tree_walker.h"

#endif // EMBR_CPP_INCLUDED
