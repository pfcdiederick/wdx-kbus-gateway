// ===========================================================================
// FB_WdxKbusIo  -  cyclic KBus I/O through the wdx data layer of kbus-gateway
// Object type in CODESYS: POU, Function Block, language ST
// Uses: FB_HttpTransaction, F_AppendStr, F_FindBytes, GVL_KbusRest
// Needs kbus-gateway 1.1.0 or newer (wdx interface /wda/parameters, filter[id]); 2.0.0 has only this interface.
//
// Every exchange is the SAME fixed sequence (constant timing):
//   1. PATCH /wda/parameters/0-0-kbus-digitaloutputvalues   (if uiNumDO > 0)  -> 204
//   2. PATCH /wda/parameters/0-0-kbus-analogoutputvalues    (if uiNumAO > 0)  -> 204
//   3. GET   /wda/parameters?filter[id]=<all value arrays>                    -> 200
//      reads all inputs and confirms the outputs, from one KBus snapshot
// The next step starts in the same task cycle as the previous one finished.
// Every PATCH carries the complete output array, so outputs are restored
// automatically after a gateway restart.
//
// Set uiNumDI/DO/AI/AO to the channel counts of the gateway, e.g. from
//   GET /wda/parameters/0-0-kbus-digitaloutputs   (instantiations [1..8])
// ===========================================================================
FUNCTION_BLOCK FB_WdxKbusIo
VAR_INPUT
    xEnable       : BOOL;
    sHost         : STRING(15) := GVL_KbusRest.gc_sGatewayHost;
    uiPort        : UINT       := GVL_KbusRest.gc_uiGatewayPort;
    uiNumDI       : UINT;
    uiNumDO       : UINT;
    uiNumAI       : UINT;
    uiNumAO       : UINT;
    tInterval     : TIME := T#0MS;                       // 0 = exchange back to back
    tTimeout      : TIME := T#500MS;                     // per request
    axDOSet       : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;  // desired DO
    auiAOSet      : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;  // desired AO (raw)
END_VAR
VAR_OUTPUT
    axDI          : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;
    auiAI         : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;
    axDO          : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;  // confirmed by the gateway
    auiAO         : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;  // confirmed by the gateway
    xOnline       : BOOL;                                // FALSE after 3 failed exchanges in a row
    udiExchanges  : UDINT;
    udiErrors     : UDINT;
    tLastExchange : TIME;                                // duration of the last complete exchange
    iLastStatus   : INT;                                 // HTTP status of the last request
    sLastError    : STRING(80);
END_VAR
VAR
    fbHttp        : FB_HttpTransaction;
    fbInterval    : TON;
    abTx          : ARRAY[0..1535] OF BYTE;
    udiTx         : UDINT;
    iState        : INT;                                 // 0 wait, 1 PATCH DO, 2 PATCH AO, 3 GET
    iPass         : INT;
    iExpected     : INT;
    uiErrStreak   : UINT;
    tStart        : TIME;
    xFirstId      : BOOL;
    i             : UINT;
    // parser
    p             : POINTER TO BYTE;
    udiN          : UDINT;
    diPos         : DINT;
    udiPos        : UDINT;
    uiIdx         : UINT;
    udiVal        : UDINT;
    xDigits       : BOOL;
END_VAR

// --- implementation --------------------------------------------------------
IF NOT xEnable THEN
    fbHttp(xExecute := FALSE);
    xOnline := FALSE;
    iState := 0;
    RETURN;
END_IF

uiNumDI := MIN(uiNumDI, GVL_KbusRest.gc_uiMaxDigital);
uiNumDO := MIN(uiNumDO, GVL_KbusRest.gc_uiMaxDigital);
uiNumAI := MIN(uiNumAI, GVL_KbusRest.gc_uiMaxAnalog);
uiNumAO := MIN(uiNumAO, GVL_KbusRest.gc_uiMaxAnalog);

// ---------------------------------------------------------------- start
IF iState = 0 THEN
    fbInterval(IN := TRUE, PT := tInterval);
    IF fbInterval.Q THEN
        fbInterval(IN := FALSE);
        tStart := TIME();
        IF uiNumDO > 0 THEN iState := 1; ELSIF uiNumAO > 0 THEN iState := 2; ELSE iState := 3; END_IF
        iExpected := -1;                                  // build the request below
    END_IF
END_IF

