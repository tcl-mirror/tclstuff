#include <tcl.h>

static int setSelectorFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);

static Tcl_ObjType selectorType =
    {"recordSelector", NULL, NULL, NULL, setSelectorFromAny};

static void shapeFreeIntRep(Tcl_Obj *obj);
static void shapeStringRep(Tcl_Obj *obj);
static int setShapeFromAny(Tcl_Interp *interp, Tcl_Obj *obj);

static Tcl_ObjType shapeType =
    {"recordShape", shapeFreeIntRep, NULL, shapeStringRep, setShapeFromAny};

static void
recordAssocDeleteProc(ClientData cd, Tcl_Interp *interp) {
    Tcl_HashTable *table;

    table = (Tcl_HashTable *)cd;
    Tcl_DeleteHashTable(table);
    ckfree(table);
}

static Tcl_HashTable *
getSymTable(Tcl_Interp *interp) {
    Tcl_HashTable *table;

    while (Tcl_GetMaster(interp) != NULL) {
      interp = Tcl_GetMaster(interp);
    }
    
    table = (Tcl_HashTable *)Tcl_GetAssocData(interp, "record:symtable", NULL);
    if (table == NULL) {
        table = ckalloc(sizeof(*table));
        Tcl_InitHashTable(table, TCL_STRING_KEYS);
        Tcl_SetAssocData(interp, "record:symtable", recordAssocDeleteProc, (ClientData)table);
    }
    return table;
}

static int
setSelectorFromAny(Tcl_Interp *interp, Tcl_Obj *obj) {
    Tcl_HashTable *table;
    Tcl_HashEntry *entry;
    int isNew;

    if (obj->typePtr == &selectorType) {
        return TCL_OK;
    }

    table = getSymTable(interp);
    entry = Tcl_CreateHashEntry(table, Tcl_GetString(obj), &isNew);
    if (isNew) {
        Tcl_SetHashValue(entry, obj);
        Tcl_IncrRefCount(obj);
    }

    if (obj->typePtr != NULL && obj->typePtr->freeIntRepProc != NULL) {
        obj->typePtr->freeIntRepProc(obj);
    }
    obj->internalRep.ptrAndLongRep.ptr = Tcl_GetHashValue(entry);
    obj->internalRep.ptrAndLongRep.value = -1;
    obj->typePtr = &selectorType;
    return TCL_OK;
}

static void
shapeFreeIntRep(Tcl_Obj *obj) {
    ckfree(obj->internalRep.ptrAndLongRep.ptr);
    obj->typePtr = NULL;
}

static void
shapeStringRep(Tcl_Obj *obj)
{
    int numFields;
    Tcl_Obj *ls, **fields;

    numFields = obj->internalRep.ptrAndLongRep.value;
    fields = obj->internalRep.ptrAndLongRep.ptr;
    ls = Tcl_NewListObj(numFields, fields);
    (void)Tcl_GetString(ls);
    obj->bytes = ls->bytes;
    obj->length = ls->length;
    ls->bytes = NULL;
    Tcl_DecrRefCount(ls);
}

static int
setShapeFromAny(Tcl_Interp *interp, Tcl_Obj *obj) {
    int i, objc, isNew;
    Tcl_Obj **objv, **fields;
    Tcl_HashTable *table;
    Tcl_HashEntry *entry;

    if (obj->typePtr == &shapeType)
        return TCL_OK;

    if (Tcl_ListObjGetElements(interp, obj, &objc, &objv) != TCL_OK)
        return TCL_ERROR;

    table = getSymTable(interp);
    fields = ckalloc(objc * sizeof(Tcl_Obj *));
    for (i = 0; i < objc; i++) {
        entry = Tcl_CreateHashEntry(table, Tcl_GetString(objv[i]), &isNew);
        if (isNew) {
            Tcl_SetHashValue(entry, objv[i]);
            Tcl_IncrRefCount(objv[i]);
        }
        fields[i] = Tcl_GetHashValue(entry);
    }

    if (obj->typePtr != NULL && obj->typePtr->freeIntRepProc != NULL) {
        obj->typePtr->freeIntRepProc(obj);
    }
    obj->internalRep.ptrAndLongRep.value = objc;
    obj->internalRep.ptrAndLongRep.ptr = fields;
    obj->typePtr = &shapeType;
    return TCL_OK;
}

int
recordObjCmd(ClientData cd, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[]) {
    int recLength, i, length, index;
    Tcl_Obj *recObj, **recVals, *field, **fields, *symbol;

    if (objc == 3) {
        recObj = objv[1];
        if (Tcl_ListObjGetElements(interp, recObj, &recLength, &recVals) != TCL_OK) {
            return TCL_ERROR;
        }
    } else if (objc == 4) {
        recObj = Tcl_ObjGetVar2(interp, objv[1], NULL, 0);
        if (recObj == NULL ||
            Tcl_ListObjGetElements(interp, recObj, &recLength, &recVals) != TCL_OK) {
            return TCL_ERROR;
        }
        if (Tcl_IsShared(recObj)) {
            /* Note: Tcl_DuplicateObj doesn't do the job because internal rep is still shared */
            recObj = Tcl_NewListObj(recLength, recVals);
            Tcl_ListObjGetElements(NULL, recObj, &recLength, &recVals);
        }
    } else {
        Tcl_WrongNumArgs(interp, 1, objv, "record field ?value?");
        return TCL_ERROR;
    }

    if (recLength < 1)
        goto bad_record;
    
    if (setShapeFromAny(interp, recVals[0]) != TCL_OK)
        return TCL_ERROR;

    length = recVals[0]->internalRep.ptrAndLongRep.value;
    fields = recVals[0]->internalRep.ptrAndLongRep.ptr;

    if (recLength < length+1)
        goto bad_record;

    field = objv[2];
    setSelectorFromAny(interp, field);
    index = field->internalRep.ptrAndLongRep.value;
    symbol = field->internalRep.ptrAndLongRep.ptr;

    if (index == -1 || index >= length || fields[index] != symbol) {
        for (i = 0; i < length; i++) {
            if (fields[i] == symbol) {
                index = field->internalRep.ptrAndLongRep.value = i;
                goto found;
            }
        }
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("field \"%s\" not found in record", Tcl_GetString(field)));
        return TCL_ERROR;
    }
 found:

    if (objc == 3) {
        Tcl_SetObjResult(interp, recVals[index+1]);
        return TCL_OK;
    }

    Tcl_DecrRefCount(recVals[index+1]);
    recVals[index+1] = objv[3];
    Tcl_IncrRefCount(recVals[index+1]);
    Tcl_InvalidateStringRep(recObj);
    if (Tcl_ObjSetVar2(interp, objv[1], NULL, recObj, TCL_LEAVE_ERR_MSG) == NULL) {
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, recObj);
    return TCL_OK;

 bad_record:
    Tcl_SetObjResult(interp, Tcl_NewStringObj("bad record", -1));
    return TCL_ERROR;
}
