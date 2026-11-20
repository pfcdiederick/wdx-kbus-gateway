// ===========================================================================
// FB_WdxReadString  -  read a STRING register of the kbus-gateway (cyclic)
// Object type in CODESYS: POU, Function Block, language ST
// Uses: FB_HttpTransaction, F_AppendStr, F_FindBytes, GVL_KbusRest
// Needs kbus-gateway 2.2.0 or newer (registers).
//
// Counterpart of FB_WdxWriteString: a container (Python) writes a text into a
// register, CODESYS reads it here, e.g. the answer of the example client:
//     Python/Response   string   ""        (/etc/kbus-gateway-registers.conf)
//     wdx id 0-0-registers-python-response-value
//
// Every tInterval: GET /wda/parameters/<sId> -> 200, the "value" of the answer
// is decoded into sValue:
//   - JSON escapes \" \\ \/ \b \f \n \r \t \u00XX
//   - UTF-8 -> Latin-1 (CODESYS STRING): e.g. C3 A9 -> 16#E9 'é'; characters
//     outside Latin-1 (e.g. the euro sign) become '?'
//
// Outputs
//   sValue    the text (at most 127 characters)
//   xNew      TRUE for ONE call when sValue changed (also after the first read)
//   xValid    the last read succeeded
//   iStatus   200 = ok; 404 = register not declared; 0 = no answer
// ===========================================================================
FUNCTION_BLOCK FB_WdxReadString
VAR_INPUT
    xEnable       : BOOL := TRUE;
    sId           : STRING(80) := '0-0-registers-python-response-value';
    tInterval     : TIME := T#200MS;
    sHost         : STRING(15) := GVL_KbusRest.gc_sGatewayHost;
    uiPort        : UINT       := GVL_KbusRest.gc_uiGatewayPort;
    tTimeout      : TIME := T#500MS;
END_VAR
VAR_OUTPUT
    sValue        : STRING(127);
    xNew          : BOOL;
    xValid        : BOOL;
    iStatus       : INT;
    sError        : STRING(80);
    udiReads      : UDINT;
    udiErrors     : UDINT;
END_VAR
VAR
    fbHttp        : FB_HttpTransaction;
    fbInterval    : TON;
    iState        : INT;                                 // 0 wait, 1 request running
    abTx          : ARRAY[0..255] OF BYTE;
    udiTx         : UDINT;
    sNew          : STRING(127);
    pB            : POINTER TO BYTE;
    pOut          : POINTER TO BYTE;
    diPos         : DINT;
    i             : UDINT;
    o             : UDINT;
    b             : BYTE;
    udiCp         : UDINT;
    k             : INT;
    h             : BYTE;
    xEnd          : BOOL;
    xFirst        : BOOL := TRUE;
END_VAR

// --- implementation --------------------------------------------------------
xNew := FALSE;
IF NOT xEnable THEN
    fbHttp(xExecute := FALSE);
    fbInterval(IN := FALSE);
    iState := 0;
    xValid := FALSE;
    RETURN;
END_IF

// ---------------------------------------------------------------- start a read
IF iState = 0 THEN
    fbInterval(IN := TRUE, PT := tInterval);
    IF fbInterval.Q OR xFirst THEN
        fbInterval(IN := FALSE);
        udiTx := 0;
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'GET /wda/parameters/', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), sId, udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), ' HTTP/1.0$R$N$R$N', udiTx);
        fbHttp(xExecute := FALSE);
        fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort,
               pRequest := ADR(abTx), udiRequestLen := udiTx, tTimeout := tTimeout);
        iState := 1;
    END_IF
ELSE
    fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort,
           pRequest := ADR(abTx), udiRequestLen := udiTx, tTimeout := tTimeout);
END_IF

