#include <errno.h>
#include <tcl.h>

#include <mbedtls/platform.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
/*#include <mbedtls/x509.h>*/
#include <mbedtls/certs.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/error.h>
#include <mbedtls/net.h>
/*#include <mbedtls/debug.h>*/

#include "qdsh.h"

typedef struct {
    mbedtls_ssl_context ssl;
    Tcl_Channel chan;
    Tcl_TimerToken timer;
} State;

typedef enum {
    PEM_KEY, PEM_CRT, PEM_CSR, PEM_CRL
} PemType;

static void freePemIntRep(Tcl_Obj *);
static void dupPemIntRep(Tcl_Obj *, Tcl_Obj *);

#define GET_PEM_TYPE(x) ((PemType)((x)->internalRep.ptrAndLongRep.value))
#define SET_PEM_TYPE(x, t) ((x)->internalRep.ptrAndLongRep.value = (t))
#define GET_PEM_PTR(x) ((x)->internalRep.ptrAndLongRep.ptr)
#define SET_PEM_PTR(x, v) ((x)->internalRep.ptrAndLongRep.ptr = (v))

/* BIO */
static int bioRecv(void *, unsigned char *, size_t);
static int bioSend(void *, const unsigned char *, size_t);

/* Tcl channel type member functions */
/*static int SslBlockModeProc(ClientData, int);*/
static int SslCloseProc(ClientData, Tcl_Interp *);
static int SslInputProc(ClientData, char *, int, int *);
static int SslOutputProc(ClientData, const char *, int, int *);
static int SslGetOptionProc(ClientData, Tcl_Interp *, const char *,
                            Tcl_DString *);
static void SslWatchProc(ClientData, int);
static int SslNotifyProc(ClientData, int);

static const Tcl_ChannelType sslChannelType = {
    "ssl",
    TCL_CHANNEL_VERSION_5,
    SslCloseProc,
    SslInputProc,
    SslOutputProc,
    NULL, /* Seek proc. */
    NULL, /* Set option proc. */
    SslGetOptionProc,
    SslWatchProc,
    NULL, /* Handle proc. */
    NULL, /* close2proc. */
    /*SslBlockModeProc*/NULL, 
    NULL, /* Flush proc. */
    SslNotifyProc,
    NULL, /* Wide seek proc. */
    NULL, /* Thread action proc. */
    NULL /* Truncate proc. */
};

const static Tcl_ObjType pemObjType = {
    "pem",
    freePemIntRep,
    dupPemIntRep,
    /*getStringFromPem*/NULL,
    NULL
};

static int initialized = 0;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;

typedef struct {
    mbedtls_ssl_config config;
    mbedtls_ssl_cache_context cache;
    Tcl_Obj *pemList; /* PEMs to keep alive */
} Config;

static char *
sslErrorStatic(int err)
{
    static char buf[256];
    
    mbedtls_strerror(err, buf, sizeof(buf));
    return buf;
}

static Tcl_Obj *
sslError(int err)
{
    Tcl_Obj *obj;
    obj = Tcl_NewStringObj(sslErrorStatic(err), -1);
    return Tcl_NewListObj(1, &obj);
}

static void
freePemIntRep(Tcl_Obj *obj)
{
    switch (GET_PEM_TYPE(obj)) {
    case PEM_KEY:
        mbedtls_pk_free((mbedtls_pk_context *)GET_PEM_PTR(obj));
        break;
    case PEM_CRT:
        mbedtls_x509_crt_free((mbedtls_x509_crt *)GET_PEM_PTR(obj));
        break;
    case PEM_CSR:
        mbedtls_x509_csr_free((mbedtls_x509_csr *)GET_PEM_PTR(obj));
        break;
    case PEM_CRL:
        mbedtls_x509_crl_free((mbedtls_x509_crl *)GET_PEM_PTR(obj));
        break;
    }
    ckfree(GET_PEM_PTR(obj));
    SET_PEM_PTR(obj, NULL);
}

static void
dupPemIntRep(Tcl_Obj *src, Tcl_Obj *dst)
{
    /* Disallow duplication. Just re-parse later. */
    dst->typePtr = NULL;
}

