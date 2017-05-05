#include <tcl.h>
#include "qdsh.h"

static Tcl_Parse parseInfo;

int
parseCommandCmd(ClientData cd, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
    const char *str, *end;
    int start = 0, strLen, nested = 0;

    if (objc == 4) {
        if (Tcl_GetBooleanFromObj(interp, objv[3], &nested) == TCL_ERROR ||
            Tcl_GetIntFromObj(interp, objv[2], &start) == TCL_ERROR) {
            return TCL_ERROR;
        }
    } else if (objc == 3) {
        if (Tcl_GetIntFromObj(interp, objv[2], &start) == TCL_ERROR) return TCL_ERROR;
    } else if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string ?start? ?nested?");
        return TCL_ERROR;
    }
    str = Tcl_GetStringFromObjAt(objv[1], start, &strLen, NULL);
    if (Tcl_ParseCommand(interp, str, strLen, nested, &parseInfo) == TCL_ERROR) {
        return TCL_ERROR;
    }

    end = parseInfo.commandStart + parseInfo.commandSize;
    Tcl_FreeParse(&parseInfo);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(str, end-str));
    return TCL_OK;
}

int
parseVarNameCmd(ClientData cd, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
    const char *str, *end;
    int start = 0, strLen;

    if (objc == 3) {
        if (Tcl_GetIntFromObj(interp, objv[2], &start) == TCL_ERROR) return TCL_ERROR;
    } else if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string ?start?");
        return TCL_ERROR;
    }
    str = Tcl_GetStringFromObjAt(objv[1], start, &strLen, NULL);
    if (Tcl_ParseVarName(interp, str, strLen, &parseInfo, 0) == TCL_ERROR) {
        return TCL_ERROR;
    }
    end = parseInfo.tokenPtr[0].start + parseInfo.tokenPtr[0].size;
    Tcl_FreeParse(&parseInfo);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(str, end-str));
    return TCL_OK;
}

int parseQuotedCmd(ClientData cd, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
    const char *str, *end;
    int start = 0, strLen;
    typedef int (*Fun)(Tcl_Interp *, const char *, int, Tcl_Parse *, int, const char **);

    if (objc == 3) {
        if (Tcl_GetIntFromObj(interp, objv[2], &start) == TCL_ERROR) return TCL_ERROR;
    } else if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string ?start?");
        return TCL_ERROR;
    }
    str = Tcl_GetStringFromObjAt(objv[1], start, &strLen, NULL);
    if (((Fun)cd)(interp, str, strLen, &parseInfo, 0, &end) == TCL_ERROR) {
        return TCL_ERROR;
    }
    Tcl_FreeParse(&parseInfo);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(str, end-str));
    return TCL_OK;
}