// up to 3 requests per task cycle: a step starts as soon as the previous one finished
FOR iPass := 1 TO 3 DO
IF iState = 0 THEN
    EXIT;
END_IF

// ---------------------------------------------------------------- build request
IF iExpected = -1 THEN
    udiTx := 0;
    CASE iState OF
    1:  // PATCH all DO: {"data":{"type":"parameters","id":"...","attributes":{"value":[true,false,...]}}}
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'PATCH /wda/parameters/0-0-kbus-digitaloutputvalues HTTP/1.0$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Content-Type: application/vnd.api+json$R$N$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), '{"data":{"type":"parameters","id":"0-0-kbus-digitaloutputvalues",', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), '"attributes":{"value":[', udiTx);
        FOR i := 1 TO uiNumDO DO
            IF i > 1 THEN F_AppendStr(ADR(abTx), SIZEOF(abTx), ',', udiTx); END_IF
            IF axDOSet[i] THEN F_AppendStr(ADR(abTx), SIZEOF(abTx), 'true', udiTx);
            ELSE               F_AppendStr(ADR(abTx), SIZEOF(abTx), 'false', udiTx);
            END_IF
        END_FOR
        F_AppendStr(ADR(abTx), SIZEOF(abTx), ']}}}', udiTx);
        iExpected := 204;
    2:  // PATCH all AO
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'PATCH /wda/parameters/0-0-kbus-analogoutputvalues HTTP/1.0$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Content-Type: application/vnd.api+json$R$N$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), '{"data":{"type":"parameters","id":"0-0-kbus-analogoutputvalues",', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), '"attributes":{"value":[', udiTx);
        FOR i := 1 TO uiNumAO DO
            IF i > 1 THEN F_AppendStr(ADR(abTx), SIZEOF(abTx), ',', udiTx); END_IF
            F_AppendStr(ADR(abTx), SIZEOF(abTx), UINT_TO_STRING(auiAOSet[i]), udiTx);
        END_FOR
        F_AppendStr(ADR(abTx), SIZEOF(abTx), ']}}}', udiTx);
        iExpected := 204;
    3:  // GET all value arrays in ONE request
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'GET /wda/parameters?filter[id]=', udiTx);
        xFirstId := TRUE;
        IF uiNumDI > 0 THEN
            F_AppendStr(ADR(abTx), SIZEOF(abTx), '0-0-kbus-digitalinputvalues', udiTx); xFirstId := FALSE;
        END_IF
        IF uiNumDO > 0 THEN
            IF NOT xFirstId THEN F_AppendStr(ADR(abTx), SIZEOF(abTx), ',', udiTx); END_IF
            F_AppendStr(ADR(abTx), SIZEOF(abTx), '0-0-kbus-digitaloutputvalues', udiTx); xFirstId := FALSE;
        END_IF
        IF uiNumAI > 0 THEN
            IF NOT xFirstId THEN F_AppendStr(ADR(abTx), SIZEOF(abTx), ',', udiTx); END_IF
            F_AppendStr(ADR(abTx), SIZEOF(abTx), '0-0-kbus-analoginputvalues', udiTx); xFirstId := FALSE;
        END_IF
        IF uiNumAO > 0 THEN
            IF NOT xFirstId THEN F_AppendStr(ADR(abTx), SIZEOF(abTx), ',', udiTx); END_IF
            F_AppendStr(ADR(abTx), SIZEOF(abTx), '0-0-kbus-analogoutputvalues', udiTx);
        END_IF
        F_AppendStr(ADR(abTx), SIZEOF(abTx), ' HTTP/1.0$R$N$R$N', udiTx);
        iExpected := 200;
    END_CASE
    fbHttp.pRequest := ADR(abTx);
    fbHttp.udiRequestLen := udiTx;
    fbHttp(xExecute := FALSE);                            // FALSE -> TRUE in the same cycle:
END_IF                                                    // the request starts at once

// ---------------------------------------------------------------- run request
IF iState > 0 THEN
    fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort, tTimeout := tTimeout);
    IF fbHttp.xDone OR fbHttp.xError THEN
        iLastStatus := fbHttp.iStatus;
        IF fbHttp.xDone AND (fbHttp.iStatus = iExpected) THEN
            IF iState = 3 THEN
                // ---- parse the GET answer: per value array find its id, then "value":[...]
                p := fbHttp.pBody;
                udiN := fbHttp.udiBodyLen;
                // digital arrays: [true,false,...]
                FOR i := 1 TO 2 DO
                    IF i = 1 THEN diPos := F_FindBytes(p, udiN, 0, '"id":"0-0-kbus-digitalinputvalues"');
                    ELSE          diPos := F_FindBytes(p, udiN, 0, '"id":"0-0-kbus-digitaloutputvalues"');
                    END_IF
                    IF diPos >= 0 THEN
                        diPos := F_FindBytes(p, udiN, TO_UDINT(diPos), '"value":[');
                    END_IF
                    IF diPos >= 0 THEN
                        udiPos := TO_UDINT(diPos) + 9;
                        uiIdx := 1;
                        WHILE (udiPos < udiN) AND (p[udiPos] <> 93) DO          // up to ']'
                            IF (p[udiPos] = 116) OR (p[udiPos] = 102) THEN      // 't' / 'f'
                                IF uiIdx <= GVL_KbusRest.gc_uiMaxDigital THEN
                                    IF i = 1 THEN axDI[uiIdx] := (p[udiPos] = 116);
                                    ELSE          axDO[uiIdx] := (p[udiPos] = 116);
                                    END_IF
                                END_IF
                                uiIdx := uiIdx + 1;
                                IF p[udiPos] = 116 THEN udiPos := udiPos + 4; ELSE udiPos := udiPos + 5; END_IF
                            ELSE
                                udiPos := udiPos + 1;                           // ',' or space
                            END_IF
                        END_WHILE
                    END_IF
                END_FOR
                // analog arrays: [0,16384,...]
                FOR i := 1 TO 2 DO
                    IF i = 1 THEN diPos := F_FindBytes(p, udiN, 0, '"id":"0-0-kbus-analoginputvalues"');
                    ELSE          diPos := F_FindBytes(p, udiN, 0, '"id":"0-0-kbus-analogoutputvalues"');
                    END_IF
                    IF diPos >= 0 THEN
                        diPos := F_FindBytes(p, udiN, TO_UDINT(diPos), '"value":[');
                    END_IF
                    IF diPos >= 0 THEN
                        udiPos := TO_UDINT(diPos) + 9;
                        uiIdx := 1;
                        udiVal := 0;
                        xDigits := FALSE;
                        WHILE udiPos < udiN DO
                            IF (p[udiPos] >= 48) AND (p[udiPos] <= 57) THEN     // '0'..'9'
                                udiVal := udiVal * 10 + (p[udiPos] - 48);
                                xDigits := TRUE;
                            ELSIF (p[udiPos] = 44) OR (p[udiPos] = 93) THEN     // ',' or ']'
                                IF xDigits AND (uiIdx <= GVL_KbusRest.gc_uiMaxAnalog) THEN
                                    IF i = 1 THEN auiAI[uiIdx] := UDINT_TO_UINT(udiVal);
                                    ELSE          auiAO[uiIdx] := UDINT_TO_UINT(udiVal);
                                    END_IF
                                END_IF
                                uiIdx := uiIdx + 1;
                                udiVal := 0;
                                xDigits := FALSE;
                                IF p[udiPos] = 93 THEN EXIT; END_IF
                            END_IF
                            udiPos := udiPos + 1;
                        END_WHILE
                    END_IF
                END_FOR
                // exchange complete
                udiExchanges := udiExchanges + 1;
                tLastExchange := TIME() - tStart;
                uiErrStreak := 0;
                xOnline := TRUE;
                iState := 0;
            ELSIF (iState = 1) AND (uiNumAO > 0) THEN
                iState := 2;
            ELSE
                iState := 3;
            END_IF
            iExpected := -1;
        ELSE
            // 400/409 = request rejected (e.g. more channels than the gateway has),
            // 403 = writes disabled, 404 = old gateway without wdx, xError = network
            udiErrors := udiErrors + 1;
            uiErrStreak := uiErrStreak + 1;
            xOnline := uiErrStreak < 3;
            IF fbHttp.xDone THEN
                sLastError := CONCAT('HTTP ', INT_TO_STRING(fbHttp.iStatus));
            ELSE
                sLastError := fbHttp.sErrorText;
            END_IF
            iState := 0;                                  // next exchange starts from step 1
            iExpected := -1;
        END_IF
        fbHttp(xExecute := FALSE);
    ELSE
        EXIT;                                             // request still running: continue next cycle
    END_IF
END_IF
END_FOR