static int
getCertFromObj(Tcl_Interp *interp, Tcl_Obj *obj,
               mbedtls_x509_crt **loc)
{
    char *buf;
    int length, ret;
    mbedtls_x509_crt *crt;

    if (obj->typePtr == &pemObjType) {
        if (GET_PEM_TYPE(obj) == PEM_CRT) {
            *loc = (mbedtls_x509_crt *)GET_PEM_PTR(obj);
            return TCL_OK;
        }
        goto fail;
    }

    /* Try to parse */
    buf = Tcl_GetStringFromObj(obj, &length);
    crt = ckalloc(sizeof(mbedtls_x509_crt));
    mbedtls_x509_crt_init(crt);

    /* Include final null byte in length */
    if ((ret = mbedtls_x509_crt_parse(crt, (unsigned char *)buf, length+1)) != 0) {
        ckfree(crt);
fail:        
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("failed to parse certificate(s) - %s",
                                               sslErrorStatic(ret)));
        return TCL_ERROR;
    }

    *loc = crt;
    return TCL_OK;
}

static int
getKeyFromObj(Tcl_Interp *interp, Tcl_Obj *obj,
              mbedtls_pk_context **ret)
{
    char *buf;
    int length;
    mbedtls_pk_context *pk;
    
    if (obj->typePtr == &pemObjType) {
        if (GET_PEM_TYPE(obj) == PEM_KEY) {
            *ret = (mbedtls_pk_context *)GET_PEM_PTR(obj);
            return TCL_OK;
        }
        goto fail;
    }

    buf = Tcl_GetStringFromObj(obj, &length);
    pk = ckalloc(sizeof(mbedtls_pk_context));
    mbedtls_pk_init(pk);

    /* Include final null byte in length */
    if (mbedtls_pk_parse_key(pk, (unsigned char *)buf, length+1, NULL, -1) != 0) {
        ckfree(pk);
fail:
        Tcl_SetObjResult(interp, Tcl_NewStringObj("failed to parse key", -1));
        return TCL_ERROR;
    }

    *ret = pk;
    return TCL_OK;
}

static void
timerHandler(ClientData cd)
{
    State *statePtr = (State *)cd;

    statePtr->timer = NULL;
    Tcl_NotifyChannel(statePtr->chan, TCL_READABLE);
}

static int
setupCmd(Tcl_Interp *interp, Config *config,
         int objc, Tcl_Obj *const objv[])
{
    Tcl_Channel chan;
    char *server_name = NULL, *opt;
    int i;
    mbedtls_ssl_context *ssl;
    State *state;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "channel ?options?");
        return TCL_ERROR;
    }

    if (TclGetChannelFromObj(interp, objv[2], &chan, NULL, 0) != TCL_OK) {
        return TCL_ERROR;
    }

    if (((objc-3) & 1) != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unpaired option", -1));
        return TCL_ERROR;
    }

    for (i = 3; i < objc; i += 2) {
        opt = Tcl_GetStringFromObj(objv[i], NULL);
        if (strcmp(opt, "-servername") == 0) {
            server_name = Tcl_GetString(objv[i+1]);
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("bad option %s", opt));
            return TCL_ERROR;
        }
    }

    state = ckalloc(sizeof(State));
    ssl = &state->ssl;
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_setup(ssl, &config->config);
    if (config->config.endpoint == MBEDTLS_SSL_IS_CLIENT && server_name != NULL) {
        mbedtls_ssl_set_hostname(ssl, server_name);
    }
    mbedtls_ssl_set_bio(ssl, state, bioSend, bioRecv, NULL);
    state->chan = Tcl_StackChannel(interp, &sslChannelType, (ClientData)state,
                                   TCL_READABLE|TCL_WRITABLE, chan);
    return TCL_OK;
}

static int
continueHandshakeCmd(ClientData cd, Tcl_Interp *interp,
                     int objc, Tcl_Obj *const objv[])
{
    
    Tcl_Channel chan;
    State *state;
    int ret, i;
    static struct {
        int code;
        char *result;
        Tcl_Obj *obj;
    } tab[] = {
        {0, "done", NULL},
        {MBEDTLS_ERR_SSL_WANT_READ, "readable", NULL},
        {MBEDTLS_ERR_SSL_WANT_WRITE, "writable", NULL}
    };
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "channel");
        return TCL_ERROR;
    }
    
    if (TclGetChannelFromObj(interp, objv[1], &chan, NULL, 0) != TCL_OK) {
        return TCL_ERROR;
    }
    chan = Tcl_GetTopChannel(chan);

    if (Tcl_GetChannelType(chan) != &sslChannelType) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("channel %s is not an ssl channel",
                                               Tcl_GetChannelName(chan)));
        return TCL_ERROR;
    }

    state = (State *)Tcl_GetChannelInstanceData(chan);
    ret = mbedtls_ssl_handshake(&state->ssl);
    for (i = 0; i < sizeof(tab)/sizeof(*tab); i++) {
        if (ret == tab[i].code) {
            if (tab[i].obj == NULL) {
                tab[i].obj = Tcl_NewStringObj(tab[i].result, -1);
                Tcl_IncrRefCount(tab[i].obj);
            }
            Tcl_SetObjResult(interp, tab[i].obj);
            return TCL_OK;
        }
    }
    Tcl_SetObjResult(interp, sslError(ret));
    return TCL_ERROR;
}

