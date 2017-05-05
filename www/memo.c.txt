#include <stdlib.h>
#include <tcl.h>

typedef struct Memo {
    Tcl_Obj *orig;
    Tcl_Obj *value;
    int cmdListLength;
    Tcl_Obj *cmdList[1];
} Memo;

#define GET_MEMO(obj) ((Memo *)(obj)->internalRep.otherValuePtr)
#define SET_MEMO(obj, memo) ((obj)->internalRep.otherValuePtr = (memo))

static void freeMemoRep(Tcl_Obj *obj);
static void dupMemoRep(Tcl_Obj *src, Tcl_Obj *dup);
static void updateMemoString(Tcl_Obj *obj);

static const Tcl_ObjType memoType = {
    "memo",
    freeMemoRep,
    dupMemoRep,
    updateMemoString,
    NULL
};

static void
freeMemoRep(Tcl_Obj *obj)
{
    int i;
    
    Memo *memo = GET_MEMO(obj);
    if (memo->orig) Tcl_DecrRefCount(memo->orig);
    Tcl_DecrRefCount(memo->value);
    for (i = 0; i < memo->cmdListLength; i++) {
        Tcl_DecrRefCount(memo->cmdList[i]);
    }
    ckfree(memo);
    obj->typePtr = NULL;
}

static Memo *
allocMemo(Tcl_Obj *orig, Tcl_Obj *value, int cmdListLength, Tcl_Obj **cmdList)
{
    Memo *m;
    int i;
    
    m = ckalloc(sizeof(Memo) + sizeof(Tcl_Obj *)*(cmdListLength-1));
    m->orig = orig;
    if (orig) Tcl_IncrRefCount(orig);
    m->value = value;
    Tcl_IncrRefCount(value);
    m->cmdListLength = cmdListLength;
    for (i = 0; i < cmdListLength; i++) {
        m->cmdList[i] = cmdList[i];
        Tcl_IncrRefCount(cmdList[i]);
    }
    return m;
}


static void
dupMemoRep(Tcl_Obj *src, Tcl_Obj *dst)
{
    Memo *srcMemo, *dstMemo;

    srcMemo = GET_MEMO(src);
    dstMemo = allocMemo(srcMemo->orig, srcMemo->value, srcMemo->cmdListLength,
                        srcMemo->cmdList);
    SET_MEMO(dst, dstMemo);
    dst->typePtr = &memoType;
}

static void updateMemoString(Tcl_Obj *obj)
{
    Memo *memo = GET_MEMO(obj);
    Tcl_GetString(memo->orig);
    obj->bytes = memo->orig->bytes;
    obj->length = memo->orig->length;
    memo->orig->bytes = NULL;
}

static int
memoPostEval(ClientData cd[], Tcl_Interp *interp, int result)
{
    Tcl_Obj *cmdList, *obj, **objv, *orig;
    int objc;
    Memo *memo;

    cmdList = (Tcl_Obj *)cd[0];
    obj = (Tcl_Obj *)cd[1];
    
    if (result == TCL_OK) {
        Tcl_ListObjGetElements(NULL, cmdList, &objc, &objv);
        orig = obj->bytes ? NULL : Tcl_DuplicateObj(obj);
        memo = allocMemo(orig, Tcl_GetObjResult(interp), objc-1, objv);
        if (obj->typePtr && obj->typePtr->freeIntRepProc) {
            obj->typePtr->freeIntRepProc(obj);
        }
        SET_MEMO(obj, memo);
        obj->typePtr = &memoType;
    }
    Tcl_DecrRefCount(cmdList);
    return result;
}

static int
memoNRCmd(ClientData cd, Tcl_Interp *interp,
          int objc, Tcl_Obj *const objv[])
{
    int i;
    Tcl_Obj *cmdList;
    Memo *memo;

    if (objc < 2) {
        return TCL_OK;
    }

    if (objv[objc-1]->typePtr != &memoType) goto calc;
    memo = GET_MEMO(objv[objc-1]);
    if (memo->cmdListLength != objc-2) goto calc;
    for (i = 0; i < memo->cmdListLength; i++) {
        if (memo->cmdList[i] != objv[i+1]) goto calc;
    }
    Tcl_SetObjResult(interp, memo->value);
    return TCL_OK;

calc:
    cmdList = Tcl_NewListObj(objc-1, &objv[1]);
    Tcl_IncrRefCount(cmdList);
    Tcl_NRAddCallback(interp, memoPostEval, (ClientData)cmdList,
                      (ClientData)objv[objc-1], NULL, NULL);
    return Tcl_NREvalObj(interp, cmdList, 0);
}

static int
memoCmd(ClientData cd, Tcl_Interp *interp,
        int objc, Tcl_Obj *const objv[])
{
    return Tcl_NRCallObjProc(interp, memoNRCmd, cd, objc, objv);
}

void
memoInit(Tcl_Interp *interp, char *cmdName)
{
    Tcl_NRCreateCommand(interp, cmdName, memoCmd, memoNRCmd, NULL, NULL);
}
