#include <tcl.h>
#include "qdsh.h"

typedef struct Node {
    Tcl_Obj *key;
    Tcl_Obj *val;
    struct Node *prev;
    struct Node *next;
} Node;

typedef struct {
    int size;
    int max;
    Node *list;
    Tcl_HashTable table;
} LRU;

static void
freeLRU(ClientData cd)
{
    Node *n, *cur;
    LRU *lru = (LRU *)cd;

    n = lru->list;
    if (n) {
        do {
            Tcl_DecrRefCount(n->key);
            Tcl_DecrRefCount(n->val);
            cur = n;
            n = n->next;
            ckfree(cur);
        } while (n != lru->list);
    }
    Tcl_DeleteHashTable(&lru->table);
}

static Tcl_Obj *
lruKeys(LRU *lru)
{
    Node *n;
    Tcl_Obj *ls = Tcl_NewListObj(lru->size, NULL);

    n = lru->list;
    if (n) {
        do {
            Tcl_ListObjAppendElement(NULL, ls, n->key);
            n = n->next;
        } while (n != lru->list);
    }
    return ls;
}

static int
lruHandler(ClientData cd, Tcl_Interp *interp,
           int objc, Tcl_Obj *const objv[])
{
    int index, isNew;
    LRU *lru;
    Tcl_HashEntry *entry;
    Node *node;
    static const char *const options[] = {
        "get", "keys", "max", "put", "size", NULL
    };
    enum option {
        OPT_GET, OPT_KEYS, OPT_MAX, OPT_PUT, OPT_SIZE
    };

    lru = (LRU *)cd;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd args ...");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], options, "option", 0,
                            &index) != TCL_OK) {
        return TCL_ERROR;
    }
    switch ((enum option)index) {
    case OPT_GET:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "key");
            return TCL_ERROR;
        }
        entry = Tcl_FindHashEntry(&lru->table, Tcl_GetString(objv[2]));
        if (entry) {
            node = (Node *)Tcl_GetHashValue(entry);
            lru->list = node;
            Tcl_SetObjResult(interp, Tcl_NewListObj(1, &node->val));
        }
        return TCL_OK;
    case OPT_KEYS:
        if (objc != 2) {
            goto zeroArg;
        }
        Tcl_SetObjResult(interp, lruKeys(lru));
        return TCL_OK;
    case OPT_MAX:
        if (objc != 2) {
            goto zeroArg;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(lru->max));
        return TCL_OK;
    case OPT_PUT:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "key value");
            return TCL_ERROR;
        }
        isNew = 0;
        entry = Tcl_CreateHashEntry(&lru->table, Tcl_GetString(objv[2]), &isNew);
        if (isNew) {
            if (lru->size == lru->max) {
                node = lru->list->prev;
                Tcl_DeleteHashEntry(Tcl_FindHashEntry(&lru->table, Tcl_GetString(node->key)));
                Tcl_SetObjResult(interp, Tcl_NewListObj(1, &node->key));
                Tcl_DecrRefCount(node->key);
                Tcl_DecrRefCount(node->val);
            } else {
                node = ckalloc(sizeof(Node));
                if (lru->size == 0) {
                    node->next = node->prev = node;
                } else {
                    Node *last = lru->list->prev;
                    lru->list->prev = node;
                    node->next = lru->list;
                    node->prev = last;
                    last->next = node;
                }
                lru->size++;
            }
            node->key = objv[2];
            node->val = objv[3];
            Tcl_IncrRefCount(node->key);
            Tcl_IncrRefCount(node->val);
            lru->list = node;
            Tcl_SetHashValue(entry, (ClientData)node);
        } else {
            node = (Node *)Tcl_GetHashValue(entry);
            Tcl_IncrRefCount(objv[3]);
            Tcl_DecrRefCount(node->val);
            node->val = objv[3];
        }
        return TCL_OK;
    case OPT_SIZE:
        if (objc != 2) {
            goto zeroArg;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(lru->size));
        return TCL_OK;
    }

zeroArg:
    Tcl_WrongNumArgs(interp, 2, objv, NULL);
    return TCL_ERROR;
}


int
lruCmd(ClientData cd, Tcl_Interp *interp,
       int objc, Tcl_Obj *const objv[])
{
    int max;
    LRU *lru;
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd max");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &max) != TCL_OK) {
        return TCL_ERROR;
    }
    if (max <= 1) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("max must be greater than 1", -1));
        return TCL_ERROR;
    }
    
    lru = ckalloc(sizeof(LRU));
    lru->max = max;
    lru->size = 0;
    lru->list = NULL;
    Tcl_InitHashTable(&lru->table, TCL_STRING_KEYS);
    Tcl_CreateObjCommand(interp, Tcl_GetString(objv[1]), lruHandler,
                         (ClientData)lru, freeLRU);
    return TCL_OK;
}
