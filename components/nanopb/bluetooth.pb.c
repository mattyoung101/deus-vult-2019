/* Automatically generated nanopb constant definitions */
/* Generated by nanopb-0.3.9.3 at Fri Jun 07 16:58:40 2019. */

#include "bluetooth.pb.h"

/* @@protoc_insertion_point(includes) */
#if PB_PROTO_HEADER_VERSION != 30
#error Regenerate this file with the current version of nanopb generator.
#endif



const pb_field_t BTProvide_fields[6] = {
    PB_FIELD(  1, STRING  , SINGULAR, STATIC  , FIRST, BTProvide, fsmState, fsmState, 0),
    PB_FIELD(  2, FLOAT   , SINGULAR, STATIC  , OTHER, BTProvide, robotX, fsmState, 0),
    PB_FIELD(  3, FLOAT   , SINGULAR, STATIC  , OTHER, BTProvide, robotY, robotX, 0),
    PB_FIELD(  4, BOOL    , SINGULAR, STATIC  , OTHER, BTProvide, onLine, robotY, 0),
    PB_FIELD(  5, BOOL    , SINGULAR, STATIC  , OTHER, BTProvide, switchOk, onLine, 0),
    PB_LAST_FIELD
};


/* @@protoc_insertion_point(eof) */
