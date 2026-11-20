// ===========================================================================
// F_WdaParseBools  -  boolean array value of one parameter -> ARRAY OF BOOL
// Object type in CODESYS: POU, Function, return type INT, language ST
// Uses: F_WdaValuePos, F_FindBytes
//
// "value":[true,false,true] -> pDst[0..2]. At most uiMax items are stored.
// Returns the number of items in the answer, or -1 when the path is missing.
// Call: F_WdaParseBools(p, n, 'KBus/DigitalInputValues', ADR(axDI), SIZEOF(axDI))
// ===========================================================================
FUNCTION F_WdaParseBools : INT
VAR_INPUT
    pBuf    : POINTER TO BYTE;
    udiLen  : UDINT;
    sPath   : STRING(60);
    pDst    : POINTER TO BOOL;      // first element of the destination array
    uiMax   : UINT;                 // number of elements of the destination array
END_VAR
VAR
    diPos   : DINT;
    i       : UDINT;
    uiIdx   : UINT;
END_VAR

// --- implementation --------------------------------------------------------
F_WdaParseBools := -1;
diPos := F_WdaValuePos(pBuf, udiLen, sPath);
IF diPos < 0 THEN
    RETURN;
END_IF
i := TO_UDINT(diPos);
WHILE (i < udiLen) AND (pBuf[i] <> 91) DO              // up to '['
    i := i + 1;
END_WHILE
i := i + 1;
WHILE (i < udiLen) AND (pBuf[i] <> 93) DO              // up to ']'
    IF (pBuf[i] = 116) OR (pBuf[i] = 102) THEN         // 't' / 'f'
        IF uiIdx < uiMax THEN
            pDst[uiIdx] := (pBuf[i] = 116);
        END_IF
        uiIdx := uiIdx + 1;
        IF pBuf[i] = 116 THEN i := i + 4; ELSE i := i + 5; END_IF
    ELSE
        i := i + 1;                                    // ',' or space
    END_IF
END_WHILE
F_WdaParseBools := TO_INT(uiIdx);