static int
sslConfigHandler(ClientData cd, Tcl_Interp *interp,
                 int objc, Tcl_Obj *const objv[])
{
    Config *config;
    int index;
    static const char *const options[] = {
        "set_own_cert", "set_ca_chain",
        "setup", NULL
    };
    enum option {
        OPT_SET_OWN_CERT, OPT_SET_CA_CHAIN,
        OPT_SETUP
    };
    mbedtls_x509_crt *crt;
    mbedtls_pk_context *pk;

    config = (Config *)cd;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], options, "option", 0,
                            &index) != TCL_OK) {
        return TCL_ERROR;
    }
    switch ((enum option)index) {
    case OPT_SET_OWN_CERT:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "cert key");
            return TCL_ERROR;
        }
        if (getCertFromObj(interp, objv[2], &crt) != TCL_OK ||
            getKeyFromObj(interp, objv[3], &pk) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_ListObjAppendElement(NULL, config->pemList, objv[2]);
        Tcl_ListObjAppendElement(NULL, config->pemList, objv[3]);
        mbedtls_ssl_conf_own_cert(&config->config, crt, pk);
        return TCL_OK;
    case OPT_SET_CA_CHAIN:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "certs");
            return TCL_ERROR;
        }
        if (getCertFromObj(interp, objv[2], &crt) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_ListObjAppendElement(NULL, config->pemList, objv[2]);
        mbedtls_ssl_conf_ca_chain(&config->config, crt, NULL);
        return TCL_OK;
    case OPT_SETUP:
        return setupCmd(interp, config, objc, objv);
    }

    /* Not reached */
    return TCL_OK;
}

static void
deleteConfig(ClientData cd)
{
    Config *config = (Config *)cd;
    mbedtls_ssl_cache_free(&config->cache);
    mbedtls_ssl_config_free(&config->config);
    Tcl_DecrRefCount(config->pemList);
    ckfree(config);
}

static int
newConfigCmd(ClientData cd, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[])
{
    int i;
    int endpoint = MBEDTLS_SSL_IS_CLIENT,
        transport = MBEDTLS_SSL_TRANSPORT_STREAM,
        preset = MBEDTLS_SSL_PRESET_DEFAULT;
    Config *config;
    char *opt, *mode;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?options");
        return TCL_ERROR;
    }

    for (i = 2; i < objc; i++) {
        opt = Tcl_GetStringFromObj(objv[i], NULL);
        if (*opt != '-') {
            break;
        }

        if (strcmp(opt, "-mode") == 0) {
            if (++i == objc)
                goto missing;
            mode = Tcl_GetStringFromObj(objv[i], NULL);
            if (strcmp(mode, "server") == 0) {
                endpoint = MBEDTLS_SSL_IS_SERVER;
            } else if (strcmp(mode, "client") != 0) {
                Tcl_SetObjResult
                    (interp, Tcl_ObjPrintf("bad mode %s, must be server or client", mode));
                return TCL_ERROR;
            }
        }
        continue;
        
missing:
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("missing value for option %s", opt));
        return TCL_ERROR;
    }

    if (!initialized) {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (unsigned char *)"qdxc", 4);
        initialized = 1;
    }
    config = ckalloc(sizeof(Config));
    mbedtls_ssl_config_init(&config->config);
    mbedtls_ssl_cache_init(&config->cache);
    config->pemList = Tcl_NewObj(); /* stay at 0 RC until deletion */
    if (mbedtls_ssl_config_defaults(&config->config, endpoint, transport, preset) != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("init failed", -1));
        return TCL_ERROR;
    }
    mbedtls_ssl_conf_rng(&config->config, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_session_cache(&config->config, &config->cache,
                                   mbedtls_ssl_cache_get,
                                   mbedtls_ssl_cache_set);
        
    Tcl_CreateObjCommand(interp, Tcl_GetString(objv[1]), sslConfigHandler,
                         (ClientData)config, deleteConfig);
    return TCL_OK;
}