// ---------------------------------------------------------------- result
IF (iState = 1) AND (fbHttp.xDone OR fbHttp.xError) THEN
    iStatus := fbHttp.iStatus;
    diPos := -1;
    IF fbHttp.xDone AND (fbHttp.iStatus = 200) THEN
        diPos := F_FindBytes(fbHttp.pBody, fbHttp.udiBodyLen, 0, '"value":"');
    END_IF
    IF diPos >= 0 THEN
        // decode the JSON string up to the closing quote
        pB := fbHttp.pBody;
        pOut := ADR(sNew);
        i := TO_UDINT(diPos) + 9;
        o := 0;
        xEnd := FALSE;
        WHILE (i < fbHttp.udiBodyLen) AND NOT xEnd AND (o < 127) DO
            b := pB[i];
            i := i + 1;
            udiCp := 16#FFFF;                              // FFFF = nothing to output
            IF b = 16#22 THEN                              // closing "
                xEnd := TRUE;
            ELSIF (b = 16#5C) AND (i < fbHttp.udiBodyLen) THEN   // escape
                b := pB[i];
                i := i + 1;
                CASE b OF
                16#6E: udiCp := 10;                        // \n
                16#72: udiCp := 13;                        // \r
                16#74: udiCp := 9;                         // \t
                16#62: udiCp := 8;                         // \b
                16#66: udiCp := 12;                        // \f
                16#75:                                     // \uXXXX
                    udiCp := 0;
                    FOR k := 1 TO 4 DO
                        IF i < fbHttp.udiBodyLen THEN
                            h := pB[i];
                            i := i + 1;
                            IF (h >= 16#30) AND (h <= 16#39) THEN udiCp := udiCp * 16 + (h - 16#30);
                            ELSIF (h >= 16#61) AND (h <= 16#66) THEN udiCp := udiCp * 16 + (h - 16#57);
                            ELSIF (h >= 16#41) AND (h <= 16#46) THEN udiCp := udiCp * 16 + (h - 16#37);
                            END_IF
                        END_IF
                    END_FOR
                ELSE
                    udiCp := b;                            // \" \\ \/
                END_CASE
            ELSIF b < 16#80 THEN
                udiCp := b;
            ELSIF (b AND 16#E0) = 16#C0 THEN               // 2-byte UTF-8
                IF i < fbHttp.udiBodyLen THEN
                    udiCp := SHL(TO_UDINT(b AND 16#1F), 6) OR TO_UDINT(pB[i] AND 16#3F);
                    i := i + 1;
                END_IF
            ELSIF (b AND 16#F0) = 16#E0 THEN               // 3-byte UTF-8: not in Latin-1
                i := i + 2;
                udiCp := 16#3F;
            ELSIF (b AND 16#F8) = 16#F0 THEN               // 4-byte UTF-8: not in Latin-1
                i := i + 3;
                udiCp := 16#3F;
            END_IF
            IF udiCp <> 16#FFFF THEN
                IF udiCp > 16#FF THEN udiCp := 16#3F; END_IF        // '?'
                IF udiCp = 0 THEN udiCp := 16#3F; END_IF
                pOut[o] := TO_BYTE(udiCp);
                o := o + 1;
            END_IF
        END_WHILE
        pOut[o] := 0;
        xValid := TRUE;
        sError := '';
        udiReads := udiReads + 1;
        IF xFirst OR (sNew <> sValue) THEN
            sValue := sNew;
            xNew := TRUE;
        END_IF
        xFirst := FALSE;
    ELSE
        xValid := FALSE;
        udiErrors := udiErrors + 1;
        IF fbHttp.xError THEN
            sError := fbHttp.sErrorText;
        ELSIF fbHttp.iStatus = 200 THEN
            sError := 'answer without a string value (wrong register type?)';
        ELSE
            sError := CONCAT('HTTP ', INT_TO_STRING(fbHttp.iStatus));
            IF fbHttp.iStatus = 404 THEN
                sError := CONCAT(sError, ': register not declared');
            END_IF
        END_IF
        xFirst := FALSE;
    END_IF
    fbHttp(xExecute := FALSE);
    iState := 0;
END_IF
