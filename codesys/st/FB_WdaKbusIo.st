// ===========================================================================
// FB_WdaKbusIo  -  cyclic KBus I/O through the WDA REST-API of kbus-gateway
// Object type in CODESYS: POU, Function Block, language ST
// Uses: FB_HttpTransaction, F_AppendStr, F_FindBytes, F_WdaValuePos,
//       F_WdaCountItems, F_WdaParseBools, F_WdaParseUints, F_WdaParseScalar,
//       GVL_KbusRest
// Needs kbus-gateway 2.4.0 or newer (WDA REST-API 1.11.0: filter[path], batch
// PATCH with one error object per parameter).
//
// Only standard WDA requests (manual chapters 6 and 9):
//   discover  GET   /wda/parameters?filter[path]=KBus/ConfigurationVersion,
//                   KBus/DigitalInputs,KBus/DigitalOutputs,KBus/AnalogInputs,
//                   KBus/AnalogOutputs                                     -> 200
//             the channel counts = number of items of the instantiations
//   write     PATCH /wda/parameters  {"data":[DO array, AO array]}         -> 204
//             only when an output changed, after an error, or every tRefresh
//   read      GET   /wda/parameters?filter[path]=KBus/Healthy,
//                   KBus/ConfigurationVersion,KBus/<class>Values (4x)      -> 200
//             all inputs and the confirmed outputs from ONE KBus snapshot
// A new KBus/ConfigurationVersion (modules added or removed, rescan) starts
// a new discovery automatically.
// Local control (KBus/LocalControl, switched in the dashboard after its login):
// the FB pauses its writes (xLocalControl = TRUE), keeps reading, and sends all
// outputs again as soon as local control is released.
//
// Timing: an exchange starts every tInterval (start to start, default 5 ms);
// write and read run in the same task cycle when possible. Use a cyclic task
// of 1 ms (2 ms at most) for tInterval = 5 ms. tLastExchange / tMaxExchange
// show what the exchange really needs.
//
// Usage:
//   fbIo(xEnable := TRUE);              axDOSet / auiAOSet in, axDI / auiAI out
//   fbIo.uiNumDI .. uiNumAO             channel counts read from the gateway
// ===========================================================================
FUNCTION_BLOCK FB_WdaKbusIo
VAR_INPUT
    xEnable       : BOOL;
    sHost         : STRING(15) := GVL_KbusRest.gc_sGatewayHost;
    uiPort        : UINT       := GVL_KbusRest.gc_uiGatewayPort;
    tInterval     : TIME := T#5MS;                       // start to start of two exchanges
    tRefresh      : TIME := T#1S;                        // outputs are sent at least this often
    tTimeout      : TIME := T#200MS;                     // per request
    axDOSet       : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;  // desired DO
    auiAOSet      : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;  // desired AO (raw)
END_VAR
VAR_OUTPUT
    axDI          : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;
    auiAI         : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;
    axDO          : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;  // confirmed by the gateway
    auiAO         : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;  // confirmed by the gateway
    uiNumDI       : UINT;                                // channel counts of the gateway
    uiNumDO       : UINT;                                //   (limited to the array sizes)
    uiNumAI       : UINT;
    uiNumAO       : UINT;
    xReady        : BOOL;                                // channel counts known, values valid
    xHealthy      : BOOL;                                // KBus/Healthy
    xLocalControl : BOOL;                                // KBus/LocalControl: the dashboard controls the KBUS,
                                                         //   axDOSet / auiAOSet are not written meanwhile
    xOnline       : BOOL;                                // FALSE after 3 failed requests in a row
    udiConfigVersion : UDINT;                            // KBus/ConfigurationVersion
    udiExchanges  : UDINT;
    udiWrites     : UDINT;
    udiErrors     : UDINT;
    tLastExchange : TIME;                                // duration of the last exchange
    tMaxExchange  : TIME;                                // longest exchange (xEnable FALSE resets)
    iLastStatus   : INT;                                 // HTTP status of the last request
    iLastWdaCode  : INT;                                 // WDA status code of the last error (0 = none)
    sLastError    : STRING(80);
