// ===========================================================================
// F_AppendStr  -  append the bytes of a STRING to a byte buffer
// Object type in CODESYS: POU, Function, return type BOOL, language ST
//
// Used to build HTTP requests longer than 255 characters (the Standard
// library string functions CONCAT/MID/FIND are limited to 255 characters).
// Returns FALSE when the buffer was full and the text was cut off.
// ===========================================================================
FUNCTION F_AppendStr : BOOL
VAR_INPUT
    pBuf    : POINTER TO BYTE;      // start of the buffer
    udiCap  : UDINT;                // size of the buffer in bytes
    sText   : STRING(80);           // text to append
END_VAR
VAR_IN_OUT
    udiLen  : UDINT;                // bytes used in the buffer (updated)
END_VAR
VAR
    pS      : POINTER TO BYTE;
    i       : UDINT;
END_VAR

// --- implementation --------------------------------------------------------
pS := ADR(sText);
i := 0;
WHILE (pS[i] <> 0) AND (udiLen < udiCap) DO
    pBuf[udiLen] := pS[i];
    udiLen := udiLen + 1;
    i := i + 1;
END_WHILE
F_AppendStr := (pS[i] = 0);
