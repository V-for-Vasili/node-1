#ifndef SRC_NODE_CONSTANTS_H_
#define SRC_NODE_CONSTANTS_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "node.h"
#include "v8.h"

#if HAVE_OPENSSL
<<<<<<< HEAD
#define DEFAULT_CIPHER_LIST_CORE "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x47\x43\x4d\x2d\x53\x48\x41\x32\x35\x36\x3a"     \
                                 "\x45\x43\x44\x48\x45\x2d\x45\x43\x44\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x47\x43\x4d\x2d\x53\x48\x41\x32\x35\x36\x3a"   \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x47\x43\x4d\x2d\x53\x48\x41\x33\x38\x34\x3a"     \
                                 "\x45\x43\x44\x48\x45\x2d\x45\x43\x44\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x47\x43\x4d\x2d\x53\x48\x41\x33\x38\x34\x3a"   \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x47\x43\x4d\x2d\x53\x48\x41\x32\x35\x36\x3a"       \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x53\x48\x41\x32\x35\x36\x3a"         \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x53\x48\x41\x32\x35\x36\x3a"           \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x33\x38\x34\x3a"         \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x33\x38\x34\x3a"           \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x32\x35\x36\x3a"         \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x32\x35\x36\x3a"           \
                                 "\x48\x49\x47\x48\x3a"                            \
                                 "\x21\x61\x4e\x55\x4c\x4c\x3a"                          \
                                 "\x21\x65\x4e\x55\x4c\x4c\x3a"                          \
                                 "\x21\x45\x58\x50\x4f\x52\x54\x3a"                         \
                                 "\x21\x44\x45\x53\x3a"                            \
                                 "\x21\x52\x43\x34\x3a"                            \
                                 "\x21\x4d\x44\x35\x3a"                            \
                                 "\x21\x50\x53\x4b\x3a"                            \
                                 "\x21\x53\x52\x50\x3a"                            \
                                 "\x21\x43\x41\x4d\x45\x4c\x4c\x49\x41"
=======

#ifndef RSA_PSS_SALTLEN_DIGEST
#define RSA_PSS_SALTLEN_DIGEST -1
#endif

#ifndef RSA_PSS_SALTLEN_MAX_SIGN
#define RSA_PSS_SALTLEN_MAX_SIGN -2
#endif

#ifndef RSA_PSS_SALTLEN_AUTO
#define RSA_PSS_SALTLEN_AUTO -2
#endif

#define DEFAULT_CIPHER_LIST_CORE "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x47\x43\x4d\x2d\x53\x48\x41\x32\x35\x36\x3a"   \
                                 "\x45\x43\x44\x48\x45\x2d\x45\x43\x44\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x47\x43\x4d\x2d\x53\x48\x41\x32\x35\x36\x3a"  \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x47\x43\x4d\x2d\x53\x48\x41\x33\x38\x34\x3a"  \
                                 "\x45\x43\x44\x48\x45\x2d\x45\x43\x44\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x47\x43\x4d\x2d\x53\x48\x41\x33\x38\x34\x3a"  \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x47\x43\x4d\x2d\x53\x48\x41\x32\x35\x36\x3a"  \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x53\x48\x41\x32\x35\x36\x3a"  \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x31\x32\x38\x2d\x53\x48\x41\x32\x35\x36\x3a"  \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x33\x38\x34\x3a"  \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x33\x38\x34\x3a"  \
                                 "\x45\x43\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x32\x35\x36\x3a"  \
                                 "\x44\x48\x45\x2d\x52\x53\x41\x2d\x41\x45\x53\x32\x35\x36\x2d\x53\x48\x41\x32\x35\x36\x3a"  \
                                 "\x48\x49\x47\x48\x3a"  \
                                 "\x21\x61\x4e\x55\x4c\x4c\x3a"  \
                                 "\x21\x65\x4e\x55\x4c\x4c\x3a"  \
                                 "\x21\x45\x58\x50\x4f\x52\x54\x3a"  \
                                 "\x21\x44\x45\x53\x3a"  \
                                 "\x21\x52\x43\x34\x3a"  \
                                 "\x21\x4d\x44\x35\x3a"  \
                                 "\x21\x50\x53\x4b\x3a"  \
                                 "\x21\x53\x52\x50\x3a"  \
                                 "\x21\x43\x41\x4d\x45\x4c\x4c\x49\x41"
#endif

namespace node {

#if HAVE_OPENSSL
extern const char* default_cipher_list;
#endif

void DefineConstants(v8::Isolate* isolate, v8::Local<v8::Object> target);
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_CONSTANTS_H_