END_VAR
VAR
    fbHttp        : FB_HttpTransaction;
    abTx          : ARRAY[0..2047] OF BYTE;
    abBody        : ARRAY[0..1279] OF BYTE;
    udiTx         : UDINT;
    udiBody       : UDINT;
    iState        : INT;                                 // 0 wait, 1 discover, 2 write, 3 read
    iExpected     : INT;                                 // -1 = request still to build
    iPass         : INT;
    xWrite        : BOOL;
    xForceWrite   : BOOL := TRUE;
    axDOSent      : ARRAY[1..GVL_KbusRest.gc_uiMaxDigital] OF BOOL;
    auiAOSent     : ARRAY[1..GVL_KbusRest.gc_uiMaxAnalog]  OF UINT;
    tStart        : TIME;
    tLastWrite    : TIME;
    uiErrStreak   : UINT;
    i             : UINT;
    j             : UDINT;
    p             : POINTER TO BYTE;
    udiN          : UDINT;
    diPos         : DINT;
    iCount        : INT;
    liVal         : LINT;
    pErr          : POINTER TO BYTE;
END_VAR

// --- implementation --------------------------------------------------------
IF NOT xEnable THEN
    fbHttp(xExecute := FALSE);
    xOnline := FALSE;
    xReady := FALSE;
    xForceWrite := TRUE;
    tMaxExchange := T#0MS;
    iState := 0;
    RETURN;
END_IF

// ---------------------------------------------------------------- start
IF iState = 0 THEN
    IF (uiErrStreak > 0) AND (TIME() - tStart < MAX(tInterval, T#100MS)) THEN
        ;                                                 // after an error: at most 10 tries per second
    ELSIF NOT xReady THEN
        tStart := TIME();
        iState := 1;
    ELSIF TIME() - tStart >= tInterval THEN
        tStart := TIME();
        xWrite := xForceWrite OR (TIME() - tLastWrite >= tRefresh);
        FOR i := 1 TO uiNumDO DO
            xWrite := xWrite OR (axDOSet[i] <> axDOSent[i]);
        END_FOR
        FOR i := 1 TO uiNumAO DO
            xWrite := xWrite OR (auiAOSet[i] <> auiAOSent[i]);
        END_FOR
        IF xWrite AND (uiNumDO + uiNumAO > 0) AND NOT xLocalControl THEN iState := 2; ELSE iState := 3; END_IF
    END_IF
    iExpected := -1;
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
    1:  // discover: channel counts and configuration version
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'GET /wda/parameters?filter[path]=KBus/ConfigurationVersion,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'KBus/DigitalInputs,KBus/DigitalOutputs,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'KBus/AnalogInputs,KBus/AnalogOutputs HTTP/1.0$R$N$R$N', udiTx);
        iExpected := 200;
    2:  // write: one batch PATCH with the DO and the AO array (all or nothing)
        udiBody := 0;
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"data":[', udiBody);
        IF uiNumDO > 0 THEN
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"type":"parameters","id":"0-0-kbus-digitaloutputvalues",', udiBody);
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '"attributes":{"value":[', udiBody);
            FOR i := 1 TO uiNumDO DO
                IF i > 1 THEN F_AppendStr(ADR(abBody), SIZEOF(abBody), ',', udiBody); END_IF
                IF axDOSet[i] THEN F_AppendStr(ADR(abBody), SIZEOF(abBody), 'true', udiBody);
                ELSE               F_AppendStr(ADR(abBody), SIZEOF(abBody), 'false', udiBody);
                END_IF
                axDOSent[i] := axDOSet[i];
            END_FOR
            F_AppendStr(ADR(abBody), SIZEOF(abBody), ']}}', udiBody);
        END_IF
        IF uiNumAO > 0 THEN
            IF uiNumDO > 0 THEN F_AppendStr(ADR(abBody), SIZEOF(abBody), ',', udiBody); END_IF
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"type":"parameters","id":"0-0-kbus-analogoutputvalues",', udiBody);
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '"attributes":{"value":[', udiBody);
            FOR i := 1 TO uiNumAO DO
                IF i > 1 THEN F_AppendStr(ADR(abBody), SIZEOF(abBody), ',', udiBody); END_IF
                F_AppendStr(ADR(abBody), SIZEOF(abBody), UINT_TO_STRING(auiAOSet[i]), udiBody);
                auiAOSent[i] := auiAOSet[i];
            END_FOR
            F_AppendStr(ADR(abBody), SIZEOF(abBody), ']}}', udiBody);
        END_IF
        F_AppendStr(ADR(abBody), SIZEOF(abBody), ']}', udiBody);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'PATCH /wda/parameters HTTP/1.0$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Content-Type: application/vnd.api+json$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), CONCAT(CONCAT('Content-Length: ', UDINT_TO_STRING(udiBody)), '$R$N$R$N'), udiTx);
        j := 0;
        WHILE (j < udiBody) AND (udiTx < SIZEOF(abTx)) DO  // header and body in ONE TCP write
            abTx[udiTx] := abBody[j];
            udiTx := udiTx + 1;
            j := j + 1;
        END_WHILE
        iExpected := 204;
    3:  // read: all value arrays in ONE request (one KBus snapshot)
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'GET /wda/parameters?filter[path]=KBus/Healthy,KBus/ConfigurationVersion,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'KBus/LocalControl,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'KBus/DigitalInputValues,KBus/DigitalOutputValues,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'KBus/AnalogInputValues,KBus/AnalogOutputValues HTTP/1.0$R$N$R$N', udiTx);
        iExpected := 200;
    END_CASE
    fbHttp.pRequest := ADR(abTx);
    fbHttp.udiRequestLen := udiTx;
    fbHttp(xExecute := FALSE);                            // FALSE -> TRUE in the same cycle:
END_IF                                                    // the request starts at once

// ---------------------------------------------------------------- run request
fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort, tTimeout := tTimeout);
IF NOT (fbHttp.xDone OR fbHttp.xError) THEN
    EXIT;                                                 // still running: continue next cycle
END_IF
iLastStatus := fbHttp.iStatus;
p := fbHttp.pBody;
udiN := fbHttp.udiBodyLen;
fbHttp(xExecute := FALSE);

IF fbHttp.xDone AND (iLastStatus = iExpected) THEN
    CASE iState OF
    1:  // channel counts = items of the instantiations
        iCount := F_WdaCountItems(p, udiN, 'KBus/DigitalInputs');
        uiNumDI := MIN(TO_UINT(MAX(iCount, 0)), GVL_KbusRest.gc_uiMaxDigital);
        iCount := F_WdaCountItems(p, udiN, 'KBus/DigitalOutputs');
        uiNumDO := MIN(TO_UINT(MAX(iCount, 0)), GVL_KbusRest.gc_uiMaxDigital);
        iCount := F_WdaCountItems(p, udiN, 'KBus/AnalogInputs');
        uiNumAI := MIN(TO_UINT(MAX(iCount, 0)), GVL_KbusRest.gc_uiMaxAnalog);
        iCount := F_WdaCountItems(p, udiN, 'KBus/AnalogOutputs');
        uiNumAO := MIN(TO_UINT(MAX(iCount, 0)), GVL_KbusRest.gc_uiMaxAnalog);
        liVal := F_WdaParseScalar(p, udiN, 'KBus/ConfigurationVersion');
        udiConfigVersion := LINT_TO_UDINT(MAX(liVal, 0));
        xReady := TRUE;
        xForceWrite := TRUE;
        IF (uiNumDO + uiNumAO > 0) AND NOT xLocalControl THEN iState := 2; ELSE iState := 3; END_IF   // first exchange at once
        iExpected := -1;
    2:  // outputs written
        udiWrites := udiWrites + 1;
        tLastWrite := TIME();
        xForceWrite := FALSE;
        iState := 3;
        iExpected := -1;
    3:  // inputs and confirmed outputs
        F_WdaParseBools(p, udiN, 'KBus/DigitalInputValues',  ADR(axDI), GVL_KbusRest.gc_uiMaxDigital);
        F_WdaParseBools(p, udiN, 'KBus/DigitalOutputValues', ADR(axDO), GVL_KbusRest.gc_uiMaxDigital);
        F_WdaParseUints(p, udiN, 'KBus/AnalogInputValues',   ADR(auiAI), GVL_KbusRest.gc_uiMaxAnalog);
        F_WdaParseUints(p, udiN, 'KBus/AnalogOutputValues',  ADR(auiAO), GVL_KbusRest.gc_uiMaxAnalog);
        xHealthy := F_WdaParseScalar(p, udiN, 'KBus/Healthy') = 1;
        xLocalControl := F_WdaParseScalar(p, udiN, 'KBus/LocalControl') = 1;
        IF xLocalControl THEN
            xForceWrite := TRUE;                          // after the release: send all outputs at once
        END_IF
        liVal := F_WdaParseScalar(p, udiN, 'KBus/ConfigurationVersion');
        IF (liVal >= 0) AND (LINT_TO_UDINT(liVal) <> udiConfigVersion) THEN
            xReady := FALSE;                              // modules changed: discover again
        END_IF
        udiExchanges := udiExchanges + 1;
        tLastExchange := TIME() - tStart;
        tMaxExchange := MAX(tMaxExchange, tLastExchange);
        uiErrStreak := 0;
        xOnline := TRUE;
        iLastWdaCode := 0;
        iState := 0;
    END_CASE
