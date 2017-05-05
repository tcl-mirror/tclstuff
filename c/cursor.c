#include <tcl.h>
#include <tclStringRep.h>
#include "cursor.h"

extern const Tcl_ObjType tclStringType;

static void freeCursorIntRep(Tcl_Obj *);
static void dupCursorIntRep(Tcl_Obj *, Tcl_Obj *);
static void updateStringOfCursor(Tcl_Obj *);
    
static Tcl_ObjType cursorType = {
    "cursor",
    freeCursorIntRep,
    dupCursorIntRep,
    updateStringOfCursor,
    setCursorFromAny
};

static void
freeCursor(Cursor *cur)
{
    Tcl_DecrRefCount(cur->string);
    if (cur->charPosObj) Tcl_DecrRefCount(cur->charPosObj);
    ckfree(cur);
}

static void
freeCursorIntRep(Tcl_Obj *obj)
{
    freeCursor((Cursor *)obj->internalRep.otherValuePtr);
    obj->typePtr = NULL;
}

static void
dupCursorIntRep(Tcl_Obj *src, Tcl_Obj *dst)
{
    Cursor *srcCur, *dstCur;

    srcCur = src->internalRep.otherValuePtr;
    dstCur = ckalloc(sizeof(Cursor));
    dstCur->string = srcCur->string;
    Tcl_IncrRefCount(dstCur->string);
    if (srcCur->charPosObj) {
        dstCur->charPosObj = srcCur->charPosObj;
        Tcl_IncrRefCount(dstCur->charPosObj);
    } else {
        dstCur->charPosObj = NULL;
    }
    dstCur->bytePos = srcCur->bytePos;
    dstCur->charPos = srcCur->charPos;
    dst->internalRep.otherValuePtr = dstCur;
    dst->typePtr = &cursorType;
}

static void
updateStringOfCursor(Tcl_Obj *obj)
{
    Cursor *cur;
    Tcl_Obj *ls, *objv[2];

    cur = obj->internalRep.otherValuePtr;
    objv[0] = cur->string;
    objv[1] = cur->charPosObj ? cur->charPosObj : Tcl_NewIntObj(cur->charPos);
    ls = Tcl_NewListObj(2, objv);
    Tcl_GetString(ls);
    obj->bytes = ls->bytes;
    obj->length = ls->length;
    ls->bytes = NULL;
    Tcl_DecrRefCount(ls);
}

static void
moveCursor(Cursor *cur, int index)
{
    int byteLength, newCharPos;
    const char *str, *end, *chPtr;

    str = Tcl_GetStringFromObj(cur->string, &byteLength);
    newCharPos = cur->charPos;
    chPtr = str + cur->bytePos;
    end = str + byteLength;

    if (newCharPos == index) {
        return;
    }

    if (newCharPos < index) {
        do {
            if (chPtr == end) break;
            chPtr = Tcl_UtfNext(chPtr);
            newCharPos++;
        } while (newCharPos < index);
    } else {
        do {
            if (chPtr == str) break;
            chPtr = Tcl_UtfPrev(chPtr, str);
            newCharPos--;
        } while (newCharPos > index);
    }

    if (newCharPos != cur->charPos) {
        if (cur->charPosObj) {
            Tcl_DecrRefCount(cur->charPosObj);
            cur->charPosObj = NULL;
        }
        cur->charPos = newCharPos;
        cur->bytePos = chPtr - str;
    }
}

int
setCursorFromAny(Tcl_Interp *interp, Tcl_Obj *obj)
{
    int objc, charPos;
    Tcl_Obj **objv;
    Cursor *cur;
    String *str;

    if (obj->typePtr == &cursorType)
        return TCL_OK;
    
    if (Tcl_ListObjGetElements(interp, obj, &objc, &objv) != TCL_OK)
        return TCL_ERROR;

    if (objc != 2) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("wrong format for cursor; must have two elements", -1));
        return TCL_ERROR;
    }

    if (Tcl_GetIntFromObj(interp, objv[1], &charPos) != TCL_OK)
        return TCL_ERROR;

    cur = ckalloc(sizeof(Cursor));
    cur->string = objv[0];
    Tcl_IncrRefCount(cur->string);
    if (objv[1]->bytes) {
        cur->charPosObj = objv[1];
        Tcl_IncrRefCount(cur->charPosObj);
    } else {
        cur->charPosObj = NULL;
    }

    if (cur->string->typePtr == &tclStringType) {
        str = GET_STRING(cur->string);
        cur->bytePos = str->byteCursor;
        cur->charPos = str->charCursor;
    } else {
        cur->bytePos = 0;
        cur->charPos = 0;
    }

    moveCursor(cur, charPos);
    if (cur->charPos != charPos) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("character index %d out of range", charPos));
        freeCursor(cur);
        return TCL_ERROR;
    }

    if (obj->typePtr && obj->typePtr->freeIntRepProc) obj->typePtr->freeIntRepProc(obj);
    obj->internalRep.otherValuePtr = cur;
    obj->typePtr = &cursorType;
    return TCL_OK;
}

