// ===========================================================================
// F_WdaCountItems  -  number of items in the array value of one parameter
// Object type in CODESYS: POU, Function, return type INT, language ST
// Uses: F_WdaValuePos, F_FindBytes
//
// For parameters of data type instantiations (KBus/DigitalInputs, ...): the
// WDA manual says only the NUMBER of items counts, their content is internal.
// Items are counted at the top level, so nested objects or strings are fine.
// Returns -1 when the path is not in the answer or the value is no array.
// ===========================================================================
FUNCTION F_WdaCountItems : INT
VAR_INPUT
    pBuf    : POINTER TO BYTE;
    udiLen  : UDINT;
    sPath   : STRING(60);
END_VAR
VAR
    diPos   : DINT;
    i       : UDINT;
    iDepth  : INT;
    iCommas : INT;
    xAny    : BOOL;
    c       : BYTE;
END_VAR

// --- implementation --------------------------------------------------------
F_WdaCountItems := -1;
diPos := F_WdaValuePos(pBuf, udiLen, sPath);
IF diPos < 0 THEN
    RETURN;
END_IF
i := TO_UDINT(diPos);
WHILE (i < udiLen) AND (pBuf[i] = 32) DO               // spaces
    i := i + 1;
END_WHILE
IF (i >= udiLen) OR (pBuf[i] <> 91) THEN               // '['
    RETURN;
END_IF
i := i + 1;
WHILE i < udiLen DO
    c := pBuf[i];
    IF c = 34 THEN                                     // '"': skip the string
        xAny := TRUE;
        i := i + 1;
        WHILE (i < udiLen) AND (pBuf[i] <> 34) DO
            IF pBuf[i] = 92 THEN i := i + 1; END_IF     // '\' escapes the next byte
            i := i + 1;
        END_WHILE
    ELSIF (c = 91) OR (c = 123) THEN                   // '[' '{'
        xAny := TRUE;
        iDepth := iDepth + 1;
    ELSIF (c = 93) OR (c = 125) THEN                   // ']' '}'
        IF iDepth = 0 THEN
            EXIT;
        END_IF
        iDepth := iDepth - 1;
    ELSIF (c = 44) AND (iDepth = 0) THEN               // ','
        iCommas := iCommas + 1;
    ELSIF c > 32 THEN
        xAny := TRUE;
    END_IF
    i := i + 1;
END_WHILE
IF xAny THEN
    F_WdaCountItems := iCommas + 1;
ELSE
    F_WdaCountItems := 0;
END_IF
