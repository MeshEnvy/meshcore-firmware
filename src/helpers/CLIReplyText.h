#pragma once

// Constrained profiles may shorten human CLI prose. Command syntax, structured
// replies, protocol acknowledgements, destructive and rollback warnings keep
// their exact text.
#ifndef MESH_CLI_COMPACT_REPLIES
#define MESH_CLI_COMPACT_REPLIES 0
#endif

#if MESH_CLI_COMPACT_REPLIES
#define MESH_CLI_REPLY_TEXT(verbose, concise) concise
#else
#define MESH_CLI_REPLY_TEXT(verbose, concise) verbose
#endif