static int
getCursor(Tcl_Interp *interp, Tcl_Obj *obj, Cursor **ret)
{
    if (setCursorFromAny(interp, obj) == TCL_ERROR) return TCL_ERROR;
    *ret = obj->internalRep.otherValuePtr;
    return TCL_OK;
}

Tcl_Obj *
newCursorObj(Tcl_Obj *string, int bytePos, int charPos)
{
    Tcl_Obj *obj;
    Cursor *cur;

    cur = ckalloc(sizeof(Cursor));
    cur->string = string;
    Tcl_IncrRefCount(cur->string);
    cur->charPosObj = NULL;
    cur->bytePos = bytePos;
    cur->charPos = charPos;
    obj = Tcl_NewObj();
    obj->typePtr = &cursorType;
    obj->internalRep.otherValuePtr = cur;
    Tcl_InvalidateStringRep(obj);
    return obj;
}

static int
incrOrConsume(Tcl_Interp *interp, Tcl_Obj *varName, Tcl_Obj *incr, int consume)
{
    Cursor *cur;
    int displacement = 1, allocated = 0, startBytePos, startCharPos;
    Tcl_Obj *varValue, *updated;

    varValue = Tcl_ObjGetVar2(interp, varName, NULL, 0);
    if (!varValue ||
        (incr && Tcl_GetIntFromObj(interp, incr, &displacement) == TCL_ERROR) ||
        getCursor(interp, varValue, &cur) == TCL_ERROR) {
        return TCL_ERROR;
    }
    
    if (Tcl_IsShared(varValue)) {
        varValue = Tcl_DuplicateObj(varValue);
        cur = varValue->internalRep.otherValuePtr;
        allocated = 1;
    }
    
    startBytePos = cur->bytePos;
    startCharPos = cur->charPos;
    moveCursor(cur, startCharPos + displacement);
    if (cur->charPos > startCharPos) {
        Tcl_InvalidateStringRep(varValue);
        if (consume) {
            /* Cursor moved forward--return string range */
            Tcl_SetObjResult(interp,
                Tcl_NewStringObjWithCharLength(cur->string->bytes + startBytePos,
                                               cur->bytePos - startBytePos,
                                               cur->charPos - startCharPos));
        }
    } else if (cur->charPos < startCharPos) {
        Tcl_InvalidateStringRep(varValue);
    } else {
        if (!consume) {
            Tcl_SetObjResult(interp, varValue);
        }
        return TCL_OK;
    }

    /* Cursor moved, update cursor variable. */
    updated = Tcl_ObjSetVar2(interp, varName, NULL, varValue, TCL_LEAVE_ERR_MSG);
    if (!updated) {
        if (allocated) Tcl_DecrRefCount(varValue);
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int
moveObj(Tcl_Interp *interp, Tcl_Obj **obj, int displacement, int *allocated)
{
    Cursor *cur;
    int oldPos;
    
    if (getCursor(interp, *obj, &cur) == TCL_ERROR) {
        return TCL_ERROR;
    }
    if (Tcl_IsShared(*obj)) {
        *obj = Tcl_DuplicateObj(*obj);
        cur = (*obj)->internalRep.otherValuePtr;
        if (allocated) *allocated = 1;
    }
    oldPos = cur->charPos;
    moveCursor(cur, oldPos + displacement);
    if (cur->charPos != oldPos) Tcl_InvalidateStringRep(*obj);
    return TCL_OK;
}

static int
cursorRange(Tcl_Interp *interp, Tcl_Obj *startObj, Tcl_Obj *endObj)
{
    Cursor *start, *end;
    char *str;
    int len;
    
    if (getCursor(interp, startObj, &start) == TCL_ERROR) {
        return TCL_ERROR;
    }
    if (endObj) {
        if (getCursor(interp, endObj, &end) == TCL_ERROR) {
            return TCL_ERROR;
        }
        if (start->string != end->string &&
            (start->string->length != end->string->length ||
             Tcl_UtfNcmp(start->string->bytes, end->string->bytes,
                         start->string->length) != 0)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("strings don't match", -1));
            return TCL_ERROR;
          }
          if (start->bytePos < end->bytePos) {
            Tcl_SetObjResult(interp,
                Tcl_NewStringObjWithCharLength(start->string->bytes + start->bytePos,
                                               end->bytePos - start->bytePos,
                                               end->charPos - start->charPos));
          }
          return TCL_OK;
    } else {
        str = Tcl_GetStringFromObj(start->string, &len);
        if (start->bytePos < len) {
            if (start->string->typePtr == &tclStringType &&
                GET_STRING(start->string)->numChars != -1) {
                Tcl_SetObjResult(interp,
                    Tcl_NewStringObjWithCharLength(start->string->bytes + start->bytePos,
                                                   len - start->bytePos,
                                                   GET_STRING(start->string)->numChars - start->charPos));
            } else {
                Tcl_SetObjResult(interp, Tcl_NewStringObj(str + start->bytePos, len - start->bytePos));
            }
        }
    }
    return TCL_OK;
}

int
cursorCmd(ClientData cd, Tcl_Interp *interp,
          int objc, Tcl_Obj *const objv[])
{
    Cursor *cur;
    int index, displacement;
    Tcl_Obj *res;
    static const char *const options[] = {
        "consume", "end",   "eof",    "find", "incr", "index",
        "move",    "range", "string", "pos",  NULL
    };
    enum option {
        OPT_CONSUME, OPT_END,  OPT_EOF,   OPT_FIND,   OPT_INCR,
        OPT_INDEX,   OPT_MOVE, OPT_RANGE, OPT_STRING, OPT_POS
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], options, "option", 0,
                            &index) != TCL_OK) {
        return TCL_ERROR;
    }

    switch ((enum option)index) {
    case OPT_CONSUME:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "cursor displacemet");
            return TCL_ERROR;
        }
        return incrOrConsume(interp, objv[2], objv[3], 1);
    case OPT_END:
    case OPT_EOF:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "cursor");
            return TCL_ERROR;
        }
        if (getCursor(interp, objv[2], &cur) == TCL_ERROR) {
            return TCL_ERROR;
        } else {
            int len;
            Tcl_GetStringFromObj(cur->string, &len);
            Tcl_SetObjResult(interp, (index == OPT_EOF) ?
                Tcl_NewBooleanObj(cur->bytePos == len) :
                             newCursorObj(cur->string, len, Tcl_GetCharLength(cur->string)));
            return TCL_OK;
        }
    case OPT_FIND:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "cursor substring");
            return TCL_ERROR;
        } else if (getCursor(interp, objv[2], &cur) == TCL_ERROR) {
            return TCL_ERROR;
        } else {
            int charPos, subLen, strLen;
            char *sub, *str, *end;
            const char *p;

            str = Tcl_GetStringFromObj(cur->string, &strLen);
            end = str + strLen;
            sub = Tcl_GetStringFromObj(objv[3], &subLen);
            charPos = cur->charPos;
            if (subLen > 0) {
              Tcl_UniChar ch, firstChar;

              Tcl_UtfToUniChar(sub, &firstChar);
              for (p = str+cur->bytePos; p+subLen <= end; p = Tcl_UtfNext(p)) {
                Tcl_UtfToUniChar(p, &ch);
                if (ch == firstChar && Tcl_UtfNcmp(sub, p, subLen) == 0) {
                  res = newCursorObj(cur->string, p-str, charPos);
                  Tcl_SetObjResult(interp, Tcl_NewListObj(1, &res));
                  return TCL_OK;
                }
                charPos++;
              }
            }
            return TCL_OK;
        }
    case OPT_INCR:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "varName ?displacement?");
            return TCL_ERROR;
        }
        return incrOrConsume(interp, objv[2], objc == 3 ? NULL : objv[3], 0);
    case OPT_INDEX:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "cursor");
            return TCL_ERROR;
        }
        if (getCursor(interp, objv[2], &cur) == TCL_ERROR) {
            return TCL_ERROR;
        }
        if (cur->bytePos < cur->string->length) {
            char *chPtr = cur->string->bytes + cur->bytePos;
            Tcl_SetObjResult(interp, Tcl_NewStringObjWithCharLength(chPtr, Tcl_UtfNext(chPtr)-chPtr, 1));
        }
        return TCL_OK;
    case OPT_MOVE:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "cursor displacemet");
            return TCL_ERROR;
        }
        res = objv[2];
        if (Tcl_GetIntFromObj(interp, objv[3], &displacement) == TCL_ERROR ||
            moveObj(interp, &res, displacement, NULL) == TCL_ERROR) {
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, res);
        return TCL_OK;
    case OPT_RANGE:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "start end");
            return TCL_ERROR;
        }
        return cursorRange(interp, objv[2], objc == 3 ? NULL : objv[3]);
    case OPT_STRING:
    case OPT_POS:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "cursor");
            return TCL_ERROR;
        }
        if (getCursor(interp, objv[2], &cur) == TCL_ERROR) {
            return TCL_ERROR;
        }
        if (index == OPT_STRING) {
            if (cur->string->typePtr == &tclStringType) {
                String *str = GET_STRING(cur->string);
                str->byteCursor = cur->bytePos;
                str->charCursor = cur->charPos;
            }
            Tcl_SetObjResult(interp, cur->string);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(cur->charPos));
        }
        return TCL_OK;
    }

    /* Not reached */
    return TCL_OK;
}
