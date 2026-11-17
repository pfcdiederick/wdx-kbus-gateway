// ===========================================================================
// F_FindBytes  -  find a text in a byte buffer
// Object type in CODESYS: POU, Function, return type DINT, language ST
//
// Returns the 0-based position of sPattern in pBuf[udiFrom..udiLen-1], or -1.
// Works on buffers longer than 255 characters (Standard FIND does not).
// ===========================================================================
FUNCTION F_FindBytes : DINT
VAR_INPUT
    pBuf     : POINTER TO BYTE;
    udiLen   : UDINT;               // bytes in the buffer
    udiFrom  : UDINT;               // start position (0-based)
    sPattern : STRING(80);
END_VAR
VAR
    pP       : POINTER TO BYTE;
    udiPLen  : UDINT;
    i        : UDINT;
    j        : UDINT;
    xMatch   : BOOL;
END_VAR

// --- implementation --------------------------------------------------------
F_FindBytes := -1;
udiPLen := TO_UDINT(LEN(sPattern));
pP := ADR(sPattern);
IF (udiPLen = 0) OR (udiLen < udiPLen) THEN
    RETURN;
END_IF
i := udiFrom;
WHILE i + udiPLen <= udiLen DO
    xMatch := TRUE;
    j := 0;
    WHILE xMatch AND (j < udiPLen) DO
        IF pBuf[i + j] <> pP[j] THEN
            xMatch := FALSE;
        END_IF
        j := j + 1;
    END_WHILE
    IF xMatch THEN
        F_FindBytes := TO_DINT(i);
        RETURN;
    END_IF
    i := i + 1;
END_WHILE
