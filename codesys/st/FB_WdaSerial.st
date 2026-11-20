// ===========================================================================
// FB_WdaSerial  -  COM1 of the controller through the WDA REST-API of kbus-gateway
// Object type in CODESYS: POU, Function Block, language ST
// Uses: FB_HttpTransaction, F_AppendStr, F_FindBytes, F_WdaValuePos,
//       F_WdaParseScalar, F_WdaDecodeBytes, GVL_KbusRest
// Needs kbus-gateway 2.5.0 or newer (Serial/Com1/...).
//
// The gateway forwards COM1 transparently; the protocol is made here.
// Settings (baud rate, format, frame gap): udiSetBaudrate / sSetFormat /
// uiSetFrameGap (0 / '' = keep the value stored in the gateway). The FB writes
// them only when they differ from the current ones: after the start and
// whenever the inputs change. The current settings are always in udiBaudrate /
// sFormat / uiFrameGap; the KBUS dashboard (Settings > COM1) shows them.
//
//   settings  PATCH /wda/parameters  {"data":[baudrate, format, framegap]} -> 204
//   send      PATCH /wda/parameters/0-0-serial-com1-transmit       -> 204
//             rising edge on xSend: abTxData[0..uiTxLen-1], or sTxData when uiTxLen = 0
//   receive   GET   /wda/parameters?filter[path]=Serial/Com1/...    -> 200, every tPoll
//             a new frame (RxFrameCount changed): xNewFrame for one call,
//             abFrame[0..uiFrameLen-1] / sFrame
// A frame is what arrived without a pause of FrameGap ms in between.
// ===========================================================================
FUNCTION_BLOCK FB_WdaSerial
VAR_INPUT
    xEnable       : BOOL;
    sHost         : STRING(15) := GVL_KbusRest.gc_sGatewayHost;
    uiPort        : UINT       := GVL_KbusRest.gc_uiGatewayPort;
    tPoll         : TIME := T#20MS;                      // receive: one GET per tPoll
    tTimeout      : TIME := T#500MS;                     // per request
    xSend         : BOOL;                                // rising edge: send
    sTxData       : STRING(255);                         // text to send (when uiTxLen = 0)
    abTxData      : ARRAY[0..255] OF BYTE;               // bytes to send (binary)
    uiTxLen       : UINT;                                // 0 = send sTxData
    udiSetBaudrate : UDINT;                              // 1200..230400; 0 = keep the current one
    sSetFormat    : STRING(3);                           // e.g. '8E1' (data bits, parity N/E/O, stop bits); '' = keep
    uiSetFrameGap : UINT;                                // 1..1000 ms; 0 = keep
END_VAR
VAR_OUTPUT
    xOpen         : BOOL;                                // Serial/Com1/Open: port open in the gateway
    udiBaudrate   : UDINT;                               // current settings (read from the gateway)
    sFormat       : STRING(3);                           //   e.g. '8E1'
    uiFrameGap    : UINT;                                //   ms
    xSettingsApplied : BOOL;                             // the current settings = the udiSet* / sSet* inputs
    xNewFrame     : BOOL;                                // TRUE for one call: new frame in abFrame / sFrame
    abFrame       : ARRAY[0..511] OF BYTE;
    uiFrameLen    : UINT;
    sFrame        : STRING(255);                         // the frame as text (bytes 0 end it)
    udiFrames     : UDINT;                               // Serial/Com1/RxFrameCount
    udiRxBytes    : UDINT;                               // Serial/Com1/RxCount
    udiTxBytes    : UDINT;                               // Serial/Com1/TxCount
    udiLineErrors : UDINT;                               // framing / parity / overrun errors
    xSendBusy     : BOOL;
    xSendDone     : BOOL;                                // TRUE until the next send: written to COM1
    xOnline       : BOOL;                                // FALSE after 3 failed requests in a row
    iLastStatus   : INT;
    iLastWdaCode  : INT;
    sLastError    : STRING(80);
