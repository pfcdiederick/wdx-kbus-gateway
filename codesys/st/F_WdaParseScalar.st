// ===========================================================================
// F_WdaParseScalar  -  boolean or unsigned integer value of one parameter
// Object type in CODESYS: POU, Function, return type LINT, language ST
// Uses: F_WdaValuePos, F_FindBytes
//
// "value":true -> 1, "value":false -> 0, "value":123 -> 123.
// Returns -1 when the path is missing or the value is neither.
// ===========================================================================
FUNCTION F_WdaParseScalar : LINT
VAR_INPUT
    pBuf    : POINTER TO BYTE;
    udiLen  : UDINT;
    sPath   : STRING(60);
END_VAR
VAR
    diPos   : DINT;
    i       : UDINT;
    liVal   : LINT;
    xDigits : BOOL;
END_VAR

// --- implementation --------------------------------------------------------
F_WdaParseScalar := -1;
diPos := F_WdaValuePos(pBuf, udiLen, sPath);
IF diPos < 0 THEN
    RETURN;
END_IF
i := TO_UDINT(diPos);
WHILE (i < udiLen) AND (pBuf[i] = 32) DO
    i := i + 1;
END_WHILE
IF i >= udiLen THEN
    RETURN;
ELSIF pBuf[i] = 116 THEN                               // true
    F_WdaParseScalar := 1;
ELSIF pBuf[i] = 102 THEN                               // false
    F_WdaParseScalar := 0;
ELSE
    WHILE (i < udiLen) AND (pBuf[i] >= 48) AND (pBuf[i] <= 57) DO
        liVal := liVal * 10 + (pBuf[i] - 48);
        xDigits := TRUE;
        i := i + 1;
    END_WHILE
    IF xDigits THEN
        F_WdaParseScalar := liVal;
    END_IF
END_IF