static int
bioRecv(void *ctx, unsigned char *buf, size_t len)
{
    int ret, err;
    Tcl_Channel chan;
    State *statePtr;

    statePtr = (State *)ctx;
    chan = Tcl_GetStackedChannel(statePtr->chan);
    
    ret = Tcl_ReadRaw(chan, (char *)buf, len);
    if (ret < 0) {
        err = Tcl_GetErrno();
        if (err == EAGAIN || err == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        } else if (err == EPIPE || err == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        } else {
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }
    return ret;
}

static int
bioSend(void *ctx, const unsigned char *buf, size_t len)
{
    int ret, err;
    Tcl_Channel chan;
    State *statePtr;

    statePtr = (State *)ctx;
    chan = Tcl_GetStackedChannel(statePtr->chan);
    
    ret = Tcl_WriteRaw(chan, (const char *)buf, len);
    if (ret < 0) {
        err = Tcl_GetErrno();
        if (err == EAGAIN || err == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        } else if (err == EPIPE || err == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        } else {
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }
    return ret;
}

/*static int
SslBlockModeProc(ClientData, int)
{
    
}*/

static int
SslCloseProc(ClientData cd, Tcl_Interp *interp)
{
    State *statePtr = (State *)cd;
    mbedtls_ssl_close_notify(&statePtr->ssl);
    mbedtls_ssl_free(&statePtr->ssl);
    if (statePtr->timer) {
        Tcl_DeleteTimerHandler(statePtr->timer);
    }
    ckfree(statePtr);
    return TCL_OK;
}

static int
SslInputProc(ClientData cd, char *buf, int toRead, int *errorCodePtr)
{
    int ret;
    State *statePtr = (State *)cd;

    ret = mbedtls_ssl_read(&statePtr->ssl, (unsigned char *)buf, toRead);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        *errorCodePtr = EAGAIN;
        return -1;
    } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    } else if (ret < 0) {
        Tcl_SetChannelError(statePtr->chan, sslError(ret));
        return -1;
    }
    
    return ret;
}

static int
SslOutputProc(ClientData cd, const char *buf, int toWrite,
              int *errorCodePtr)
{
    int ret;
    State *statePtr = (State *)cd;
    
    ret = mbedtls_ssl_write(&statePtr->ssl, (unsigned char *)buf, toWrite);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        *errorCodePtr = EAGAIN;
        return -1;
    } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    } else if (ret < 0) {
        Tcl_SetChannelError(statePtr->chan, sslError(ret));
        return -1;
    }
    
    return ret;
}

static int
SslGetOptionProc(ClientData cd, Tcl_Interp *Interp,
                 const char *optionName, Tcl_DString *dsPtr)
{
    return 0;
}

static void
SslWatchProc(ClientData cd, int mask)
{
    State *statePtr;
    Tcl_Channel parent;

    statePtr = (State *)cd;
    parent = Tcl_GetStackedChannel(statePtr->chan);

    Tcl_GetChannelType(parent)
        ->watchProc(Tcl_GetChannelInstanceData(parent), mask);
    
    if (statePtr->timer != NULL) {
        Tcl_DeleteTimerHandler(statePtr->timer);
        statePtr->timer = NULL;
    }
    if ((mask & TCL_READABLE) && mbedtls_ssl_get_bytes_avail(&statePtr->ssl) > 0) {
        statePtr->timer = Tcl_CreateTimerHandler(0, timerHandler, (ClientData)statePtr);
    }
}

static int
SslNotifyProc(ClientData cd, int mask) {
    State *statePtr = (State *)cd;

    if ((mask & TCL_READABLE) && statePtr->timer != NULL) {
        Tcl_DeleteTimerHandler(statePtr->timer);
        statePtr->timer = NULL;
    }
        
    return mask;
}

void
sslInit(Tcl_Interp *interp)
{
    Tcl_Eval(interp, "namespace eval ssl {}");
    Tcl_CreateObjCommand(interp, "ssl::new_config", newConfigCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "ssl::_continue_handshake", continueHandshakeCmd, NULL, NULL);
}