ELSIF fbHttp.xDone AND (iState = 2) AND (iLastStatus = 403) AND (F_FindBytes(p, udiN, 0, 'local control') >= 0) THEN
    // local control was switched on just now: not an error, read on
    xLocalControl := TRUE;
    xForceWrite := TRUE;
    iState := 3;
    iExpected := -1;
ELSE
    // 400 = rejected (WDA code in iLastWdaCode, e.g. 39 more values than channels),
    // 403 = writes disabled (57), 404 = gateway without WDA, xError = network
    udiErrors := udiErrors + 1;
    uiErrStreak := uiErrStreak + 1;
    xOnline := uiErrStreak < 3;
    iLastWdaCode := 0;
    IF fbHttp.xDone THEN
        sLastError := CONCAT('HTTP ', INT_TO_STRING(iLastStatus));
        diPos := F_FindBytes(p, udiN, 0, '"code":"');
        IF diPos >= 0 THEN
            j := TO_UDINT(diPos) + 8;
            WHILE (j < udiN) AND (p[j] >= 48) AND (p[j] <= 57) DO
                iLastWdaCode := iLastWdaCode * 10 + (p[j] - 48);
                j := j + 1;
            END_WHILE
        END_IF
        diPos := F_FindBytes(p, udiN, 0, '"detail":"');   // first error: copy its detail
        IF diPos >= 0 THEN
            pErr := ADR(sLastError);
            j := 0;
            WHILE (TO_UDINT(diPos) + 10 + j < udiN) AND (j < 79) AND (p[TO_UDINT(diPos) + 10 + j] <> 34) DO
                pErr[j] := p[TO_UDINT(diPos) + 10 + j];
                j := j + 1;
            END_WHILE
            pErr[j] := 0;
        END_IF
    ELSE
        sLastError := fbHttp.sErrorText;
    END_IF
    xReady := FALSE;                                      // discover again (gateway restart, rescan)
    xForceWrite := TRUE;                                  // and send the outputs again
    iState := 0;
END_IF
END_FOR
