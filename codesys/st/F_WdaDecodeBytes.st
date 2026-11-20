// ===========================================================================
// F_WdaDecodeBytes  -  JSON string value of one parameter -> bytes
// Object type in CODESYS: POU, Function, return type INT, language ST
// Uses: F_WdaValuePos, F_FindBytes
//
// For byte strings of the gateway (Serial/Com1/RxFrame, RxData, Format): one
// character per byte, printable ASCII as is, every other byte as \u00XX.
// "value":"AB\u000d\u000a" -> pDst[0..3] = 65, 66, 13, 10. At most uiMax bytes
// are stored. Returns the number of bytes, or -1 when the path is missing.
// ===========================================================================
FUNCTION F_WdaDecodeBytes : INT
VAR_INPUT
    pBuf    : POINTER TO BYTE;
    udiLen  : UDINT;
    sPath   : STRING(60);
    pDst    : POINTER TO BYTE;      // first element of the destination array
    uiMax   : UINT;                 // size of the destination array
END_VAR
VAR
    diPos   : DINT;
    i       : UDINT;
    j       : UDINT;
    uiN     : UINT;
    c       : BYTE;
    b       : BYTE;
    uiV     : UINT;
    h       : BYTE;
END_VAR

// --- implementation --------------------------------------------------------
F_WdaDecodeBytes := -1;
diPos := F_WdaValuePos(pBuf, udiLen, sPath);
IF diPos < 0 THEN
    RETURN;
END_IF
i := TO_UDINT(diPos);
WHILE (i < udiLen) AND (pBuf[i] <> 34) DO              // up to the opening '"'
    i := i + 1;
END_WHILE
i := i + 1;
WHILE (i < udiLen) AND (pBuf[i] <> 34) DO              // up to the closing '"'
    c := pBuf[i];
    b := c;
    i := i + 1;
    IF (c = 92) AND (i < udiLen) THEN                  // '\' escape
        c := pBuf[i];
        i := i + 1;
        CASE c OF
        110: b := 10;                                  // \n
        114: b := 13;                                  // \r
        116: b := 9;                                   // \t
        98:  b := 8;                                   // \b
        102: b := 12;                                  // \f
        117:                                           // \uXXXX (gateway: <= 00FF)
            uiV := 0;
            FOR j := 0 TO 3 DO
                IF i < udiLen THEN
                    h := pBuf[i];
                    IF (h >= 48) AND (h <= 57) THEN uiV := uiV * 16 + (h - 48);
                    ELSIF (h >= 97) AND (h <= 102) THEN uiV := uiV * 16 + (h - 87);
                    ELSIF (h >= 65) AND (h <= 70) THEN uiV := uiV * 16 + (h - 55);
                    END_IF
                    i := i + 1;
                END_IF
            END_FOR
            b := UINT_TO_BYTE(uiV);
        ELSE
            b := c;                                    // \" \\ \/
        END_CASE
    END_IF
    IF uiN < uiMax THEN
        pDst[uiN] := b;
    END_IF
    uiN := uiN + 1;
END_WHILE
F_WdaDecodeBytes := TO_INT(MIN(uiN, uiMax));