END_VAR
VAR
    fbHttp        : FB_HttpTransaction;
    rtSend        : R_TRIG;
    xSendPending  : BOOL;
    abTx          : ARRAY[0..2559] OF BYTE;
    abBody        : ARRAY[0..1791] OF BYTE;
    udiTx         : UDINT;
    udiBody       : UDINT;
    iState        : INT;                                 // 0 wait, 1 send, 2 poll, 3 settings
    xKnown        : BOOL;                                // settings read at least once
    udiTriedBaud  : UDINT;                               // last settings written (or rejected):
    sTriedFormat  : STRING(3);                           //   not written again until the inputs change
    uiTriedGap    : UINT;
    xBaudDiff     : BOOL;
    xFormatDiff   : BOOL;
    xGapDiff      : BOOL;
    xNeedComma    : BOOL;
    tLastPoll     : TIME;
    xFirst        : BOOL := TRUE;
    uiErrStreak   : UINT;
    i             : UINT;
    j             : UDINT;
    b             : BYTE;
    uiLen         : UINT;
    pS            : POINTER TO BYTE;
    p             : POINTER TO BYTE;
    udiN          : UDINT;
    diPos         : DINT;
    liVal         : LINT;
    iN            : INT;
    pErr          : POINTER TO BYTE;
    sHex          : STRING(16) := '0123456789abcdef';
    pHex          : POINTER TO BYTE;
END_VAR

// --- implementation --------------------------------------------------------
xNewFrame := FALSE;
rtSend(CLK := xSend);
IF rtSend.Q THEN
    xSendPending := TRUE;
    xSendDone := FALSE;
END_IF
IF NOT xEnable THEN
    fbHttp(xExecute := FALSE);
    xOnline := FALSE;
    xFirst := TRUE;
    xKnown := FALSE;
    udiTriedBaud := 0; sTriedFormat := ''; uiTriedGap := 0;
    iState := 0;
    RETURN;
END_IF

// settings: which of the requested ones differ from the current ones (and were not tried yet)
xBaudDiff   := xKnown AND (udiSetBaudrate <> 0) AND (udiSetBaudrate <> udiBaudrate) AND (udiSetBaudrate <> udiTriedBaud);
xFormatDiff := xKnown AND (LEN(sSetFormat) = 3) AND (sSetFormat <> sFormat) AND (sSetFormat <> sTriedFormat);
xGapDiff    := xKnown AND (uiSetFrameGap <> 0) AND (uiSetFrameGap <> uiFrameGap) AND (uiSetFrameGap <> uiTriedGap);
xSettingsApplied := xKnown AND ((udiSetBaudrate = 0) OR (udiSetBaudrate = udiBaudrate))
                           AND ((LEN(sSetFormat) <> 3) OR (sSetFormat = sFormat))
                           AND ((uiSetFrameGap = 0) OR (uiSetFrameGap = uiFrameGap));

