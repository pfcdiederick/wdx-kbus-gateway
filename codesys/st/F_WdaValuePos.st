// ===========================================================================
// F_WdaValuePos  -  position of the value of one parameter in a WDA answer
// Object type in CODESYS: POU, Function, return type DINT, language ST
// Uses: F_FindBytes
//
// A WDA parameter resource looks like
//   {"type":"parameters","id":"...","attributes":{"path":"KBus/AnalogInputValues",
//    "dataType":"uint16","dataRank":"array","value":[0,16384],"stringValue":[...]}}
// Returns the 0-based position of the first character after "value": of the
// resource with this path, or -1 when the path is not in the answer.
// The closing quote is part of the search, so "KBus/DigitalInputs" does not
// match "KBus/DigitalInputs/1/Value".
// ===========================================================================
FUNCTION F_WdaValuePos : DINT
VAR_INPUT
    pBuf    : POINTER TO BYTE;
    udiLen  : UDINT;
    sPath   : STRING(60);           // e.g. 'KBus/DigitalInputValues'
END_VAR
VAR
    diPos   : DINT;
END_VAR

// --- implementation --------------------------------------------------------
F_WdaValuePos := -1;
diPos := F_FindBytes(pBuf, udiLen, 0, CONCAT(CONCAT('"path":"', sPath), '"'));
IF diPos >= 0 THEN
    diPos := F_FindBytes(pBuf, udiLen, TO_UDINT(diPos), '"value":');
    IF diPos >= 0 THEN
        F_WdaValuePos := diPos + 8;
    END_IF
END_IF
