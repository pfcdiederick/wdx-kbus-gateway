// ===========================================================================
// F_WdaParseUints  -  uint16 array value of one parameter -> ARRAY OF UINT
// Object type in CODESYS: POU, Function, return type INT, language ST
// Uses: F_WdaValuePos, F_FindBytes
//
// "value":[0,16384,32767] -> pDst[0..2]. At most uiMax items are stored.
// Returns the number of items in the answer, or -1 when the path is missing.
// Call: F_WdaParseUints(p, n, 'KBus/AnalogInputValues', ADR(auiAI), SIZEOF(auiAI) / 2)
// ===========================================================================
FUNCTION F_WdaParseUints : INT
VAR_INPUT
    pBuf    : POINTER TO BYTE;
    udiLen  : UDINT;
    sPath   : STRING(60);
    pDst    : POINTER TO UINT;      // first element of the destination array
    uiMax   : UINT;                 // number of elements of the destination array
END_VAR
VAR
    diPos   : DINT;
    i       : UDINT;
    uiIdx   : UINT;
    udiVal  : UDINT;
    xDigits : BOOL;
END_VAR

// --- implementation --------------------------------------------------------
F_WdaParseUints := -1;
diPos := F_WdaValuePos(pBuf, udiLen, sPath);
IF diPos < 0 THEN
    RETURN;
END_IF
i := TO_UDINT(diPos);
WHILE (i < udiLen) AND (pBuf[i] <> 91) DO              // up to '['
    i := i + 1;
END_WHILE
i := i + 1;
WHILE i < udiLen DO
    IF (pBuf[i] >= 48) AND (pBuf[i] <= 57) THEN        // '0'..'9'
        udiVal := udiVal * 10 + (pBuf[i] - 48);
        xDigits := TRUE;
    ELSIF (pBuf[i] = 44) OR (pBuf[i] = 93) THEN        // ',' or ']'
        IF xDigits THEN
            IF uiIdx < uiMax THEN
                pDst[uiIdx] := UDINT_TO_UINT(udiVal);
            END_IF
            uiIdx := uiIdx + 1;
        END_IF
        udiVal := 0;
        xDigits := FALSE;
        IF pBuf[i] = 93 THEN
            EXIT;
        END_IF
    END_IF
    i := i + 1;
END_WHILE
F_WdaParseUints := TO_INT(uiIdx);