// ---------------------------------------------------------------- start a request
IF iState = 0 THEN
    IF (uiErrStreak > 0) AND (TIME() - tLastPoll < T#200MS) THEN
        ;                                                 // after an error: at most 5 tries per second
    ELSIF xBaudDiff OR xFormatDiff OR xGapDiff THEN
        // {"data":[{"type":"parameters","id":"0-0-serial-com1-baudrate","attributes":{"value":19200}}, ...]}
        udiBody := 0;
        xNeedComma := FALSE;
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"data":[', udiBody);
        IF xBaudDiff THEN
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"type":"parameters","id":"0-0-serial-com1-baudrate","attributes":{"value":', udiBody);
            F_AppendStr(ADR(abBody), SIZEOF(abBody), CONCAT(UDINT_TO_STRING(udiSetBaudrate), '}}'), udiBody);
            xNeedComma := TRUE;
        END_IF
        IF xFormatDiff THEN
            IF xNeedComma THEN F_AppendStr(ADR(abBody), SIZEOF(abBody), ',', udiBody); END_IF
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"type":"parameters","id":"0-0-serial-com1-format","attributes":{"value":"', udiBody);
            F_AppendStr(ADR(abBody), SIZEOF(abBody), CONCAT(sSetFormat, '"}}'), udiBody);
            xNeedComma := TRUE;
        END_IF
        IF xGapDiff THEN
            IF xNeedComma THEN F_AppendStr(ADR(abBody), SIZEOF(abBody), ',', udiBody); END_IF
            F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"type":"parameters","id":"0-0-serial-com1-framegap","attributes":{"value":', udiBody);
            F_AppendStr(ADR(abBody), SIZEOF(abBody), CONCAT(UINT_TO_STRING(uiSetFrameGap), '}}'), udiBody);
        END_IF
        F_AppendStr(ADR(abBody), SIZEOF(abBody), ']}', udiBody);
        udiTx := 0;
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'PATCH /wda/parameters HTTP/1.0$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Content-Type: application/vnd.api+json$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), CONCAT(CONCAT('Content-Length: ', UDINT_TO_STRING(udiBody)), '$R$N$R$N'), udiTx);
        FOR j := 0 TO udiBody - 1 DO
            abTx[udiTx] := abBody[j]; udiTx := udiTx + 1;
        END_FOR
        udiTriedBaud := udiSetBaudrate;                   // also when rejected: no endless retries
        sTriedFormat := sSetFormat;
        uiTriedGap := uiSetFrameGap;
        iState := 3;
    ELSIF xSendPending THEN
        // body {"data":{"type":"parameters","id":"0-0-serial-com1-transmit","attributes":{"value":"..."}}}
        udiBody := 0;
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"data":{"type":"parameters","id":"0-0-serial-com1-transmit",', udiBody);
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '"attributes":{"value":"', udiBody);
        IF uiTxLen > 0 THEN
            uiLen := MIN(uiTxLen, 256);
            pS := ADR(abTxData);
        ELSE
            uiLen := TO_UINT(LEN(sTxData));
            pS := ADR(sTxData);
        END_IF
        pHex := ADR(sHex);
        FOR i := 0 TO uiLen - 1 DO                        // one character per byte: \u00XX outside printable ASCII
            IF uiLen = 0 THEN EXIT; END_IF
            b := pS[i];
            IF (b >= 32) AND (b < 127) AND (b <> 34) AND (b <> 92) THEN
                abBody[udiBody] := b; udiBody := udiBody + 1;
            ELSE
                F_AppendStr(ADR(abBody), SIZEOF(abBody), '\u00', udiBody);
                abBody[udiBody] := pHex[b / 16]; abBody[udiBody + 1] := pHex[b MOD 16];
                udiBody := udiBody + 2;
            END_IF
        END_FOR
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '"}}}', udiBody);
        udiTx := 0;
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'PATCH /wda/parameters/0-0-serial-com1-transmit HTTP/1.0$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Content-Type: application/vnd.api+json$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), CONCAT(CONCAT('Content-Length: ', UDINT_TO_STRING(udiBody)), '$R$N$R$N'), udiTx);
        FOR j := 0 TO udiBody - 1 DO
            abTx[udiTx] := abBody[j]; udiTx := udiTx + 1;
        END_FOR
        xSendPending := FALSE;
        xSendBusy := TRUE;
        iState := 1;
    ELSIF TIME() - tLastPoll >= tPoll THEN
        udiTx := 0;
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'GET /wda/parameters?filter[path]=Serial/Com1/Open,Serial/Com1/Baudrate,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Serial/Com1/Format,Serial/Com1/FrameGap,Serial/Com1/TxCount,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Serial/Com1/RxCount,Serial/Com1/RxFrame,Serial/Com1/RxFrameCount,', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Serial/Com1/Errors HTTP/1.0$R$N$R$N', udiTx);
        iState := 2;
    END_IF
    IF iState > 0 THEN
        tLastPoll := TIME();
        fbHttp.pRequest := ADR(abTx);
        fbHttp.udiRequestLen := udiTx;
        fbHttp(xExecute := FALSE);                        // FALSE -> TRUE in the same cycle
    END_IF
END_IF

// ---------------------------------------------------------------- run request
IF iState > 0 THEN
    fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort, tTimeout := tTimeout);
    IF fbHttp.xDone OR fbHttp.xError THEN
        iLastStatus := fbHttp.iStatus;
        p := fbHttp.pBody;
        udiN := fbHttp.udiBodyLen;
        IF fbHttp.xDone AND ((((iState = 1) OR (iState = 3)) AND (iLastStatus = 204)) OR ((iState = 2) AND (iLastStatus = 200))) THEN
            IF iState = 3 THEN
                tLastPoll := TIME() - tPoll;              // settings written: read them back at once
            ELSIF iState = 1 THEN
                xSendBusy := FALSE;
                xSendDone := TRUE;
            ELSE
                xOpen := F_WdaParseScalar(p, udiN, 'Serial/Com1/Open') = 1;
                udiBaudrate := LINT_TO_UDINT(MAX(F_WdaParseScalar(p, udiN, 'Serial/Com1/Baudrate'), 0));
                uiFrameGap := LINT_TO_UINT(MAX(F_WdaParseScalar(p, udiN, 'Serial/Com1/FrameGap'), 0));
                udiTxBytes := LINT_TO_UDINT(MAX(F_WdaParseScalar(p, udiN, 'Serial/Com1/TxCount'), 0));
                udiRxBytes := LINT_TO_UDINT(MAX(F_WdaParseScalar(p, udiN, 'Serial/Com1/RxCount'), 0));
                udiLineErrors := LINT_TO_UDINT(MAX(F_WdaParseScalar(p, udiN, 'Serial/Com1/Errors'), 0));
                iN := F_WdaDecodeBytes(p, udiN, 'Serial/Com1/Format', ADR(sFormat), 3);
                IF iN >= 0 THEN pS := ADR(sFormat); pS[iN] := 0; END_IF
                liVal := F_WdaParseScalar(p, udiN, 'Serial/Com1/RxFrameCount');
                IF (liVal >= 0) AND (LINT_TO_UDINT(liVal) <> udiFrames) THEN
                    iN := F_WdaDecodeBytes(p, udiN, 'Serial/Com1/RxFrame', ADR(abFrame), 512);
                    uiFrameLen := TO_UINT(MAX(iN, 0));
                    pS := ADR(sFrame);                    // as text: up to 255 bytes
                    FOR i := 0 TO 254 DO
                        IF i >= uiFrameLen THEN EXIT; END_IF
                        pS[i] := abFrame[i];
                    END_FOR
                    pS[MIN(uiFrameLen, 255)] := 0;
                    xNewFrame := NOT xFirst;              // not for the frame that was there before the start
                    udiFrames := LINT_TO_UDINT(liVal);
                END_IF
                xFirst := FALSE;
                xKnown := TRUE;
            END_IF
            uiErrStreak := 0;
            xOnline := TRUE;
            iLastWdaCode := 0;
        ELSE
            // 400/500 with WDA code (e.g. 21 = COM1 not open, 39 = character > U+00FF),
            // 404 = gateway without COM1 (SERIAL_DEVICE empty or older than 2.5.0)
            uiErrStreak := uiErrStreak + 1;
            xOnline := uiErrStreak < 3;
            IF iState = 1 THEN xSendBusy := FALSE; END_IF
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
                diPos := F_FindBytes(p, udiN, 0, '"detail":"');
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
        END_IF
        fbHttp(xExecute := FALSE);
        iState := 0;
    END_IF
END_IF
